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

} // namespace pristine::lsp