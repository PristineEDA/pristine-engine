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

TEST_CASE("AstIndex prefers typed same-range symbol references deterministically",
          "[analysis][semantic][ast-index][lookup][types]") {
    SnapshotData data;
    const auto location = SemanticLocation{.uri = "file:///workspace/typed.sv",
                                           .range = rangeAt(1, 26, 30)};
    const auto untyped_identity = SemanticSymbolIdentity{.stable_id = "symbol|data|port",
                                                         .name = "data",
                                                         .kind = "Port",
                                                         .location = location};
    const auto typed_identity = SemanticSymbolIdentity{.stable_id = "symbol|data|internal",
                                                       .name = "data",
                                                       .kind = "Port",
                                                       .location = location};

    data.symbols_by_id.emplace(untyped_identity.stable_id,
                               SnapshotIndexedSymbol{.identity = untyped_identity,
                                                     .symbol = nullptr,
                                                     .type_display = {}});
    data.symbols_by_id.emplace(typed_identity.stable_id,
                               SnapshotIndexedSymbol{.identity = typed_identity,
                                                     .symbol = nullptr,
                                                     .type_display = "logic [WIDTH-1:0]"});
    data.references.push_back(SnapshotIndexedReference{.stable_id = untyped_identity.stable_id,
                                                       .name = "data",
                                                       .location = location,
                                                       .is_declaration = true});
    data.references.push_back(SnapshotIndexedReference{.stable_id = typed_identity.stable_id,
                                                       .name = "data",
                                                       .location = location,
                                                       .is_declaration = true});

    const auto id = symbolIdAtLocation(data, "file:///workspace/typed.sv", 1, 28);

    REQUIRE(id.has_value());
    CHECK(*id == typed_identity.stable_id);
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
    data.assignment_edges_by_uri["file:///workspace/top.sv"] = {
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|ready",
                               .to_symbol_id = "symbol|clk",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(3, 2, 20)},
                               .expression_location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(3, 17, 20)},
                               .expression = "clk"}};
    data.package_imports_by_uri["file:///workspace/top.sv"] = {
        PackageImport{.package_name = "defs",
                      .package_range = rangeAt(1, 9, 13),
                      .range = rangeAt(1, 2, 16)}};
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
    CHECK(view.assignment_edges_by_uri.at("file:///workspace/top.sv").size() == 1);
    CHECK(view.package_imports_by_uri.at("file:///workspace/top.sv").front().package_name == "defs");
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

