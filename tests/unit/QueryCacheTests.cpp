#include "../../src/analysis/semantic/QueryCache.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

namespace pristine::analysis::semantic {
namespace {

constexpr std::string_view kTop = "top";
constexpr std::string_view kOther = "other";

} // namespace

TEST_CASE("QueryCache reports visible query hit miss and entry counters",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;

    CHECK(cache.stats().total_entries == 0);
    CHECK_FALSE(cache.workspaceSymbols(1, "top", 128).has_value());

    auto stats = cache.stats();
    CHECK(stats.hits == 0);
    CHECK(stats.misses == 1);
    CHECK(stats.stores == 0);
    CHECK(stats.workspace_symbols_entries == 0);

    SemanticWorkspaceSymbolResult symbols;
    symbols.generation = 1;
    cache.storeWorkspaceSymbols(1, "top", 128, std::move(symbols));

    stats = cache.stats();
    CHECK(stats.stores == 1);
    CHECK(stats.workspace_symbols_entries == 1);
    CHECK(stats.total_entries == 1);

    const auto hit = cache.workspaceSymbols(1, "top", 128);
    REQUIRE(hit.has_value());
    CHECK(hit->generation == 1);

    CHECK_FALSE(cache.workspaceSymbols(2, "top", 128).has_value());

    stats = cache.stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 2);

    cache.resetStats();
    stats = cache.stats();
    CHECK(stats.hits == 0);
    CHECK(stats.misses == 0);
    CHECK(stats.stores == 0);
    CHECK(stats.evictions == 0);
    CHECK(stats.workspace_symbols_entries == 1);
}

TEST_CASE("QueryCache evicts oldest bounded visible query entries",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;
    cache.setMaxEntriesPerQuery(1);

    SemanticModuleHierarchyResult first;
    first.generation = 1;
    cache.storeModuleHierarchy(1, kTop, 8, std::move(first));

    SemanticModuleHierarchyResult second;
    second.generation = 1;
    cache.storeModuleHierarchy(1, kOther, 8, std::move(second));

    auto stats = cache.stats();
    CHECK(stats.stores == 2);
    CHECK(stats.evictions == 1);
    CHECK(stats.module_hierarchy_entries == 1);

    CHECK_FALSE(cache.moduleHierarchy(1, kTop, 8).has_value());
    const auto retained = cache.moduleHierarchy(1, kOther, 8);
    REQUIRE(retained.has_value());
    CHECK(retained->generation == 1);
}

TEST_CASE("QueryCache keeps diagnostics entries outside visible query eviction",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;
    cache.setMaxEntriesPerQuery(0);

    cache.storeDiagnostics(1, "file:///a.sv", {});
    cache.storeDiagnostics(1, "file:///b.sv", {});

    auto stats = cache.stats();
    CHECK(stats.diagnostics_entries == 2);
    CHECK(stats.total_entries == 2);

    CHECK(cache.diagnostics(1, "file:///a.sv").has_value());
    CHECK(cache.diagnostics(1, "file:///b.sv").has_value());
}

} // namespace pristine::analysis::semantic
