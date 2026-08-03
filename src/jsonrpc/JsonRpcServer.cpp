#include "pristine/jsonrpc/JsonRpcServer.h"

#include "pristine/transport/StdioTransport.h"

#include <stdexcept>
#include <thread>

namespace pristine::jsonrpc {
namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInternalError = -32603;
constexpr int kRequestCancelled = -32800;

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

void JsonRpcServer::registerRequestHandler(std::string method, ContextualRequestHandler handler) {
    contextual_request_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

void JsonRpcServer::registerNotificationHandler(std::string method, NotificationHandler handler) {
    notification_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

int JsonRpcServer::run(transport::MessageTransport& transport) {
    {
        std::lock_guard lock(write_mutex_);
        active_transport_ = &transport;
    }
    {
        std::lock_guard lock(queue_mutex_);
        queue_.clear();
        cancellation_states_.clear();
        pending_server_requests_.clear();
        next_server_request_id_ = 1;
        input_closed_ = false;
    }
    stop_requested_.store(false, std::memory_order_release);
    exit_code_.store(0, std::memory_order_release);

    std::thread dispatcher([this, &transport]() { dispatchLoop(transport); });
    while (!stop_requested_.load(std::memory_order_acquire)) {
        auto payload = transport.read();
        if (!payload.has_value()) {
            break;
        }
        enqueueIncoming(transport, payload.value());

        try {
            const auto message = Json::parse(payload.value());
            const auto method_it = message.find("method");
            if (method_it != message.end() && method_it->is_string() &&
                method_it->get_ref<const std::string&>() == "exit") {
                break;
            }
        }
        catch (...) {
        }
    }

    {
        std::lock_guard lock(queue_mutex_);
        pending_server_requests_.clear();
        input_closed_ = true;
    }
    queue_cv_.notify_all();
    dispatcher.join();

    {
        std::lock_guard lock(write_mutex_);
        active_transport_ = nullptr;
    }

    return exit_code_.load(std::memory_order_acquire);
}

void JsonRpcServer::requestStop(int exit_code) {
    stop_requested_.store(true, std::memory_order_release);
    exit_code_.store(exit_code, std::memory_order_release);
}

void JsonRpcServer::sendNotification(std::string method, Json params) {
    std::lock_guard lock(write_mutex_);
    if (!active_transport_) {
        throw std::runtime_error("Cannot send a notification when no transport is active");
    }

    active_transport_->write(makeNotification(std::move(method), std::move(params)).dump());
}

std::optional<std::string> JsonRpcServer::requestKey(const Json& id) {
    if (id.is_string()) {
        return std::string("s:") + id.get<std::string>();
    }
    if (id.is_number_integer() || id.is_number_unsigned()) {
        return std::string("n:") + id.dump();
    }
    return std::nullopt;
}

Json JsonRpcServer::sendRequest(std::string method,
                                Json params,
                                ServerResponseHandler response_handler) {
    Json id;
    std::string key;
    {
        std::lock_guard lock(queue_mutex_);
        id = next_server_request_id_++;
        key = *requestKey(id);
        pending_server_requests_.insert_or_assign(
            key,
            PendingServerRequest{.method = method, .response_handler = std::move(response_handler)});
    }

    try {
        std::lock_guard lock(write_mutex_);
        if (!active_transport_) {
            throw std::runtime_error("Cannot send a request when no transport is active");
        }
        active_transport_->write(makeRequest(id, std::move(method), std::move(params)).dump());
    }
    catch (...) {
        std::lock_guard lock(queue_mutex_);
        pending_server_requests_.erase(key);
        throw;
    }
    return id;
}

size_t JsonRpcServer::pendingServerRequestCount() {
    std::lock_guard lock(queue_mutex_);
    return pending_server_requests_.size();
}

void JsonRpcServer::cancelRequest(const Json& params) {
    const auto id_it = params.find("id");
    if (id_it == params.end()) {
        return;
    }
    const auto key = requestKey(*id_it);
    if (!key.has_value()) {
        return;
    }
    std::lock_guard lock(queue_mutex_);
    const auto state_it = cancellation_states_.find(*key);
    if (state_it != cancellation_states_.end()) {
        state_it->second.cancel();
    }
}

bool JsonRpcServer::consumeServerResponse(const Json& message) {
    const auto id_it = message.find("id");
    if (id_it == message.end()) {
        return false;
    }
    const auto key = requestKey(*id_it);
    if (!key.has_value()) {
        return false;
    }

    ServerResponseHandler handler;
    {
        std::lock_guard lock(queue_mutex_);
        const auto pending = pending_server_requests_.find(*key);
        if (pending == pending_server_requests_.end()) {
            return false;
        }
        handler = std::move(pending->second.response_handler);
        pending_server_requests_.erase(pending);
    }
    if (handler) {
        try {
            handler(message);
        }
        catch (...) {
            // A client response cannot invalidate the serial request dispatcher.
        }
    }
    return true;
}

void JsonRpcServer::enqueueIncoming(transport::MessageTransport& transport,
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
            // Responses to server-originated requests have an id but no method.
            // Unknown responses are intentionally ignored per JSON-RPC.
            consumeServerResponse(message);
        }
        return;
    }

    const auto& method = method_it->get_ref<const std::string&>();
    const Json params = getParamsOrDefault(message);
    if (!has_id && method == "$/cancelRequest") {
        cancelRequest(params);
        return;
    }

    QueuedMessage queued{.message = std::move(message), .cancellation = {}, .request_key = {}};
    if (has_id) {
        const auto key = requestKey(message_id);
        if (!key.has_value()) {
            auto response = makeErrorResponse(message_id, kInvalidRequest, "Request id must be a string or integer")
                                .dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
            return;
        }
        queued.request_key = *key;
        queued.cancellation = pristine::CancellationSource{};
        std::lock_guard lock(queue_mutex_);
        if (cancellation_states_.contains(*key)) {
            auto response = makeErrorResponse(message_id, kInvalidRequest, "Duplicate request id").dump();
            std::lock_guard write_lock(write_mutex_);
            transport.write(response);
            return;
        }
        cancellation_states_.emplace(*key, queued.cancellation);
        queue_.push_back(std::move(queued));
        queue_cv_.notify_one();
        return;
    }

    {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(std::move(queued));
    }
    queue_cv_.notify_one();
}

void JsonRpcServer::dispatchLoop(transport::MessageTransport& transport) {
    while (true) {
        QueuedMessage queued;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return input_closed_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            queued = std::move(queue_.front());
            queue_.pop_front();
        }
        dispatchIncoming(transport, std::move(queued));
        if (stop_requested_.load(std::memory_order_acquire)) {
            std::lock_guard lock(queue_mutex_);
            for (auto& pending : queue_) {
                pending.cancellation.cancel();
            }
        }
    }
}

