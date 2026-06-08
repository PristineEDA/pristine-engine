#include "pristine/waveform/WaveformSource.h"

#include "pristine/waveform/WaveformBinaryProtocol.h"
#include "pristine/waveform/WaveformMockGenerator.h"
#include "pristine/waveform/WaveformViewportEncoder.h"

#include <utility>

namespace pristine::waveform {
namespace {

class DataSetWaveformSource final : public WaveformSource {
public:
    DataSetWaveformSource(WaveformDataSet data,
                          std::string source_kind,
                          std::optional<std::string> file_uri) :
        data_(std::move(data)),
        source_kind_(std::move(source_kind)),
        file_uri_(std::move(file_uri)) {}

    [[nodiscard]] const WaveformDataSet& dataSet() const override { return data_; }

    [[nodiscard]] std::string_view sourceKind() const override { return source_kind_; }

    [[nodiscard]] std::optional<std::string> fileUri() const override { return file_uri_; }

private:
    WaveformDataSet data_;
    std::string source_kind_;
    std::optional<std::string> file_uri_;
};

} // namespace

std::optional<std::string> WaveformSource::fileUri() const {
    return std::nullopt;
}

std::vector<std::uint8_t> WaveformSource::encodeHelloResponse() const {
    return encodeHelloResponsePayload(dataSet());
}

std::vector<std::uint8_t> WaveformSource::encodeCatalogResponse() const {
    return encodeCatalogResponsePayload(dataSet());
}

std::vector<std::uint8_t> WaveformSource::encodeViewportFrame(
    const WaveformViewportRequest& request) const {
    return encodeViewportFramePayload(dataSet(), request);
}

std::vector<std::uint8_t> WaveformSource::encodeViewportFrameV2(
    const WaveformViewportRequestV2& request) const {
    return encodeViewportFramePayloadV2(dataSet(), request);
}

std::shared_ptr<WaveformSource> makeDataSetWaveformSource(WaveformDataSet data,
                                                          std::string source_kind,
                                                          std::optional<std::string> file_uri) {
    return std::make_shared<DataSetWaveformSource>(std::move(data),
                                                   std::move(source_kind),
                                                   std::move(file_uri));
}

std::shared_ptr<WaveformSource> makeMockWaveformSource() {
    return makeDataSetWaveformSource(makeMockWaveformDataSet(), "mock");
}

} // namespace pristine::waveform
