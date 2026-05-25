#include "pristine/lsp/Protocol.h"

#include <stdexcept>

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

VersionedTextDocumentIdentifier parseVersionedTextDocumentIdentifier(const Json& value) {
    return VersionedTextDocumentIdentifier{.uri = value.at("uri").get<std::string>(),
                                           .version = value.at("version").get<int>()};
}

} // namespace

Json makeInitializeResult(std::string_view server_name, std::string_view server_version) {
    return Json{
        {"capabilities",
         Json{{"positionEncoding", "utf-16"},
              {"textDocumentSync",
               Json{{"openClose", true}, {"change", 2}, {"save", Json{{"includeText", false}}}}}}},
        {"serverInfo", Json{{"name", server_name}, {"version", server_version}}},
    };
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
    DidChangeTextDocumentParams result{
        .text_document = parseVersionedTextDocumentIdentifier(params.at("textDocument"))};

    for (const auto& change : params.at("contentChanges")) {
        TextDocumentContentChangeEvent content_change{.text = change.at("text").get<std::string>()};

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
    DidSaveTextDocumentParams result{.text_document =
                                         parseTextDocumentIdentifier(params.at("textDocument"))};

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

} // namespace pristine::lsp