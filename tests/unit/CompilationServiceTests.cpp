#include "pristine/analysis/CompilationService.h"

#include <catch2/catch_test_macros.hpp>

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