#include "pristine/server/ServerSession.h"

#include <nlohmann/json.hpp>

#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/lsp/Protocol.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

namespace pristine::server {
namespace {

namespace fs = std::filesystem;

jsonrpc::Json toRangeJson(const analysis::ParseRange& range) {
    return jsonrpc::Json{{"start",
                          jsonrpc::Json{{"line", range.start_line},
                                         {"character", range.start_character}}},
                         {"end",
                          jsonrpc::Json{{"line", range.end_line},
                                         {"character", range.end_character}}}};
}

jsonrpc::Json toLocationJson(const analysis::Location& location) {
    return jsonrpc::Json{{"uri", location.uri}, {"range", toRangeJson(location.range)}};
}

int toCompletionItemKind(int symbol_kind) {
    switch (symbol_kind) {
        case 2:
            return 9;
        case 5:
            return 7;
        case 10:
            return 13;
        case 11:
            return 8;
        case 12:
            return 3;
        case 13:
            return 6;
        case 14:
            return 21;
        case 22:
            return 20;
        case 26:
            return 25;
        default:
            return 18;
    }
}

std::string percentEncodePath(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':') {
            result.push_back(static_cast<char>(ch));
            continue;
        }

        result.push_back('%');
        result.push_back(hex[(ch >> 4U) & 0x0FU]);
        result.push_back(hex[ch & 0x0FU]);
    }

    return result;
}

std::string toFileUri(const fs::path& path) {
    std::error_code error;
    auto normalized = fs::weakly_canonical(path, error);
    if (error) {
        normalized = fs::absolute(path, error);
    }
    const auto generic = normalized.generic_string();
    return std::string("file://") + (generic.starts_with('/') ? "" : "/") + percentEncodePath(generic);
}

std::optional<std::string> readFileText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

jsonrpc::Json toDocumentSymbolJson(const analysis::DocumentSymbol& symbol) {
    jsonrpc::Json result{{"name", symbol.name},
                         {"kind", symbol.kind},
                         {"range", toRangeJson(symbol.range)},
                         {"selectionRange", toRangeJson(symbol.selection_range)}};

    if (!symbol.children.empty()) {
        result["children"] = jsonrpc::Json::array();
        for (const auto& child : symbol.children) {
            result["children"].push_back(toDocumentSymbolJson(child));
        }
    }

    return result;
}

} // namespace

ServerSession::ServerSession(std::string server_name, std::string server_version) :
    server_name_(std::move(server_name)), server_version_(std::move(server_version)) {}

void ServerSession::bind(jsonrpc::JsonRpcServer& server) {
    server_ = &server;

    server.registerRequestHandler("initialize", [this](const jsonrpc::Json& params) {
        return handleInitialize(params);
    });
    server.registerRequestHandler("textDocument/documentSymbol", [this](const jsonrpc::Json& params) {
        return handleDocumentSymbol(params);
    });
    server.registerRequestHandler("textDocument/hover", [this](const jsonrpc::Json& params) {
        return handleHover(params);
    });
    server.registerRequestHandler("textDocument/definition", [this](const jsonrpc::Json& params) {
        return handleDefinition(params);
    });
    server.registerRequestHandler("textDocument/references", [this](const jsonrpc::Json& params) {
        return handleReferences(params);
    });
    server.registerRequestHandler("workspace/symbol", [this](const jsonrpc::Json& params) {
        return handleWorkspaceSymbol(params);
    });
    server.registerRequestHandler("textDocument/completion", [this](const jsonrpc::Json& params) {
        return handleCompletion(params);
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
    symbol_index_.clear();
    indexWorkspaceSources();
    initialized_ = true;
    shutdown_requested_ = false;
    return lsp::makeInitializeResult(server_name_, server_version_);
}

jsonrpc::Json ServerSession::handleDocumentSymbol(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/documentSymbol received before initialize");
    }

    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto* document = document_store_.find(uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : compilation_service_.documentSymbols(document->text, document->uri)) {
        result.push_back(toDocumentSymbolJson(symbol));
    }

    return result;
}

jsonrpc::Json ServerSession::handleHover(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/hover received before initialize");
    }

    const auto hover = lsp::parseHoverParams(params);
    const auto* document = document_store_.find(hover.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto result = compilation_service_.hover(document->text, document->uri, hover.position.line,
                                                   hover.position.character);
    if (!result) {
        return nullptr;
    }

    return jsonrpc::Json{{"contents", jsonrpc::Json{{"kind", "markdown"}, {"value", result->contents}}},
                         {"range", toRangeJson(result->range)}};
}

jsonrpc::Json ServerSession::handleDefinition(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/definition received before initialize");
    }

    const auto definition = lsp::parseDefinitionParams(params);
    const auto* document = document_store_.find(definition.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto identifier = compilation_service_.identifierAt(document->text, definition.position.line,
                                                              definition.position.character);
    if (!identifier) {
        return nullptr;
    }

    const auto definitions = symbol_index_.definitions(identifier->name, document->uri);
    if (definitions.empty()) {
        return nullptr;
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : definitions) {
        result.push_back(toLocationJson(symbol.location));
    }

    return result;
}

jsonrpc::Json ServerSession::handleReferences(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/references received before initialize");
    }

    const auto references = lsp::parseReferenceParams(params);
    const auto* document = document_store_.find(references.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto identifier = compilation_service_.identifierAt(document->text, references.position.line,
                                                              references.position.character);
    if (!identifier) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& reference : symbol_index_.references(identifier->name,
                                                          references.context.include_declaration)) {
        result.push_back(toLocationJson(reference.location));
    }

    return result;
}

