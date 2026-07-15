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

TEST_CASE("AstIndex records modport-qualified interface port type display",
          "[analysis][semantic][ast-index][inlay][interface][modport]") {
    SnapshotBuildInput input{
        .generation = 37,
        .documents = {{"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
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

    const auto& data = *output.data;
    const auto port_it = std::find_if(data.symbols_by_id.begin(),
                                      data.symbols_by_id.end(),
                                      [](const auto& entry) {
                                          const auto& indexed = entry.second;
                                          return indexed.identity.kind == "InterfacePort" &&
                                                 indexed.identity.name == "bus";
                                      });
    REQUIRE(port_it != data.symbols_by_id.end());
    CHECK(port_it->second.type_display == "bus_if.master");
}

TEST_CASE("AstIndex derives parameter override inlay facts from AST module parameters",
          "[analysis][semantic][ast-index][inlay][parameter]") {
    SnapshotBuildInput input{
        .generation = 38,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/child.sv",
                           .text = "module child #(parameter int WIDTH = 8, parameter int DEPTH = 4) "
                                   "(input logic clk);\n"
                                   "endmodule\n",
                           .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  child #(.WIDTH(16), .DEPTH(2)) u_child(.clk(clk));\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_definition = view.module_signatures_by_name.at("child").definition;
    CHECK(std::any_of(child_definition.parameter_details.begin(),
                      child_definition.parameter_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "WIDTH" && port.direction == "parameter" &&
                                 port.width_text == "int";
                      }));
    CHECK(std::any_of(child_definition.parameter_details.begin(),
                      child_definition.parameter_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "DEPTH" && port.direction == "parameter" &&
                                 port.width_text == "int";
                      }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top_cells = view.module_signatures_by_name.at("top").schematic.cells;
    REQUIRE(top_cells.size() == 1);
    REQUIRE(top_cells.front().connections.size() == 1);
    CHECK(top_cells.front().connections.front().port_name == "clk");

    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    const auto& inlay_instances = view.signature_module_instances_by_uri.at("file:///workspace/top.sv");
    REQUIRE(inlay_instances.size() == 1);
    const auto& inlay_connections = inlay_instances.front().connections;
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "WIDTH" && connection.signal == "16" &&
                                 connection.range.start_line == 2;
                      }));
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "DEPTH" && connection.signal == "2" &&
                                 connection.range.start_line == 2;
                      }));
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "clk" && connection.signal == "clk";
                      }));
}

TEST_CASE("AstIndex exposes parameterized cross-module cone inputs to design graph",
          "[analysis][semantic][ast-index][cone][parameterized][cross-module]") {
    SnapshotBuildInput input{
        .generation = 41,
        .documents = {{"file:///workspace/cone.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/cone.sv",
                           .text =
                               "module child #(parameter int WIDTH = 8) "
                               "(input logic [WIDTH-1:0] in, output logic [WIDTH-1:0] out);\n"
                               "  assign out = in;\n"
                               "endmodule\n"
                               "module top;\n"
                               "  logic [3:0] a;\n"
                               "  logic [3:0] y;\n"
                               "  child #(.WIDTH(4)) u_child(.in(a), .out(y));\n"
                               "endmodule\n",
                           .version = 1,
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_ports = view.module_signatures_by_name.at("child").definition.port_details;
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "in" && port.direction == "input";
    }));
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "out" && port.direction == "output";
    }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    REQUIRE(cells.size() == 1);
    CHECK(std::any_of(cells.front().connections.begin(),
                      cells.front().connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "a";
                      }));
    CHECK(std::any_of(cells.front().connections.begin(),
                      cells.front().connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "y";
                      }));

    REQUIRE(view.assignment_edges_by_uri.contains("file:///workspace/cone.sv"));
    CHECK(std::any_of(view.assignment_edges_by_uri.at("file:///workspace/cone.sv").begin(),
                      view.assignment_edges_by_uri.at("file:///workspace/cone.sv").end(),
                      [&](const SnapshotAssignmentEdge& edge) {
                          const auto from = view.design_graph_symbols_by_id.at(edge.from_symbol_id)
                                                .identity.name;
                          const auto to = view.design_graph_symbols_by_id.at(edge.to_symbol_id)
                                              .identity.name;
                          return from == "out" && to == "in";
                      }));
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

