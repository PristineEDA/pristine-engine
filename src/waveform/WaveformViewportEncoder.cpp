#include "pristine/waveform/WaveformViewportEncoder.h"

#include "pristine/waveform/WaveformBinaryProtocol.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace pristine::waveform {
namespace {

constexpr std::uint8_t kViewportFrameMagic[] = {'P', 'W', 'V', 'F'};
constexpr std::uint16_t kViewportFrameHeaderSize = 56;
constexpr std::uint16_t kViewportFrameHeaderSizeV2 = 96;
constexpr std::uint16_t kViewportFrameProtocolVersionV2 = 2;
constexpr std::uint32_t kNoLabel = std::numeric_limits<std::uint32_t>::max();

struct SegmentColumns {
    std::vector<std::uint32_t> signal_table;
    std::vector<float> x0;
    std::vector<float> x1;
    std::vector<float> lane_y;
    std::vector<std::uint8_t> value_kind;
    std::vector<std::uint32_t> label_index;
    std::vector<double> time0;
    std::vector<double> time1;
    std::vector<std::uint8_t> label_bytes;
    std::uint32_t flags = 0;
};

void patchU16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
    output.at(offset + 1) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void patchU32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    output.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
    output.at(offset + 1) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    output.at(offset + 2) = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    output.at(offset + 3) = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void patchF64(std::vector<std::uint8_t>& output, std::size_t offset, double value) {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(sizeof(double));
    appendF64(encoded, value);
    std::copy(encoded.begin(), encoded.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::uint32_t checkedOffset(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform viewport frame exceeds uint32 offset range");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t checkedCount(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform viewport frame exceeds uint32 count range");
    }
    return static_cast<std::uint32_t>(value);
}

float timeToX(double time, const WaveformViewportRequest& request) {
    const auto span = request.end_time - request.start_time;
    if (span <= 0.0 || request.viewport_pixel_width <= 0.0F) {
        return 0.0F;
    }
    const auto normalized = (time - request.start_time) / span;
    const auto clamped = std::clamp(normalized, 0.0, 1.0);
    return static_cast<float>(clamped * static_cast<double>(request.viewport_pixel_width));
}

float timeToX(double time, const WaveformViewportRequestV2& request) {
    const auto span = request.viewport_end_time - request.viewport_start_time;
    if (span <= 0.0 || request.viewport_pixel_width <= 0.0F) {
        return 0.0F;
    }
    const auto normalized = (time - request.viewport_start_time) / span;
    const auto clamped = std::clamp(normalized, 0.0, 1.0);
    return static_cast<float>(clamped * static_cast<double>(request.viewport_pixel_width));
}

WaveformValueKind valueKindFor(const WaveformSignal& signal, std::string_view value) {
    if (value == "0") {
        return WaveformValueKind::Low;
    }
    if (value == "1") {
        return WaveformValueKind::High;
    }
    if (value == "x" || value == "X") {
        return WaveformValueKind::Unknown;
    }
    if (value == "z" || value == "Z") {
        return WaveformValueKind::HighImpedance;
    }
    return signal.kind == WaveformSignalKind::Bus ? WaveformValueKind::Bus
                                                  : WaveformValueKind::Unknown;
}

std::uint32_t appendLabel(std::vector<std::uint8_t>& labels, std::string_view value) {
    if (labels.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform label table exceeds uint32 range");
    }
    const auto offset = static_cast<std::uint32_t>(labels.size());
    appendString(labels, value);
    return offset;
}

std::uint32_t labelOffsetFor(std::vector<std::uint8_t>& labels,
                             const WaveformSignal& signal,
                             std::string_view value) {
    if (signal.kind != WaveformSignalKind::Bus) {
        return kNoLabel;
    }
    return appendLabel(labels, value);
}

std::vector<const WaveformSignal*> selectedSignals(const WaveformDataSet& data,
                                                   const WaveformViewportRequest& request) {
    std::vector<const WaveformSignal*> result;
    if (request.signal_ids.empty()) {
        result.reserve(data.signals.size());
        for (const auto& signal : data.signals) {
            result.push_back(&signal);
        }
        return result;
    }

    std::unordered_set<std::string> emitted;
    for (const auto& signal_id : request.signal_ids) {
        if (!emitted.insert(signal_id).second) {
            continue;
        }
        const auto signal_it = std::find_if(data.signals.begin(), data.signals.end(),
                                            [&](const WaveformSignal& signal) {
                                                return signal.id == signal_id;
                                            });
        if (signal_it == data.signals.end()) {
            throw std::runtime_error("Unknown waveform signal id: " + signal_id);
        }
        result.push_back(&*signal_it);
    }
    return result;
}

std::vector<const WaveformSignal*> selectedSignals(const WaveformDataSet& data,
                                                   const WaveformViewportRequestV2& request) {
    std::vector<const WaveformSignal*> result;
    if (request.signal_ids.empty()) {
        result.reserve(data.signals.size());
        for (const auto& signal : data.signals) {
            result.push_back(&signal);
        }
        return result;
    }

    std::unordered_set<std::string> emitted;
    for (const auto& signal_id : request.signal_ids) {
        if (!emitted.insert(signal_id).second) {
            continue;
        }
        const auto signal_it = std::find_if(data.signals.begin(), data.signals.end(),
                                            [&](const WaveformSignal& signal) {
                                                return signal.id == signal_id;
                                            });
        if (signal_it == data.signals.end()) {
            throw std::runtime_error("Unknown waveform signal id: " + signal_id);
        }
        result.push_back(&*signal_it);
    }
    return result;
}

std::string valueAtStart(const WaveformSignal& signal, double start_time) {
    std::string value = signal.transitions.front().value;
    for (const auto& transition : signal.transitions) {
        if (transition.time > start_time) {
            break;
        }
        value = transition.value;
    }
    return value;
}

bool appendSegment(SegmentColumns& columns,
                   const WaveformSignal& signal,
                   std::string_view value,
                   double start,
                   double end,
                   float lane_y,
                   const WaveformViewportRequest& request) {
    if (end <= start) {
        return true;
    }
    if (request.max_segments != 0 &&
        columns.x0.size() >= static_cast<std::size_t>(request.max_segments)) {
        columns.flags |= kWaveformFrameFlagTruncated;
        return false;
    }

    columns.x0.push_back(timeToX(start, request));
    columns.x1.push_back(timeToX(end, request));
    columns.lane_y.push_back(lane_y);
    columns.value_kind.push_back(static_cast<std::uint8_t>(valueKindFor(signal, value)));
    columns.label_index.push_back(labelOffsetFor(columns.label_bytes, signal, value));
    return true;
}

bool appendSegmentV2(SegmentColumns& columns,
                     const WaveformSignal& signal,
                     std::string_view value,
                     double start,
                     double end,
                     float lane_y,
                     const WaveformViewportRequestV2& request) {
    if (end <= start) {
        return true;
    }
    if (request.max_segments != 0 &&
        columns.x0.size() >= static_cast<std::size_t>(request.max_segments)) {
        columns.flags |= kWaveformFrameFlagTruncated;
        return false;
    }

    columns.x0.push_back(timeToX(start, request));
    columns.x1.push_back(timeToX(end, request));
    columns.lane_y.push_back(lane_y);
    columns.value_kind.push_back(static_cast<std::uint8_t>(valueKindFor(signal, value)));
    columns.label_index.push_back(labelOffsetFor(columns.label_bytes, signal, value));
    columns.time0.push_back(start);
    columns.time1.push_back(end);
    return true;
}

void appendSignalSegments(SegmentColumns& columns,
                          const WaveformSignal& signal,
                          std::size_t signal_index,
                          float lane_y,
                          const WaveformViewportRequest& request) {
    const auto first_segment = checkedCount(columns.x0.size());
    std::string current_value = valueAtStart(signal, request.start_time);
    double current_time = request.start_time;

    for (const auto& transition : signal.transitions) {
        if (transition.time <= request.start_time) {
            continue;
        }
        if (transition.time > request.end_time) {
            break;
        }
        if (!appendSegment(columns,
                           signal,
                           current_value,
                           current_time,
                           transition.time,
                           lane_y,
                           request)) {
            break;
        }
        current_time = transition.time;
        current_value = transition.value;
    }

    if ((columns.flags & kWaveformFrameFlagTruncated) == 0U) {
        (void)appendSegment(columns,
                            signal,
                            current_value,
                            current_time,
                            request.end_time,
                            lane_y,
                            request);
    }

    const auto segment_count = checkedCount(columns.x0.size()) - first_segment;
    columns.signal_table.push_back(checkedCount(signal_index));
    columns.signal_table.push_back(first_segment);
    columns.signal_table.push_back(segment_count);
    columns.signal_table.push_back(0);
}

void appendSignalSegmentsV2(SegmentColumns& columns,
                            const WaveformSignal& signal,
                            std::size_t signal_index,
                            float lane_y,
                            const WaveformViewportRequestV2& request) {
    const auto first_segment = checkedCount(columns.x0.size());
    std::string current_value = valueAtStart(signal, request.prepared_start_time);
    double current_time = request.prepared_start_time;

    for (const auto& transition : signal.transitions) {
        if (transition.time <= request.prepared_start_time) {
            continue;
        }
        if (transition.time > request.prepared_end_time) {
            break;
        }
        if (!appendSegmentV2(columns,
                             signal,
                             current_value,
                             current_time,
                             transition.time,
                             lane_y,
                             request)) {
            break;
        }
        current_time = transition.time;
        current_value = transition.value;
    }

    if ((columns.flags & kWaveformFrameFlagTruncated) == 0U) {
        (void)appendSegmentV2(columns,
                              signal,
                              current_value,
                              current_time,
                              request.prepared_end_time,
                              lane_y,
                              request);
    }

    const auto segment_count = checkedCount(columns.x0.size()) - first_segment;
    columns.signal_table.push_back(checkedCount(signal_index));
    columns.signal_table.push_back(first_segment);
    columns.signal_table.push_back(segment_count);
    columns.signal_table.push_back(0);
}

void appendU32Array(std::vector<std::uint8_t>& output, const std::vector<std::uint32_t>& values) {
    alignTo(output, 4);
    for (const auto value : values) {
        appendU32(output, value);
    }
}

void appendF32Array(std::vector<std::uint8_t>& output, const std::vector<float>& values) {
    alignTo(output, 4);
    for (const auto value : values) {
        appendF32(output, value);
    }
}

void appendF64Array(std::vector<std::uint8_t>& output, const std::vector<double>& values) {
    alignTo(output, 8);
    for (const auto value : values) {
        appendF64(output, value);
    }
}

} // namespace

