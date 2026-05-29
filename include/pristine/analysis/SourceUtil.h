#pragma once

#include "pristine/analysis/CompilationService.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace slang {
class Diagnostic;
class SourceManager;
class SourceRange;
} // namespace slang

namespace pristine::analysis {

[[nodiscard]] bool isFileUri(std::string_view value);
[[nodiscard]] bool isWindowsAbsolutePath(std::string_view value);
[[nodiscard]] std::string toForwardSlashes(std::string_view value);
[[nodiscard]] std::string normalizeFileUri(std::string_view uri);
[[nodiscard]] std::string withoutTrailingSlash(std::string value);
[[nodiscard]] std::string uriDirectory(std::string_view uri);
[[nodiscard]] std::string joinFileUri(std::string_view base_uri, std::string_view target);
[[nodiscard]] std::string fileUriToPath(std::string_view uri);
[[nodiscard]] std::string pathToFileUri(const std::filesystem::path& path);
[[nodiscard]] bool parseRangeContainsPosition(const ParseRange& range,
                                              int line,
                                              int character);
[[nodiscard]] ParseRange pointRangeAtPosition(int line, int character);
[[nodiscard]] std::optional<size_t> utf8OffsetAtUtf16Position(std::string_view text,
                                                              int line,
                                                              int character);
[[nodiscard]] std::optional<ParseRange> lineRangeAtPosition(std::string_view text,
                                                            int line,
                                                            int character);
[[nodiscard]] std::optional<std::string> textForParseRange(std::string_view text,
                                                           const ParseRange& range);
[[nodiscard]] ParseRange sourceRangeForDiagnostic(const slang::SourceManager& source_manager,
                                                  const slang::Diagnostic& diagnostic);
[[nodiscard]] ParseRange sourceRangeForSourceRange(const slang::SourceManager& source_manager,
                                                   slang::SourceRange range);

} // namespace pristine::analysis
