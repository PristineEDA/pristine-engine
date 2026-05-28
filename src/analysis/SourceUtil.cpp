#include "pristine/analysis/SourceUtil.h"

#include "slang/diagnostics/Diagnostics.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace pristine::analysis {
namespace {

bool isDriveSegment(std::string_view value) {
    return value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':';
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
    return path;
}

std::string pathToFileUri(const std::filesystem::path& path) {
    auto normalized = toForwardSlashes(path.string());
    if (normalized.empty()) {
        return {};
    }
    if (!normalized.empty() && normalized.front() == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
    }
    if (isWindowsAbsolutePath(normalized)) {
        return withoutTrailingSlash(normalizeFileUri(std::string("file:///") + normalized));
    }
    return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
}

ParseRange sourceRangeForDiagnostic(const slang::SourceManager& source_manager,
                                    const slang::Diagnostic& diagnostic) {
    slang::SourceLocation start = diagnostic.location;
    slang::SourceLocation end = diagnostic.location;
    if (!diagnostic.ranges.empty()) {
        start = diagnostic.ranges.front().start();
        end = diagnostic.ranges.front().end();
    }

    if (!start.valid()) {
        return {};
    }
    if (!end.valid() || end.buffer() != start.buffer() || end.offset() < start.offset()) {
        end = start + 1;
    }

    return ParseRange{
        .start_line = static_cast<int>(source_manager.getLineNumber(start)) - 1,
        .start_character = static_cast<int>(source_manager.getColumnNumber(start)) - 1,
        .end_line = static_cast<int>(source_manager.getLineNumber(end)) - 1,
        .end_character = static_cast<int>(source_manager.getColumnNumber(end)) - 1};
}

} // namespace pristine::analysis
