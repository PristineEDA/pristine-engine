#include "pristine/analysis/SyntaxDocumentCache.h"

#include <catch2/catch_test_macros.hpp>

namespace pristine::analysis {

TEST_CASE("SyntaxDocumentCache reuses document symbols by uri version and text hash",
          "[analysis][syntax-cache]") {
    CompilationService service;
    SyntaxDocumentCache cache;

    constexpr std::string_view source = "module top(input logic clk); endmodule\n";

    const auto& first = cache.documentSymbols(service, "file:///workspace/top.sv", 1, source);
    REQUIRE(first.size() == 1);
    CHECK(first.front().name == "top");
    auto stats = cache.stats();
    CHECK(stats.hits == 0);
    CHECK(stats.misses == 1);
    CHECK(stats.stores == 1);
    CHECK(stats.entries == 1);

    const auto& second = cache.documentSymbols(service, "file:///workspace/top.sv", 1, source);
    CHECK(second.size() == first.size());
    stats = cache.stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 1);
    CHECK(stats.stores == 1);
    CHECK(stats.entries == 1);
}

TEST_CASE("SyntaxDocumentCache invalidates when text changes without version change",
          "[analysis][syntax-cache]") {
    CompilationService service;
    SyntaxDocumentCache cache;

    constexpr std::string_view first_source = "module top; endmodule\n";
    constexpr std::string_view second_source = "module renamed; endmodule\n";

    const auto& first = cache.documentSymbols(service, "file:///workspace/top.sv", 7, first_source);
    REQUIRE(first.size() == 1);
    CHECK(first.front().name == "top");

    const auto& second = cache.documentSymbols(service, "file:///workspace/top.sv", 7, second_source);
    REQUIRE(second.size() == 1);
    CHECK(second.front().name == "renamed");

    const auto stats = cache.stats();
    CHECK(stats.hits == 0);
    CHECK(stats.misses == 2);
    CHECK(stats.stores == 2);
    CHECK(stats.entries == 1);
}

TEST_CASE("SyntaxDocumentCache invalidate removes only the requested document",
          "[analysis][syntax-cache]") {
    CompilationService service;
    SyntaxDocumentCache cache;

    constexpr std::string_view top_source = "module top; endmodule\n";
    constexpr std::string_view child_source = "module child; endmodule\n";

    (void)cache.documentSymbols(service, "file:///workspace/top.sv", 1, top_source);
    (void)cache.documentSymbols(service, "file:///workspace/child.sv", 1, child_source);
    cache.invalidate("file:///workspace/top.sv");

    (void)cache.documentSymbols(service, "file:///workspace/child.sv", 1, child_source);
    (void)cache.documentSymbols(service, "file:///workspace/top.sv", 1, top_source);

    const auto stats = cache.stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 3);
    CHECK(stats.stores == 3);
    CHECK(stats.invalidations == 1);
    CHECK(stats.entries == 2);
}

TEST_CASE("SyntaxDocumentCache keeps outline-only metadata out of cached document symbols",
          "[analysis][syntax-cache][outline]") {
    CompilationService service;
    SyntaxDocumentCache cache;

    constexpr std::string_view source =
        "module top(input logic [7:0] data_i);\n"
        "  localparam logic [2:0] FSM_IDLE = 3'd0;\n"
        "endmodule\n";

    const auto outline = service.outline(source, "file:///workspace/top.sv", 1, 9);
    REQUIRE_FALSE(outline.items.empty());
    const auto& cached = cache.documentSymbols(service, "file:///workspace/top.sv", 1, source);
    REQUIRE(cached.size() == 1);
    REQUIRE_FALSE(cached.front().children.empty());
    CHECK(cached.front().children.front().metadata.detail.empty());

    const auto stats = cache.stats();
    CHECK(stats.misses == 1);
    CHECK(stats.entries == 1);
}

} // namespace pristine::analysis
