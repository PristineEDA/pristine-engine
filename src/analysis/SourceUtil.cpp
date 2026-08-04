#include "pristine/analysis/SourceUtil.h"

#include "pristine/text/Utf.h"

#include "slang/diagnostics/Diagnostics.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace pristine::analysis {
namespace {

bool isDriveSegment(std::string_view value) {
    return value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':';
}

#ifdef _WIN32
constexpr std::string_view kInMemoryUriRoot = "C:/__pristine_inmemory_uri__";

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}
#endif

int utf16ColumnForLocation(const slang::SourceManager& source_manager,
                           slang::SourceLocation location) {
    const auto fallback_character = static_cast<int>(source_manager.getColumnNumber(location)) - 1;
    const auto byte_offset = location.offset();
    const auto byte_column = source_manager.getColumnNumber(location);
    if (byte_column == 0) {
        return fallback_character;
    }

    const auto byte_index_in_line = byte_column - 1;
    const auto text = source_manager.getSourceText(location.buffer());
    if (byte_offset < byte_index_in_line || byte_offset > text.size()) {
        return fallback_character;
    }

    const auto line_start_offset = byte_offset - byte_index_in_line;
    try {
        return static_cast<int>(
            text::utf16UnitsForUtf8Prefix(text.substr(line_start_offset, byte_offset - line_start_offset),
                                          byte_offset - line_start_offset));
    }
    catch (const std::runtime_error&) {
        return fallback_character;
    }
}

ParseRange sourceRangeForLocations(const slang::SourceManager& source_manager,
                                   slang::SourceLocation start,
                                   slang::SourceLocation end) {
    if (!start.valid()) {
        return {};
    }
    if (!end.valid() || end.buffer() != start.buffer() || end.offset() < start.offset()) {
        end = start + 1;
    }

    return ParseRange{
        .start_line = static_cast<int>(source_manager.getLineNumber(start)) - 1,
        .start_character = utf16ColumnForLocation(source_manager, start),
        .end_line = static_cast<int>(source_manager.getLineNumber(end)) - 1,
        .end_character = utf16ColumnForLocation(source_manager, end)};
}

} // namespace

bool isFileUri(std::string_view value) {
    return value.starts_with("file://");
}

bool isWindowsAbsolutePath(std::string_view value) {
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

std::string toForwardSlashes(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string normalizeFileUri(std::string_view uri) {
    if (!isFileUri(uri)) {
        const auto normalized_path = toForwardSlashes(uri);
        if (isWindowsAbsolutePath(normalized_path)) {
            return std::string("file:///") + normalized_path;
        }
        return normalized_path;
    }

    constexpr std::string_view prefix = "file://";
    const auto path = toForwardSlashes(uri.substr(prefix.size()));
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> segments;

    size_t position = 0;
    while (position <= path.size()) {
        const auto separator = path.find('/', position);
        const auto segment = path.substr(position, separator == std::string::npos
                                                       ? std::string::npos
                                                       : separator - position);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (!segments.empty() && !isDriveSegment(segments.back())) {
                    segments.pop_back();
                }
                else if (!absolute) {
                    segments.emplace_back(segment);
                }
            }
            else {
                segments.emplace_back(segment);
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        position = separator + 1;
    }

    std::string normalized_path = absolute ? "/" : "";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized_path.push_back('/');
        }
        normalized_path += segments[index];
    }
    return std::string(prefix) + normalized_path;
}

std::string withoutTrailingSlash(std::string value) {
    constexpr std::string_view root_uri = "file:///";
    while (value.size() > root_uri.size() && value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

std::string uriDirectory(std::string_view uri) {
    auto normalized = withoutTrailingSlash(normalizeFileUri(uri));
    constexpr std::string_view prefix = "file://";
    const auto separator = normalized.rfind('/');
    if (separator == std::string::npos || separator <= prefix.size()) {
        return normalized;
    }
    return normalized.substr(0, separator);
}

std::string joinFileUri(std::string_view base_uri, std::string_view target) {
    if (target.empty()) {
        return {};
    }
    if (isFileUri(target)) {
        return withoutTrailingSlash(normalizeFileUri(target));
    }

    const auto normalized_target = toForwardSlashes(target);
    if (!normalized_target.empty() && normalized_target.front() == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized_target));
    }
    if (isWindowsAbsolutePath(normalized_target)) {
        return withoutTrailingSlash(normalizeFileUri(std::string("file:///") + normalized_target));
    }

    auto base = withoutTrailingSlash(normalizeFileUri(base_uri));
    return withoutTrailingSlash(normalizeFileUri(base + "/" + normalized_target));
}

std::string fileUriToPath(std::string_view uri) {
    auto normalized = withoutTrailingSlash(normalizeFileUri(uri));
    constexpr std::string_view prefix = "file://";
    if (!normalized.starts_with(prefix)) {
        return std::string(normalized);
    }

    auto path = std::string(normalized.substr(prefix.size()));
    if (path.size() >= 3 && path.front() == '/' && isDriveSegment(std::string_view(path).substr(1, 2))) {
        path.erase(path.begin());
    }
#ifdef _WIN32
    // A drive-less absolute URI is an in-memory LSP document, not a path on the
    // current Windows drive. Keep it under a stable synthetic root so slang
    // include resolution and source-location round-trips address the same buffer.
    if (!path.empty() && path.front() == '/') {
        path = std::string(kInMemoryUriRoot) + path;
    }
    // SourceManager caches assigned buffers by the native filesystem spelling.
    // Normalizing before assignText keeps a later local-include lookup on the
    // same cache key instead of attempting to read a separate on-disk file.
    return std::filesystem::path(path).lexically_normal().string();
#else
    return path;
#endif
}

