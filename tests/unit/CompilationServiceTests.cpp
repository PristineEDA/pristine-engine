#include "pristine/analysis/CompilationService.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace pristine::analysis {

TEST_CASE("CompilationService parses valid SystemVerilog text", "[analysis][parse]") {
    CompilationService service;

    const auto result = service.parse("module top; endmodule\n", "file:///workspace/top.sv");

    REQUIRE(result.syntax_tree != nullptr);
    CHECK(result.has_errors == false);
    CHECK(result.diagnostics.empty());
}

TEST_CASE("CompilationService surfaces parse diagnostics", "[analysis][parse]") {
    CompilationService service;

    const auto result = service.parse("module broken\n", "file:///workspace/broken.sv");

    REQUIRE(result.syntax_tree != nullptr);
    CHECK(result.has_errors == true);
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().message.empty() == false);
    CHECK(result.diagnostics.front().severity == 1);
}

TEST_CASE("CompilationService reports UTF-16 diagnostic columns", "[analysis][parse]") {
    CompilationService service;

    const auto result = service.parse("module top; string s = \"😀\"; ? endmodule\n",
                                      "file:///workspace/unicode-diagnostic.sv");

    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().range.start_line == 0);
    CHECK(result.diagnostics.front().range.start_character == 29);
}

TEST_CASE("CompilationService returns hover for declaration symbols", "[analysis][hover]") {
    CompilationService service;

    const auto hover = service.hover("module top;\n  logic ready;\nendmodule\n",
                                     "file:///workspace/hover.sv", 1, 8);

    REQUIRE(hover.has_value());
    CHECK(hover->contents == "**Variable** `ready`");
    CHECK(hover->range.start_line == 1);
    CHECK(hover->range.start_character == 8);
    CHECK(hover->range.end_line == 1);
    CHECK(hover->range.end_character == 13);
}

TEST_CASE("CompilationService extracts identifier ranges outside comments and strings", "[analysis][identifiers]") {
    CompilationService service;

    const auto identifiers = service.identifiers(
        "module top;\n"
        "  logic ready; // fake_comment\n"
        "  string name = \"fake_string\";\n"
        "endmodule\n");

    const auto contains_name = [&](std::string_view name) {
        return std::any_of(identifiers.begin(), identifiers.end(), [&](const Identifier& identifier) {
            return identifier.name == name;
        });
    };

    CHECK(contains_name("ready"));
    CHECK_FALSE(contains_name("fake_comment"));
    CHECK_FALSE(contains_name("fake_string"));

    const auto ready = service.identifierAt("module top;\n  logic ready;\nendmodule\n", 1, 9);
    REQUIRE(ready.has_value());
    CHECK(ready->name == "ready");
    CHECK(ready->range.start_line == 1);
    CHECK(ready->range.start_character == 8);
    CHECK(ready->range.end_character == 13);
}

TEST_CASE("CompilationService computes completion prefix", "[analysis][completion]") {
    CompilationService service;

    const auto prefix = service.completionPrefix(
        "module top;\n"
        "  logic ready;\n"
        "  assign ready = re\n"
        "endmodule\n",
        2, 19);

    CHECK(prefix == "re");
}

TEST_CASE("CompilationService extracts include directives", "[analysis][links]") {
    CompilationService service;

    const auto includes = service.includeDirectives(
        "// `include \"ignored.svh\"\n"
        "`include \"defs.svh\"\n"
        "string path = \"`include fake.svh\";\n"
        "`include <pkg/common.svh>\n");

    REQUIRE(includes.size() == 2);
    CHECK(includes[0].target == "defs.svh");
    CHECK(includes[0].range.start_line == 1);
    CHECK(includes[0].range.start_character == 10);
    CHECK(includes[0].range.end_character == 18);
    CHECK(includes[1].target == "pkg/common.svh");
    CHECK(includes[1].range.start_line == 3);
    CHECK(includes[1].range.start_character == 10);
}