TEST_CASE("AstIndex promotes generated module instances into schematic cells",
          "[analysis][semantic][ast-index][schematic][generate][no-fallback]") {
    SnapshotBuildInput input{.generation = 38,
                             .documents = {{"file:///workspace/generated.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated.sv",
                                                .text = "module leaf(input logic clk);\n"
                                                        "endmodule\n"
                                                        "module top;\n"
                                                        "  logic clk;\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      leaf u_leaf(.clk(clk));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("leaf"));
    CHECK(std::any_of(view.module_signatures_by_name.at("leaf").definition.port_details.begin(),
                      view.module_signatures_by_name.at("leaf").definition.port_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "clk" && port.direction == "input";
                      }));
    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top = view.module_signatures_by_name.at("top");
    CHECK(std::any_of(top.definition.instances.begin(),
                      top.definition.instances.end(),
                      [](const ModuleInstantiation& instance) {
                          return instance.module_name == "leaf" && instance.instance_name == "u_leaf";
                      }));
    CHECK(std::any_of(top.schematic.cells.begin(),
                      top.schematic.cells.end(),
                      [](const SchematicCell& cell) {
                          return cell.name == "u_leaf" && cell.type == "leaf" && cell.kind == "module";
                      }));
    CHECK(std::any_of(top.schematic.cells.begin(),
                      top.schematic.cells.end(),
                      [](const SchematicCell& cell) {
                          return cell.name == "u_leaf" &&
                                 std::any_of(cell.connections.begin(),
                                             cell.connections.end(),
                                             [](const SchematicConnection& connection) {
                                                 return connection.port_name == "clk" &&
                                                        connection.signal == "clk";
                                             });
                      }));
}

TEST_CASE("AstIndex preserves generated schematic port width and connection facts",
          "[analysis][semantic][ast-index][schematic][generate][port-net][no-fallback]") {
    SnapshotBuildInput input{.generation = 40,
                             .documents = {{"file:///workspace/generated-port-net.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated-port-net.sv",
                                                .text = "module child(input logic [3:0] in,\n"
                                                        "             output logic [3:0] out);\n"
                                                        "endmodule\n"
                                                        "module top(input logic [3:0] data_i,\n"
                                                        "           output logic [3:0] data_o);\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      child u_child(.in(data_i), .out(data_o));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_ports = view.module_signatures_by_name.at("child").definition.port_details;
    std::string child_port_dump;
    for (const auto& port : child_ports) {
        child_port_dump += port.name + ":" + port.direction + ":" + port.width_text + ";";
    }
    INFO(child_port_dump);
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "in" && port.direction == "input" && port.width_text == "logic [3:0]";
    }));
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "out" && port.direction == "output" && port.width_text == "logic [3:0]";
    }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top = view.module_signatures_by_name.at("top");
    CHECK(std::any_of(top.schematic.ports.begin(), top.schematic.ports.end(), [](const SchematicPort& port) {
        return port.name == "data_i" && port.direction == "input" && port.width_text == "logic [3:0]";
    }));
    CHECK(std::any_of(top.schematic.ports.begin(), top.schematic.ports.end(), [](const SchematicPort& port) {
        return port.name == "data_o" && port.direction == "output" && port.width_text == "logic [3:0]";
    }));

    const auto cell_it = std::find_if(top.schematic.cells.begin(),
                                      top.schematic.cells.end(),
                                      [](const SchematicCell& cell) {
                                          return cell.name == "u_child" && cell.type == "child";
                                      });
    REQUIRE(cell_it != top.schematic.cells.end());
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "data_i";
                      }));
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "data_o";
                      }));
}