TEST_CASE("AstIndex derives module instances from slang AST rather than syntax model",
          "[analysis][semantic][ast-index][module-instance][no-fallback]") {
    SnapshotBuildInput input{
        .generation = 36,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                              .text = "module child(input logic clk, output logic ready);\n"
                                                      "endmodule\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  logic ready;\n"
                                                      "  child #(.WIDTH(4)) u_child(\n"
                                                      "    .clk(clk),\n"
                                                      "    .ready(ready)\n"
                                                      "  );\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto instances_it = view.module_instances_by_uri.find("file:///workspace/top.sv");
    REQUIRE(instances_it != view.module_instances_by_uri.end());
    const auto instance_it = std::find_if(instances_it->second.begin(),
                                          instances_it->second.end(),
                                          [](const SnapshotModuleInstance& candidate) {
                                              return candidate.module_name == "child" &&
                                                     candidate.instance_name == "u_child" &&
                                                     !candidate.target_stable_id.empty();
                                          });
    REQUIRE(instance_it != instances_it->second.end());
    const auto& instance = *instance_it;
    CHECK(instance.module_name == "child");
    CHECK(instance.instance_name == "u_child");
    CHECK_FALSE(instance.target_stable_id.empty());
    CHECK(instance.range.start_line == 3);
    CHECK(instance.range.end_line == 6);
    CHECK(instance.module_selection_range.start_line == 3);
    CHECK(instance.module_selection_range.start_character == 2);
    CHECK(instance.selection_range.start_line == 3);
    CHECK(instance.selection_range.start_character > instance.module_selection_range.start_character);

    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    REQUIRE(view.signature_module_instances_by_uri.at("file:///workspace/top.sv").size() == 1);
    const auto& inlay_instance = view.signature_module_instances_by_uri.at("file:///workspace/top.sv").front();
    REQUIRE(inlay_instance.connections.size() == 2);
    CHECK(inlay_instance.connections[0].port_name == "clk");
    CHECK(inlay_instance.connections[1].port_name == "ready");
}

TEST_CASE("AstIndex attaches self-cycle and unresolved instance candidates to module definitions",
          "[analysis][semantic][ast-index][module-instance][hierarchy][no-fallback]") {
    SnapshotBuildInput input{.generation = 37,
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  top u_self();\n"
                                                        "  missing u_missing();\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.modules_by_name.contains("top"));
    const auto& top = view.modules_by_name.at("top");
    REQUIRE(top.instances.size() == 2);
    CHECK(top.instances[0].module_name == "top");
    CHECK(top.instances[0].instance_name == "u_self");
    CHECK(top.instances[1].module_name == "missing");
    CHECK(top.instances[1].instance_name == "u_missing");
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

    const auto edges_it = view.assignment_edges_by_uri.find("file:///workspace/top.sv");
    REQUIRE(edges_it != view.assignment_edges_by_uri.end());
    const auto edgeTargetNames = [&](std::string_view expected) {
        return std::any_of(edges_it->second.begin(),
                           edges_it->second.end(),
                           [&](const SnapshotAssignmentEdge& edge) {
                               const auto from_it = view.design_graph_symbols_by_id.find(edge.from_symbol_id);
                               const auto to_it = view.design_graph_symbols_by_id.find(edge.to_symbol_id);
                               return from_it != view.design_graph_symbols_by_id.end() &&
                                      to_it != view.design_graph_symbols_by_id.end() &&
                                      from_it->second.identity.name == "y" &&
                                      to_it->second.identity.name == expected &&
                                      edge.expression == "sel ? n1 : (a | b)";
                           });
    };
    CHECK(edgeTargetNames("sel"));
    CHECK(edgeTargetNames("n1"));
    CHECK(edgeTargetNames("a"));
    CHECK(edgeTargetNames("b"));
}

TEST_CASE("AstIndex derives declared type references from slang AST",
          "[analysis][semantic][ast-index][type-definition][no-fallback]") {
    SnapshotBuildInput input{.generation = 35,
                             .documents = {{"file:///workspace/types.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/types.sv",
                                                .text = "module top;\n"
                                                        "  typedef logic [3:0] nibble_t;\n"
                                                        "  nibble_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    auto locations = typeDefinitionLocationsAt(view, "file:///workspace/types.sv", 2, 3);

    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == "file:///workspace/types.sv");
    CHECK(locations.front().range.start_line == 1);
    CHECK(locations.front().range.start_character == 22);
    CHECK(locations.front().range.end_character == 30);
}

TEST_CASE("AstIndex resolves typedef alias chain type references by indexed type facts",
          "[analysis][semantic][ast-index][type-definition][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 36,
                             .documents = {{"file:///workspace/alias.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/alias.sv",
                                                .text = "package defs;\n"
                                                        "  typedef logic [7:0] base_t;\n"
                                                        "  typedef base_t alias_t;\n"
                                                        "endpackage\n"
                                                        "module top;\n"
                                                        "  import defs::*;\n"
                                                        "  alias_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto alias_locations = typeDefinitionLocationsAt(view, "file:///workspace/alias.sv", 6, 3);
    REQUIRE(alias_locations.size() == 1);
    CHECK(alias_locations.front().uri == "file:///workspace/alias.sv");
    CHECK(alias_locations.front().range.start_line == 2);
    CHECK(alias_locations.front().range.start_character == 17);
    CHECK(alias_locations.front().range.end_character == 24);

    const auto base_locations = typeDefinitionLocationsAt(view, "file:///workspace/alias.sv", 2, 10);
    REQUIRE(base_locations.size() == 1);
    CHECK(base_locations.front().range.start_line == 1);
    CHECK(base_locations.front().range.start_character == 22);
    CHECK(base_locations.front().range.end_character == 28);
}

TEST_CASE("AstIndex alias lookup does not resolve unimported package members by name",
          "[analysis][semantic][ast-index][type-definition][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 37,
                             .documents = {{"file:///workspace/defs.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/defs.sv",
                                                .text = "package defs;\n"
                                                        "  typedef logic exported_t;\n"
                                                        "endpackage\n",
                                                .version = 1}},
                                           {"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  exported_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto locations = typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 3);

    CHECK(locations.empty());
}

TEST_CASE("AstIndex resolves package-qualified alias chains without leaking same-file packages",
          "[analysis][semantic][ast-index][type-definition][package][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 38,
                             .documents = {{"file:///workspace/packages.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/packages.sv",
                                                .text = "package other;\n"
                                                        "  typedef logic [3:0] alias_t;\n"
                                                        "endpackage\n"
                                                        "package defs;\n"
                                                        "  typedef logic [7:0] base_t;\n"
                                                        "  typedef base_t alias_t;\n"
                                                        "endpackage\n"
                                                        "module top;\n"
                                                        "  defs::alias_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto package_locations = typeDefinitionLocationsAt(view, "file:///workspace/packages.sv", 8, 10);

    REQUIRE(package_locations.size() == 1);
    CHECK(package_locations.front().uri == "file:///workspace/packages.sv");
    CHECK(package_locations.front().range.start_line == 5);
    CHECK(package_locations.front().range.start_character == 17);
    CHECK(package_locations.front().range.end_character == 24);
}

TEST_CASE("AstIndex resolves interface modport type references from interface ports",
          "[analysis][semantic][ast-index][type-definition][interface][modport][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/if-port.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/if-port.sv",
                                                .text = "interface bus_if;\n"
                                                        "  logic ready;\n"
                                                        "  modport master(input ready);\n"
                                                        "endinterface\n"
                                                        "module top(bus_if.master bus);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto interface_locations = typeDefinitionLocationsAt(view, "file:///workspace/if-port.sv", 4, 12);
    REQUIRE(interface_locations.size() == 1);
    CHECK(interface_locations.front().range.start_line == 0);
    CHECK(interface_locations.front().range.start_character == 10);
    CHECK(interface_locations.front().range.end_character == 16);

    const auto modport_locations = typeDefinitionLocationsAt(view, "file:///workspace/if-port.sv", 4, 18);
    REQUIRE(modport_locations.size() == 1);
    CHECK(modport_locations.front().range.start_line == 2);
    CHECK(modport_locations.front().range.start_character == 10);
    CHECK(modport_locations.front().range.end_character == 16);
}

TEST_CASE("AstIndex derives function and task signature calls from slang AST",
          "[analysis][semantic][ast-index][signature][function][task][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/calls.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/calls.sv",
                                                .text = "module top;\n"
                                                        "  function automatic int add(input int lhs, input int rhs);\n"
                                                        "    return lhs + rhs;\n"
                                                        "  endfunction\n"
                                                        "  task automatic emit(input logic ready);\n"
                                                        "  endtask\n"
                                                        "  initial begin\n"
                                                        "    int value = add(1, 2);\n"
                                                        "    emit(1'b1);\n"
                                                        "  end\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.signature_calls_by_uri.contains("file:///workspace/calls.sv"));
    const auto& calls = view.signature_calls_by_uri.at("file:///workspace/calls.sv");

    const auto add_call = std::find_if(calls.begin(), calls.end(), [](const SignatureInlayCall& call) {
        return call.name == "add";
    });
    REQUIRE(add_call != calls.end());
    CHECK(add_call->kind == "function");
    CHECK(add_call->return_type == "int");
    CHECK(add_call->range.start_line == 7);
    CHECK(add_call->range.start_character == 16);
    CHECK(add_call->selection_range.start_line == 7);
    CHECK(add_call->selection_range.start_character == 16);
    CHECK(add_call->selection_range.end_character == 19);
    REQUIRE(add_call->parameters.size() == 2);
    CHECK(add_call->parameters[0] == "input int lhs");
    CHECK(add_call->parameters[1] == "input int rhs");

    const auto emit_call = std::find_if(calls.begin(), calls.end(), [](const SignatureInlayCall& call) {
        return call.name == "emit";
    });
    REQUIRE(emit_call != calls.end());
    CHECK(emit_call->kind == "task");
    CHECK(emit_call->range.start_line == 8);
    CHECK(emit_call->range.start_character == 4);
    CHECK(emit_call->selection_range.start_line == 8);
    CHECK(emit_call->selection_range.start_character == 4);
    CHECK(emit_call->selection_range.end_character == 8);
    REQUIRE(emit_call->parameters.size() == 1);
    CHECK(emit_call->parameters[0] == "input logic ready");
}

TEST_CASE("AstIndex narrows package-qualified type references to the AST type token",
          "[analysis][semantic][ast-index][type-definition][package][no-fallback]") {
    SnapshotBuildInput input{
        .generation = 38,
        .documents = {{"file:///workspace/defs.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                              .text = "package defs;\n"
                                                      "  typedef logic [7:0] word_t;\n"
                                                      "endpackage\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  defs::word_t value;\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 3).empty());

    auto locations = typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 10);
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == "file:///workspace/defs.sv");
    CHECK(locations.front().range.start_line == 1);
    CHECK(locations.front().range.start_character == 22);
    CHECK(locations.front().range.end_character == 28);
}

} // namespace
} // namespace pristine::analysis::semantic
