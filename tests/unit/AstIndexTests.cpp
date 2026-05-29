#include "../../src/analysis/semantic/AstIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

AstIndexSymbol symbol(std::string stable_id,
                      std::string name,
                      std::string kind,
                      std::string uri,
                      int line) {
    return AstIndexSymbol{.stable_id = std::move(stable_id),
                          .identity = SemanticSymbolIdentity{
                              .stable_id = stable_id,
                              .name = std::move(name),
                              .kind = std::move(kind),
                              .location = SemanticLocation{.uri = std::move(uri),
                                                           .range = rangeAt(line, 2, 12)}}};
}

TEST_CASE("AstIndex filters, sorts, and maps workspace symbols",
          "[analysis][semantic][ast-index][workspace-symbol]") {
    const AstIndexContext context{
        .generation = 5,
        .snapshot_available = true,
        .symbols = {symbol("symbol|ready", "ready", "Variable", "file:///workspace/top.sv", 2),
                    symbol("symbol|pkg", "control_pkg", "Package", "file:///workspace/pkg.sv", 0),
                    symbol("symbol|type", "nibble_t", "TypeAlias", "file:///workspace/pkg.sv", 1)}};

    const auto packages = workspaceSymbols(context, "ct", 100);
    REQUIRE_FALSE(packages.unresolved);
    REQUIRE(packages.symbols.size() == 1);
    CHECK(packages.symbols.front().name == "control_pkg");
    CHECK(packages.symbols.front().kind == 4);
    CHECK(packages.symbols.front().stable_id == "symbol|pkg");

    const auto typedefs = workspaceSymbols(context, "nt", 100);
    CHECK(std::any_of(typedefs.symbols.begin(), typedefs.symbols.end(), [](const SemanticWorkspaceSymbol& symbol) {
        return symbol.name == "nibble_t" && symbol.kind == 26;
    }));

    const auto truncated = workspaceSymbols(context, "", 1);
    CHECK(truncated.truncated);
    REQUIRE(truncated.symbols.size() == 1);
    CHECK(truncated.messages.front().find("truncated") != std::string::npos);
}

TEST_CASE("AstIndex reports unavailable snapshot for workspace symbols",
          "[analysis][semantic][ast-index][workspace-symbol][unresolved]") {
    const AstIndexContext context{.generation = 8, .snapshot_available = false};

    const auto result = workspaceSymbols(context, "", 100);

    CHECK(result.unresolved);
    CHECK(result.generation == 8);
    REQUIRE_FALSE(result.messages.empty());
    CHECK(result.messages.front().find("snapshot is unavailable") != std::string::npos);
}

TEST_CASE("AstIndex builds provider-facing symbol and reference views",
          "[analysis][semantic][ast-index][provider-view]") {
    SnapshotData data;
    const auto ready_identity = SemanticSymbolIdentity{
        .stable_id = "symbol|ready",
        .name = "ready",
        .kind = "Variable",
        .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                     .range = rangeAt(2, 8, 13)}};
    data.symbols_by_id.emplace("symbol|ready",
                               SnapshotIndexedSymbol{.identity = ready_identity,
                                                     .type_display = "logic"});
    data.references.push_back(SnapshotIndexedReference{
        .stable_id = "symbol|ready",
        .name = "ready",
        .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                     .range = rangeAt(4, 9, 14)},
        .is_declaration = false});
    data.references.push_back(SnapshotIndexedReference{
        .stable_id = "symbol|ready",
        .name = "ready",
        .location = ready_identity.location,
        .is_declaration = true});

    const auto view = buildAstIndexView(&data, 42);

    CHECK(view.generation == 42);
    CHECK(view.snapshot_available);
    REQUIRE(view.symbols.size() == 1);
    CHECK(view.symbols.front().stable_id == "symbol|ready");
    REQUIRE(view.navigation_symbols_by_id.contains("symbol|ready"));
    CHECK(view.navigation_symbols_by_id.at("symbol|ready").name == "ready");
    REQUIRE(view.diagnostic_symbols_by_id.contains("symbol|ready"));
    CHECK(view.diagnostic_symbols_by_id.at("symbol|ready").type_display == "logic");
    REQUIRE(view.design_graph_symbols_by_id.contains("symbol|ready"));
    REQUIRE(view.navigation_references.size() == 2);
    CHECK(std::any_of(view.navigation_references.begin(),
                      view.navigation_references.end(),
                      [](const NavigationReference& reference) {
                          return reference.is_declaration;
                      }));
    REQUIRE(view.design_graph_symbol_ranges_by_uri.contains("file:///workspace/top.sv"));
    CHECK(view.design_graph_symbol_ranges_by_uri.at("file:///workspace/top.sv").size() == 2);
}

} // namespace
} // namespace pristine::analysis::semantic