TEST_CASE("CompilationService extracts macro definitions", "[analysis][macros]") {
    CompilationService service;

    const auto macros = service.macroDefinitions(
        "// `define IGNORED 0\n"
        "`define FEATURE 1\n"
        "`define ADD(a, b) ((a) + (b))\n"
        "string text = \"`define FAKE 1\";\n");

    REQUIRE(macros.size() == 2);
    CHECK(macros[0].name == "FEATURE");
    CHECK(macros[0].body == "1");
    CHECK_FALSE(macros[0].function_like);
    CHECK(macros[0].selection_range.start_line == 1);
    CHECK(macros[0].selection_range.start_character == 8);
    CHECK(macros[1].name == "ADD");
    REQUIRE(macros[1].parameters.size() == 2);
    CHECK(macros[1].parameters[0] == "a");
    CHECK(macros[1].parameters[1] == "b");
    CHECK(macros[1].body == "((a) + (b))");
    CHECK(macros[1].function_like);
}

TEST_CASE("CompilationService extracts package imports", "[analysis][imports]") {
    CompilationService service;

    const auto imports = service.packageImports(
        "// import ignored::*;\n"
        "import defs::*;\n"
        "string text = \"import fake::*;\";\n"
        "module top; import defs::word_t, util::flag_t; endmodule\n");

    REQUIRE(imports.size() == 3);
    CHECK(imports[0].package_name == "defs");
    CHECK_FALSE(imports[0].item_name.has_value());
    CHECK(imports[0].range.start_line == 1);
    CHECK(imports[1].package_name == "defs");
    REQUIRE(imports[1].item_name.has_value());
    CHECK(*imports[1].item_name == "word_t");
    CHECK(imports[2].package_name == "util");
    REQUIRE(imports[2].item_name.has_value());
    CHECK(*imports[2].item_name == "flag_t");
}

TEST_CASE("CompilationService extracts semantic type metadata", "[analysis][types]") {
    CompilationService service;

    const auto metadata = service.semanticSymbolMetadata(
        "module top #(parameter int WIDTH = 8) (input logic [WIDTH-1:0] data);\n"
        "  typedef logic [7:0] byte_t;\n"
        "endmodule\n",
        "file:///workspace/types.sv");

    const auto find_metadata = [&](std::string_view name) {
        return std::find_if(metadata.begin(), metadata.end(), [&](const SemanticSymbolMetadata& candidate) {
            return candidate.name == name;
        });
    };

    const auto width = find_metadata("WIDTH");
    REQUIRE(width != metadata.end());
    CHECK(width->kind == 14);
    CHECK(width->type_name == "int");
    CHECK(width->value_expression == "8");

    const auto data = find_metadata("data");
    REQUIRE(data != metadata.end());
    CHECK(data->kind == 13);
    CHECK(data->direction == "input");
    CHECK(data->type_name == "logic");
    CHECK(data->type_display_name.find("WIDTH") != std::string::npos);

    const auto byte_t = find_metadata("byte_t");
    REQUIRE(byte_t != metadata.end());
    CHECK(byte_t->kind == 26);
    CHECK(byte_t->alias_target.find("logic") != std::string::npos);
    CHECK(byte_t->alias_target.find("[7:0]") != std::string::npos);
}

TEST_CASE("CompilationService extracts top-level document symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "package pkg; endpackage\ninterface bus; endinterface\nmodule top; endmodule\n",
        "file:///workspace/symbols.sv");

    REQUIRE(symbols.size() == 3);
    CHECK(symbols[0].name == "pkg");
    CHECK(symbols[0].kind == 4);
    CHECK(symbols[1].name == "bus");
    CHECK(symbols[1].kind == 11);
    CHECK(symbols[2].name == "top");
    CHECK(symbols[2].kind == 2);
}

