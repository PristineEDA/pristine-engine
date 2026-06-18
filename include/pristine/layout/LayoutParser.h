#pragma once

#include "pristine/layout/LayoutData.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

namespace pristine::layout {

struct LayoutGdsParseProgress {
    LayoutSourcePhase phase = LayoutSourcePhase::Unknown;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t record_count = 0;
    std::uint32_t cell_count = 0;
    std::uint32_t reference_count = 0;
    std::uint32_t element_count = 0;
    std::uint32_t point_count = 0;
    std::uint32_t string_count = 0;
    std::uint32_t diagnostic_count = 0;
    std::uint32_t suppressed_diagnostic_count = 0;
    std::uint32_t arena_growth_count = 0;
    std::uint32_t cancel_check_count = 0;
};

struct LayoutGdsParseControl {
    std::function<bool()> should_cancel{};
    std::function<void(const LayoutGdsParseProgress&)> publish_progress{};
    std::uint32_t progress_record_interval = 4096;
};

[[nodiscard]] ParseResult<LayoutLefLibrary> parseLef(std::string_view text,
                                                     std::string_view file_name = {});
[[nodiscard]] ParseResult<LayoutDefDesign> parseDef(std::string_view text,
                                                    std::string_view file_name = {});
[[nodiscard]] ParseResult<LayoutGdsLibrary> parseGds(const std::vector<std::uint8_t>& bytes,
                                                     std::string_view file_name = {});
[[nodiscard]] ParseResult<LayoutGdsLibrary> parseGds(const std::vector<std::uint8_t>& bytes,
                                                     std::string_view file_name,
                                                     LayoutGdsParseControl* control);
[[nodiscard]] ParseResult<LayoutLefLibrary> parseLefFile(const std::filesystem::path& path);
[[nodiscard]] ParseResult<LayoutDefDesign> parseDefFile(const std::filesystem::path& path);
[[nodiscard]] ParseResult<LayoutGdsLibrary> parseGdsFile(const std::filesystem::path& path);
[[nodiscard]] ParseResult<LayoutGdsLibrary> parseGdsFile(const std::filesystem::path& path,
                                                         LayoutGdsParseControl* control);

} // namespace pristine::layout
