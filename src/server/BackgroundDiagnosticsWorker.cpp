#include "pristine/server/BackgroundDiagnosticsWorker.h"

#include "pristine/analysis/SemanticWorkspace.h"
#include "pristine/analysis/SourceUtil.h"
#include "../analysis/semantic/DebugTrace.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace pristine::server {
namespace {

std::optional<std::string> readFileText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

BackgroundDiagnosticsWorker::BackgroundDiagnosticsWorker(PublishCallback publish,
                                                         ShouldPublishCallback should_publish,
                                                         std::chrono::milliseconds debounce) :
    publish_(std::move(publish)),
    should_publish_(std::move(should_publish)),
    debounce_(debounce),
    thread_([this]() { loop(); }) {}

BackgroundDiagnosticsWorker::~BackgroundDiagnosticsWorker() {
    stop();
}

void BackgroundDiagnosticsWorker::schedule(Job job) {
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return;
        }
        job.request_generation = ++request_generation_;
        job_started_at_ = std::chrono::steady_clock::now();
        state_.request_generation = job.request_generation;
        state_.open_document_count = job.open_documents.size();
        setStateLocked(StateKind::Pending, "scheduled");
        pending_job_ = std::move(job);
    }
    cv_.notify_one();
}

void BackgroundDiagnosticsWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        pending_job_.reset();
        ++request_generation_;
        setStateLocked(StateKind::Idle, "stopped");
    }
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

BackgroundDiagnosticsWorker::StateSnapshot BackgroundDiagnosticsWorker::stateSnapshot() const {
    std::lock_guard lock(mutex_);
    auto snapshot = state_;
    if (snapshot.state == StateKind::Pending || snapshot.state == StateKind::Running) {
        snapshot.elapsed_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - job_started_at_)
                                      .count();
    }
    return snapshot;
}

bool BackgroundDiagnosticsWorker::waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this]() { return isIdleLocked(); });
}

bool BackgroundDiagnosticsWorker::isCurrentJob(std::uint64_t request_generation,
                                               std::string_view stale_reason) {
    std::lock_guard lock(mutex_);
    if (stop_requested_ || request_generation != request_generation_) {
        setStateLocked(StateKind::StaleSkipped, stale_reason, {}, stale_reason);
        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                              std::string(stale_reason));
        cv_.notify_all();
        return false;
    }
    return true;
}

void BackgroundDiagnosticsWorker::setStateLocked(StateKind state,
                                                 std::string_view phase,
                                                 std::string_view uri,
                                                 std::string_view skip_reason) {
    state_.state = state;
    state_.last_phase = std::string(phase);
    state_.last_uri = std::string(uri);
    state_.last_skip_reason = std::string(skip_reason);
    state_.elapsed_micros =
        job_started_at_ == std::chrono::steady_clock::time_point{}
            ? 0
            : std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - job_started_at_)
                  .count();
}

void BackgroundDiagnosticsWorker::setState(StateKind state,
                                           std::string_view phase,
                                           std::string_view uri,
                                           std::string_view skip_reason) {
    {
        std::lock_guard lock(mutex_);
        setStateLocked(state, phase, uri, skip_reason);
    }
    cv_.notify_all();
}

bool BackgroundDiagnosticsWorker::isIdleLocked() const {
    return !pending_job_.has_value() && state_.state != StateKind::Pending &&
           state_.state != StateKind::Running;
}

void BackgroundDiagnosticsWorker::loop() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stop_requested_ || pending_job_.has_value(); });
            if (stop_requested_) {
                return;
            }
            job = std::move(*pending_job_);
            pending_job_.reset();
            setStateLocked(StateKind::Running, "debounce");
        }
        cv_.notify_all();

        try {
            PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.backgroundDiagnostics");
            std::this_thread::sleep_for(debounce_);
            if (!isCurrentJob(job.request_generation, "stale-before-build")) {
                continue;
            }
            if (!job.allow_cold_snapshot_build && !job.workspace_had_fresh_snapshot) {
                setState(StateKind::StaleSkipped,
                         "large-workspace-cold-snapshot",
                         {},
                         "large-workspace-cold-snapshot");
                analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                      "large-workspace-cold-snapshot");
                continue;
            }

            setState(StateKind::Running, "configure-workspace");
            analysis::SemanticWorkspace background_workspace;
            background_workspace.configureSemanticEngine(job.config);
            background_workspace.setWorkspaceRoot(job.workspace_root_uri);

            bool stale = false;
            setState(StateKind::Running, "index-source-paths");
            for (const auto& path : job.indexed_source_paths) {
                const auto text = readFileText(path);
                if (text.has_value()) {
                    background_workspace.updateDocument(analysis::pathToFileUri(path), *text);
                }
                if (!isCurrentJob(job.request_generation, "stale-during-index")) {
                    stale = true;
                    break;
                }
            }
            if (stale || !isCurrentJob(job.request_generation, "stale-after-index")) {
                continue;
            }

            setState(StateKind::Running, "open-documents");
            for (const auto& document : job.open_documents) {
                background_workspace.updateDocument(document.uri,
                                                    document.text,
                                                    analysis::SemanticDocumentState{
                                                        .version = document.version,
                                                        .is_open = true,
                                                        .dirty = document.dirty,
                                                        .invalidate_dependents = true});
                if (!isCurrentJob(job.request_generation, "stale-during-open-documents")) {
                    stale = true;
                    break;
                }
            }
            if (stale) {
                continue;
            }

            for (const auto& document : job.open_documents) {
                setState(StateKind::Running, "diagnostics", document.uri);
                if (!isCurrentJob(job.request_generation, "stale-before-publish")) {
                    break;
                }

                auto diagnostics = background_workspace.engineDiagnosticsFor(document.uri);

                if (!isCurrentJob(job.request_generation, "stale-after-diagnostics")) {
                    break;
                }

                const auto decision = should_publish_(document, job.semantic_generation);
                if (!decision.publish) {
                    const auto state = decision.skip_reason.find("closed") != std::string::npos
                                           ? StateKind::ClosedSkipped
                                           : StateKind::StaleSkipped;
                    setState(state,
                             "publish-suppressed",
                             document.uri,
                             decision.skip_reason.empty() ? "publish-suppressed"
                                                          : decision.skip_reason);
                    analysis::semantic::debugTraceInstant(
                        "server.backgroundDiagnostics.skip",
                        decision.skip_reason.empty() ? "publish-suppressed" : decision.skip_reason);
                    continue;
                }
                publish_(document.uri, std::move(diagnostics));
                setState(StateKind::Published, "published", document.uri);
            }
            if (job.open_documents.empty()) {
                setState(StateKind::Idle, "no-open-documents");
            }
        }
        catch (const std::exception& error) {
            setState(StateKind::Failed, "exception", {}, error.what());
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", error.what());
        }
        catch (...) {
            setState(StateKind::Failed, "exception", {}, "unknown");
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", "unknown");
        }
    }
}

} // namespace pristine::server
