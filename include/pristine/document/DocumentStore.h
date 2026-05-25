#pragma once

#include "pristine/lsp/Protocol.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace pristine::document {

struct TextDocument {
    std::string uri;
    std::string language_id;
    int version = 0;
    std::string text;
    bool dirty = false;
};

class DocumentStore {
public:
    void open(const lsp::DidOpenTextDocumentParams& params);
    void applyChanges(const lsp::DidChangeTextDocumentParams& params);
    void save(const lsp::DidSaveTextDocumentParams& params);
    void close(const lsp::DidCloseTextDocumentParams& params);

    const TextDocument* find(std::string_view uri) const;
    size_t size() const { return documents_.size(); }

private:
    static void applyChange(TextDocument& document, const lsp::TextDocumentContentChangeEvent& change);

    std::unordered_map<std::string, TextDocument> documents_;
};

} // namespace pristine::document