void JsonRpcServer::dispatchIncoming(transport::MessageTransport& transport,
                                     QueuedMessage queued) {
    const auto& message = queued.message;
    const auto method_it = message.find("method");
    const bool has_id = message.contains("id");
    const Json message_id = has_id ? message.at("id") : Json(nullptr);
    const auto& method = method_it->get_ref<const std::string&>();
    const Json params = getParamsOrDefault(message);

    if (has_id) {
        auto handler_it = request_handlers_.find(method);
        auto contextual_it = contextual_request_handlers_.find(method);
        if (handler_it == request_handlers_.end() && contextual_it == contextual_request_handlers_.end()) {
            auto response =
                makeErrorResponse(message_id, kMethodNotFound, "Method not found: " + method)
                    .dump();
            {
                std::lock_guard queue_lock(queue_mutex_);
                cancellation_states_.erase(queued.request_key);
            }
            {
                std::lock_guard lock(write_mutex_);
                transport.write(response);
            }
            return;
        }

        try {
            RequestContext context{
                .id = message_id,
                .cancellation = queued.cancellation.token(),
                .work_done_token = std::nullopt,
                .report_progress = [](Json) {}};
            if (const auto token_it = params.find("workDoneToken"); token_it != params.end() &&
                (token_it->is_string() || token_it->is_number_integer() || token_it->is_number_unsigned())) {
                context.work_done_token = *token_it;
                context.report_progress = [this, token = *token_it](Json value) {
                    sendNotification("$/progress", Json{{"token", token}, {"value", std::move(value)}});
                };
            }
            context.cancellation.throwIfCancellationRequested();
            Json result = contextual_it != contextual_request_handlers_.end()
                              ? contextual_it->second(params, context)
                              : handler_it->second(params);
            context.cancellation.throwIfCancellationRequested();
            auto response = makeResponse(message_id, std::move(result)).dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        catch (const pristine::OperationCancelled& exception) {
            auto response = makeErrorResponse(message_id, kRequestCancelled, exception.what()).dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        catch (const std::exception& exception) {
            auto response = makeErrorResponse(message_id, kInternalError, exception.what()).dump();
            std::lock_guard lock(write_mutex_);
            transport.write(response);
        }
        {
            std::lock_guard lock(queue_mutex_);
            cancellation_states_.erase(queued.request_key);
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

Json JsonRpcServer::makeRequest(Json id, std::string method, Json params) {
    return Json{{"jsonrpc", "2.0"},
                {"id", std::move(id)},
                {"method", std::move(method)},
                {"params", std::move(params)}};
}

Json JsonRpcServer::makeErrorResponse(const Json& id, int code, std::string message) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", Json{{"code", code}, {"message", std::move(message)}}}};
}

} // namespace pristine::jsonrpc
