#include "pristine/server/BackgroundDiagnosticsWorker.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pristine::server {
namespace {

BackgroundDiagnosticsWorker::Job singleDocumentJob(std::string text) {
    BackgroundDiagnosticsWorker::Job job;
    job.semantic_generation = 1;
    job.allow_cold_snapshot_build = true;
    job.workspace_had_fresh_snapshot = false;
    job.workspace_root_uri = "file:///workspace";
    job.open_documents.push_back(BackgroundDiagnosticsWorker::Document{
        .uri = "file:///workspace/top.sv",
        .text = std::move(text),
        .version = 1,
        .dirty = false,
    });
    return job;
}

} // namespace

TEST_CASE("BackgroundDiagnosticsWorker publishes full diagnostics for a current document",
          "[server][diagnostics][background]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool published = false;
    std::vector<analysis::SemanticEngineDiagnostic> published_diagnostics;

    BackgroundDiagnosticsWorker worker(
        [&](std::string, std::vector<analysis::SemanticEngineDiagnostic> diagnostics) {
            {
                std::lock_guard lock(mutex);
                published = true;
                published_diagnostics = std::move(diagnostics);
            }
            cv.notify_one();
        },
        [](const BackgroundDiagnosticsWorker::Document&, std::uint64_t) {
            return BackgroundDiagnosticsWorker::PublishDecision{.publish = true, .skip_reason = {}};
        },
        std::chrono::milliseconds(1));

    worker.schedule(singleDocumentJob("module top;\n  missing_child u_missing();\nendmodule\n"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&]() { return published; }));
    CHECK_FALSE(published_diagnostics.empty());
    lock.unlock();
    REQUIRE(worker.waitUntilIdle(std::chrono::seconds(5)));
    const auto state = worker.stateSnapshot();
    CHECK(state.state == BackgroundDiagnosticsWorker::StateKind::Published);
    CHECK(state.last_uri == "file:///workspace/top.sv");
    CHECK(state.last_phase == "published");
    worker.stop();
}

TEST_CASE("BackgroundDiagnosticsWorker suppresses stale publish decisions",
          "[server][diagnostics][background]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool decision_called = false;
    bool published = false;

    BackgroundDiagnosticsWorker worker(
        [&](std::string, std::vector<analysis::SemanticEngineDiagnostic>) {
            std::lock_guard lock(mutex);
            published = true;
        },
        [&](const BackgroundDiagnosticsWorker::Document&, std::uint64_t) {
            {
                std::lock_guard lock(mutex);
                decision_called = true;
            }
            cv.notify_one();
            return BackgroundDiagnosticsWorker::PublishDecision{
                .publish = false,
                .skip_reason = "document-stale-or-closed",
            };
        },
        std::chrono::milliseconds(1));

    worker.schedule(singleDocumentJob("module top;\n  missing_child u_missing();\nendmodule\n"));

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&]() { return decision_called; }));
    }
    worker.stop();

    std::lock_guard lock(mutex);
    CHECK_FALSE(published);
}

TEST_CASE("BackgroundDiagnosticsWorker exposes pending running and idle state snapshots",
          "[server][diagnostics][background]") {
    BackgroundDiagnosticsWorker worker(
        [](std::string, std::vector<analysis::SemanticEngineDiagnostic>) {},
        [](const BackgroundDiagnosticsWorker::Document&, std::uint64_t) {
            return BackgroundDiagnosticsWorker::PublishDecision{.publish = false,
                                                                .skip_reason = "document-closed"};
        },
        std::chrono::milliseconds(20));

    worker.schedule(singleDocumentJob("module top; endmodule\n"));
    const auto scheduled = worker.stateSnapshot();
    CHECK((scheduled.state == BackgroundDiagnosticsWorker::StateKind::Pending ||
           scheduled.state == BackgroundDiagnosticsWorker::StateKind::Running));
    CHECK(scheduled.open_document_count == 1);

    REQUIRE(worker.waitUntilIdle(std::chrono::seconds(5)));
    const auto completed = worker.stateSnapshot();
    CHECK(completed.state == BackgroundDiagnosticsWorker::StateKind::ClosedSkipped);
    CHECK(completed.last_skip_reason == "document-closed");
    CHECK(completed.elapsed_micros >= 0);
    worker.stop();
}

} // namespace pristine::server
