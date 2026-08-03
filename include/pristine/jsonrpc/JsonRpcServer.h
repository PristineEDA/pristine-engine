#pragma once

#include "pristine/Cancellation.h"

#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pristine::transport {
class MessageTransport;
}

namespace pristine::jsonrpc {

using Json = nlohmann::json;

struct RequestContext {
    Json id;
    pristine::CancellationToken cancellation;
    std::optional<Json> work_done_token;
    std::function<void(Json)> report_progress;
};

class JsonRpcServer {
public:
    using RequestHandler = std::function<Json(const Json&)>;
    using ContextualRequestHandler = std::function<Json(const Json&, const RequestContext&)>;
    using NotificationHandler = std::function<void(const Json&)>;
    using ServerResponseHandler = std::function<void(const Json&)>;

    void registerRequestHandler(std::string method, RequestHandler handler);
    void registerRequestHandler(std::string method, ContextualRequestHandler handler);
    void registerNotificationHandler(std::string method, NotificationHandler handler);

    int run(transport::MessageTransport& transport);
    void requestStop(int exit_code);
    void sendNotification(std::string method, Json params);
    [[nodiscard]] Json sendRequest(std::string method,
                                   Json params,
                                   ServerResponseHandler response_handler = {});
    [[nodiscard]] size_t pendingServerRequestCount();

private:
    struct QueuedMessage {
        Json message;
        pristine::CancellationSource cancellation;
        std::string request_key;
    };

    struct PendingServerRequest {
        std::string method;
        ServerResponseHandler response_handler;
    };

    void dispatchLoop(transport::MessageTransport& transport);
    void dispatchIncoming(transport::MessageTransport& transport, QueuedMessage queued);
    void enqueueIncoming(transport::MessageTransport& transport, const std::string& payload);
    void cancelRequest(const Json& params);
    bool consumeServerResponse(const Json& message);
    static std::optional<std::string> requestKey(const Json& id);

    static Json makeNotification(std::string method, Json params);
    static Json makeRequest(Json id, std::string method, Json params);
    static Json makeResponse(const Json& id, Json result);
    static Json makeErrorResponse(const Json& id, int code, std::string message);

    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, ContextualRequestHandler> contextual_request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_int exit_code_ = 0;
    transport::MessageTransport* active_transport_ = nullptr;
    std::mutex write_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedMessage> queue_;
    std::unordered_map<std::string, pristine::CancellationSource> cancellation_states_;
    std::unordered_map<std::string, PendingServerRequest> pending_server_requests_;
    std::uint64_t next_server_request_id_ = 1;
    bool input_closed_ = false;
};

} // namespace pristine::jsonrpc