TEST_CASE("CompilationService extracts nested module member symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "module top #(parameter int WIDTH = 8);\n"
        "  logic ready;\n"
        "  wire clk, rst_n;\n"
        "  typedef logic [7:0] byte_t;\n"
        "  function automatic int sum();\n"
        "  endfunction\n"
        "endmodule\n",
        "file:///workspace/nested-symbols.sv");

    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].name == "top");
    REQUIRE(symbols[0].children.size() == 6);
    CHECK(symbols[0].children[0].name == "WIDTH");
    CHECK(symbols[0].children[0].kind == 14);
    CHECK(symbols[0].children[1].name == "ready");
    CHECK(symbols[0].children[1].kind == 13);
    CHECK(symbols[0].children[2].name == "clk");
    CHECK(symbols[0].children[3].name == "rst_n");
    CHECK(symbols[0].children[4].name == "byte_t");
    CHECK(symbols[0].children[5].name == "sum");
    CHECK(symbols[0].children[5].kind == 12);
}

TEST_CASE("CompilationService extracts ansi header port symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "module top(input logic clk, output logic [7:0] data);\n"
        "endmodule\n",
        "file:///workspace/ansi-ports.sv");

    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].name == "top");
    REQUIRE(symbols[0].children.size() == 2);
    CHECK(symbols[0].children[0].name == "clk");
    CHECK(symbols[0].children[0].kind == 13);
    CHECK(symbols[0].children[1].name == "data");
    CHECK(symbols[0].children[1].kind == 13);
}

TEST_CASE("CompilationService extracts interface modport symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "interface bus_if(input logic clk);\n"
        "  logic ready;\n"
        "  function void sample();\n"
        "  endfunction\n"
        "  modport master(input clk, ready, import function sample);\n"
        "endinterface\n",
        "file:///workspace/interface-modport.sv");

    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].name == "bus_if");
    REQUIRE(symbols[0].children.size() == 4);
    CHECK(symbols[0].children[0].name == "clk");
    CHECK(symbols[0].children[1].name == "ready");
    CHECK(symbols[0].children[2].name == "sample");
    CHECK(symbols[0].children[3].name == "master");
    CHECK(symbols[0].children[3].kind == 11);
    REQUIRE(symbols[0].children[3].children.size() == 3);
    CHECK(symbols[0].children[3].children[0].name == "clk");
    CHECK(symbols[0].children[3].children[1].name == "ready");
    CHECK(symbols[0].children[3].children[2].name == "sample");
    CHECK(symbols[0].children[3].children[2].kind == 12);
}

TEST_CASE("CompilationService extracts class enum and instance symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "class Packet;\n"
        "  rand int len;\n"
        "  function void clear();\n"
        "  endfunction\n"
        "endclass\n"
        "typedef enum logic [1:0] { Idle, Busy } state_t;\n"
        "module top;\n"
        "  child child_i();\n"
        "endmodule\n",
        "file:///workspace/class-enum-instance.sv");

    REQUIRE(symbols.size() == 3);
    CHECK(symbols[0].name == "Packet");
    CHECK(symbols[0].kind == 5);
    REQUIRE(symbols[0].children.size() == 2);
    CHECK(symbols[0].children[0].name == "len");
    CHECK(symbols[0].children[0].kind == 13);
    CHECK(symbols[0].children[1].name == "clear");
    CHECK(symbols[0].children[1].kind == 12);
    CHECK(symbols[1].name == "state_t");
    CHECK(symbols[1].kind == 10);
    REQUIRE(symbols[1].children.size() == 2);
    CHECK(symbols[1].children[0].name == "Idle");
    CHECK(symbols[1].children[0].kind == 22);
    CHECK(symbols[1].children[1].name == "Busy");
    CHECK(symbols[2].name == "top");
    REQUIRE(symbols[2].children.size() == 1);
    CHECK(symbols[2].children[0].name == "child_i");
    CHECK(symbols[2].children[0].kind == 19);
}

