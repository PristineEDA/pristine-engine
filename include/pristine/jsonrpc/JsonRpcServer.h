#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace pristine::transport {
class MessageTransport;
}

namespace pristine::jsonrpc {

using Json = nlohmann::json;

class JsonRpcServer {
public:
    using RequestHandler = std::function<Json(const Json&)>;
    using NotificationHandler = std::function<void(const Json&)>;

    void registerRequestHandler(std::string method, RequestHandler handler);
    void registerNotificationHandler(std::string method, NotificationHandler handler);

    int run(transport::MessageTransport& transport);
    void requestStop(int exit_code);
    void sendNotification(std::string method, Json params);

private:
    void handleIncoming(transport::MessageTransport& transport, const std::string& payload);

    static Json makeNotification(std::string method, Json params);
    static Json makeResponse(const Json& id, Json result);
    static Json makeErrorResponse(const Json& id, int code, std::string message);

    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
    bool stop_requested_ = false;
    int exit_code_ = 0;
    transport::MessageTransport* active_transport_ = nullptr;
};

} // namespace pristine::jsonrpc