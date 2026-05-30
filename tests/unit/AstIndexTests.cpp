#include "../../src/analysis/semantic/AstIndex.h"
#include "../../src/analysis/semantic/SnapshotBuilder.h"

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

TEST_CASE("AstIndex builds provider-facing graph, diagnostic, and signature views",
          "[analysis][semantic][ast-index][provider-view]") {
    SnapshotData data;
    data.modules_by_name.emplace("child",
                                 ModuleDefinition{.name = "child",
                                                  .kind = "module",
                                                  .range = rangeAt(0, 0, 48),
                                                  .selection_range = rangeAt(0, 7, 12),
                                                  .port_details = {SchematicPort{.name = "clk",
                                                                                 .direction = "input",
                                                                                 .width_text = "logic",
                                                                                 .range = rangeAt(0, 13, 28),
                                                                                 .selection_range = rangeAt(0, 25, 28)}}});
    data.module_uris_by_name.emplace("child", "file:///workspace/child.sv");
    data.module_entries.push_back(SnapshotModuleEntry{.uri = "file:///workspace/child.sv",
                                                      .definition = data.modules_by_name.at("child")});
    data.assignments_by_uri["file:///workspace/top.sv"] = {
        ContinuousAssignment{.left_expression = "ready",
                             .right_expression = "clk",
                             .range = rangeAt(3, 2, 20),
                             .left_range = rangeAt(3, 9, 14),
                             .right_range = rangeAt(3, 17, 20)}};
    data.identifiers_by_uri["file:///workspace/top.sv"] = {Identifier{.name = "ready",
                                                                       .range = rangeAt(3, 9, 14)}};
    data.package_imports_by_uri["file:///workspace/top.sv"] = {
        PackageImport{.package_name = "defs",
                      .package_range = rangeAt(1, 9, 13),
                      .range = rangeAt(1, 2, 16)}};
    data.metadata_by_uri["file:///workspace/top.sv"] = {
        SemanticSymbolMetadata{.name = "ready",
                               .selection_range = rangeAt(2, 8, 13),
                               .type_display_name = "logic"}};
    data.macros_by_uri["file:///workspace/top.sv"] = {
        MacroDefinition{.name = "READY",
                        .body = "1",
                        .range = rangeAt(0, 0, 15),
                        .selection_range = rangeAt(0, 8, 13)}};
    data.module_instances_by_uri["file:///workspace/top.sv"] = {
        SnapshotModuleInstance{.module_name = "child",
                               .instance_name = "u_child",
                               .uri = "file:///workspace/top.sv",
                               .range = rangeAt(4, 2, 26),
                               .selection_range = rangeAt(4, 8, 15),
                               .module_selection_range = rangeAt(4, 2, 7)}};

    const auto view = buildAstIndexView(&data, 77);

    CHECK(view.modules_by_name.contains("child"));
    CHECK(view.module_uris_by_name.at("child") == "file:///workspace/child.sv");
    REQUIRE(view.design_graph_module_entries.size() == 1);
    CHECK(view.assignments_by_uri.at("file:///workspace/top.sv").size() == 1);
    CHECK(view.identifiers_by_uri.at("file:///workspace/top.sv").size() == 1);
    CHECK(view.package_imports_by_uri.at("file:///workspace/top.sv").front().package_name == "defs");
    CHECK(view.metadata_by_uri.at("file:///workspace/top.sv").front().name == "ready");
    CHECK(view.macros_by_uri.at("file:///workspace/top.sv").front().name == "READY");
    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    CHECK(view.signature_module_instances_by_uri.at("file:///workspace/top.sv").front().module_name == "child");
}

TEST_CASE("AstIndex builds AST symbol, reference, and module instance indexes",
          "[analysis][semantic][ast-index][build]") {
    SnapshotBuildInput input{.generation = 21,
                             .documents = {{"file:///workspace/child.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                                                   .text = "module child; endmodule\n",
                                                                   .version = 1}},
                                           {"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  logic ready;\n"
                                                        "  assign ready = ready;\n"
                                                        "  child u_child();\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto& data = *output.data;
    const auto ready_id = symbolIdAtLocation(data, "file:///workspace/top.sv", 1, 9);
    REQUIRE(ready_id.has_value());
    bool ready_truncated = false;
    const auto ready_locations = locationsForSymbol(data, *ready_id, true, 100, ready_truncated);
    REQUIRE_FALSE(ready_truncated);
    CHECK(ready_locations.size() == 3);

    const auto child_id = findDefinitionSymbolId(data, "child");
    REQUIRE(child_id.has_value());
    REQUIRE(data.symbols_by_id.contains(*child_id));
    CHECK(data.symbols_by_id.at(*child_id).identity.location.uri == "file:///workspace/child.sv");

    const auto instance = moduleInstanceAt(data, "file:///workspace/top.sv", 3, 4);
    REQUIRE(instance.has_value());
    CHECK(instance->module_name == "child");
    CHECK(instance->instance_name == "u_child");
    CHECK(instance->target_stable_id == *child_id);

    bool truncated = false;
    const auto implementations = moduleImplementationLocations(data, "child", 100, truncated);
    REQUIRE_FALSE(truncated);
    REQUIRE(implementations.size() == 1);
    CHECK(implementations.front().uri == "file:///workspace/top.sv");
}

TEST_CASE("AstIndex derives module signatures and schematic views from slang AST",
          "[analysis][semantic][ast-index][signature][schematic]") {
    SnapshotBuildInput input{
        .generation = 33,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                              .text = "module child(input logic clk, output logic [3:0] data);\n"
                                                      "endmodule\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  logic [3:0] data;\n"
                                                      "  child u_child(.clk(clk), .data(data));\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child = view.module_signatures_by_name.at("child");
    CHECK(child.definition.name == "child");
    REQUIRE(child.definition.port_details.size() == 2);
    CHECK(child.definition.port_details[0].name == "clk");
    CHECK(child.definition.port_details[0].direction == "input");
    CHECK(child.definition.port_details[1].name == "data");
    CHECK(child.definition.port_details[1].direction == "output");
    CHECK(child.definition.port_details[1].width_text.find("logic") != std::string::npos);

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top_schematic = view.module_signatures_by_name.at("top").schematic;
    REQUIRE(top_schematic.cells.size() == 1);
    CHECK(top_schematic.cells.front().name == "u_child");
    CHECK(top_schematic.cells.front().type == "child");
    REQUIRE(top_schematic.cells.front().connections.size() == 2);
    CHECK(top_schematic.cells.front().connections[0].port_name == "clk");
    CHECK(top_schematic.cells.front().connections[1].port_name == "data");
}

TEST_CASE("AstIndex derives primitive and assignment schematic cells from slang AST",
          "[analysis][semantic][ast-index][schematic][no-fallback]") {
    SnapshotBuildInput input{.generation = 34,
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top(input logic a, input logic b, input logic sel, output logic y);\n"
                                                        "  logic n1;\n"
                                                        "  and u_and(n1, a, b);\n"
                                                        "  assign y = sel ? n1 : (a | b);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_and" && cell.kind == "and";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "or";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "mux";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "buf";
    }));
}

} // namespace
} // namespace pristine::analysis::semantic
