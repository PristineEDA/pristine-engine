#pragma once

#include "pristine/waveform/WaveformData.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace pristine::waveform {

class WaveformSource;

struct WaveformPipeEndpoint {
    std::string kind;
    std::string path;
};

struct WaveformSessionInfo {
    std::string session_id;
    std::string protocol;
    WaveformPipeEndpoint endpoint;
    std::string title;
    double duration = 0.0;
    std::string timescale_unit;
    std::size_t group_count = 0;
    std::size_t signal_count = 0;
    std::string source;
    std::optional<std::string> file_uri;
};

class WaveformPipeService {
public:
    WaveformPipeService() = default;
    ~WaveformPipeService();

    WaveformPipeService(const WaveformPipeService&) = delete;
    WaveformPipeService& operator=(const WaveformPipeService&) = delete;

    [[nodiscard]] WaveformSessionInfo openMockSession();
    [[nodiscard]] WaveformSessionInfo openSession(std::shared_ptr<WaveformSource> source);
    [[nodiscard]] bool closeSession(std::string_view session_id);
    void stop();
    [[nodiscard]] bool shouldStop(std::string_view session_id) const;

private:
    void runSession(std::string session_id,
                    std::string endpoint_path,
                    std::shared_ptr<WaveformSource> source);
    void wakeEndpoint(const std::string& endpoint_path);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::optional<WaveformSessionInfo> active_session_;
    bool stop_requested_ = false;
    std::uint64_t next_session_number_ = 1;
};

} // namespace pristine::waveform
