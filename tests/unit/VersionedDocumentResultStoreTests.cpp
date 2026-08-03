#include "pristine/server/VersionedDocumentResultStore.h"

#include <catch2/catch_test_macros.hpp>

namespace pristine::server {
namespace {

DocumentResultMetadata metadata(int version, std::uint64_t generation) {
    return DocumentResultMetadata{.document_version = version,
                                  .semantic_generation = generation,
                                  .snapshot_identity = "snapshot-" + std::to_string(generation)};
}

} // namespace

TEST_CASE("VersionedDocumentResultStore returns only the current diagnostic version",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto stored = store.storeDiagnostics("file:///top.sv", nlohmann::json::array({{"message", "bad"}}),
                                                metadata(1, 7));

    REQUIRE(store.currentDiagnostics("file:///top.sv", metadata(1, 7)).has_value());
    CHECK_FALSE(store.currentDiagnostics("file:///top.sv", metadata(2, 7)).has_value());
    CHECK_FALSE(store.currentDiagnostics("file:///top.sv", metadata(1, 8)).has_value());
    CHECK(stored.result_id.starts_with("pristine-diagnostics-v1-"));
}

TEST_CASE("VersionedDocumentResultStore retains bounded semantic token history",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    std::vector<std::string> ids;
    for (int version = 1; version <= 5; ++version) {
        ids.push_back(store.storeSemanticTokens("file:///top.sv", {version, 0, 1, 6, 0}, metadata(version, version)).result_id);
    }

    CHECK_FALSE(store.semanticTokensForResultId("file:///top.sv", ids.front()).has_value());
    REQUIRE(store.semanticTokensForResultId("file:///top.sv", ids.back()).has_value());
    const auto stats = store.stats();
    CHECK(stats.semantic_token_entry_count == VersionedDocumentResultStore::kMaxSemanticTokenHistory);
    CHECK(stats.eviction_count >= 1);
}

TEST_CASE("VersionedDocumentResultStore uses deterministic bounded bucket eviction",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    for (size_t index = 0; index < VersionedDocumentResultStore::kMaxDocumentBuckets; ++index) {
        const auto uri = "file:///" + std::to_string(index) + ".sv";
        (void)store.storeDiagnostics(uri, nlohmann::json::array(), metadata(1, 1));
    }
    REQUIRE(store.currentDiagnostics("file:///0.sv", metadata(1, 1)).has_value());
    (void)store.storeDiagnostics("file:///overflow.sv", nlohmann::json::array(), metadata(1, 1));

    CHECK_FALSE(store.currentDiagnostics("file:///1.sv", metadata(1, 1)).has_value());
    CHECK(store.currentDiagnostics("file:///0.sv", metadata(1, 1)).has_value());
    CHECK(store.stats().bucket_count == VersionedDocumentResultStore::kMaxDocumentBuckets);
}

TEST_CASE("VersionedDocumentResultStore clears closed document history and resets counters",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    (void)store.storeDiagnostics("file:///top.sv", nlohmann::json::array(), metadata(1, 1));
    (void)store.storeSemanticTokens("file:///top.sv", {0, 0, 3, 6, 0}, metadata(1, 1));
    store.clearUri("file:///top.sv");
    CHECK(store.stats().bucket_count == 0);

    store.resetStats();
    const auto stats = store.stats();
    CHECK(stats.hit_count == 0);
    CHECK(stats.miss_count == 0);
    CHECK(stats.store_count == 0);
}

TEST_CASE("VersionedDocumentResultStore gives identical diagnostics a stable result id",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto first = store.storeDiagnostics("file:///top.sv", nlohmann::json::array({{{"message", "same"}}}), metadata(1, 1));
    const auto second = store.storeDiagnostics("file:///top.sv", nlohmann::json::array({{{"message", "same"}}}), metadata(2, 2));
    CHECK(first.result_id == second.result_id);
}

TEST_CASE("VersionedDocumentResultStore includes the URI in opaque result identity",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto first = store.storeDiagnostics("file:///one.sv", nlohmann::json::array(), metadata(1, 1));
    const auto second = store.storeDiagnostics("file:///two.sv", nlohmann::json::array(), metadata(1, 1));
    CHECK(first.result_id != second.result_id);
}

TEST_CASE("VersionedDocumentResultStore replaces duplicate token result identity",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto first = store.storeSemanticTokens("file:///top.sv", {0, 0, 1, 6, 0}, metadata(1, 1));
    const auto second = store.storeSemanticTokens("file:///top.sv", {0, 0, 1, 6, 0}, metadata(2, 2));
    CHECK(first.result_id == second.result_id);
    CHECK(store.stats().semantic_token_entry_count == 1);
    REQUIRE(store.semanticTokensForResultId("file:///top.sv", second.result_id).has_value());
}

TEST_CASE("VersionedDocumentResultStore keeps result ids scoped by token legend",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto first = store.storeSemanticTokens("file:///top.sv", {0, 0, 1, 6, 0}, metadata(1, 1), 1);
    const auto second = store.storeSemanticTokens("file:///top.sv", {0, 0, 1, 6, 0}, metadata(1, 1), 2);
    CHECK(first.result_id != second.result_id);
    CHECK_FALSE(store.semanticTokensForResultId("file:///top.sv", first.result_id, 2).has_value());
}

TEST_CASE("VersionedDocumentResultStore LRU touch protects a diagnostic bucket",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    for (size_t index = 0; index < VersionedDocumentResultStore::kMaxDocumentBuckets; ++index) {
        (void)store.storeDiagnostics("file:///lru-" + std::to_string(index) + ".sv",
                                     nlohmann::json::array(),
                                     metadata(1, 1));
    }
    REQUIRE(store.currentDiagnostics("file:///lru-0.sv", metadata(1, 1)).has_value());
    (void)store.storeDiagnostics("file:///lru-overflow.sv", nlohmann::json::array(), metadata(1, 1));
    CHECK(store.currentDiagnostics("file:///lru-0.sv", metadata(1, 1)).has_value());
    CHECK_FALSE(store.currentDiagnostics("file:///lru-1.sv", metadata(1, 1)).has_value());
}

TEST_CASE("VersionedDocumentResultStore clear removes all current lookup values",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    (void)store.storeDiagnostics("file:///top.sv", nlohmann::json::array(), metadata(1, 1));
    (void)store.storeSemanticTokens("file:///other.sv", {0, 0, 1, 6, 0}, metadata(1, 1));
    store.clear();
    CHECK_FALSE(store.currentDiagnostics("file:///top.sv", metadata(1, 1)).has_value());
    CHECK_FALSE(store.currentSemanticTokens("file:///other.sv", metadata(1, 1)).has_value());
}

TEST_CASE("VersionedDocumentResultStore diagnostic payload changes rotate the result id",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    const auto first = store.storeDiagnostics("file:///top.sv", nlohmann::json::array({{{"message", "one"}}}), metadata(1, 1));
    const auto second = store.storeDiagnostics("file:///top.sv", nlohmann::json::array({{{"message", "two"}}}), metadata(1, 1));
    CHECK(first.result_id != second.result_id);
}

TEST_CASE("VersionedDocumentResultStore stats distinguish cache hit and miss",
          "[server][result-store]") {
    VersionedDocumentResultStore store;
    (void)store.storeDiagnostics("file:///top.sv", nlohmann::json::array(), metadata(1, 1));
    REQUIRE(store.currentDiagnostics("file:///top.sv", metadata(1, 1)).has_value());
    CHECK_FALSE(store.currentDiagnostics("file:///top.sv", metadata(2, 1)).has_value());
    const auto stats = store.stats();
    CHECK(stats.hit_count == 1);
    CHECK(stats.miss_count == 1);
}

} // namespace pristine::server
