#include "pristine/waveform/WaveformMockGenerator.h"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace pristine::waveform {
namespace {

constexpr double kMockDuration = 200.0;
constexpr int kDenseSignalCount = 160;
constexpr std::uint32_t kDenseSignalColors[] = {
    0x2dd4bfU, 0x22c55eU, 0x84cc16U, 0xeab308U, 0xf97316U,
    0xfb7185U, 0xa78bfaU, 0x60a5faU, 0x38bdf8U, 0x34d399U,
};

std::vector<WaveformTransition> makeClockTransitions(double period, std::string initial_value = "0") {
    std::vector<WaveformTransition> transitions;
    std::string value = std::move(initial_value);
    for (double time = 0.0; time <= kMockDuration; time += period / 2.0) {
        transitions.push_back(WaveformTransition{.time = time, .value = value});
        value = value == "0" ? "1" : "0";
    }
    return transitions;
}

double denseTransitionStep(int index, int step_index, WaveformSignalKind kind) {
    const double base = kind == WaveformSignalKind::Clock ? 0.42
        : kind == WaveformSignalKind::Bus                       ? 0.58
                                                                 : 0.5;
    const double burst_scale = index % 7 == 0 && step_index % 17 < 8 ? 0.58 : 1.0;
    const double jitter = static_cast<double>((index * 17 + step_index * 11) % 9) * 0.055;
    const double drift = static_cast<double>((index + step_index) % 5) * 0.13;
    return std::max(0.18, base * burst_scale + jitter + drift);
}

std::string hexValue(int value) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << value;
    return stream.str();
}

std::vector<WaveformTransition> makeDenseTransitions(int index, WaveformSignalKind kind) {
    std::vector<WaveformTransition> transitions;
    double time = 0.0;
    std::string value = index % 3 == 0 && kind != WaveformSignalKind::Clock ? "x"
                                                                            : std::to_string(index % 2);
    transitions.push_back(WaveformTransition{.time = time, .value = value});

    for (int step_index = 1; time < kMockDuration; ++step_index) {
        const auto step = denseTransitionStep(index, step_index, kind);
        time = std::min(kMockDuration, time + step);
        time = static_cast<double>(static_cast<int>(time * 1000.0 + 0.5)) / 1000.0;

        if (kind == WaveformSignalKind::Clock) {
            value = value == "1" ? "0" : "1";
        }
        else if (kind == WaveformSignalKind::Bus) {
            if (step_index % 41 == 0) {
                value = "z";
            }
            else if (step_index % 23 == 0) {
                value = "x";
            }
            else {
                value = hexValue((step_index * (index + 5) + index * 13) % 4096);
            }
        }
        else if (step_index % 53 == 0) {
            value = "z";
        }
        else if (step_index % 37 == 0) {
            value = "x";
        }
        else {
            value = std::to_string((step_index + index) % 2);
        }

        transitions.push_back(WaveformTransition{.time = time, .value = value});
    }

    return transitions;
}

std::string twoDigit(int value) {
    std::ostringstream stream;
    stream << std::setw(2) << std::setfill('0') << value;
    return stream.str();
}

std::string kindName(WaveformSignalKind kind) {
    switch (kind) {
        case WaveformSignalKind::Clock:
            return "clock";
        case WaveformSignalKind::Bus:
            return "bus";
        case WaveformSignalKind::Logic:
            return "logic";
    }
    return "logic";
}

WaveformSignal makeSignal(std::string id,
                          std::size_t group_index,
                          std::string name,
                          std::string path,
                          WaveformSignalKind kind,
                          std::uint32_t color_rgb,
                          std::vector<WaveformTransition> transitions,
                          std::uint32_t width = 1) {
    return WaveformSignal{.id = std::move(id),
                          .group_index = group_index,
                          .name = std::move(name),
                          .path = std::move(path),
                          .kind = kind,
                          .color_rgb = color_rgb,
                          .width = width,
                          .transitions = std::move(transitions)};
}

} // namespace

