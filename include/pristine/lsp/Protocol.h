#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::lsp {

using Json = nlohmann::json;

struct Position {
	int line = 0;
	int character = 0;
};

struct Range {
	Position start;
	Position end;
};

struct TextDocumentIdentifier {
	std::string uri;
};

struct VersionedTextDocumentIdentifier {
	std::string uri;
	int version = 0;
};

struct TextDocumentItem {
	std::string uri;
	std::string language_id;
	int version = 0;
	std::string text;
};

struct TextDocumentContentChangeEvent {
	std::optional<Range> range;
	std::optional<int> range_length;
	std::string text;
};

struct DidOpenTextDocumentParams {
	TextDocumentItem text_document;
};

struct DidChangeTextDocumentParams {
	VersionedTextDocumentIdentifier text_document;
	std::vector<TextDocumentContentChangeEvent> content_changes;
};

struct DidSaveTextDocumentParams {
	TextDocumentIdentifier text_document;
	std::optional<std::string> text;
};

struct DidCloseTextDocumentParams {
	TextDocumentIdentifier text_document;
};

enum class FileChangeType {
	Created = 1,
	Changed = 2,
	Deleted = 3,
};

struct FileChangeEvent {
	std::string uri;
	FileChangeType type = FileChangeType::Changed;
};

struct DidChangeWatchedFilesParams {
	std::vector<FileChangeEvent> changes;
};

struct HoverParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct DefinitionParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct TypeDefinitionParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct ImplementationParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct DocumentHighlightParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct DocumentLinkParams {
	TextDocumentIdentifier text_document;
};

struct InlayHintParams {
	TextDocumentIdentifier text_document;
	Range range;
};

struct CodeActionParams {
	TextDocumentIdentifier text_document;
	Range range;
};

struct FoldingRangeParams {
	TextDocumentIdentifier text_document;
};

struct SemanticTokensParams {
	TextDocumentIdentifier text_document;
};

struct SelectionRangeParams {
	TextDocumentIdentifier text_document;
	std::vector<Position> positions;
};

struct SignatureHelpParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct CallHierarchyPrepareParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct CallHierarchyItem {
	std::string name;
	std::string uri;
	Range range;
	Range selection_range;
};

struct CallHierarchyCallsParams {
	CallHierarchyItem item;
};

struct ReferenceContext {
	bool include_declaration = false;
};

struct ReferenceParams {
	TextDocumentIdentifier text_document;
	Position position;
	ReferenceContext context;
};

struct WorkspaceSymbolParams {
	std::string query;
};

struct CompletionContext {
	int trigger_kind = 1;
	std::optional<std::string> trigger_character;
};

struct CompletionParams {
	TextDocumentIdentifier text_document;
	Position position;
	std::optional<CompletionContext> context;
};

struct PrepareRenameParams {
	TextDocumentIdentifier text_document;
	Position position;
};

struct RenameParams {
	TextDocumentIdentifier text_document;
	Position position;
	std::string new_name;
};

struct WorkspaceFolder {
	std::string uri;
	std::string name;
};

struct InitializeParams {
	std::optional<std::vector<WorkspaceFolder>> workspace_folders;
	std::optional<std::string> root_uri;
	std::optional<std::string> root_path;
};

Json makeInitializeResult(std::string_view server_name, std::string_view server_version);

InitializeParams parseInitializeParams(const Json& params);
DidOpenTextDocumentParams parseDidOpenTextDocumentParams(const Json& params);
DidChangeTextDocumentParams parseDidChangeTextDocumentParams(const Json& params);
DidSaveTextDocumentParams parseDidSaveTextDocumentParams(const Json& params);
DidCloseTextDocumentParams parseDidCloseTextDocumentParams(const Json& params);
DidChangeWatchedFilesParams parseDidChangeWatchedFilesParams(const Json& params);
HoverParams parseHoverParams(const Json& params);
DefinitionParams parseDefinitionParams(const Json& params);
TypeDefinitionParams parseTypeDefinitionParams(const Json& params);
ImplementationParams parseImplementationParams(const Json& params);
DocumentHighlightParams parseDocumentHighlightParams(const Json& params);
DocumentLinkParams parseDocumentLinkParams(const Json& params);
InlayHintParams parseInlayHintParams(const Json& params);
CodeActionParams parseCodeActionParams(const Json& params);
FoldingRangeParams parseFoldingRangeParams(const Json& params);
SemanticTokensParams parseSemanticTokensParams(const Json& params);
SelectionRangeParams parseSelectionRangeParams(const Json& params);
SignatureHelpParams parseSignatureHelpParams(const Json& params);
CallHierarchyPrepareParams parseCallHierarchyPrepareParams(const Json& params);
CallHierarchyCallsParams parseCallHierarchyCallsParams(const Json& params);
ReferenceParams parseReferenceParams(const Json& params);
WorkspaceSymbolParams parseWorkspaceSymbolParams(const Json& params);
CompletionParams parseCompletionParams(const Json& params);
PrepareRenameParams parsePrepareRenameParams(const Json& params);
RenameParams parseRenameParams(const Json& params);

} // namespace pristine::lsp
