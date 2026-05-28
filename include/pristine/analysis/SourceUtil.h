#pragma once

#include "pristine/analysis/CompilationService.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace slang {
class Diagnostic;
class SourceManager;
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
[[nodiscard]] ParseRange sourceRangeForDiagnostic(const slang::SourceManager& source_manager,
                                                  const slang::Diagnostic& diagnostic);

} // namespace pristine::analysis
