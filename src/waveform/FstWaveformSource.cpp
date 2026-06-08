#include "pristine/waveform/FstWaveformSource.h"

#include "pristine/waveform/fst/FstReader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <sstream>
#include <utility>

namespace pristine::waveform {
namespace {

namespace fs = std::filesystem;

std::string timescaleUnit(std::int8_t timescale) {
    switch (timescale) {
        case -15:
            return "fs";
        case -12:
            return "ps";
        case -9:
            return "ns";
        case -6:
            return "us";
        case -3:
            return "ms";
        case 0:
            return "s";
        default:
            return "10^" + std::to_string(static_cast<int>(timescale)) + "s";
    }
}

WaveformSignalKind signalKindFor(const fst::FstSignal& signal) {
    if (signal.width > 1) {
        return WaveformSignalKind::Bus;
    }
    const auto lower_name = [&] {
        std::string result = signal.name;
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return result;
    }();
    if (lower_name == "clk" || lower_name == "clock" || lower_name.ends_with("_clk") ||
        lower_name.ends_with("_clock")) {
        return WaveformSignalKind::Clock;
    }
    return WaveformSignalKind::Logic;
}

std::uint32_t colorFor(std::size_t index) {
    constexpr std::uint32_t colors[] = {
        0x38d8ffU, 0x5ee37cU, 0xffcb6bU, 0xac8dffU, 0xf78c6cU,
        0x88f7a6U, 0x60a5faU, 0x34d399U, 0xfb7185U, 0xa78bfaU,
    };
    return colors[index % std::size(colors)];
}

std::vector<WaveformTransition> transitionsFor(std::uint32_t handle,
                                               const fst::FstData& data) {
    std::vector<WaveformTransition> transitions;
    for (const auto& transition : data.transitions) {
        if (transition.handle == handle) {
            transitions.push_back(WaveformTransition{.time = static_cast<double>(transition.time),
                                                     .value = transition.value});
        }
    }
    std::sort(transitions.begin(), transitions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.time < rhs.time;
    });
    if (transitions.empty() ||
        transitions.front().time > static_cast<double>(data.header.start_time)) {
        transitions.insert(transitions.begin(),
                           WaveformTransition{.time = static_cast<double>(data.header.start_time),
                                              .value = "x"});
    }
    return transitions;
}

WaveformDataSet toWaveformDataSet(const fst::FstData& fst_data) {
    auto title = fst_data.file_path.stem().string();
    if (title.empty()) {
        title = "waveform";
    }
    WaveformDataSet data{.id = "fst:" + fst_data.file_path.generic_string(),
                         .title = std::move(title),
                         .timescale_unit = timescaleUnit(fst_data.header.timescale),
                         .duration = static_cast<double>(fst_data.header.end_time -
                                                         fst_data.header.start_time),
                         .groups = {},
                         .signals = {}};

    if (fst_data.scopes.empty()) {
        data.groups.push_back(WaveformSignalGroup{.id = "fst:root", .label = "root"});
    }
    else {
        data.groups.reserve(fst_data.scopes.size());
        for (const auto& scope : fst_data.scopes) {
            data.groups.push_back(WaveformSignalGroup{.id = "fst:scope:" +
                                                            std::to_string(scope.index),
                                                      .label = scope.path.empty() ? scope.name
                                                                                  : scope.path});
        }
    }

    data.signals.reserve(fst_data.signals.size());
    for (std::size_t index = 0; index < fst_data.signals.size(); ++index) {
        const auto& signal = fst_data.signals[index];
        auto group_index = static_cast<std::size_t>(signal.scope_index);
        if (group_index >= data.groups.size()) {
            group_index = 0;
        }
        data.signals.push_back(WaveformSignal{.id = "fst:" + std::to_string(signal.handle),
                                              .group_index = group_index,
                                              .name = signal.name,
                                              .path = signal.path.empty() ? signal.name
                                                                          : signal.path,
                                              .kind = signalKindFor(signal),
                                              .color_rgb = colorFor(index),
                                              .width = std::max(1U, signal.width),
                                              .transitions = transitionsFor(signal.handle,
                                                                            fst_data)});
    }
    return data;
}

} // namespace

std::shared_ptr<WaveformSource> openFstWaveformSource(
    const std::filesystem::path& path,
    std::string file_uri,
    std::optional<std::filesystem::path> workspace_root) {
    const fst::FstReadOptions options{.workspace_root = std::move(workspace_root),
                                      .decode_transitions = true};
    auto fst_data = fst::readFstFile(path, options);
    return makeDataSetWaveformSource(toWaveformDataSet(fst_data), "fst", std::move(file_uri));
}

} // namespace pristine::waveform
