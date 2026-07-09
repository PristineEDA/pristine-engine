#include "pristine/server/BackgroundDiagnosticsWorker.h"

#include "pristine/analysis/SemanticWorkspace.h"
#include "pristine/analysis/SourceUtil.h"
#include "../analysis/semantic/DebugTrace.h"

#include <fstream>
#include <iterator>
#include <optional>
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
    }
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool BackgroundDiagnosticsWorker::isCurrentJob(std::uint64_t request_generation,
                                               std::string_view stale_reason) const {
    std::lock_guard lock(mutex_);
    if (stop_requested_ || request_generation != request_generation_) {
        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                              std::string(stale_reason));
        return false;
    }
    return true;
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
        }

        try {
            PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.backgroundDiagnostics");
            std::this_thread::sleep_for(debounce_);
            if (!isCurrentJob(job.request_generation, "stale-before-build")) {
                continue;
            }
            if (!job.allow_cold_snapshot_build && !job.workspace_had_fresh_snapshot) {
                analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                      "large-workspace-cold-snapshot");
                continue;
            }

            analysis::SemanticWorkspace background_workspace;
            background_workspace.configureSemanticEngine(job.config);
            background_workspace.setWorkspaceRoot(job.workspace_root_uri);

            bool stale = false;
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
                if (!isCurrentJob(job.request_generation, "stale-before-publish")) {
                    break;
                }

                auto diagnostics = background_workspace.engineDiagnosticsFor(document.uri);

                if (!isCurrentJob(job.request_generation, "stale-after-diagnostics")) {
                    break;
                }

                const auto decision = should_publish_(document, job.semantic_generation);
                if (!decision.publish) {
                    analysis::semantic::debugTraceInstant(
                        "server.backgroundDiagnostics.skip",
                        decision.skip_reason.empty() ? "publish-suppressed" : decision.skip_reason);
                    continue;
                }
                publish_(document.uri, std::move(diagnostics));
            }
        }
        catch (const std::exception& error) {
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", error.what());
        }
        catch (...) {
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", "unknown");
        }
    }
}

} // namespace pristine::server
