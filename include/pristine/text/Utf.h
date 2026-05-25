#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace pristine::text {

struct DecodedCodePoint {
    char32_t value;
    size_t byte_length;
};

inline DecodedCodePoint decodeNextCodePoint(std::string_view text, size_t offset) {
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

inline size_t utf16CodeUnitWidth(char32_t code_point) {
    return code_point > 0xFFFF ? 2U : 1U;
}

inline size_t utf16UnitsForUtf8Prefix(std::string_view text, size_t byte_offset) {
    if (byte_offset > text.size()) {
        throw std::runtime_error("Byte offset is out of range");
    }

    size_t offset = 0;
    size_t utf16_units = 0;
    while (offset < byte_offset) {
        const auto decoded = decodeNextCodePoint(text, offset);
        if (offset + decoded.byte_length > byte_offset) {
            throw std::runtime_error("Byte offset splits a UTF-8 code point");
        }

        utf16_units += utf16CodeUnitWidth(decoded.value);
        offset += decoded.byte_length;
    }

    return utf16_units;
}

} // namespace pristine::text