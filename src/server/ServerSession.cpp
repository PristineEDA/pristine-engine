#include "pristine/server/ServerSession.h"

#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/lsp/Protocol.h"

namespace pristine::server {

ServerSession::ServerSession(std::string server_name, std::string server_version) :
    server_name_(std::move(server_name)), server_version_(std::move(server_version)) {}

void ServerSession::bind(jsonrpc::JsonRpcServer& server) {
    server_ = &server;

    server.registerRequestHandler("initialize", [this](const jsonrpc::Json& params) {
        return handleInitialize(params);
    });
    server.registerRequestHandler("shutdown", [this](const jsonrpc::Json& params) {
        return handleShutdown(params);
    });

    server.registerNotificationHandler("initialized", [this](const jsonrpc::Json& params) {
        handleInitialized(params);
    });
    server.registerNotificationHandler("exit", [this](const jsonrpc::Json& params) {
        handleExit(params);
    });
}

jsonrpc::Json ServerSession::handleInitialize(const jsonrpc::Json&) {
    initialized_ = true;
    shutdown_requested_ = false;
    return lsp::makeInitializeResult(server_name_, server_version_);
}

jsonrpc::Json ServerSession::handleShutdown(const jsonrpc::Json&) {
    shutdown_requested_ = true;
    return nullptr;
}

void ServerSession::handleInitialized(const jsonrpc::Json&) {}

void ServerSession::handleExit(const jsonrpc::Json&) {
    if (!server_) {
        return;
    }

    server_->requestStop(shutdown_requested_ ? 0 : 1);
}

} // namespace pristine::server