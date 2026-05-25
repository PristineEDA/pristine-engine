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

} // namespace pristine::analysis