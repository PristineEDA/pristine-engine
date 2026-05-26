#include "pristine/analysis/SymbolIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis {

TEST_CASE("SymbolIndex resolves definitions and references", "[analysis][index]") {
    SymbolIndex index;
    index.updateDocument("file:///workspace/child.sv", "module child; endmodule\n");
    index.updateDocument("file:///workspace/top.sv",
                         "module top;\n"
                         "  child child_i();\n"
                         "  logic ready;\n"
                         "  assign ready = ready;\n"
                         "endmodule\n");

    const auto definitions = index.definitions("child", "file:///workspace/top.sv");
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().location.uri == "file:///workspace/child.sv");
    CHECK(definitions.front().location.range.start_line == 0);
    CHECK(definitions.front().location.range.start_character == 7);

    const auto references = index.references("ready", false);
    REQUIRE(references.size() == 2);
    CHECK(references[0].location.range.start_line == 3);
    CHECK(references[1].location.range.start_line == 3);
}

TEST_CASE("SymbolIndex returns workspace symbols and completions", "[analysis][index]") {
    SymbolIndex index;
    index.updateDocument("file:///workspace/child.sv", "module child; endmodule\n");
    index.updateDocument("file:///workspace/top.sv",
                         "module top;\n"
                         "  child child_i();\n"
                         "endmodule\n");

    const auto symbols = index.workspaceSymbols("ch");
    CHECK(std::any_of(symbols.begin(), symbols.end(), [](const SymbolEntry& symbol) {
        return symbol.name == "child" && symbol.kind == 2;
    }));

    const auto completions = index.completions("ch", "file:///workspace/top.sv");
    REQUIRE(completions.size() >= 2);
    CHECK(completions[0].label == "child_i");
    CHECK(std::any_of(completions.begin(), completions.end(), [](const CompletionEntry& item) {
        return item.label == "child" && item.detail == "Module";
    }));
}

} // namespace pristine::analysis