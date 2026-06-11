#pragma once

#include "pristine/layout/LayoutData.h"

#include <filesystem>
#include <string_view>

namespace pristine::layout {

[[nodiscard]] ParseResult<LayoutLefLibrary> parseLef(std::string_view text,
                                                     std::string_view file_name = {});
[[nodiscard]] ParseResult<LayoutDefDesign> parseDef(std::string_view text,
                                                    std::string_view file_name = {});
[[nodiscard]] ParseResult<LayoutLefLibrary> parseLefFile(const std::filesystem::path& path);
[[nodiscard]] ParseResult<LayoutDefDesign> parseDefFile(const std::filesystem::path& path);

} // namespace pristine::layout
