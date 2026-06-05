#include "pristine/jsonrpc/JsonRpcServer.h"

#include "pristine/transport/StdioTransport.h"

#include <stdexcept>

namespace pristine::jsonrpc {
namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInternalError = -32603;

Json getParamsOrDefault(const Json& message) {
    auto params_it = message.find("params");
    if (params_it == message.end()) {
        return Json::object();
    }
    return *params_it;
}

} // namespace

void JsonRpcServer::registerRequestHandler(std::string method, RequestHandler handler) {
    request_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

void JsonRpcServer::registerNotificationHandler(std::string method, NotificationHandler handler) {
    notification_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

int JsonRpcServer::run(transport::MessageTransport& transport) {
    {
        std::lock_guard lock(write_mutex_);
        active_transport_ = &transport;
    }
    while (!stop_requested_) {
        auto payload = transport.read();
        if (!payload.has_value()) {
            break;
        }

        handleIncoming(transport, payload.value());
    }

    {
        std::lock_guard lock(write_mutex_);
        active_transport_ = nullptr;
    }

    return exit_code_;
}

void JsonRpcServer::requestStop(int exit_code) {
    stop_requested_ = true;
    exit_code_ = exit_code;
}

void JsonRpcServer::sendNotification(std::string method, Json params) {
    std::lock_guard lock(write_mutex_);
    if (!active_transport_) {
        throw std::runtime_error("Cannot send a notification when no transport is active");
    }

    active_transport_->write(makeNotification(std::move(method), std::move(params)).dump());
}

void JsonRpcServer::handleIncoming(transport::MessageTransport& transport,
                                   const std::string& payload) {
    Json message;
    try {
        message = Json::parse(payload);
    }
    catch (const std::exception& exception) {
        auto response = makeErrorResponse(nullptr, kParseError, exception.what()).dump();
        std::lock_guard lock(write_mutex_);
        transport.write(response);
        return;
    }

    if (!message.is_object()) {
        auto response = makeErrorResponse(nullptr, kInvalidRequest, "Expected a JSON object").dump();
        std::lock_guard lock(write_mutex_);
        transport.write(response);
        return;
    }

    const auto method_it = message.find("method");
    const bool has_id = message.contains("id");
    const Json message_id = has_id ? message.at("id") : Json(nullptr);

    if (method_it == message.end() || !method_it->is_string()) {
        if (has_id) {
            auto response = makeErrorResponse(message_id, kInvalidRequest, "Missing method").dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        return;
    }

    const auto& method = method_it->get_ref<const std::string&>();
    const Json params = getParamsOrDefault(message);

    if (has_id) {
        auto handler_it = request_handlers_.find(method);
        if (handler_it == request_handlers_.end()) {
            auto response =
                makeErrorResponse(message_id, kMethodNotFound, "Method not found: " + method)
                    .dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
            return;
        }

        try {
            auto response = makeResponse(message_id, handler_it->second(params)).dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        catch (const std::exception& exception) {
            auto response = makeErrorResponse(message_id, kInternalError, exception.what()).dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        return;
    }

    auto notification_it = notification_handlers_.find(method);
    if (notification_it == notification_handlers_.end()) {
        return;
    }

    try {
        notification_it->second(params);
    }
    catch (...) {
        // Notifications do not carry a response channel. The session owns any logging.
    }
}

Json JsonRpcServer::makeResponse(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json JsonRpcServer::makeNotification(std::string method, Json params) {
    return Json{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

Json JsonRpcServer::makeErrorResponse(const Json& id, int code, std::string message) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", Json{{"code", code}, {"message", std::move(message)}}}};
}

} // namespace pristine::jsonrpc
