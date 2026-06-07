#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace pristine::waveform {

enum class WaveformSignalKind : std::uint8_t {
    Clock = 1,
    Logic = 2,
    Bus = 3,
};

enum class WaveformValueKind : std::uint8_t {
    Low = 0,
    High = 1,
    Unknown = 2,
    HighImpedance = 3,
    Bus = 4,
};

struct WaveformTransition {
    double time = 0.0;
    std::string value;
};

struct WaveformSignalGroup {
    std::string id;
    std::string label;
};

struct WaveformSignal {
    std::string id;
    std::size_t group_index = 0;
    std::string name;
    std::string path;
    WaveformSignalKind kind = WaveformSignalKind::Logic;
    std::uint32_t color_rgb = 0;
    std::uint32_t width = 1;
    std::vector<WaveformTransition> transitions;
};

struct WaveformDataSet {
    std::string id;
    std::string title;
    std::string timescale_unit;
    double duration = 0.0;
    std::vector<WaveformSignalGroup> groups;
    std::vector<WaveformSignal> signals;
};

struct WaveformViewportRequest {
    double start_time = 0.0;
    double end_time = 0.0;
    float viewport_pixel_width = 0.0F;
    float lane_height = 0.0F;
    float header_height = 0.0F;
    std::uint32_t max_segments = 0;
    std::vector<std::string> signal_ids;
};

struct WaveformViewportRequestV2 {
    double prepared_start_time = 0.0;
    double prepared_end_time = 0.0;
    double viewport_start_time = 0.0;
    double viewport_end_time = 0.0;
    float viewport_pixel_width = 0.0F;
    float lane_height = 0.0F;
    float header_height = 0.0F;
    std::uint32_t max_segments = 0;
    std::vector<std::string> signal_ids;
};

} // namespace pristine::waveform
