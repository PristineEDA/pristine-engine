#include "pristine/lsp/Protocol.h"

#include <stdexcept>
#include <utility>

namespace pristine::lsp {
namespace {

Position parsePosition(const Json& value) {
    return Position{.line = value.at("line").get<int>(),
                    .character = value.at("character").get<int>()};
}

Range parseRange(const Json& value) {
    return Range{.start = parsePosition(value.at("start")),
                 .end = parsePosition(value.at("end"))};
}

TextDocumentIdentifier parseTextDocumentIdentifier(const Json& value) {
    return TextDocumentIdentifier{.uri = value.at("uri").get<std::string>()};
}

std::optional<std::string> parseOptionalString(const Json& value, std::string_view key) {
    const auto field_it = value.find(key);
    if (field_it == value.end() || field_it->is_null()) {
        return std::nullopt;
    }

    return field_it->get<std::string>();
}

VersionedTextDocumentIdentifier parseVersionedTextDocumentIdentifier(const Json& value) {
    return VersionedTextDocumentIdentifier{.uri = value.at("uri").get<std::string>(),
                                           .version = value.at("version").get<int>()};
}

FileChangeType parseFileChangeType(const Json& value) {
    switch (value.get<int>()) {
        case 1:
            return FileChangeType::Created;
        case 2:
            return FileChangeType::Changed;
        case 3:
            return FileChangeType::Deleted;
        default:
            throw std::runtime_error("Unknown file change type");
    }
}

} // namespace

Json makeInitializeResult(std::string_view server_name, std::string_view server_version) {
    return Json{
        {"capabilities",
         Json{{"positionEncoding", "utf-16"},
              {"documentSymbolProvider", true},
              {"hoverProvider", true},
              {"definitionProvider", true},
              {"typeDefinitionProvider", true},
              {"implementationProvider", true},
              {"documentHighlightProvider", true},
              {"documentLinkProvider", Json{{"resolveProvider", false}}},
              {"inlayHintProvider", Json{{"resolveProvider", false}}},
              {"codeActionProvider",
               Json{{"resolveProvider", false},
                    {"codeActionKinds", Json::array({"quickfix"})}}},
              {"foldingRangeProvider", true},
              {"semanticTokensProvider",
               Json{{"legend",
                   Json{{"tokenTypes",
                       Json::array({"namespace", "type", "class", "enum", "interface",
                                "function", "variable", "parameter", "enumMember"})},
                      {"tokenModifiers", Json::array()}}},
                  {"full", true},
                  {"range", false}}},
              {"selectionRangeProvider", true},
              {"signatureHelpProvider",
               Json{{"triggerCharacters", Json::array({"(", ","})},
                    {"retriggerCharacters", Json::array({","})}}},
              {"callHierarchyProvider", true},
              {"referencesProvider", true},
              {"renameProvider", Json{{"prepareProvider", true}}},
              {"workspaceSymbolProvider", true},
              {"completionProvider",
                Json{{"resolveProvider", true},
                    {"triggerCharacters", Json::array({".", "`", ":"})}}},
              {"experimental",
               Json{{"pristineWaveformProvider",
                     Json{{"transport", "pipe"},
                          {"protocol", "pristine-waveform-columnar-v1"},
                          {"mock", true},
                          {"sources", Json::array({"mock", "fst"})}}}}},
              {"textDocumentSync",
               Json{{"openClose", true}, {"change", 2}, {"save", Json{{"includeText", false}}}}},
              {"workspace",
               Json{{"workspaceFolders",
                     Json{{"supported", true}, {"changeNotifications", false}}}}}}},
        {"serverInfo", Json{{"name", server_name}, {"version", server_version}}},
    };
}

InitializeParams parseInitializeParams(const Json& params) {
    InitializeParams result{};
    result.root_uri = parseOptionalString(params, "rootUri");
    result.root_path = parseOptionalString(params, "rootPath");

    const auto workspace_folders_it = params.find("workspaceFolders");
    if (workspace_folders_it != params.end() && !workspace_folders_it->is_null()) {
        result.workspace_folders = std::vector<WorkspaceFolder>{};
        for (const auto& folder : *workspace_folders_it) {
            result.workspace_folders->push_back(WorkspaceFolder{.uri = folder.at("uri").get<std::string>(),
                                                                .name = folder.at("name").get<std::string>()});
        }
    }

    return result;
}

DidOpenTextDocumentParams parseDidOpenTextDocumentParams(const Json& params) {
    const auto& text_document = params.at("textDocument");
    return DidOpenTextDocumentParams{.text_document =
                                         TextDocumentItem{.uri = text_document.at("uri").get<std::string>(),
                                                          .language_id =
                                                              text_document.at("languageId").get<std::string>(),
                                                          .version = text_document.at("version").get<int>(),
                                                          .text = text_document.at("text").get<std::string>()}};
}

DidChangeTextDocumentParams parseDidChangeTextDocumentParams(const Json& params) {
    DidChangeTextDocumentParams result{};
    result.text_document = parseVersionedTextDocumentIdentifier(params.at("textDocument"));

    for (const auto& change : params.at("contentChanges")) {
        TextDocumentContentChangeEvent content_change{};
        content_change.text = change.at("text").get<std::string>();

        const auto range_it = change.find("range");
        if (range_it != change.end() && !range_it->is_null()) {
            content_change.range = parseRange(*range_it);
        }

        const auto range_length_it = change.find("rangeLength");
        if (range_length_it != change.end() && !range_length_it->is_null()) {
            content_change.range_length = range_length_it->get<int>();
        }

        result.content_changes.push_back(std::move(content_change));
    }

    return result;
}

DidSaveTextDocumentParams parseDidSaveTextDocumentParams(const Json& params) {
    DidSaveTextDocumentParams result{};
    result.text_document = parseTextDocumentIdentifier(params.at("textDocument"));

    const auto text_it = params.find("text");
    if (text_it != params.end() && !text_it->is_null()) {
        result.text = text_it->get<std::string>();
    }

    return result;
}

DidCloseTextDocumentParams parseDidCloseTextDocumentParams(const Json& params) {
    return DidCloseTextDocumentParams{.text_document =
                                          parseTextDocumentIdentifier(params.at("textDocument"))};
}

DidChangeWatchedFilesParams parseDidChangeWatchedFilesParams(const Json& params) {
    DidChangeWatchedFilesParams result{};
    for (const auto& change : params.at("changes")) {
        result.changes.push_back(FileChangeEvent{.uri = change.at("uri").get<std::string>(),
                                                 .type = parseFileChangeType(change.at("type"))});
    }
    return result;
}

HoverParams parseHoverParams(const Json& params) {
    return HoverParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                       .position = parsePosition(params.at("position"))};
}

