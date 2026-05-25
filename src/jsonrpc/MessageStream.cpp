#include "pristine/jsonrpc/MessageStream.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace pristine::jsonrpc {
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }

    return value;
}

bool startsWithInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), value.begin(), [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) ==
               std::tolower(static_cast<unsigned char>(rhs));
    });
}

} // namespace

std::optional<std::string> MessageStream::read(std::istream& input) const {
    std::string header_line;
    std::optional<size_t> content_length;
    bool saw_any_header = false;

    while (std::getline(input, header_line)) {
        saw_any_header = true;

        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }

        if (header_line.empty()) {
            break;
        }

        if (auto parsed = parseContentLength(header_line)) {
            content_length = parsed;
        }
    }

    if (!saw_any_header) {
        return std::nullopt;
    }

    if (!content_length.has_value()) {
        throw std::runtime_error("Missing Content-Length header");
    }

    std::string payload(content_length.value(), '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
        throw std::runtime_error("Unexpected end of stream while reading payload");
    }

    return payload;
}

void MessageStream::write(std::ostream& output, std::string_view payload) const {
    output << "Content-Length: " << payload.size() << "\r\n\r\n";
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
}

std::optional<size_t> MessageStream::parseContentLength(std::string_view header_line) {
    constexpr std::string_view prefix = "Content-Length:";
    if (!startsWithInsensitive(header_line, prefix)) {
        return std::nullopt;
    }

    const auto trimmed = trim(header_line.substr(prefix.size()));
    if (trimmed.empty()) {
        throw std::runtime_error("Content-Length header is empty");
    }

    size_t value = 0;
    for (char character : trimmed) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            throw std::runtime_error("Content-Length header contains a non-digit");
        }
        value = (value * 10) + static_cast<size_t>(character - '0');
    }

    return value;
}

} // namespace pristine::jsonrpc