TEST_CASE("AstIndex exposes generated-only instance connection facts for backward cone",
          "[analysis][semantic][ast-index][cone][generate][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/generated-cone.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated-cone.sv",
                                                .text = "module child(input logic in, output logic out);\n"
                                                        "  assign out = in;\n"
                                                        "endmodule\n"
                                                        "module top;\n"
                                                        "  logic a;\n"
                                                        "  logic y;\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      child u_child(.in(a), .out(y));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    const auto cell_it = std::find_if(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_child" && cell.type == "child";
    });
    REQUIRE(cell_it != cells.end());
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "a";
                      }));
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "y";
                      }));

    const auto edge_it = view.assignment_edges_by_uri.find("file:///workspace/generated-cone.sv");
    REQUIRE(edge_it != view.assignment_edges_by_uri.end());
    CHECK(std::any_of(edge_it->second.begin(), edge_it->second.end(), [&](const SnapshotAssignmentEdge& edge) {
        const auto from_it = view.design_graph_symbols_by_id.find(edge.from_symbol_id);
        const auto to_it = view.design_graph_symbols_by_id.find(edge.to_symbol_id);
        return from_it != view.design_graph_symbols_by_id.end() &&
               to_it != view.design_graph_symbols_by_id.end() &&
               from_it->second.identity.name == "out" && to_it->second.identity.name == "in";
    }));
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
    REQUIRE(view.callable_invocations_by_uri.contains("file:///workspace/calls.sv"));
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/calls.sv");

    const auto add_call = std::find_if(calls.begin(), calls.end(), [](const CallableInvocationFact& call) {
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

    const auto emit_call = std::find_if(calls.begin(), calls.end(), [](const CallableInvocationFact& call) {
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

TEST_CASE("AstIndex scope visibility exposes parent context and URI-local inlay facts",
          "[analysis][semantic][ast-index][scope-visibility][inlay]") {
    SnapshotBuildInput input{.generation = 40,
                             .documents = {{"file:///workspace/scopes.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/scopes.sv",
                                                .text = "module top;\n"
                                                        "  logic ready;\n"
                                                        "  initial begin : outer_scope\n"
                                                        "    logic local_ready;\n"
                                                        "    begin : inner_scope\n"
                                                        "      local_ready = ready;\n"
                                                        "    end\n"
                                                        "  end\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto scopes_it = view.scope_visibility_by_uri.find("file:///workspace/scopes.sv");
    REQUIRE(scopes_it != view.scope_visibility_by_uri.end());
    CHECK(std::all_of(scopes_it->second.begin(), scopes_it->second.end(), [](const auto& scope) {
        return !scope.context_kind.empty();
    }));
    CHECK(std::any_of(scopes_it->second.begin(), scopes_it->second.end(), [](const auto& scope) {
        return !scope.parent_stable_id.empty();
    }));

    const auto inlay_it = view.inlay_symbols_by_uri.find("file:///workspace/scopes.sv");
    REQUIRE(inlay_it != view.inlay_symbols_by_uri.end());
    CHECK(std::all_of(inlay_it->second.begin(), inlay_it->second.end(), [](const auto& symbol) {
        return symbol.identity.location.uri == "file:///workspace/scopes.sv";
    }));
}

TEST_CASE("AstIndex workspace visibility indexes class declarations for prefix completion",
          "[analysis][semantic][ast-index][completion][prefix-index]") {
    SnapshotBuildInput input{.generation = 41,
                             .documents = {{"file:///workspace/class-prefix.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/class-prefix.sv",
                                                .text = "class packet;\nendclass\nmodule top;\n  pac\nendmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::any_of(view.workspace_visibility.begin(),
                      view.workspace_visibility.end(),
                      [](const auto& candidate) {
                          return candidate.identity.name == "packet" &&
                                 candidate.identity.kind == "ClassType";
                      }));
}
TEST_CASE("AstIndex document visibility retains AST explicit imports during recovery",
          "[analysis][semantic][ast-index][completion][import][recovery]") {
    SnapshotBuildInput input{.generation = 42,
                             .documents = {
                                 {"file:///workspace/defs.sv",
                                  SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                                         .text = "package defs; typedef logic token_t; endpackage\n",
                                                         .version = 1}},
                                 {"file:///workspace/user.sv",
                                  SemanticEngineDocument{.uri = "file:///workspace/user.sv",
                                                         .text = "module top;\n"
                                                                 "  import defs::token_t;\n"
                                                                 "  tok\n"
                                                                 "endmodule\n",
                                                         .version = 1,
                                                         .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto candidates = view.document_visibility_by_uri.find("file:///workspace/user.sv");
    REQUIRE(candidates != view.document_visibility_by_uri.end());
    CHECK(std::any_of(candidates->second.begin(), candidates->second.end(), [](const auto& candidate) {
        return candidate.identity.name == "token_t" &&
               candidate.origin == SnapshotVisibilityOrigin::ExplicitImport;
    }));
}
TEST_CASE("AstIndex derives array-of-struct member completion facts from declared types",
          "[analysis][semantic][ast-index][completion][member][array]") {
    SnapshotBuildInput input{
        .generation = 40,
        .documents = {{"file:///workspace/array-struct.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/array-struct.sv",
                                              .text = "typedef struct packed {\n"
                                                      "  logic status_valid;\n"
                                                      "  logic status_ready;\n"
                                                      "  logic payload;\n"
                                                      "} packet_t;\n"
                                                      "module top;\n"
                                                      "  packet_t lanes [2];\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/array-struct.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view name) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == "lanes" &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == "Field";
                           });
    };

    CHECK(has_member("status_valid"));
    CHECK(has_member("status_ready"));
    CHECK(has_member("payload"));
}

TEST_CASE("AstIndex derives class property and method member completion facts from declared types",
          "[analysis][semantic][ast-index][completion][member][class]") {
    SnapshotBuildInput input{
        .generation = 41,
        .documents = {{"file:///workspace/class-member.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/class-member.sv",
                                              .text = "class packet;\n"
                                                      "  int size_bytes;\n"
                                                      "  int size_words;\n"
                                                      "  function int size_sum();\n"
                                                      "    return size_bytes + size_words;\n"
                                                      "  endfunction\n"
                                                      "endclass\n"
                                                      "module top;\n"
                                                      "  packet pkt;\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/class-member.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view name, std::string_view kind) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == "pkt" &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == kind;
                           });
    };

    CHECK(has_member("size_bytes", "Field"));
    CHECK(has_member("size_words", "Field"));
    CHECK(has_member("size_sum", "Subroutine"));
}

TEST_CASE("AstIndex derives interface instance and modport member completion facts",
          "[analysis][semantic][ast-index][completion][member][interface][modport]") {
    SnapshotBuildInput input{
        .generation = 42,
        .documents = {{"file:///workspace/interface-member.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/interface-member.sv",
                                              .text = "interface bus_if;\n"
                                                      "  logic status_valid;\n"
                                                      "  logic status_ready;\n"
                                                      "  logic payload;\n"
                                                      "  modport master(input status_ready, output status_valid);\n"
                                                      "endinterface\n"
                                                      "module consumer(bus_if.master master_bus);\n"
                                                      "endmodule\n"
                                                      "module top;\n"
                                                      "  bus_if bus();\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/interface-member.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view qualifier,
                                std::string_view name,
                                std::string_view kind) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == qualifier &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == kind;
                           });
    };

    CHECK(has_member("bus", "status_valid", "Variable"));
    CHECK(has_member("bus", "status_ready", "Variable"));
    CHECK(has_member("bus", "payload", "Variable"));
    CHECK(has_member("master_bus", "status_valid", "Field"));
    CHECK(has_member("master_bus", "status_ready", "Field"));
    CHECK_FALSE(has_member("master_bus", "payload", "Net"));
    CHECK_FALSE(has_member("master_bus", "payload", "Field"));
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

TEST_CASE("AstIndex builds package re-export and wildcard scope visibility facts",
          "[analysis][semantic][ast-index][visibility][package-export]") {
    SnapshotBuildInput input{
        .generation = 61,
        .documents = {
            {"file:///workspace/defs.sv",
             SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                    .text = "package defs; parameter int WIDTH = 8; typedef logic [7:0] word_t; endpackage\n"}},
            {"file:///workspace/api.sv",
             SemanticEngineDocument{.uri = "file:///workspace/api.sv",
                                    .text = "package api; import defs::*; export defs::*; endpackage\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "module top; import api::*; localparam int W = WIDTH; endmodule\n",
                                    .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto api = view.package_visibility_by_name.find("api");
    REQUIRE(api != view.package_visibility_by_name.end());
    CHECK(std::find(api->second.exported_packages.begin(),
                    api->second.exported_packages.end(),
                    "defs") != api->second.exported_packages.end());
    CHECK(std::any_of(api->second.candidates.begin(),
                      api->second.candidates.end(),
                      [](const auto& candidate) { return candidate.identity.name == "WIDTH"; }));
    CHECK(std::any_of(api->second.candidates.begin(),
                      api->second.candidates.end(),
                      [](const auto& candidate) { return candidate.identity.name == "word_t"; }));
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/defs.sv",
              AffectedDependencyEdgeKind::SemanticExport) ==
          std::vector<std::string>{"file:///workspace/api.sv"});
}

TEST_CASE("AstIndex indexes nested typed member qualifiers without global candidates",
          "[analysis][semantic][ast-index][visibility][member][nested]") {
    SnapshotBuildInput input{
        .generation = 62,
        .documents = {{"file:///workspace/nested.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/nested.sv",
                           .text = "typedef struct packed { logic ready; logic error; } master_t;\n"
                                   "typedef struct packed { master_t master; } bus_t;\n"
                                   "module top; bus_t bus; initial bus.master.ready = 1'b1; endmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto members = view.member_completions_by_uri.find("file:///workspace/nested.sv");
    REQUIRE(members != view.member_completions_by_uri.end());
    CHECK(std::any_of(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "bus.master" && member.identity.name == "ready";
    }));
    CHECK(std::any_of(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "bus.master" && member.identity.name == "error";
    }));
}

TEST_CASE("AstIndex indexes ordinary module instance members for hierarchical completion",
          "[analysis][semantic][ast-index][visibility][member][instance]") {
    SnapshotBuildInput input{
        .generation = 63,
        .documents = {{"file:///workspace/instance.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/instance.sv",
                           .text = "module child(input logic data_i, output logic data_o); logic state; endmodule\n"
                                   "module top; child u_child(); endmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto members = view.member_completions_by_uri.find("file:///workspace/instance.sv");
    REQUIRE(members != view.member_completions_by_uri.end());
    for (const auto name : {"data_i", "data_o", "state"}) {
        CAPTURE(name);
        CHECK(std::any_of(members->second.begin(), members->second.end(), [&](const auto& member) {
            return member.qualifier == "u_child" && member.identity.name == name;
        }));
    }
    const auto data_i = std::find_if(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "u_child" && member.identity.name == "data_i";
    });
    REQUIRE(data_i != members->second.end());
    const auto resolve = view.completion_resolve_by_id.find(data_i->identity.stable_id);
    REQUIRE(resolve != view.completion_resolve_by_id.end());
    CHECK(resolve->second.kind == SnapshotCompletionResolveKind::Member);
    CHECK(resolve->second.type_display == "logic");
}

TEST_CASE("AstIndex callable invocation facts preserve AST argument ranges",
          "[analysis][semantic][ast-index][callable][arguments]") {
    SnapshotBuildInput input{.generation = 70,
                             .documents = {{"file:///workspace/args.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/args.sv",
                                                .text = "module top; function int add(input int lhs, input int rhs); return lhs + rhs; endfunction int value = add(10, 20); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/args.sv");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.front().argument_ranges.size() == 2);
    CHECK(calls.front().argument_ranges[0].start_character <
          calls.front().argument_ranges[1].start_character);
    CHECK(calls.front().resolved);
    CHECK_FALSE(calls.front().target_stable_id.empty());
}

TEST_CASE("AstIndex macro invocation resolves the preceding definition",
          "[analysis][semantic][ast-index][macro][definition]") {
    SnapshotBuildInput input{.generation = 71,
                             .documents = {{"file:///workspace/macro.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/macro.sv",
                                                .text = "`define ADD(a, b) ((a) + (b))\nmodule top; int value = `ADD(1, 2); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/macro.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().definition.parameters == std::vector<std::string>{"a", "b"});
    CHECK(macros.front().expansion_text == "((1) + (2))");
    const auto visible = view.visible_macros_by_uri.find("file:///workspace/macro.sv");
    REQUIRE(visible != view.visible_macros_by_uri.end());
    REQUIRE_FALSE(visible->second.empty());
    const auto resolve_id = macroCompletionResolveId(visible->second.front());
    CAPTURE(resolve_id, view.completion_resolve_by_id.size());
    CHECK(view.completion_resolve_by_id.contains(resolve_id));
}

TEST_CASE("AstIndex indexes disabled conditional regions and excludes inactive macro invocations",
          "[analysis][semantic][ast-index][macro][inactive-region][no-fallback]") {
    SnapshotBuildInput input{.generation = 711,
                             .documents = {{"file:///workspace/inactive.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/inactive.sv",
                                                .text = "`define VALUE 1\n"
                                                        "`ifdef DISABLED\n"
                                                        "  int disabled_value = `VALUE;\n"
                                                        "`else\n"
                                                        "  int active_value = `VALUE;\n"
                                                        "`endif\n"
                                                        "module top; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto regions = inactiveRegionsForUri(view, "file:///workspace/inactive.sv");
    REQUIRE_FALSE(regions.empty());
    CHECK(regions.front().start_line == 2);
    CHECK(view.inactive_region_count >= 1);
    CHECK(view.inactive_region_build_micros >= 0);
    const auto macros = view.macro_invocations_by_uri.find("file:///workspace/inactive.sv");
    REQUIRE(macros != view.macro_invocations_by_uri.end());
    REQUIRE(macros->second.size() == 1);
    CHECK(macros->second.front().resolved);
    CHECK(macros->second.front().range.start_line == 4);
}

TEST_CASE("AstIndex macro invocation does not see a later definition",
          "[analysis][semantic][ast-index][macro][ordering][no-fallback]") {
    SnapshotBuildInput input{.generation = 72,
                             .documents = {{"file:///workspace/later.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/later.sv",
                                                .text = "module top; int value = `LATER; endmodule\n`define LATER 1\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/later.sv");
    REQUIRE(macros.size() == 1);
    CHECK_FALSE(macros.front().resolved);
    CHECK(macros.front().definition_uri.empty());
}

TEST_CASE("AstIndex macro invocation selects the latest preceding redefine",
          "[analysis][semantic][ast-index][macro][redefine]") {
    SnapshotBuildInput input{.generation = 73,
                             .documents = {{"file:///workspace/redefine.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/redefine.sv",
                                                .text = "`define VALUE 1\n`define VALUE 2\nmodule top; int value = `VALUE; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/redefine.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().expansion_text == "2");
}

TEST_CASE("AstIndex macro invocation rejects a definition after undef",
          "[analysis][semantic][ast-index][macro][undef][no-fallback]") {
    SnapshotBuildInput input{.generation = 74,
                             .documents = {{"file:///workspace/undef.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/undef.sv",
                                                .text = "`define VALUE 1\n`undef VALUE\nmodule top; int value = `VALUE; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/undef.sv");
    REQUIRE(macros.size() == 1);
    CHECK_FALSE(macros.front().resolved);
    CHECK(output.data->macro_undefs_by_uri.at("file:///workspace/undef.sv").size() == 1);
}

TEST_CASE("AstIndex resolves included macros and records macro include invalidation",
          "[analysis][semantic][ast-index][macro][include][affected]") {
    SnapshotBuildInput input{
        .generation = 75,
        .documents = {
            {"file:///workspace/defs.svh",
             SemanticEngineDocument{.uri = "file:///workspace/defs.svh", .text = "`define FLAG 1\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "`include \"defs.svh\"\nmodule top; int value = `FLAG; endmodule\n",
                                    .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/top.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().definition_uri == "file:///workspace/defs.svh");
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/defs.svh", AffectedDependencyEdgeKind::MacroInclude) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("AstIndex records cross-file callable type invalidation edges",
          "[analysis][semantic][ast-index][callable][affected]") {
    SnapshotBuildInput input{
        .generation = 76,
        .documents = {
            {"file:///workspace/api.sv",
             SemanticEngineDocument{.uri = "file:///workspace/api.sv",
                                    .text = "package api; function int add(input int value); return value; endfunction endpackage\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "module top; import api::*; int value = add(1); endmodule\n",
                                    .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/api.sv", AffectedDependencyEdgeKind::CallableType) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("AstIndex orders nested callable invocations by source range deterministically",
          "[analysis][semantic][ast-index][callable][nested][deterministic]") {
    SnapshotBuildInput input{.generation = 77,
                             .documents = {{"file:///workspace/nested-calls.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/nested-calls.sv",
                                                .text = "module top; function int id(input int value); return value; endfunction int value = id(id(1)); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/nested-calls.sv");
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].range.start_character <= calls[1].range.start_character);
    CHECK(calls[0].target_stable_id == calls[1].target_stable_id);
}

TEST_CASE("AstIndex expands nested function-like macros during index construction",
          "[analysis][semantic][ast-index][macro][nested-expansion]") {
    SnapshotBuildInput input{.generation = 78,
                             .documents = {{"file:///workspace/nested-macro.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/nested-macro.sv",
                                                .text = "`define ADD(a, b) ((a) + (b))\n`define TWICE(x) `ADD(x, x)\nmodule top; int value = `TWICE(3); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/nested-macro.sv");
    const auto twice = std::find_if(macros.begin(), macros.end(), [](const auto& macro) {
        return macro.name == "TWICE";
    });
    REQUIRE(twice != macros.end());
    CHECK(twice->resolved);
    CHECK(twice->expansion_text == "((3) + (3))");
}

TEST_CASE("AstIndex bounds recursive macro expansion cycles",
          "[analysis][semantic][ast-index][macro][expansion-cycle]") {
    SnapshotBuildInput input{.generation = 79,
                             .documents = {{"file:///workspace/macro-cycle.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/macro-cycle.sv",
                                                .text = "`define A `B\n`define B `A\nmodule top; int value = `A; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/macro-cycle.sv");
    const auto invocation = std::find_if(macros.begin(), macros.end(), [](const auto& macro) {
        return macro.name == "A" && macro.range.start_line == 2;
    });
    REQUIRE(invocation != macros.end());
    CHECK(invocation->resolved);
    CHECK(invocation->expansion_text.size() <= 2);
}

TEST_CASE("AstIndex preserves empty named-port ranges for signature active parameter",
          "[analysis][semantic][ast-index][signature][module][named-port]") {
    SnapshotBuildInput input{.generation = 80,
                             .documents = {{"file:///workspace/empty-ports.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/empty-ports.sv",
                                                .text = "module child(input logic clk, output logic rst_n); endmodule\nmodule top; child child_i(.clk(), .rst_n()); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& instances =
        view.signature_module_instances_by_uri.at("file:///workspace/empty-ports.sv");
    REQUIRE(instances.size() == 1);
    REQUIRE(instances.front().connections.size() == 2);
    CHECK(instances.front().connections[0].port_name == "clk");
    CHECK(instances.front().connections[1].port_name == "rst_n");
    CHECK(instances.front().connections[0].range.start_character <
          instances.front().connections[1].range.start_character);
}

TEST_CASE("SnapshotBuilder limits macro visibility to document include closure",
          "[analysis][semantic][ast-index][visibility][macro]") {
    SnapshotBuildInput input{
        .generation = 64,
        .documents = {
            {"file:///workspace/included.svh",
             SemanticEngineDocument{.uri = "file:///workspace/included.svh",
                                    .text = "`define INCLUDED_FLAG 1\n"}},
            {"file:///workspace/unrelated.svh",
             SemanticEngineDocument{.uri = "file:///workspace/unrelated.svh",
                                    .text = "`define UNRELATED_FLAG 1\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "`include \"included.svh\"\nmodule top; endmodule\n",
                                    .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto macros = output.data->visible_macros_by_uri.find("file:///workspace/top.sv");
    REQUIRE(macros != output.data->visible_macros_by_uri.end());
    CHECK(std::any_of(macros->second.begin(), macros->second.end(), [](const auto& macro) {
        return macro.definition.name == "INCLUDED_FLAG";
    }));
    CHECK(std::none_of(macros->second.begin(), macros->second.end(), [](const auto& macro) {
        return macro.definition.name == "UNRELATED_FLAG";
    }));
}

TEST_CASE("AstIndex scope visibility metrics and ordering are deterministic",
          "[analysis][semantic][ast-index][visibility][deterministic]") {
    const auto build = [](std::uint64_t generation) {
        SnapshotBuildInput input{
            .generation = generation,
            .documents = {{"file:///workspace/order.sv",
                           SemanticEngineDocument{.uri = "file:///workspace/order.sv",
                                                  .text = "package defs; parameter int B = 2; parameter int A = 1; endpackage\n"
                                                          "module top; import defs::*; logic z; logic a; endmodule\n",
                                                  .is_open = true}}}};
        return SnapshotBuilder{}.build(std::move(input));
    };
    auto first = build(65);
    auto second = build(66);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    const auto first_view = buildAstIndexView(first.data.get(), first.snapshot.generation);
    const auto second_view = buildAstIndexView(second.data.get(), second.snapshot.generation);
    REQUIRE(first_view.package_visibility_by_name.contains("defs"));
    REQUIRE(second_view.package_visibility_by_name.contains("defs"));
    const auto names = [](const auto& candidates) {
        std::vector<std::string> result;
        for (const auto& candidate : candidates) {
            result.push_back(candidate.identity.name);
        }
        return result;
    };
    CHECK(names(first_view.package_visibility_by_name.at("defs").candidates) ==
          names(second_view.package_visibility_by_name.at("defs").candidates));
    CHECK(first_view.scope_visibility_count > 0);
    CHECK(first_view.package_visibility_count == 1);
    CHECK(first_view.scope_visibility_build_micros >= 0);
}

TEST_CASE("AstIndex indexes class method calls as callable visibility facts",
          "[analysis][semantic][ast-index][visibility][callable][class]") {
    SnapshotBuildInput input{
        .generation = 66,
        .documents = {{"file:///workspace/class-call.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/class-call.sv",
                           .text = "class packet;\n"
                                   "  function int sum(input int lhs, input int rhs); return lhs + rhs; endfunction\n"
                                   "endclass\nmodule top;\n  packet pkt;\n"
                                   "  initial begin\n"
                                   "    int value;\n"
                                   "    pkt = new;\n"
                                   "    value = pkt.sum(1, 2);\n"
                                   "  end\nendmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto calls = view.callable_invocations_by_uri.find("file:///workspace/class-call.sv");
    REQUIRE(calls != view.callable_invocations_by_uri.end());
    REQUIRE(calls->second.size() == 1);
    const auto& call = calls->second.front();
    CHECK(call.name == "sum");
    CHECK(call.parameters.size() == 2);
    CHECK(call.range.start_line == 8);
    CHECK(call.range.start_character == 12);
    CHECK(call.range.end_line == 8);
    CHECK(call.range.end_character == 25);
    CHECK(call.selection_range.start_character == 16);
    CHECK(call.selection_range.end_character == 19);
}

} // namespace
} // namespace pristine::analysis::semantic
