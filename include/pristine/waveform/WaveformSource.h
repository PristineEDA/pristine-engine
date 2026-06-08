#pragma once

#include "pristine/waveform/WaveformData.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::waveform {

class WaveformSource {
public:
    virtual ~WaveformSource() = default;

    [[nodiscard]] virtual const WaveformDataSet& dataSet() const = 0;
    [[nodiscard]] virtual std::string_view sourceKind() const = 0;
    [[nodiscard]] virtual std::optional<std::string> fileUri() const;

    [[nodiscard]] virtual std::vector<std::uint8_t> encodeHelloResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeCatalogResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeViewportFrame(
        const WaveformViewportRequest& request) const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeViewportFrameV2(
        const WaveformViewportRequestV2& request) const;
};

[[nodiscard]] std::shared_ptr<WaveformSource> makeDataSetWaveformSource(
    WaveformDataSet data,
    std::string source_kind,
    std::optional<std::string> file_uri = std::nullopt);

[[nodiscard]] std::shared_ptr<WaveformSource> makeMockWaveformSource();

} // namespace pristine::waveform
