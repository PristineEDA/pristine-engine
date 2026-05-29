#include "pristine/analysis/SemanticEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace pristine::analysis {
namespace {

struct SemanticGoldenCase {
    std::string_view name;
    std::string_view uri;
    std::string_view text;
    int line = 0;
    int character = 0;
    std::string_view expected_symbol;
    size_t expected_references = 0;
};

constexpr SemanticGoldenCase kGoldenCases[] = {
    SemanticGoldenCase{.name = "local signal references",
                       .uri = "file:///golden/local.sv",
                       .text = "module top;\n"
                               "  logic ready;\n"
                               "  assign ready = ready;\n"
                               "endmodule\n",
                       .line = 1,
                       .character = 9,
                       .expected_symbol = "ready",
                       .expected_references = 3},
    SemanticGoldenCase{.name = "module identifier lookup",
                       .uri = "file:///golden/module.sv",
                       .text = "module child;\n"
                               "endmodule\n"
                               "module top;\n"
                               "  child u_child();\n"
                               "endmodule\n",
                       .line = 3,
                       .character = 4,
                       .expected_symbol = "child",
                       .expected_references = 2},
    SemanticGoldenCase{.name = "shadowed locals stay scoped",
                       .uri = "file:///golden/shadow.sv",
                       .text = "module first;\n"
                               "  logic value;\n"
                               "  assign value = value;\n"
                               "endmodule\n"
                               "module second;\n"
                               "  logic value;\n"
                               "  assign value = value;\n"
                               "endmodule\n",
                       .line = 1,
                       .character = 9,
                       .expected_symbol = "value",
                       .expected_references = 3},
    SemanticGoldenCase{.name = "typedef value lookup",
                       .uri = "file:///golden/type.sv",
                       .text = "module top;\n"
                               "  typedef logic [7:0] byte_t;\n"
                               "  byte_t data;\n"
                               "  assign data = '0;\n"
                               "endmodule\n",
                       .line = 2,
                       .character = 10,
                       .expected_symbol = "data",
                       .expected_references = 2},
};

} // namespace

TEST_CASE("Semantic golden cases exercise first-batch query contracts",
          "[analysis][golden][semantic]") {
    for (const auto& test_case : kGoldenCases) {
        CAPTURE(test_case.name);
        SemanticEngine engine;
        engine.updateDocument(test_case.uri,
                              test_case.text,
                              SemanticEngineDocumentState{.version = 1, .is_open = true});

        const auto lookup = engine.lookupAt(test_case.uri, test_case.line, test_case.character);
        REQUIRE_FALSE(lookup.unresolved);
        REQUIRE(lookup.symbol.has_value());
        CHECK(lookup.symbol->name == test_case.expected_symbol);

        const auto references = engine.referencesAt(test_case.uri,
                                                    test_case.line,
                                                    test_case.character,
                                                    true);
        REQUIRE_FALSE(references.unresolved);
        CHECK(references.locations.size() == test_case.expected_references);

        const auto hover = engine.hoverAt(test_case.uri, test_case.line, test_case.character);
        REQUIRE_FALSE(hover.unresolved);
        CHECK(hover.contents.find(test_case.expected_symbol) != std::string::npos);

        const auto rename = engine.renameAt(test_case.uri,
                                           test_case.line,
                                           test_case.character,
                                           "renamed_symbol");
        REQUIRE_FALSE(rename.unresolved);
        CHECK(rename.edits.size() == test_case.expected_references);
    }
}

} // namespace pristine::analysis
