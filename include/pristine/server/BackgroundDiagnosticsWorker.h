#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
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
    enum class StateKind {
        Idle,
        Pending,
        Running,
        StaleSkipped,
        ClosedSkipped,
        Published,
        Failed,
    };

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

    struct PublishPayload {
        std::vector<analysis::SemanticEngineDiagnostic> diagnostics;
        analysis::SemanticInactiveRegionResult inactive_regions;
    };

    struct StateSnapshot {
        StateKind state = StateKind::Idle;
        std::uint64_t request_generation = 0;
        std::string last_uri;
        std::string last_phase;
        std::string last_skip_reason;
        std::int64_t elapsed_micros = 0;
        size_t open_document_count = 0;
    };

    using PublishCallback = std::function<void(std::string, PublishPayload)>;
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
    [[nodiscard]] StateSnapshot stateSnapshot() const;
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);

private:
    [[nodiscard]] bool isCurrentJob(std::uint64_t request_generation,
                                    std::string_view stale_reason);
    void setStateLocked(StateKind state,
                        std::string_view phase,
                        std::string_view uri = {},
                        std::string_view skip_reason = {});
    void setState(StateKind state,
                  std::string_view phase,
                  std::string_view uri = {},
                  std::string_view skip_reason = {});
    [[nodiscard]] bool isIdleLocked() const;
    void loop();

    PublishCallback publish_;
    ShouldPublishCallback should_publish_;
    std::chrono::milliseconds debounce_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::optional<Job> pending_job_;
    std::uint64_t request_generation_ = 0;
    StateSnapshot state_;
    std::chrono::steady_clock::time_point job_started_at_{};
    bool stop_requested_ = false;
};

} // namespace pristine::server
