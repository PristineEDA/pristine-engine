#pragma once

#include "pristine/waveform/WaveformData.h"

#include <cstdint>
#include <vector>

namespace pristine::waveform {

[[nodiscard]] std::vector<std::uint8_t> encodeViewportFramePayload(
    const WaveformDataSet& data,
    const WaveformViewportRequest& request);

} // namespace pristine::waveform
