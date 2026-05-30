#include "../../src/analysis/semantic/DiagnosticProvider.h"

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

SemanticLocation locationAt(std::string uri, ParseRange range) {
    return SemanticLocation{.uri = std::move(uri), .range = range};
}

} // namespace

TEST_CASE("DiagnosticProvider aggregates UX diagnostics and dedupes snapshot diagnostics",
          "[analysis][semantic][diagnostic-provider]") {
    constexpr std::string_view uri = "file:///workspace/top.sv";
    DiagnosticContext context{
        .generation = 8,
        .snapshot_available = true,
        .workspace_root_uri = "file:///workspace",
        .document = SemanticEngineDocument{.uri = std::string(uri),
                                           .text = "module top;\n"
                                                   "  logic ready;\n"
                                                   "  logic ready;\n"
                                                   "  import pkg_a::*;\n"
                                                   "  import pkg_b::*;\n"
                                                   "  word_t value;\n"
                                                   "  missing_t missing;\n"
                                                   "  logic [3:0] lhs;\n"
                                                   "  logic [7:0] rhs;\n"
                                                   "  assign lhs = rhs;\n"
                                                   "endmodule\n"},
        .snapshot_diagnostics = {SemanticEngineDiagnostic{.uri = std::string(uri),
                                                          .code = "slang:test",
                                                          .message = "same",
                                                          .range = rangeAt(0, 0, 1),
                                                          .severity = 1},
                                 SemanticEngineDiagnostic{.uri = std::string(uri),
                                                          .code = "slang:test",
                                                          .message = "same",
                                                          .range = rangeAt(0, 0, 1),
                                                          .severity = 1}},
        .symbols_by_id = {{"pkg_a",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg_a",
                                                                               .name = "pkg_a",
                                                                               .kind = "Package",
                                                                               .location = locationAt("file:///workspace/pkg_a.sv",
                                                                                                      rangeAt(0, 8, 13))}}},
                          {"pkg_b",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg_b",
                                                                               .name = "pkg_b",
                                                                               .kind = "Package",
                                                                               .location = locationAt("file:///workspace/pkg_b.sv",
                                                                                                      rangeAt(0, 8, 13))}}},
                          {"lhs",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "lhs",
                                                                               .name = "lhs",
                                                                               .kind = "Variable",
                                                                               .location = locationAt(std::string(uri),
                                                                                                      rangeAt(7, 14, 17))}}},
                          {"rhs",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "rhs",
                                                                               .name = "rhs",
                                                                               .kind = "Variable",
                                                                               .location = locationAt(std::string(uri),
                                                                                                      rangeAt(8, 14, 17))}}}},
        .references = {DiagnosticReference{.stable_id = "lhs",
                                           .location = locationAt(std::string(uri), rangeAt(9, 9, 12))},
                       DiagnosticReference{.stable_id = "rhs",
                                           .location = locationAt(std::string(uri), rangeAt(9, 15, 18))}},
        .assignments_by_uri = {{std::string(uri),
                                {ContinuousAssignment{.left_expression = "lhs",
                                                      .right_expression = "rhs",
                                                      .range = rangeAt(9, 2, 18),
                                                      .left_range = rangeAt(9, 9, 12),
                                                      .right_range = rangeAt(9, 15, 18)}}}},
        .identifiers_by_uri = {{std::string(uri),
                                {Identifier{.name = "word_t", .range = rangeAt(5, 2, 8)},
                                 Identifier{.name = "value", .range = rangeAt(5, 9, 14)},
                                 Identifier{.name = "missing_t", .range = rangeAt(6, 2, 11)},
                                 Identifier{.name = "missing", .range = rangeAt(6, 12, 19)}}}},
        .include_directives_by_uri = {{std::string(uri),
                                       {IncludeDirective{.target = "missing.svh",
                                                         .range = rangeAt(10, 0, 22)}}}},
        .package_imports_by_uri = {{std::string(uri),
                                    {PackageImport{.package_name = "pkg_a",
                                                   .package_range = rangeAt(3, 9, 14),
                                                   .range = rangeAt(3, 2, 18)},
                                     PackageImport{.package_name = "pkg_b",
                                                   .package_range = rangeAt(4, 9, 14),
                                                   .range = rangeAt(4, 2, 18)}}}},
        .metadata_by_uri = {{std::string(uri),
                             {SemanticSymbolMetadata{.name = "ready",
                                                     .kind = 13,
                                                     .selection_range = rangeAt(1, 8, 13),
                                                     .type_name = "logic",
                                                     .type_display_name = "logic"},
                              SemanticSymbolMetadata{.name = "ready",
                                                     .kind = 13,
                                                     .selection_range = rangeAt(2, 8, 13),
                                                     .type_name = "logic",
                                                     .type_display_name = "logic"},
                              SemanticSymbolMetadata{.name = "missing",
                                                     .kind = 13,
                                                     .selection_range = rangeAt(6, 12, 19),
                                                     .type_name = "missing_t",
                                                     .type_display_name = "missing_t"},
                              SemanticSymbolMetadata{.name = "lhs",
                                                     .kind = 13,
                                                     .selection_range = rangeAt(7, 14, 17),
                                                     .type_name = "logic",
                                                     .type_display_name = "logic [3:0]"},
                              SemanticSymbolMetadata{.name = "rhs",
                                                     .kind = 13,
                                                     .selection_range = rangeAt(8, 14, 17),
                                                     .type_name = "logic",
                                                     .type_display_name = "logic [7:0]"}}},
                            {"file:///workspace/pkg_a.sv",
                             {SemanticSymbolMetadata{.name = "word_t",
                                                     .kind = 26,
                                                     .selection_range = rangeAt(1, 22, 28)}}},
                            {"file:///workspace/pkg_b.sv",
                             {SemanticSymbolMetadata{.name = "word_t",
                                                     .kind = 26,
                                                     .selection_range = rangeAt(1, 23, 29)}}}},
        .modules_by_name = {{"top", ModuleDefinition{.name = "top"}}},
        .module_instances_by_uri = {{std::string(uri),
                                     {SnapshotModuleInstance{.module_name = "missing_child",
                                                             .instance_name = "u_missing",
                                                             .uri = std::string(uri),
                                                             .range = rangeAt(11, 2, 28),
                                                             .selection_range = rangeAt(11, 16, 25),
                                                             .module_selection_range = rangeAt(11, 2, 15)}}}}};

    const auto diagnostics = diagnosticsFor(context);

    CHECK(std::count_if(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
              return diagnostic.code == "slang:test";
          }) == 1);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "duplicateSymbol";
    }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "ambiguousReference";
    }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "unresolvedType";
    }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "widthMismatch" && diagnostic.severity == 2;
    }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "unknownInclude";
    }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "unresolvedModule";
    }));
}

} // namespace pristine::analysis::semantic
