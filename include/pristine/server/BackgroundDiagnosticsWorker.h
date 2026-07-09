#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pristine::server {

class BackgroundDiagnosticsWorker {
public:
    struct Document {
        std::string uri;
        std::string text;
        int version = -1;
        bool dirty = false;
    };

    struct Job {
        std::uint64_t request_generation = 0;
        std::uint64_t semantic_generation = 0;
        bool allow_cold_snapshot_build = true;
        bool workspace_had_fresh_snapshot = false;
        std::string workspace_root_uri;
        analysis::SemanticEngineConfig config;
        std::vector<std::filesystem::path> indexed_source_paths;
        std::vector<Document> open_documents;
    };

    struct PublishDecision {
        bool publish = false;
        std::string skip_reason;
    };

    using PublishCallback =
        std::function<void(std::string, std::vector<analysis::SemanticEngineDiagnostic>)>;
    using ShouldPublishCallback =
        std::function<PublishDecision(const Document&, std::uint64_t semantic_generation)>;

    BackgroundDiagnosticsWorker(PublishCallback publish,
                                ShouldPublishCallback should_publish,
                                std::chrono::milliseconds debounce = std::chrono::milliseconds(500));
    ~BackgroundDiagnosticsWorker();

    BackgroundDiagnosticsWorker(const BackgroundDiagnosticsWorker&) = delete;
    BackgroundDiagnosticsWorker& operator=(const BackgroundDiagnosticsWorker&) = delete;

    void schedule(Job job);
    void stop();

private:
    [[nodiscard]] bool isCurrentJob(std::uint64_t request_generation,
                                    std::string_view stale_reason) const;
    void loop();

    PublishCallback publish_;
    ShouldPublishCallback should_publish_;
    std::chrono::milliseconds debounce_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::optional<Job> pending_job_;
    std::uint64_t request_generation_ = 0;
    bool stop_requested_ = false;
};

} // namespace pristine::server