std::vector<std::uint8_t> encodeViewportFramePayload(const WaveformDataSet& data,
                                                     const WaveformViewportRequest& request) {
    if (!std::isfinite(request.start_time) || !std::isfinite(request.end_time) ||
        request.end_time <= request.start_time) {
        throw std::runtime_error("Invalid waveform viewport time range");
    }
    if (!std::isfinite(request.viewport_pixel_width) || request.viewport_pixel_width <= 0.0F ||
        !std::isfinite(request.lane_height) || request.lane_height <= 0.0F ||
        !std::isfinite(request.header_height) || request.header_height < 0.0F) {
        throw std::runtime_error("Invalid waveform viewport geometry");
    }

    const auto signals = selectedSignals(data, request);
    SegmentColumns columns;
    columns.signal_table.reserve(signals.size() * 4U);

    for (std::size_t index = 0; index < signals.size(); ++index) {
        const auto lane_y = request.header_height +
                            (static_cast<float>(index) * request.lane_height) +
                            (request.lane_height / 2.0F);
        const auto signal_index = static_cast<std::size_t>(signals[index] - data.signals.data());
        appendSignalSegments(columns, *signals[index], signal_index, lane_y, request);
        if ((columns.flags & kWaveformFrameFlagTruncated) != 0U) {
            for (std::size_t missing = index + 1; missing < signals.size(); ++missing) {
                const auto missing_index =
                    static_cast<std::size_t>(signals[missing] - data.signals.data());
                columns.signal_table.push_back(checkedCount(missing_index));
                columns.signal_table.push_back(checkedCount(columns.x0.size()));
                columns.signal_table.push_back(0);
                columns.signal_table.push_back(0);
            }
            break;
        }
    }

    std::vector<std::uint8_t> result(kViewportFrameHeaderSize, 0);
    result[0] = kViewportFrameMagic[0];
    result[1] = kViewportFrameMagic[1];
    result[2] = kViewportFrameMagic[2];
    result[3] = kViewportFrameMagic[3];
    patchU16(result, 4, kWaveformProtocolVersion);
    patchU16(result, 6, kViewportFrameHeaderSize);
    patchU32(result, 8, checkedCount(signals.size()));
    patchU32(result, 12, checkedCount(columns.x0.size()));

    alignTo(result, 4);
    const auto signal_table_offset = checkedOffset(result.size());
    appendU32Array(result, columns.signal_table);
    const auto x0_offset = checkedOffset(result.size());
    appendF32Array(result, columns.x0);
    const auto x1_offset = checkedOffset(result.size());
    appendF32Array(result, columns.x1);
    const auto lane_y_offset = checkedOffset(result.size());
    appendF32Array(result, columns.lane_y);
    const auto value_kind_offset = checkedOffset(result.size());
    result.insert(result.end(), columns.value_kind.begin(), columns.value_kind.end());
    alignTo(result, 4);
    const auto label_index_offset = checkedOffset(result.size());
    appendU32Array(result, columns.label_index);
    const auto label_bytes_offset = checkedOffset(result.size());
    result.insert(result.end(), columns.label_bytes.begin(), columns.label_bytes.end());

    patchU32(result, 16, signal_table_offset);
    patchU32(result, 20, x0_offset);
    patchU32(result, 24, x1_offset);
    patchU32(result, 28, lane_y_offset);
    patchU32(result, 32, value_kind_offset);
    patchU32(result, 36, label_index_offset);
    patchU32(result, 40, label_bytes_offset);
    patchU32(result, 44, checkedCount(columns.label_bytes.size()));
    patchU32(result, 48, columns.flags);
    patchU32(result, 52, checkedCount(columns.x0.size()));

    return result;
}

