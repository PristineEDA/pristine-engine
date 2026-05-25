#include "pristine/server/ServerSession.h"

#include <nlohmann/json.hpp>

#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/lsp/Protocol.h"

#include <stdexcept>

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
    server.registerNotificationHandler("textDocument/didOpen", [this](const jsonrpc::Json& params) {
        handleDidOpen(params);
    });
    server.registerNotificationHandler("textDocument/didChange", [this](const jsonrpc::Json& params) {
        handleDidChange(params);
    });
    server.registerNotificationHandler("textDocument/didSave", [this](const jsonrpc::Json& params) {
        handleDidSave(params);
    });
    server.registerNotificationHandler("textDocument/didClose", [this](const jsonrpc::Json& params) {
        handleDidClose(params);
    });
    server.registerNotificationHandler("exit", [this](const jsonrpc::Json& params) {
        handleExit(params);
    });
}

jsonrpc::Json ServerSession::handleInitialize(const jsonrpc::Json& params) {
    workspace_manager_.initialize(lsp::parseInitializeParams(params));
    initialized_ = true;
    shutdown_requested_ = false;
    return lsp::makeInitializeResult(server_name_, server_version_);
}

jsonrpc::Json ServerSession::handleShutdown(const jsonrpc::Json&) {
    shutdown_requested_ = true;
    return nullptr;
}

void ServerSession::handleInitialized(const jsonrpc::Json&) {}

void ServerSession::handleDidOpen(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didOpen received before initialize");
    }

    const auto did_open = lsp::parseDidOpenTextDocumentParams(params);
    document_store_.open(did_open);
    publishDiagnostics(did_open.text_document.uri);
}

void ServerSession::handleDidChange(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didChange received before initialize");
    }

    const auto did_change = lsp::parseDidChangeTextDocumentParams(params);
    document_store_.applyChanges(did_change);
    publishDiagnostics(did_change.text_document.uri);
}

void ServerSession::handleDidSave(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didSave received before initialize");
    }

    const auto did_save = lsp::parseDidSaveTextDocumentParams(params);
    document_store_.save(did_save);
    publishDiagnostics(did_save.text_document.uri);
}

void ServerSession::handleDidClose(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didClose received before initialize");
    }

    const auto did_close = lsp::parseDidCloseTextDocumentParams(params);
    clearDiagnostics(did_close.text_document.uri);
    document_store_.close(did_close);
}

void ServerSession::handleExit(const jsonrpc::Json&) {
    if (!server_) {
        return;
    }

    server_->requestStop(shutdown_requested_ ? 0 : 1);
}

void ServerSession::publishDiagnostics(std::string_view uri) {
    if (!server_) {
        return;
    }

    const auto* document = document_store_.find(uri);
    if (!document) {
        return;
    }

    const auto parse_result = compilation_service_.parse(document->text, document->uri);

    jsonrpc::Json diagnostics = jsonrpc::Json::array();
    for (const auto& diagnostic : parse_result.diagnostics) {
        diagnostics.push_back(jsonrpc::Json{{"range",
                                             jsonrpc::Json{{"start",
                                                            jsonrpc::Json{{"line", diagnostic.range.start_line},
                                                                           {"character", diagnostic.range.start_character}}},
                                                           {"end",
                                                            jsonrpc::Json{{"line", diagnostic.range.end_line},
                                                                           {"character", diagnostic.range.end_character}}}}},
                                            {"severity", diagnostic.severity},
                                            {"code", diagnostic.code},
                                            {"source", server_name_},
                                            {"message", diagnostic.message}});
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", document->uri},
                                            {"diagnostics", std::move(diagnostics)}});
}

void ServerSession::clearDiagnostics(std::string_view uri) {
    if (!server_) {
        return;
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", std::string(uri)},
                                            {"diagnostics", jsonrpc::Json::array()}});
}

} // namespace pristine::server