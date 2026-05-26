#pragma once

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SymbolIndex.h"
#include "pristine/document/DocumentStore.h"
#include "pristine/workspace/WorkspaceManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

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
    jsonrpc::Json handleModuleHierarchy(const jsonrpc::Json& params);
    jsonrpc::Json handleHover(const jsonrpc::Json& params);
    jsonrpc::Json handleDefinition(const jsonrpc::Json& params);
    jsonrpc::Json handleReferences(const jsonrpc::Json& params);
    jsonrpc::Json handleWorkspaceSymbol(const jsonrpc::Json& params);
    jsonrpc::Json handleCompletion(const jsonrpc::Json& params);
    jsonrpc::Json handleShutdown(const jsonrpc::Json& params);
    void handleInitialized(const jsonrpc::Json& params);
    void handleDidOpen(const jsonrpc::Json& params);
    void handleDidChange(const jsonrpc::Json& params);
    void handleDidSave(const jsonrpc::Json& params);
    void handleDidClose(const jsonrpc::Json& params);
    void handleExit(const jsonrpc::Json& params);
    void indexWorkspaceSources();
    void updateSymbolIndex(std::string_view uri, std::string_view text);
    void updateHierarchyIndex(std::string_view uri, std::string_view text);
    void restoreClosedDocumentIndex(std::string_view uri);
    void removeDocumentIndexes(std::string_view uri);
    void publishDiagnostics(std::string_view uri);
    void clearDiagnostics(std::string_view uri);

    std::string server_name_;
    std::string server_version_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
    jsonrpc::JsonRpcServer* server_ = nullptr;
    analysis::CompilationService compilation_service_;
    analysis::SymbolIndex symbol_index_;
    std::unordered_map<std::string, std::vector<analysis::ModuleDefinition>> hierarchy_documents_;
    document::DocumentStore document_store_;
    workspace::WorkspaceManager workspace_manager_;
};

} // namespace pristine::server