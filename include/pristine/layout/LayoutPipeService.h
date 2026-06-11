#pragma once

#include "pristine/layout/LayoutData.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace pristine::layout {

class LayoutSource;

struct LayoutPipeEndpoint {
    std::string kind;
    std::string path;
};

struct LayoutSessionInfo {
    std::string session_id;
    std::string protocol;
    LayoutPipeEndpoint endpoint;
    std::string title;
    std::size_t lef_count = 0;
    bool def_present = false;
    std::uint32_t units_per_micron = 1000;
    std::optional<LayoutRect> bounds;
    std::size_t layer_count = 0;
    std::size_t macro_count = 0;
    std::size_t component_count = 0;
    std::size_t net_count = 0;
    std::size_t diagnostic_count = 0;
    std::vector<std::string> messages;
    std::vector<std::string> file_uris;
};

class LayoutPipeService {
public:
    LayoutPipeService() = default;
    ~LayoutPipeService();

    LayoutPipeService(const LayoutPipeService&) = delete;
    LayoutPipeService& operator=(const LayoutPipeService&) = delete;

    [[nodiscard]] LayoutSessionInfo openSession(std::shared_ptr<LayoutSource> source,
                                                std::size_t lef_count,
                                                bool def_present);
    [[nodiscard]] bool closeSession(std::string_view session_id);
    void stop();
    [[nodiscard]] bool shouldStop(std::string_view session_id) const;

private:
    void runSession(std::string session_id,
                    std::string endpoint_path,
                    std::shared_ptr<LayoutSource> source);
    void wakeEndpoint(const std::string& endpoint_path);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::optional<LayoutSessionInfo> active_session_;
    bool stop_requested_ = false;
    std::uint64_t next_session_number_ = 1;
};

} // namespace pristine::layout
