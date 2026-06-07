#pragma once

#include "pristine/waveform/WaveformData.h"

#include <cstdint>
#include <vector>

namespace pristine::waveform {

[[nodiscard]] std::vector<std::uint8_t> encodeViewportFramePayload(
    const WaveformDataSet& data,
    const WaveformViewportRequest& request);
[[nodiscard]] std::vector<std::uint8_t> encodeViewportFramePayloadV2(
    const WaveformDataSet& data,
    const WaveformViewportRequestV2& request);

} // namespace pristine::waveform
