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

} // namespace pristine::analysis