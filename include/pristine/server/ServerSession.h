#pragma once

#include "pristine/analysis/CompilationService.h"
#include "pristine/document/DocumentStore.h"
#include "pristine/workspace/WorkspaceManager.h"

#include <nlohmann/json.hpp>

#include <string>

namespace pristine::jsonrpc {
class JsonRpcServer;
using Json = nlohmann::json;
} // namespace pristine::jsonrpc

namespace pristine::server {

class ServerSession {
public:
    ServerSession(std::string server_name, std::string server_version);

    void bind(jsonrpc::JsonRpcServer& server);

    const document::DocumentStore& documents() const { return document_store_; }
    const workspace::WorkspaceManager& workspace() const { return workspace_manager_; }

private:
    jsonrpc::Json handleInitialize(const jsonrpc::Json& params);
    jsonrpc::Json handleDocumentSymbol(const jsonrpc::Json& params);
    jsonrpc::Json handleHover(const jsonrpc::Json& params);
    jsonrpc::Json handleShutdown(const jsonrpc::Json& params);
    void handleInitialized(const jsonrpc::Json& params);
    void handleDidOpen(const jsonrpc::Json& params);
    void handleDidChange(const jsonrpc::Json& params);
    void handleDidSave(const jsonrpc::Json& params);
    void handleDidClose(const jsonrpc::Json& params);
    void handleExit(const jsonrpc::Json& params);
    void publishDiagnostics(std::string_view uri);
    void clearDiagnostics(std::string_view uri);

    std::string server_name_;
    std::string server_version_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
    jsonrpc::JsonRpcServer* server_ = nullptr;
    analysis::CompilationService compilation_service_;
    document::DocumentStore document_store_;
    workspace::WorkspaceManager workspace_manager_;
};

} // namespace pristine::server