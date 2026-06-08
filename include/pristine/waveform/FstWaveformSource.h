#pragma once

#include "pristine/waveform/WaveformSource.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace pristine::waveform {

[[nodiscard]] std::shared_ptr<WaveformSource> openFstWaveformSource(
    const std::filesystem::path& path,
    std::string file_uri,
    std::optional<std::filesystem::path> workspace_root = std::nullopt);

} // namespace pristine::waveform
