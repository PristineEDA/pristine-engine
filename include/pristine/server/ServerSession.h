#pragma once

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SemanticWorkspace.h"
#include "pristine/analysis/SyntaxDocumentCache.h"
#include "pristine/document/DocumentStore.h"
#include "pristine/waveform/WaveformPipeService.h"
#include "pristine/workspace/WorkspaceManager.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pristine::jsonrpc {
class JsonRpcServer;
using Json = nlohmann::json;
} // namespace pristine::jsonrpc

namespace pristine::server {

class ServerSession {
public:
    ServerSession(std::string server_name, std::string server_version);
    ~ServerSession();

    void bind(jsonrpc::JsonRpcServer& server);

    const document::DocumentStore& documents() const { return document_store_; }
    const workspace::WorkspaceManager& workspace() const { return workspace_manager_; }

private:
    jsonrpc::Json handleInitialize(const jsonrpc::Json& params);
    jsonrpc::Json handleDocumentSymbol(const jsonrpc::Json& params);
    jsonrpc::Json handleOutline(const jsonrpc::Json& params);
    jsonrpc::Json handleModuleHierarchy(const jsonrpc::Json& params);
    jsonrpc::Json handleSchematic(const jsonrpc::Json& params);
    jsonrpc::Json handleBackwardCone(const jsonrpc::Json& params);
    jsonrpc::Json handleWaveformOpen(const jsonrpc::Json& params);
    jsonrpc::Json handleWaveformClose(const jsonrpc::Json& params);
    jsonrpc::Json handleHover(const jsonrpc::Json& params);
    jsonrpc::Json handleDefinition(const jsonrpc::Json& params);
    jsonrpc::Json handleTypeDefinition(const jsonrpc::Json& params);
    jsonrpc::Json handleImplementation(const jsonrpc::Json& params);
    jsonrpc::Json handleDocumentHighlight(const jsonrpc::Json& params);
    jsonrpc::Json handleDocumentLink(const jsonrpc::Json& params);
    jsonrpc::Json handleInlayHint(const jsonrpc::Json& params);
    jsonrpc::Json handleCodeAction(const jsonrpc::Json& params);
    jsonrpc::Json handleFoldingRange(const jsonrpc::Json& params);
    jsonrpc::Json handleSemanticTokensFull(const jsonrpc::Json& params);
    jsonrpc::Json handleSelectionRange(const jsonrpc::Json& params);
    jsonrpc::Json handleSignatureHelp(const jsonrpc::Json& params);
    jsonrpc::Json handlePrepareCallHierarchy(const jsonrpc::Json& params);
    jsonrpc::Json handleIncomingCalls(const jsonrpc::Json& params);
    jsonrpc::Json handleOutgoingCalls(const jsonrpc::Json& params);
    jsonrpc::Json handleReferences(const jsonrpc::Json& params);
    jsonrpc::Json handleWorkspaceSymbol(const jsonrpc::Json& params);
    jsonrpc::Json handleCompletion(const jsonrpc::Json& params);
    jsonrpc::Json handleCompletionItemResolve(const jsonrpc::Json& params);
    jsonrpc::Json handlePrepareRename(const jsonrpc::Json& params);
    jsonrpc::Json handleRename(const jsonrpc::Json& params);
    jsonrpc::Json handleShutdown(const jsonrpc::Json& params);
    void handleInitialized(const jsonrpc::Json& params);
    void handleDidOpen(const jsonrpc::Json& params);
    void handleDidChange(const jsonrpc::Json& params);
    void handleDidSave(const jsonrpc::Json& params);
    void handleDidClose(const jsonrpc::Json& params);
    void handleDidChangeWatchedFiles(const jsonrpc::Json& params);
    void handleExit(const jsonrpc::Json& params);
    void indexWorkspaceSources();
    void updateSemanticDocument(std::string_view uri,
                                std::string_view text,
                                analysis::SemanticDocumentState semantic_state = {});
    void restoreClosedDocument(std::string_view uri);
    void removeSemanticDocument(std::string_view uri);
    void publishDiagnostics(std::string_view uri);
    void publishDiagnostics(std::string_view uri, std::vector<analysis::SemanticEngineDiagnostic> diagnostics);
    void scheduleSemanticDiagnosticsPublish();
    void stopBackgroundDiagnostics();
    void clearDiagnostics(std::string_view uri);
    const std::vector<analysis::DocumentSymbol>& cachedDocumentSymbols(const document::TextDocument& document);
    void invalidateSyntaxCache(std::string_view uri);

    std::string server_name_;
    std::string server_version_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
    jsonrpc::JsonRpcServer* server_ = nullptr;
    analysis::CompilationService compilation_service_;
    analysis::SyntaxDocumentCache syntax_cache_;
    analysis::SemanticWorkspace semantic_workspace_;
    waveform::WaveformPipeService waveform_service_;
    document::DocumentStore document_store_;
    workspace::WorkspaceManager workspace_manager_;
    std::atomic<std::uint64_t> semantic_generation_cache_ = 0;
    mutable std::mutex state_mutex_;
    mutable std::mutex semantic_mutex_;
    std::mutex background_mutex_;
    std::thread diagnostics_thread_;
    std::uint64_t diagnostics_request_generation_ = 0;
    bool diagnostics_stop_requested_ = false;
};

} // namespace pristine::server
