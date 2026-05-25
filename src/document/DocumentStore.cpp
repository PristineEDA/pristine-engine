#include "pristine/document/DocumentStore.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace pristine::document {
namespace {

struct DecodedCodePoint {
    char32_t value;
    size_t byte_length;
};

DecodedCodePoint decodeNextCodePoint(std::string_view text, size_t offset) {
    if (offset >= text.size()) {
        throw std::runtime_error("Unexpected end of UTF-8 input");
    }

    const auto first = static_cast<unsigned char>(text[offset]);
    if ((first & 0x80U) == 0) {
        return DecodedCodePoint{.value = static_cast<char32_t>(first), .byte_length = 1};
    }

    size_t expected_length = 0;
    char32_t code_point = 0;
    if ((first & 0xE0U) == 0xC0U) {
        expected_length = 2;
        code_point = static_cast<char32_t>(first & 0x1FU);
    }
    else if ((first & 0xF0U) == 0xE0U) {
        expected_length = 3;
        code_point = static_cast<char32_t>(first & 0x0FU);
    }
    else if ((first & 0xF8U) == 0xF0U) {
        expected_length = 4;
        code_point = static_cast<char32_t>(first & 0x07U);
    }
    else {
        throw std::runtime_error("Invalid UTF-8 leading byte");
    }

    if (offset + expected_length > text.size()) {
        throw std::runtime_error("Truncated UTF-8 sequence");
    }

    for (size_t index = 1; index < expected_length; ++index) {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        if ((byte & 0xC0U) != 0x80U) {
            throw std::runtime_error("Invalid UTF-8 continuation byte");
        }

        code_point = static_cast<char32_t>((code_point << 6) | (byte & 0x3FU));
    }

    return DecodedCodePoint{.value = code_point, .byte_length = expected_length};
}

size_t utf16Width(char32_t code_point) {
    return code_point > 0xFFFF ? 2U : 1U;
}

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

        const auto decoded = decodeNextCodePoint(text, offset);
        const auto width = static_cast<int>(utf16Width(decoded.value));
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