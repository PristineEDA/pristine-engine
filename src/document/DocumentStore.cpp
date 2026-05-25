#include "pristine/document/DocumentStore.h"

#include "pristine/text/Utf.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace pristine::document {
namespace {

size_t findLineOffset(std::string_view text, int line) {
    if (line < 0) {
        throw std::runtime_error("Line cannot be negative");
    }

    size_t offset = 0;
    for (int current_line = 0; current_line < line; ++current_line) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            throw std::runtime_error("Line is out of range");
        }
        offset = newline + 1;
    }

    return offset;
}

bool isLineBreak(std::string_view text, size_t offset) {
    if (offset >= text.size()) {
        return false;
    }

    return text[offset] == '\n' || text[offset] == '\r';
}

size_t findByteOffset(std::string_view text, const lsp::Position& position) {
    if (position.character < 0) {
        throw std::runtime_error("Character cannot be negative");
    }

    size_t offset = findLineOffset(text, position.line);
    int consumed_utf16 = 0;

    while (offset < text.size() && !isLineBreak(text, offset)) {
        if (consumed_utf16 == position.character) {
            return offset;
        }

        const auto decoded = text::decodeNextCodePoint(text, offset);
        const auto width = static_cast<int>(text::utf16CodeUnitWidth(decoded.value));
        if (consumed_utf16 + width > position.character) {
            throw std::runtime_error("Position splits a UTF-16 surrogate pair");
        }

        consumed_utf16 += width;
        offset += decoded.byte_length;
    }

    if (consumed_utf16 == position.character) {
        return offset;
    }

    throw std::runtime_error("Character is out of range");
}

} // namespace

void DocumentStore::open(const lsp::DidOpenTextDocumentParams& params) {
    documents_.insert_or_assign(
        params.text_document.uri,
        TextDocument{.uri = params.text_document.uri,
                     .language_id = params.text_document.language_id,
                     .version = params.text_document.version,
                     .text = params.text_document.text,
                     .dirty = false});
}

void DocumentStore::applyChanges(const lsp::DidChangeTextDocumentParams& params) {
    auto document_it = documents_.find(params.text_document.uri);
    if (document_it == documents_.end()) {
        throw std::runtime_error("textDocument/didChange received for an unopened document");
    }

    auto& document = document_it->second;
    if (params.text_document.version <= document.version) {
        throw std::runtime_error("textDocument/didChange version must advance");
    }

    for (const auto& change : params.content_changes) {
        applyChange(document, change);
    }

    document.version = params.text_document.version;
    document.dirty = true;
}

void DocumentStore::save(const lsp::DidSaveTextDocumentParams& params) {
    auto document_it = documents_.find(params.text_document.uri);
    if (document_it == documents_.end()) {
        throw std::runtime_error("textDocument/didSave received for an unopened document");
    }

    auto& document = document_it->second;
    if (params.text.has_value()) {
        document.text = *params.text;
    }
    document.dirty = false;
}

void DocumentStore::close(const lsp::DidCloseTextDocumentParams& params) {
    documents_.erase(params.text_document.uri);
}

const TextDocument* DocumentStore::find(std::string_view uri) const {
    auto document_it = documents_.find(std::string(uri));
    if (document_it == documents_.end()) {
        return nullptr;
    }

    return &document_it->second;
}

void DocumentStore::applyChange(TextDocument& document,
                                const lsp::TextDocumentContentChangeEvent& change) {
    if (!change.range.has_value()) {
        document.text = change.text;
        return;
    }

    const auto start_offset = findByteOffset(document.text, change.range->start);
    const auto end_offset = findByteOffset(document.text, change.range->end);
    if (end_offset < start_offset) {
        throw std::runtime_error("textDocument/didChange range end precedes range start");
    }

    document.text.replace(start_offset, end_offset - start_offset, change.text);
}

} // namespace pristine::document