DefinitionParams parseDefinitionParams(const Json& params) {
    return DefinitionParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                            .position = parsePosition(params.at("position"))};
}

TypeDefinitionParams parseTypeDefinitionParams(const Json& params) {
    return TypeDefinitionParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                                .position = parsePosition(params.at("position"))};
}

ImplementationParams parseImplementationParams(const Json& params) {
    return ImplementationParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                                .position = parsePosition(params.at("position"))};
}

DocumentHighlightParams parseDocumentHighlightParams(const Json& params) {
    return DocumentHighlightParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                                   .position = parsePosition(params.at("position"))};
}

DocumentLinkParams parseDocumentLinkParams(const Json& params) {
    return DocumentLinkParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument"))};
}

InlayHintParams parseInlayHintParams(const Json& params) {
    return InlayHintParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                           .range = parseRange(params.at("range"))};
}

CodeActionParams parseCodeActionParams(const Json& params) {
    return CodeActionParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                            .range = parseRange(params.at("range"))};
}

FoldingRangeParams parseFoldingRangeParams(const Json& params) {
    return FoldingRangeParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument"))};
}

SemanticTokensParams parseSemanticTokensParams(const Json& params) {
    return SemanticTokensParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument"))};
}

SelectionRangeParams parseSelectionRangeParams(const Json& params) {
    SelectionRangeParams result{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                                .positions = {}};
    for (const auto& position : params.at("positions")) {
        result.positions.push_back(parsePosition(position));
    }
    return result;
}

SignatureHelpParams parseSignatureHelpParams(const Json& params) {
    return SignatureHelpParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                               .position = parsePosition(params.at("position"))};
}

CallHierarchyItem parseCallHierarchyItem(const Json& value) {
    return CallHierarchyItem{.name = value.at("name").get<std::string>(),
                             .uri = value.at("uri").get<std::string>(),
                             .range = parseRange(value.at("range")),
                             .selection_range = parseRange(value.at("selectionRange"))};
}

CallHierarchyPrepareParams parseCallHierarchyPrepareParams(const Json& params) {
    return CallHierarchyPrepareParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                                      .position = parsePosition(params.at("position"))};
}

CallHierarchyCallsParams parseCallHierarchyCallsParams(const Json& params) {
    return CallHierarchyCallsParams{.item = parseCallHierarchyItem(params.at("item"))};
}

ReferenceParams parseReferenceParams(const Json& params) {
    ReferenceParams result{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                           .position = parsePosition(params.at("position")),
                           .context = ReferenceContext{}};

    const auto context_it = params.find("context");
    if (context_it != params.end() && !context_it->is_null()) {
        const auto include_declaration_it = context_it->find("includeDeclaration");
        if (include_declaration_it != context_it->end() && !include_declaration_it->is_null()) {
            result.context.include_declaration = include_declaration_it->get<bool>();
        }
    }

    return result;
}

WorkspaceSymbolParams parseWorkspaceSymbolParams(const Json& params) {
    WorkspaceSymbolParams result{};
    const auto query_it = params.find("query");
    if (query_it != params.end() && !query_it->is_null()) {
        result.query = query_it->get<std::string>();
    }
    return result;
}

CompletionParams parseCompletionParams(const Json& params) {
    CompletionParams result{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                            .position = parsePosition(params.at("position")),
                            .context = std::nullopt};

    const auto context_it = params.find("context");
    if (context_it != params.end() && !context_it->is_null()) {
        CompletionContext context{};
        const auto trigger_kind_it = context_it->find("triggerKind");
        if (trigger_kind_it != context_it->end() && !trigger_kind_it->is_null()) {
            context.trigger_kind = trigger_kind_it->get<int>();
        }

        const auto trigger_character_it = context_it->find("triggerCharacter");
        if (trigger_character_it != context_it->end() && !trigger_character_it->is_null()) {
            context.trigger_character = trigger_character_it->get<std::string>();
        }

        result.context = std::move(context);
    }

    return result;
}

PrepareRenameParams parsePrepareRenameParams(const Json& params) {
    return PrepareRenameParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                               .position = parsePosition(params.at("position"))};
}

RenameParams parseRenameParams(const Json& params) {
    return RenameParams{.text_document = parseTextDocumentIdentifier(params.at("textDocument")),
                        .position = parsePosition(params.at("position")),
                        .new_name = params.at("newName").get<std::string>()};
}

} // namespace pristine::lsp