std::string pathToFileUri(const std::filesystem::path& path) {
    auto normalized = toForwardSlashes(path.string());
    if (normalized.empty()) {
        return {};
    }
#ifdef _WIN32
    if (startsWithIgnoreCase(normalized, kInMemoryUriRoot) &&
        normalized.size() > kInMemoryUriRoot.size() &&
        normalized[kInMemoryUriRoot.size()] == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") +
                                                     normalized.substr(kInMemoryUriRoot.size())));
    }
#endif
    if (!normalized.empty() && normalized.front() == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
    }
    if (isWindowsAbsolutePath(normalized)) {
        return withoutTrailingSlash(normalizeFileUri(std::string("file:///") + normalized));
    }
    return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
}

bool parseRangeContainsPosition(const ParseRange& range, int line, int character) {
    if (line < range.start_line || line > range.end_line) {
        return false;
    }
    if (line == range.start_line && character < range.start_character) {
        return false;
    }
    if (line == range.end_line && character >= range.end_character) {
        return false;
    }
    return true;
}

ParseRange pointRangeAtPosition(int line, int character) {
    return ParseRange{.start_line = line,
                      .start_character = character,
                      .end_line = line,
                      .end_character = character};
}

std::optional<size_t> utf8OffsetAtUtf16Position(std::string_view text,
                                                int line,
                                                int character) {
    if (line < 0 || character < 0) {
        return std::nullopt;
    }

    int current_line = 0;
    size_t line_start = 0;
    for (size_t offset = 0; offset < text.size() && current_line < line; ++offset) {
        if (text[offset] == '\n') {
            ++current_line;
            line_start = offset + 1;
        }
    }
    if (current_line != line) {
        return std::nullopt;
    }

    size_t offset = line_start;
    int current_character = 0;
    while (offset < text.size() && text[offset] != '\n' && text[offset] != '\r') {
        if (current_character == character) {
            return offset;
        }
        try {
            const auto decoded = text::decodeNextCodePoint(text, offset);
            const auto width = static_cast<int>(text::utf16CodeUnitWidth(decoded.value));
            if (current_character + width > character) {
                return std::nullopt;
            }
            current_character += width;
            offset += decoded.byte_length;
        }
        catch (const std::runtime_error&) {
            return std::nullopt;
        }
    }

    return current_character == character ? std::optional<size_t>{offset} : std::nullopt;
}

std::optional<ParseRange> lineRangeAtPosition(std::string_view text, int line, int character) {
    const auto offset = utf8OffsetAtUtf16Position(text, line, character);
    if (!offset.has_value()) {
        return std::nullopt;
    }

    size_t line_start = *offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }

    size_t line_end = *offset;
    while (line_end < text.size() && text[line_end] != '\n' && text[line_end] != '\r') {
        ++line_end;
    }

    size_t trimmed_start = line_start;
    while (trimmed_start < line_end &&
           (text[trimmed_start] == ' ' || text[trimmed_start] == '\t')) {
        ++trimmed_start;
    }
    size_t trimmed_end = line_end;
    while (trimmed_end > trimmed_start &&
           (text[trimmed_end - 1] == ' ' || text[trimmed_end - 1] == '\t')) {
        --trimmed_end;
    }
    if (trimmed_start == trimmed_end) {
        return std::nullopt;
    }

    const auto end_character = [&]() -> int {
        try {
            return static_cast<int>(text::utf16UnitsForUtf8Prefix(text.substr(line_start, trimmed_end - line_start),
                                                                  trimmed_end - line_start));
        }
        catch (const std::runtime_error&) {
            return character;
        }
    }();
    return ParseRange{.start_line = line,
                      .start_character = static_cast<int>(trimmed_start - line_start),
                      .end_line = line,
                      .end_character = end_character};
}

std::optional<std::string> textForParseRange(std::string_view text, const ParseRange& range) {
    const auto start_offset = utf8OffsetAtUtf16Position(text, range.start_line, range.start_character);
    const auto end_offset = utf8OffsetAtUtf16Position(text, range.end_line, range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value() || *end_offset < *start_offset) {
        return std::nullopt;
    }
    return std::string(text.substr(*start_offset, *end_offset - *start_offset));
}

ParseRange sourceRangeForDiagnostic(const slang::SourceManager& source_manager,
                                    const slang::Diagnostic& diagnostic) {
    slang::SourceLocation start = diagnostic.location;
    slang::SourceLocation end = diagnostic.location;
    if (!diagnostic.ranges.empty()) {
        start = diagnostic.ranges.front().start();
        end = diagnostic.ranges.front().end();
    }

    return sourceRangeForLocations(source_manager, start, end);
}

ParseRange sourceRangeForSourceRange(const slang::SourceManager& source_manager,
                                     slang::SourceRange range) {
    return sourceRangeForLocations(source_manager, range.start(), range.end());
}

} // namespace pristine::analysis
