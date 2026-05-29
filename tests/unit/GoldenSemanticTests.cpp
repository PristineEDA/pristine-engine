#include "pristine/analysis/SemanticEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

struct SemanticDiagnosticGoldenCase {
    std::string_view name;
    std::string_view uri;
    std::string_view text;
    std::string_view code;
    std::string_view message_fragment;
    bool expected_present = true;
    int expected_severity = 0;
    int expected_start_line = -1;
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

constexpr SemanticDiagnosticGoldenCase kDiagnosticGoldenCases[] = {
    SemanticDiagnosticGoldenCase{.name = "duplicate symbol diagnostic",
                                 .uri = "file:///golden/diagnostics/duplicate.sv",
                                 .text = "module top;\n"
                                         "  logic ready;\n"
                                         "  logic ready;\n"
                                         "endmodule\n",
                                 .code = "duplicateSymbol",
                                 .message_fragment = "Duplicate symbol 'ready'",
                                 .expected_severity = 1,
                                 .expected_start_line = 2},
    SemanticDiagnosticGoldenCase{.name = "unresolved package diagnostic",
                                 .uri = "file:///golden/diagnostics/missing-package.sv",
                                 .text = "module top;\n"
                                         "  import missing_pkg::*;\n"
                                         "endmodule\n",
                                 .code = "unresolvedPackage",
                                 .message_fragment = "missing_pkg",
                                 .expected_severity = 1,
                                 .expected_start_line = 1},
    SemanticDiagnosticGoldenCase{.name = "assignment width mismatch diagnostic",
                                 .uri = "file:///golden/diagnostics/width.sv",
                                 .text = "module top;\n"
                                         "  logic [3:0] lhs;\n"
                                         "  logic [7:0] rhs;\n"
                                         "  assign lhs = rhs;\n"
                                         "endmodule\n",
                                 .code = "widthMismatch",
                                 .message_fragment = "assigning 8-bit 'rhs' to 4-bit 'lhs'",
                                 .expected_severity = 2,
                                 .expected_start_line = 3},
    SemanticDiagnosticGoldenCase{.name = "local symbol does not shadow package resolution",
                                 .uri = "file:///golden/diagnostics/package-shadow.sv",
                                 .text = "package defs; endpackage\n"
                                         "module top;\n"
                                         "  logic defs;\n"
                                         "  import defs::*;\n"
                                         "endmodule\n",
                                 .code = "unresolvedPackage",
                                 .message_fragment = "defs",
                                 .expected_present = false},
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

TEST_CASE("Semantic diagnostic golden cases exercise engine-owned diagnostics",
          "[analysis][golden][semantic][diagnostics]") {
    for (const auto& test_case : kDiagnosticGoldenCases) {
        CAPTURE(test_case.name);
        SemanticEngine engine;
        engine.updateDocument(test_case.uri,
                              test_case.text,
                              SemanticEngineDocumentState{.version = 1, .is_open = true});

        const auto diagnostics = engine.diagnosticsFor(test_case.uri);
        const auto diagnostic = std::find_if(diagnostics.begin(),
                                             diagnostics.end(),
                                             [&](const SemanticEngineDiagnostic& item) {
                                                 return item.code == test_case.code &&
                                                        item.message.find(test_case.message_fragment) !=
                                                            std::string::npos;
                                             });

        if (!test_case.expected_present) {
            CHECK(diagnostic == diagnostics.end());
            continue;
        }

        REQUIRE(diagnostic != diagnostics.end());
        CHECK(diagnostic->severity == test_case.expected_severity);
        CHECK(diagnostic->range.start_line == test_case.expected_start_line);
    }
}

} // namespace pristine::analysis
