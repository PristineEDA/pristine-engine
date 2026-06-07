#include "pristine/waveform/WaveformBinaryProtocol.h"

#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace pristine::waveform {
namespace {

constexpr std::uint8_t kFrameMagic[] = {'P', 'W', 'F', '1'};
constexpr std::uint16_t kFrameHeaderSize = 24;
constexpr std::size_t kMaxPayloadSize = 128U * 1024U * 1024U;

void requireAvailable(std::size_t size, std::size_t offset, std::size_t length) {
    if (offset > size || length > size - offset) {
        throw std::runtime_error("Waveform binary payload is truncated");
    }
}

std::string readString(const std::uint8_t* bytes, std::size_t size, std::size_t& offset) {
    const auto length = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    requireAvailable(size, offset, length);
    std::string result(reinterpret_cast<const char*>(bytes + offset),
                       reinterpret_cast<const char*>(bytes + offset + length));
    offset += length;
    return result;
}

void appendCount(std::vector<std::uint8_t>& output, std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform binary count exceeds uint32 range");
    }
    appendU32(output, static_cast<std::uint32_t>(value));
}

std::uint32_t stringOffset(std::vector<std::uint8_t>& table, std::string_view value) {
    if (table.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform string table exceeds uint32 range");
    }
    const auto offset = static_cast<std::uint32_t>(table.size());
    appendString(table, value);
    return offset;
}

} // namespace

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void appendF32(std::vector<std::uint8_t>& output, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(output, bits);
}

void appendF64(std::vector<std::uint8_t>& output, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xffULL));
    }
}

void appendString(std::vector<std::uint8_t>& output, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform string exceeds uint32 range");
    }
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void alignTo(std::vector<std::uint8_t>& output, std::size_t alignment) {
    if (alignment == 0) {
        return;
    }
    while (output.size() % alignment != 0) {
        output.push_back(0);
    }
}

std::uint16_t readU16(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(std::uint16_t));
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(std::uint32_t));
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

