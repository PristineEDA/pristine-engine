#pragma once

#include "pristine/waveform/WaveformData.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::waveform {

inline constexpr std::string_view kWaveformProtocolName = "pristine-waveform-columnar-v1";
inline constexpr std::uint16_t kWaveformProtocolVersion = 1;
inline constexpr std::uint32_t kWaveformFrameFlagTruncated = 1U;

enum class WaveformMessageType : std::uint16_t {
    Hello = 1,
    HelloResponse = 2,
    CatalogRequest = 3,
    CatalogResponse = 4,
    ViewportFrameRequest = 5,
    ViewportFrameResponse = 6,
    ErrorResponse = 7,
    Close = 8,
};

enum class WaveformErrorCode : std::uint32_t {
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    UnknownMessage = 3,
    InternalError = 4,
};

struct WaveformFrame {
    WaveformMessageType message_type = WaveformMessageType::Hello;
    std::uint32_t request_id = 0;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> encodeFrame(const WaveformFrame& frame);
[[nodiscard]] WaveformFrame decodeFrame(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] WaveformFrame decodeFrame(const std::uint8_t* bytes, std::size_t size);

[[nodiscard]] std::vector<std::uint8_t> encodeHelloResponsePayload(const WaveformDataSet& data);
[[nodiscard]] std::vector<std::uint8_t> encodeCatalogResponsePayload(const WaveformDataSet& data);
[[nodiscard]] std::vector<std::uint8_t> encodeErrorPayload(WaveformErrorCode code,
                                                           std::string_view message);
[[nodiscard]] WaveformViewportRequest decodeViewportFrameRequestPayload(
    const std::vector<std::uint8_t>& payload);

void appendString(std::vector<std::uint8_t>& output, std::string_view value);
void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value);
void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value);
void appendF32(std::vector<std::uint8_t>& output, float value);
void appendF64(std::vector<std::uint8_t>& output, double value);
void alignTo(std::vector<std::uint8_t>& output, std::size_t alignment);

[[nodiscard]] std::uint16_t readU16(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] std::uint32_t readU32(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] float readF32(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] double readF64(const std::uint8_t* bytes, std::size_t size, std::size_t offset);

} // namespace pristine::waveform