WaveformDataSet makeMockWaveformDataSet() {
    WaveformDataSet data{.id = "counter-waveform-mock",
                         .title = "counter_tb",
                         .timescale_unit = "ns",
                         .duration = kMockDuration,
                         .groups = {WaveformSignalGroup{.id = "tb_top_module1",
                                                         .label = "tb_top_module1"},
                                    WaveformSignalGroup{.id = "u_top_module1",
                                                        .label = "u_top_module1"},
                                    WaveformSignalGroup{.id = "dense_test_signals",
                                                        .label = "dense_test_signals"}},
                         .signals = {}};

    data.signals.push_back(makeSignal("tb_top_module1-clk",
                                      0,
                                      "clk",
                                      "tb_top_module1.clk",
                                      WaveformSignalKind::Clock,
                                      0x38d8ffU,
                                      makeClockTransitions(20.0)));
    data.signals.push_back(makeSignal("tb_top_module1-data",
                                      0,
                                      "data",
                                      "tb_top_module1.data",
                                      WaveformSignalKind::Logic,
                                      0x5ee37cU,
                                      {{0.0, "0"},
                                       {45.0, "1"},
                                       {70.0, "0"},
                                       {82.0, "1"},
                                       {96.0, "0"},
                                       {156.0, "1"},
                                       {176.0, "0"},
                                       {188.0, "z"}}));
    data.signals.push_back(makeSignal("tb_top_module1-reset",
                                      0,
                                      "reset",
                                      "tb_top_module1.reset",
                                      WaveformSignalKind::Logic,
                                      0xffcb6bU,
                                      {{0.0, "1"}, {18.0, "0"}}));
    data.signals.push_back(makeSignal("tb_top_module1-done_counting",
                                      0,
                                      "done_counting",
                                      "tb_top_module1.done_counting",
                                      WaveformSignalKind::Logic,
                                      0xac8dffU,
                                      {{0.0, "0"}, {142.0, "1"}, {182.0, "0"}}));
    data.signals.push_back(makeSignal("u_top_module1-clk",
                                      1,
                                      "clk",
                                      "tb_top_module1.u_top_module1.clk",
                                      WaveformSignalKind::Clock,
                                      0xffe66dU,
                                      makeClockTransitions(20.0)));
    data.signals.push_back(makeSignal("u_top_module1-shift_ena",
                                      1,
                                      "shift_ena",
                                      "tb_top_module1.u_top_module1.shift_ena",
                                      WaveformSignalKind::Logic,
                                      0x7bdff2U,
                                      {{0.0, "z"}, {10.0, "0"}, {82.0, "1"}, {128.0, "0"}}));
    data.signals.push_back(makeSignal("u_top_module1-done",
                                      1,
                                      "done",
                                      "tb_top_module1.u_top_module1.done",
                                      WaveformSignalKind::Logic,
                                      0xf78c6cU,
                                      {{0.0, "x"}, {10.0, "0"}, {148.0, "1"}}));
    data.signals.push_back(makeSignal("u_top_module1-counting",
                                      1,
                                      "counting",
                                      "tb_top_module1.u_top_module1.counting",
                                      WaveformSignalKind::Bus,
                                      0x88f7a6U,
                                      {{0.0, "x"},
                                       {10.0, "0"},
                                       {52.0, "1"},
                                       {82.0, "2"},
                                       {112.0, "3"},
                                       {142.0, "4"},
                                       {172.0, "5"},
                                       {190.0, "z"}},
                                      4));

    for (int offset = 0; offset < kDenseSignalCount; ++offset) {
        const int number = offset + 1;
        const auto suffix = twoDigit(number);
        const auto kind = offset % 5 == 0 ? WaveformSignalKind::Bus
            : offset % 4 == 0                ? WaveformSignalKind::Clock
                                             : WaveformSignalKind::Logic;
        const auto kind_name = kindName(kind);
        const auto width = kind == WaveformSignalKind::Bus
            ? static_cast<std::uint32_t>(offset % 10 == 0 ? 16 : 8)
            : 1U;
        data.signals.push_back(
            makeSignal("dense-signal-" + suffix,
                       2,
                       "dense_" + kind_name + "_" + suffix,
                       "tb_top_module1.dense_test_signals.dense_" + kind_name + "_" + suffix,
                       kind,
                       kDenseSignalColors[offset % static_cast<int>(std::size(kDenseSignalColors))],
                       makeDenseTransitions(number, kind),
                       width));
    }

    return data;
}

} // namespace pristine::waveform
