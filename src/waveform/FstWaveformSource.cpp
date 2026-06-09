#include "pristine/waveform/FstWaveformSource.h"

#include "pristine/waveform/fst/FstReader.h"
#include "pristine/waveform/WaveformViewportEncoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
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

std::uint32_t handleFromSignalId(std::string_view signal_id) {
    constexpr std::string_view prefix = "fst:";
    if (!signal_id.starts_with(prefix)) {
        throw std::runtime_error("Unsupported FST waveform signal id: " +
                                 std::string(signal_id));
    }
    auto value = std::uint64_t{0};
    const auto digits = signal_id.substr(prefix.size());
    if (digits.empty()) {
        throw std::runtime_error("Invalid FST waveform signal id: " +
                                 std::string(signal_id));
    }
    for (const auto ch : digits) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("Invalid FST waveform signal id: " +
                                     std::string(signal_id));
        }
        value = (value * 10U) + static_cast<std::uint64_t>(ch - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("FST waveform signal id exceeds uint32 range");
        }
    }
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint32_t> handlesForRequest(const WaveformDataSet& catalog,
                                             const std::vector<std::string>& signal_ids) {
    std::vector<std::uint32_t> handles;
    const auto append_unique = [&handles](std::uint32_t handle) {
        if (std::ranges::find(handles, handle) == handles.end()) {
            handles.push_back(handle);
        }
    };

    if (signal_ids.empty()) {
        handles.reserve(catalog.signals.size());
        for (const auto& signal : catalog.signals) {
            append_unique(handleFromSignalId(signal.id));
        }
        return handles;
    }

    handles.reserve(signal_ids.size());
    for (const auto& signal_id : signal_ids) {
        append_unique(handleFromSignalId(signal_id));
    }
    return handles;
}

std::uint64_t decodeStart(double time) {
    if (!std::isfinite(time) || time <= 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::floor(time));
}

std::uint64_t decodeEnd(double time) {
    if (!std::isfinite(time) || time <= 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::ceil(time));
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

struct DecodeCacheKey {
    std::uint64_t start_time = 0;
    std::uint64_t end_time = 0;
    std::vector<std::uint32_t> handles;

    [[nodiscard]] bool operator==(const DecodeCacheKey& other) const {
        return start_time == other.start_time &&
               end_time == other.end_time &&
               handles == other.handles;
    }
};

class FstDataSetWaveformSource final : public WaveformSource {
public:
    FstDataSetWaveformSource(std::filesystem::path path,
                             std::string file_uri,
                             std::optional<std::filesystem::path> workspace_root,
                             WaveformDataSet catalog) :
        path_(std::move(path)),
        file_uri_(std::move(file_uri)),
        workspace_root_(std::move(workspace_root)),
        catalog_(std::move(catalog)) {}

    [[nodiscard]] const WaveformDataSet& dataSet() const override { return catalog_; }

    [[nodiscard]] std::string_view sourceKind() const override { return "fst"; }

    [[nodiscard]] std::optional<std::string> fileUri() const override { return file_uri_; }

    [[nodiscard]] std::vector<std::uint8_t> encodeViewportFrame(
        const WaveformViewportRequest& request) const override {
        return encodeViewportFramePayload(decodedDataSet(request), request);
    }

    [[nodiscard]] std::vector<std::uint8_t> encodeViewportFrameV2(
        const WaveformViewportRequestV2& request) const override {
        return encodeViewportFramePayloadV2(decodedDataSet(request), request);
    }

private:
    [[nodiscard]] WaveformDataSet decodedDataSet(const WaveformViewportRequest& request) const {
        return decodedDataSet(handlesForRequest(catalog_, request.signal_ids),
                              decodeStart(request.start_time),
                              decodeEnd(request.end_time));
    }

    [[nodiscard]] WaveformDataSet decodedDataSet(const WaveformViewportRequestV2& request) const {
        return decodedDataSet(handlesForRequest(catalog_, request.signal_ids),
                              decodeStart(request.prepared_start_time),
                              decodeEnd(request.prepared_end_time));
    }

    [[nodiscard]] WaveformDataSet decodedDataSet(std::vector<std::uint32_t> handles,
                                                 std::uint64_t start_time,
                                                 std::uint64_t end_time) const {
        std::ranges::sort(handles);
        DecodeCacheKey key{.start_time = start_time,
                           .end_time = end_time,
                           .handles = std::move(handles)};
        {
            std::lock_guard lock(cache_mutex_);
            if (cache_key_.has_value() && *cache_key_ == key && cache_data_.has_value()) {
                return *cache_data_;
            }
        }

        auto decoded = catalog_;
        try {
            fst::FstReadOptions options{.workspace_root = workspace_root_,
                                        .decode_transitions = true,
                                        .decode_start_time = start_time,
                                        .decode_end_time = end_time,
                                        .decode_signal_handles = key.handles};
            auto fst_data = fst::readFstFile(path_, options);
            decoded = toWaveformDataSet(fst_data);
        }
        catch (const std::exception&) {
            // Keep pipe sessions robust while the FST value-change decoder is still being
            // expanded for rarer libfst chain variants; the catalog/index path remains valid.
        }
        {
            std::lock_guard lock(cache_mutex_);
            cache_key_ = key;
            cache_data_ = decoded;
        }
        return decoded;
    }

    std::filesystem::path path_;
    std::string file_uri_;
    std::optional<std::filesystem::path> workspace_root_;
    WaveformDataSet catalog_;
    mutable std::mutex cache_mutex_;
    mutable std::optional<DecodeCacheKey> cache_key_;
    mutable std::optional<WaveformDataSet> cache_data_;
};

} // namespace

std::shared_ptr<WaveformSource> openFstWaveformSource(
    const std::filesystem::path& path,
    std::string file_uri,
    std::optional<std::filesystem::path> workspace_root) {
    const fst::FstReadOptions options{.workspace_root = std::move(workspace_root),
                                      .decode_transitions = false};
    auto fst_data = fst::readFstFile(path, options);
    auto catalog = toWaveformDataSet(fst_data);
    return std::make_shared<FstDataSetWaveformSource>(path,
                                                      std::move(file_uri),
                                                      options.workspace_root,
                                                      std::move(catalog));
}

} // namespace pristine::waveform
