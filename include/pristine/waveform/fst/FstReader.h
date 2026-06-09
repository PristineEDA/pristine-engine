#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pristine::waveform::fst {

enum class FstBlockType : std::uint8_t {
    Header = 0,
    ValueChangeData = 1,
    Blackout = 2,
    Geometry = 3,
    Hierarchy = 4,
    ValueChangeDataDynamicAlias = 5,
    HierarchyLz4 = 6,
    HierarchyLz4Duo = 7,
    ValueChangeDataDynamicAlias2 = 8,
    ZWrapper = 254,
    Skip = 255,
};

enum class FstCompressionKind : std::uint8_t {
    None = 0,
    Zlib = 1,
    Lz4 = 2,
    FastLz = 3,
    Unknown = 255,
};

struct FstHeader {
    std::uint64_t start_time = 0;
    std::uint64_t end_time = 0;
    std::uint64_t memory_used_by_writer = 0;
    std::uint64_t scope_count = 0;
    std::uint64_t variable_count = 0;
    std::uint64_t max_handle = 0;
    std::uint64_t value_change_section_count = 0;
    std::int8_t timescale = -9;
    std::string version;
    std::string date;
    std::uint8_t file_type = 0;
    std::uint64_t timezero = 0;
    bool float_endian_matches_host = true;
};

struct FstScope {
    std::uint32_t index = 0;
    std::uint32_t parent_index = 0;
    std::uint8_t type = 0;
    std::string name;
    std::string component;
    std::string path;
};

struct FstSignal {
    std::uint32_t handle = 0;
    std::uint32_t alias_handle = 0;
    std::uint8_t var_type = 0;
    std::uint8_t direction = 0;
    std::uint32_t width = 1;
    std::uint32_t scope_index = 0;
    std::string name;
    std::string path;
};

struct FstSignalGeometry {
    std::uint32_t length = 1;
    bool is_real = false;
};

struct FstBlockIndexEntry {
    FstBlockType type = FstBlockType::Skip;
    std::uint64_t file_offset = 0;
    std::uint64_t section_length = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t begin_time = 0;
    std::uint64_t end_time = 0;
    std::uint64_t memory_required_for_traversal = 0;
    FstCompressionKind compression = FstCompressionKind::Unknown;
};

struct FstTransition {
    std::uint32_t handle = 0;
    std::uint64_t time = 0;
    std::string value;
};

struct FstReadMetrics {
    std::uint64_t file_bytes = 0;
    std::uint64_t file_read_micros = 0;
    std::uint64_t header_parse_micros = 0;
    std::uint64_t sidecar_hierarchy_parse_micros = 0;
    std::uint64_t hierarchy_parse_micros = 0;
    std::uint64_t geometry_parse_micros = 0;
    std::uint64_t value_block_index_micros = 0;
    std::uint64_t block_scan_micros = 0;
    std::uint64_t value_decode_micros = 0;
    std::uint64_t total_micros = 0;
};

struct FstData {
    std::filesystem::path file_path;
    std::optional<std::filesystem::path> hierarchy_sidecar_path;
    FstHeader header;
    std::vector<FstScope> scopes;
    std::vector<FstSignal> signals;
    std::vector<std::uint32_t> signal_widths_by_handle;
    std::vector<FstSignalGeometry> signal_geometry_by_handle;
    std::vector<FstBlockIndexEntry> value_blocks;
    std::vector<FstTransition> transitions;
    std::optional<FstReadMetrics> metrics;
};

struct FstReadOptions {
    std::optional<std::filesystem::path> workspace_root = std::nullopt;
    bool decode_transitions = true;
    bool collect_metrics = false;
    std::optional<std::uint64_t> decode_start_time = std::nullopt;
    std::optional<std::uint64_t> decode_end_time = std::nullopt;
    std::vector<std::uint32_t> decode_signal_handles = {};
};

[[nodiscard]] FstData readFstFile(const std::filesystem::path& path,
                                  const FstReadOptions& options = {});

} // namespace pristine::waveform::fst