jsonrpc::Json ServerSession::handleWorkspaceSymbol(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("workspace/symbol received before initialize");
    }

    const auto workspace_symbol = lsp::parseWorkspaceSymbolParams(params);
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : symbol_index_.workspaceSymbols(workspace_symbol.query)) {
        result.push_back(jsonrpc::Json{{"name", symbol.name},
                                       {"kind", symbol.kind},
                                       {"location", toLocationJson(symbol.location)}});
    }

    return result;
}

jsonrpc::Json ServerSession::handleCompletion(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/completion received before initialize");
    }

    const auto completion = lsp::parseCompletionParams(params);
    const auto* document = document_store_.find(completion.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto prefix = compilation_service_.completionPrefix(document->text, completion.position.line,
                                                              completion.position.character);
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& item : symbol_index_.completions(prefix, document->uri)) {
        result.push_back(jsonrpc::Json{{"label", item.label},
                                       {"kind", toCompletionItemKind(item.kind)},
                                       {"detail", item.detail}});
    }

    return result;
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
    updateSymbolIndex(did_open.text_document.uri, did_open.text_document.text);
    publishDiagnostics(did_open.text_document.uri);
}

void ServerSession::handleDidChange(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didChange received before initialize");
    }

    const auto did_change = lsp::parseDidChangeTextDocumentParams(params);
    document_store_.applyChanges(did_change);
    if (const auto* document = document_store_.find(did_change.text_document.uri)) {
        updateSymbolIndex(document->uri, document->text);
    }
    publishDiagnostics(did_change.text_document.uri);
}

void ServerSession::handleDidSave(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didSave received before initialize");
    }

    const auto did_save = lsp::parseDidSaveTextDocumentParams(params);
    document_store_.save(did_save);
    if (const auto* document = document_store_.find(did_save.text_document.uri)) {
        updateSymbolIndex(document->uri, document->text);
    }
    publishDiagnostics(did_save.text_document.uri);
}

void ServerSession::handleDidClose(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didClose received before initialize");
    }

    const auto did_close = lsp::parseDidCloseTextDocumentParams(params);
    clearDiagnostics(did_close.text_document.uri);
    document_store_.close(did_close);
    restoreClosedDocumentIndex(did_close.text_document.uri);
}

void ServerSession::handleExit(const jsonrpc::Json&) {
    if (!server_) {
        return;
    }

    server_->requestStop(shutdown_requested_ ? 0 : 1);
}

void ServerSession::indexWorkspaceSources() {
    for (const auto& path : workspace_manager_.sourceFilesForIndex()) {
        const auto text = readFileText(path);
        if (!text.has_value()) {
            continue;
        }
        updateSymbolIndex(toFileUri(path), *text);
    }
}

void ServerSession::updateSymbolIndex(std::string_view uri, std::string_view text) {
    symbol_index_.updateDocument(uri, text);
}

void ServerSession::restoreClosedDocumentIndex(std::string_view uri) {
    const auto path = workspace::WorkspaceManager::pathFromFileUri(uri);
    if (!path.has_value()) {
        symbol_index_.removeDocument(uri);
        return;
    }

    std::error_code error;
    if (!fs::exists(*path, error) || !fs::is_regular_file(*path, error)) {
        symbol_index_.removeDocument(uri);
        return;
    }

    const auto text = readFileText(*path);
    if (!text.has_value()) {
        symbol_index_.removeDocument(uri);
        return;
    }

    updateSymbolIndex(uri, *text);
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