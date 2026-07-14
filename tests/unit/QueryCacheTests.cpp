#include "../../src/analysis/semantic/QueryCache.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
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

TEST_CASE("QueryCache distinguishes inferred and explicit module hierarchy keys",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;

    SemanticModuleHierarchyResult inferred;
    inferred.generation = 1;
    inferred.roots.push_back(SemanticHierarchyNode{.module_name = "top_a"});
    cache.storeModuleHierarchy(1, std::nullopt, 8, std::move(inferred));

    SemanticModuleHierarchyResult explicit_module;
    explicit_module.generation = 1;
    explicit_module.roots.push_back(SemanticHierarchyNode{.module_name = "<inferred>"});
    cache.storeModuleHierarchy(1, std::string_view("<inferred>"), 8, std::move(explicit_module));

    const auto inferred_hit = cache.moduleHierarchy(1, std::nullopt, 8);
    REQUIRE(inferred_hit.has_value());
    REQUIRE(inferred_hit->roots.size() == 1);
    CHECK(inferred_hit->roots.front().module_name == "top_a");

    const auto explicit_hit = cache.moduleHierarchy(1, std::string_view("<inferred>"), 8);
    REQUIRE(explicit_hit.has_value());
    REQUIRE(explicit_hit->roots.size() == 1);
    CHECK(explicit_hit->roots.front().module_name == "<inferred>");
}

TEST_CASE("QueryCache typed completion keys avoid delimiter collisions",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;

    SemanticCompletionResult first;
    first.generation = 1;
    first.items.push_back(SemanticCompletionItem{.label = "from-uri-with-delimiter"});
    cache.storeCompletions(1, "file:///workspace/a|1", 2, 3, "x", std::move(first));

    SemanticCompletionResult second;
    second.generation = 1;
    second.items.push_back(SemanticCompletionItem{.label = "from-prefix-with-delimiter"});
    cache.storeCompletions(1, "file:///workspace/a", 1, 2, "3|x", std::move(second));

    const auto first_hit = cache.completions(1, "file:///workspace/a|1", 2, 3, "x");
    REQUIRE(first_hit.has_value());
    REQUIRE(first_hit->items.size() == 1);
    CHECK(first_hit->items.front().label == "from-uri-with-delimiter");

    const auto second_hit = cache.completions(1, "file:///workspace/a", 1, 2, "3|x");
    REQUIRE(second_hit.has_value());
    REQUIRE(second_hit->items.size() == 1);
    CHECK(second_hit->items.front().label == "from-prefix-with-delimiter");

    CHECK(cache.stats().completions_entries == 2);
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

TEST_CASE("QueryCache snapshots and resets counters without clearing entries",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;

    cache.storeDiagnostics(7, "file:///workspace/top.sv", {});

    SemanticSignatureHelpResult signature_help;
    signature_help.generation = 7;
    cache.storeSignatureHelp(7, "file:///workspace/top.sv", 1, 2, std::move(signature_help));

    SemanticInlayHintResult inlay_hints;
    inlay_hints.generation = 7;
    cache.storeInlayHints(7,
                          "file:///workspace/top.sv",
                          ParseRange{.start_line = 0,
                                     .start_character = 0,
                                     .end_line = 1,
                                     .end_character = 0},
                          std::move(inlay_hints));

    SemanticCodeActionResult code_actions;
    code_actions.generation = 7;
    cache.storeCodeActions(7,
                           "file:///workspace/top.sv",
                           ParseRange{.start_line = 1,
                                      .start_character = 0,
                                      .end_line = 1,
                                      .end_character = 4},
                           std::move(code_actions));

    CHECK(cache.signatureHelp(7, "file:///workspace/top.sv", 1, 2).has_value());
    CHECK(cache.inlayHints(7,
                           "file:///workspace/top.sv",
                           ParseRange{.start_line = 0,
                                      .start_character = 0,
                                      .end_line = 1,
                                      .end_character = 0})
              .has_value());

    const auto snapshot = cache.snapshotAndResetStats();
    CHECK(snapshot.stores == 4);
    CHECK(snapshot.hits == 2);
    CHECK(snapshot.diagnostics_entries == 1);
    CHECK(snapshot.signature_help_entries == 1);
    CHECK(snapshot.inlay_hints_entries == 1);
    CHECK(snapshot.code_actions_entries == 1);
    CHECK(snapshot.total_entries == 4);

    const auto reset = cache.stats();
    CHECK(reset.hits == 0);
    CHECK(reset.misses == 0);
    CHECK(reset.stores == 0);
    CHECK(reset.evictions == 0);
    CHECK(reset.total_entries == 4);
}

TEST_CASE("QueryCache aggregates and resets URI-local invocation scan telemetry",
          "[analysis][semantic][query-cache][telemetry][callable][macro]") {
    QueryCache cache;
    cache.storeSignatureHelp(
        1,
        "file:///workspace/top.sv",
        2,
        8,
        SemanticSignatureHelpResult{.scanned_invocation_count = 3,
                                    .scanned_macro_definition_count = 2,
                                    .scanned_global_symbol_count = 0});
    cache.storeInlayHints(
        1,
        "file:///workspace/top.sv",
        ParseRange{.start_line = 0, .start_character = 0, .end_line = 4, .end_character = 0},
        SemanticInlayHintResult{.scanned_invocation_count = 5,
                                .scanned_macro_definition_count = 1,
                                .scanned_global_symbol_count = 0});

    const auto stats = cache.stats();
    CHECK(stats.signature_scanned_invocations == 3);
    CHECK(stats.inlay_scanned_invocations == 5);
    CHECK(stats.macro_scanned_visible_definitions == 3);
    CHECK(stats.scanned_global_symbols == 0);

    const auto snapshot = cache.snapshotAndResetStats();
    CHECK(snapshot.signature_scanned_invocations == 3);
    const auto reset = cache.stats();
    CHECK(reset.signature_scanned_invocations == 0);
    CHECK(reset.inlay_scanned_invocations == 0);
    CHECK(reset.macro_scanned_visible_definitions == 0);
    CHECK(reset.scanned_global_symbols == 0);
}

} // namespace pristine::analysis::semantic