std::vector<std::uint8_t> encodeViewportFramePayloadV2(
    const WaveformDataSet& data,
    const WaveformViewportRequestV2& request) {
    if (!std::isfinite(request.prepared_start_time) ||
        !std::isfinite(request.prepared_end_time) ||
        request.prepared_end_time <= request.prepared_start_time ||
        !std::isfinite(request.viewport_start_time) ||
        !std::isfinite(request.viewport_end_time) ||
        request.viewport_end_time <= request.viewport_start_time ||
        request.viewport_start_time < request.prepared_start_time ||
        request.viewport_end_time > request.prepared_end_time) {
        throw std::runtime_error("Invalid waveform viewport v2 time range");
    }
    if (!std::isfinite(request.viewport_pixel_width) || request.viewport_pixel_width <= 0.0F ||
        !std::isfinite(request.lane_height) || request.lane_height <= 0.0F ||
        !std::isfinite(request.header_height) || request.header_height < 0.0F) {
        throw std::runtime_error("Invalid waveform viewport geometry");
    }

    const auto signals = selectedSignals(data, request);
    SegmentColumns columns;
    columns.signal_table.reserve(signals.size() * 4U);

    for (std::size_t index = 0; index < signals.size(); ++index) {
        const auto lane_y = request.header_height +
                            (static_cast<float>(index) * request.lane_height) +
                            (request.lane_height / 2.0F);
        const auto signal_index = static_cast<std::size_t>(signals[index] - data.signals.data());
        appendSignalSegmentsV2(columns, *signals[index], signal_index, lane_y, request);
        if ((columns.flags & kWaveformFrameFlagTruncated) != 0U) {
            for (std::size_t missing = index + 1; missing < signals.size(); ++missing) {
                const auto missing_index =
                    static_cast<std::size_t>(signals[missing] - data.signals.data());
                columns.signal_table.push_back(checkedCount(missing_index));
                columns.signal_table.push_back(checkedCount(columns.x0.size()));
                columns.signal_table.push_back(0);
                columns.signal_table.push_back(0);
            }
            break;
        }
    }

    std::vector<std::uint8_t> result(kViewportFrameHeaderSizeV2, 0);
    result[0] = kViewportFrameMagic[0];
    result[1] = kViewportFrameMagic[1];
    result[2] = kViewportFrameMagic[2];
    result[3] = kViewportFrameMagic[3];
    patchU16(result, 4, kViewportFrameProtocolVersionV2);
    patchU16(result, 6, kViewportFrameHeaderSizeV2);
    patchU32(result, 8, checkedCount(signals.size()));
    patchU32(result, 12, checkedCount(columns.x0.size()));

    alignTo(result, 4);
    const auto signal_table_offset = checkedOffset(result.size());
    appendU32Array(result, columns.signal_table);
    const auto x0_offset = checkedOffset(result.size());
    appendF32Array(result, columns.x0);
    const auto x1_offset = checkedOffset(result.size());
    appendF32Array(result, columns.x1);
    const auto lane_y_offset = checkedOffset(result.size());
    appendF32Array(result, columns.lane_y);
    const auto value_kind_offset = checkedOffset(result.size());
    result.insert(result.end(), columns.value_kind.begin(), columns.value_kind.end());
    alignTo(result, 4);
    const auto label_index_offset = checkedOffset(result.size());
    appendU32Array(result, columns.label_index);
    alignTo(result, 8);
    const auto time0_offset = checkedOffset(result.size());
    appendF64Array(result, columns.time0);
    alignTo(result, 8);
    const auto time1_offset = checkedOffset(result.size());
    appendF64Array(result, columns.time1);
    alignTo(result, 4);
    const auto label_bytes_offset = checkedOffset(result.size());
    result.insert(result.end(), columns.label_bytes.begin(), columns.label_bytes.end());

    patchU32(result, 16, signal_table_offset);
    patchU32(result, 20, x0_offset);
    patchU32(result, 24, x1_offset);
    patchU32(result, 28, lane_y_offset);
    patchU32(result, 32, value_kind_offset);
    patchU32(result, 36, label_index_offset);
    patchU32(result, 40, label_bytes_offset);
    patchU32(result, 44, checkedCount(columns.label_bytes.size()));
    patchU32(result, 48, columns.flags);
    patchU32(result, 52, checkedCount(columns.x0.size()));
    patchU32(result, 56, time0_offset);
    patchU32(result, 60, time1_offset);
    patchF64(result, 64, request.prepared_start_time);
    patchF64(result, 72, request.prepared_end_time);
    patchF64(result, 80, request.viewport_start_time);
    patchF64(result, 88, request.viewport_end_time);

    return result;
}

} // namespace pristine::waveform
