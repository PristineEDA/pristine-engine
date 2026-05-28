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
                               "endmodule\n",
                       .line = 0,
                       .character = 8,
                       .expected_symbol = "child",
                       .expected_references = 1},
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
    }
}

} // namespace pristine::analysis