float readF32(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    const auto bits = readU32(bytes, size, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double readF64(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(double));
    std::uint64_t bits = 0;
    for (int index = 0; index < 8; ++index) {
        bits |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) <<
                (index * 8);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<std::uint8_t> encodeFrame(const WaveformFrame& frame) {
    if (frame.payload.size() > kMaxPayloadSize ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Waveform payload is too large");
    }
    std::vector<std::uint8_t> result;
    result.reserve(kFrameHeaderSize + frame.payload.size());
    result.insert(result.end(), std::begin(kFrameMagic), std::end(kFrameMagic));
    appendU16(result, kWaveformProtocolVersion);
    appendU16(result, static_cast<std::uint16_t>(frame.message_type));
    appendU32(result, frame.request_id);
    appendU32(result, frame.flags);
    appendU32(result, static_cast<std::uint32_t>(frame.payload.size()));
    appendU32(result, 0);
    result.insert(result.end(), frame.payload.begin(), frame.payload.end());
    return result;
}

WaveformFrame decodeFrame(const std::vector<std::uint8_t>& bytes) {
    return decodeFrame(bytes.data(), bytes.size());
}

WaveformFrame decodeFrame(const std::uint8_t* bytes, std::size_t size) {
    requireAvailable(size, 0, kFrameHeaderSize);
    if (bytes[0] != kFrameMagic[0] || bytes[1] != kFrameMagic[1] ||
        bytes[2] != kFrameMagic[2] || bytes[3] != kFrameMagic[3]) {
        throw std::runtime_error("Invalid waveform frame magic");
    }
    const auto version = readU16(bytes, size, 4);
    if (version != kWaveformProtocolVersion) {
        throw std::runtime_error("Unsupported waveform frame version");
    }
    const auto message_type = readU16(bytes, size, 6);
    const auto request_id = readU32(bytes, size, 8);
    const auto flags = readU32(bytes, size, 12);
    const auto payload_size = readU32(bytes, size, 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Waveform payload is too large");
    }
    requireAvailable(size, kFrameHeaderSize, payload_size);
    if (size != kFrameHeaderSize + payload_size) {
        throw std::runtime_error("Waveform frame has trailing bytes");
    }

    return WaveformFrame{.message_type = static_cast<WaveformMessageType>(message_type),
                         .request_id = request_id,
                         .flags = flags,
                         .payload = std::vector<std::uint8_t>(bytes + kFrameHeaderSize,
                                                              bytes + kFrameHeaderSize +
                                                                  payload_size)};
}

std::vector<std::uint8_t> encodeHelloResponsePayload(const WaveformDataSet& data) {
    std::vector<std::uint8_t> result;
    appendU16(result, kWaveformProtocolVersion);
    appendU16(result, 0);
    appendF64(result, data.duration);
    appendCount(result, data.groups.size());
    appendCount(result, data.signals.size());
    appendString(result, data.id);
    appendString(result, data.title);
    appendString(result, data.timescale_unit);
    return result;
}

std::vector<std::uint8_t> encodeCatalogResponsePayload(const WaveformDataSet& data) {
    std::vector<std::uint8_t> strings;
    std::vector<std::uint8_t> group_table;
    std::vector<std::uint8_t> signal_table;

    for (const auto& group : data.groups) {
        appendU32(group_table, stringOffset(strings, group.id));
        appendU32(group_table, stringOffset(strings, group.label));
    }

    for (const auto& signal : data.signals) {
        if (signal.group_index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Waveform signal group index exceeds uint32 range");
        }
        appendU32(signal_table, stringOffset(strings, signal.id));
        appendU32(signal_table, stringOffset(strings, signal.name));
        appendU32(signal_table, stringOffset(strings, signal.path));
        appendU32(signal_table, static_cast<std::uint32_t>(signal.group_index));
        signal_table.push_back(static_cast<std::uint8_t>(signal.kind));
        signal_table.push_back(0);
        signal_table.push_back(0);
        signal_table.push_back(0);
        appendU32(signal_table, signal.color_rgb);
        appendU32(signal_table, signal.width);
    }

    std::vector<std::uint8_t> result;
    appendCount(result, data.groups.size());
    appendCount(result, data.signals.size());
    appendU32(result, static_cast<std::uint32_t>(group_table.size()));
    appendU32(result, static_cast<std::uint32_t>(signal_table.size()));
    appendU32(result, static_cast<std::uint32_t>(strings.size()));
    result.insert(result.end(), group_table.begin(), group_table.end());
    result.insert(result.end(), signal_table.begin(), signal_table.end());
    result.insert(result.end(), strings.begin(), strings.end());
    return result;
}

std::vector<std::uint8_t> encodeErrorPayload(WaveformErrorCode code, std::string_view message) {
    std::vector<std::uint8_t> result;
    appendU32(result, static_cast<std::uint32_t>(code));
    appendString(result, message);
    return result;
}

WaveformViewportRequest decodeViewportFrameRequestPayload(const std::vector<std::uint8_t>& payload) {
    const auto* bytes = payload.data();
    const auto size = payload.size();
    std::size_t offset = 0;
    WaveformViewportRequest result{};
    result.start_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.end_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.viewport_pixel_width = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.lane_height = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.header_height = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.max_segments = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    const auto signal_count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    result.signal_ids.reserve(signal_count);
    for (std::uint32_t index = 0; index < signal_count; ++index) {
        result.signal_ids.push_back(readString(bytes, size, offset));
    }
    if (offset != size) {
        throw std::runtime_error("Viewport request payload has trailing bytes");
    }
    return result;
}

WaveformViewportRequestV2 decodeViewportFrameRequestPayloadV2(
    const std::vector<std::uint8_t>& payload) {
    const auto* bytes = payload.data();
    const auto size = payload.size();
    std::size_t offset = 0;
    WaveformViewportRequestV2 result{};
    result.prepared_start_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.prepared_end_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.viewport_start_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.viewport_end_time = readF64(bytes, size, offset);
    offset += sizeof(double);
    result.viewport_pixel_width = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.lane_height = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.header_height = readF32(bytes, size, offset);
    offset += sizeof(float);
    result.max_segments = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    const auto signal_count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    result.signal_ids.reserve(signal_count);
    for (std::uint32_t index = 0; index < signal_count; ++index) {
        result.signal_ids.push_back(readString(bytes, size, offset));
    }
    if (offset != size) {
        throw std::runtime_error("Viewport v2 request payload has trailing bytes");
    }
    return result;
}

} // namespace pristine::waveform