TEST_CASE("CompilationService extracts module definitions and direct instantiations", "[analysis][hierarchy]") {
    CompilationService service;

    const auto modules = service.moduleDefinitions(
        "module leaf(input logic clk, output logic done); endmodule\n"
        "module child;\n"
        "  leaf u_leaf();\n"
        "endmodule\n"
        "module top;\n"
        "  child u_child();\n"
        "endmodule\n",
        "file:///workspace/hierarchy.sv");

    REQUIRE(modules.size() == 3);
    CHECK(modules[0].name == "leaf");
    CHECK(modules[0].kind == "module");
    REQUIRE(modules[0].ports.size() == 2);
    CHECK(modules[0].ports[0] == "clk");
    CHECK(modules[0].ports[1] == "done");
    REQUIRE(modules[0].port_details.size() == 2);
    CHECK(modules[0].port_details[0].direction == "input");
    CHECK(modules[0].port_details[0].width_text == "logic");
    CHECK(modules[0].port_details[1].direction == "output");
    CHECK(modules[0].instances.empty());

    CHECK(modules[1].name == "child");
    REQUIRE(modules[1].instances.size() == 1);
    CHECK(modules[1].instances[0].module_name == "leaf");
    CHECK(modules[1].instances[0].instance_name == "u_leaf");
    CHECK(modules[1].instances[0].selection_range.start_line == 2);
    CHECK(modules[1].instances[0].selection_range.start_character == 7);

    CHECK(modules[2].name == "top");
    REQUIRE(modules[2].instances.size() == 1);
    CHECK(modules[2].instances[0].module_name == "child");
    CHECK(modules[2].instances[0].instance_name == "u_child");
    CHECK(modules[2].instances[0].module_selection_range.start_line == 5);
    CHECK(modules[2].instances[0].module_selection_range.start_character == 2);
}

TEST_CASE("CompilationService extracts interface definitions and instantiations", "[analysis][hierarchy]") {
    CompilationService service;

    const auto definitions = service.moduleDefinitions(
        "interface bus_if(input logic clk); endinterface\n"
        "module top;\n"
        "  bus_if bus();\n"
        "endmodule\n",
        "file:///workspace/interface-hierarchy.sv");

    REQUIRE(definitions.size() == 2);
    CHECK(definitions[0].name == "bus_if");
    CHECK(definitions[0].kind == "interface");
    REQUIRE(definitions[0].ports.size() == 1);
    CHECK(definitions[0].ports[0] == "clk");

    CHECK(definitions[1].name == "top");
    CHECK(definitions[1].kind == "module");
    REQUIRE(definitions[1].instances.size() == 1);
    CHECK(definitions[1].instances[0].module_name == "bus_if");
    CHECK(definitions[1].instances[0].instance_name == "bus");
}

TEST_CASE("CompilationService extracts hierarchy instantiations inside generate blocks", "[analysis][hierarchy]") {
    CompilationService service;

    const auto modules = service.moduleDefinitions(
        "module lane; endmodule\n"
        "module top;\n"
        "  generate\n"
        "    if (1) begin : enabled\n"
        "      lane u_lane();\n"
        "    end\n"
        "  endgenerate\n"
        "endmodule\n",
        "file:///workspace/generate-hierarchy.sv");

    REQUIRE(modules.size() == 2);
    CHECK(modules[1].name == "top");
    REQUIRE(modules[1].instances.size() == 1);
    CHECK(modules[1].instances[0].module_name == "lane");
    CHECK(modules[1].instances[0].instance_name == "u_lane");
}

TEST_CASE("CompilationService extracts schematic ports cells and assign logic", "[analysis][schematic]") {
    CompilationService service;

    const auto schematics = service.moduleSchematics(
        "module child(input logic a, output logic y); endmodule\n"
        "module top(input logic a, input logic b, input logic sel, output logic y);\n"
        "  logic n1, n2;\n"
        "  child u_child(.a(a), .y(n1));\n"
        "  and u_and(n2, a, b);\n"
        "  assign y = sel ? n1 : (a | b);\n"
        "endmodule\n",
        "file:///workspace/schematic.sv");

    REQUIRE(schematics.size() == 2);
    const auto& top = schematics[1];
    CHECK(top.name == "top");
    REQUIRE(top.ports.size() == 4);
    CHECK(top.ports[0].name == "a");
    CHECK(top.ports[0].direction == "input");
    CHECK(top.ports[3].name == "y");
    CHECK(top.ports[3].direction == "output");

    const auto has_cell_kind = [&](std::string_view kind) {
        return std::any_of(top.cells.begin(), top.cells.end(), [&](const SchematicCell& cell) {
            return cell.kind == kind;
        });
    };

    CHECK(has_cell_kind("module"));
    CHECK(has_cell_kind("and"));
    CHECK(has_cell_kind("or"));
    CHECK(has_cell_kind("mux"));

    const auto child_cell = std::find_if(top.cells.begin(), top.cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_child";
    });
    REQUIRE(child_cell != top.cells.end());
    CHECK(child_cell->type == "child");
    REQUIRE(child_cell->connections.size() == 2);
    CHECK(child_cell->connections[0].port_name == "a");
    CHECK(child_cell->connections[0].signal == "a");
    CHECK(child_cell->connections[1].port_name == "y");
    CHECK(child_cell->connections[1].signal == "n1");

    const auto primitive_cell = std::find_if(top.cells.begin(), top.cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_and";
    });
    REQUIRE(primitive_cell != top.cells.end());
    REQUIRE(primitive_cell->connections.size() == 3);
    CHECK(primitive_cell->connections[0].port_name == "Y");
    CHECK(primitive_cell->connections[0].signal == "n2");

    const auto mux_cell = std::find_if(top.cells.begin(), top.cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "mux";
    });
    REQUIRE(mux_cell != top.cells.end());
    CHECK(std::any_of(mux_cell->connections.begin(), mux_cell->connections.end(), [](const SchematicConnection& connection) {
        return connection.port_name == "Y" && connection.signal == "y";
    }));
    CHECK(std::any_of(mux_cell->connections.begin(), mux_cell->connections.end(), [](const SchematicConnection& connection) {
        return connection.port_name == "S" && connection.signal == "sel";
    }));
}

TEST_CASE("CompilationService extracts named generate block symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "module top;\n"
        "  generate\n"
        "    begin : gen_blk\n"
        "      logic enabled;\n"
        "    end\n"
        "  endgenerate\n"
        "endmodule\n",
        "file:///workspace/generate-blocks.sv");

    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].name == "top");
    REQUIRE(symbols[0].children.size() == 1);
    CHECK(symbols[0].children[0].name == "gen_blk");
    CHECK(symbols[0].children[0].kind == 3);
    REQUIRE(symbols[0].children[0].children.size() == 1);
    CHECK(symbols[0].children[0].children[0].name == "enabled");
    CHECK(symbols[0].children[0].children[0].kind == 13);
}

TEST_CASE("CompilationService extracts if and loop generate symbols", "[analysis][symbols]") {
    CompilationService service;

    const auto symbols = service.documentSymbols(
        "module top;\n"
        "  generate\n"
        "    if (1) begin : has_feature\n"
        "      logic enabled;\n"
        "    end else begin : no_feature\n"
        "      logic disabled;\n"
        "    end\n"
        "    for (genvar i = 0; i < 2; i++) begin : lane\n"
        "      logic ready;\n"
        "    end\n"
        "  endgenerate\n"
        "endmodule\n",
        "file:///workspace/generate-branches.sv");

    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].name == "top");
    REQUIRE(symbols[0].children.size() == 3);
    CHECK(symbols[0].children[0].name == "has_feature");
    CHECK(symbols[0].children[0].kind == 3);
    REQUIRE(symbols[0].children[0].children.size() == 1);
    CHECK(symbols[0].children[0].children[0].name == "enabled");
    CHECK(symbols[0].children[1].name == "no_feature");
    REQUIRE(symbols[0].children[1].children.size() == 1);
    CHECK(symbols[0].children[1].children[0].name == "disabled");
    CHECK(symbols[0].children[2].name == "lane");
    REQUIRE(symbols[0].children[2].children.size() == 1);
    CHECK(symbols[0].children[2].children[0].name == "ready");
}

} // namespace pristine::analysis