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
                                                   "  pkg_only_t imported_later;\n"
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
                                                                                                      rangeAt(7, 14, 17))},
                                            .type_display = "logic [3:0]"}},
                          {"rhs",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "rhs",
                                                                               .name = "rhs",
                                                                               .kind = "Variable",
                                                                               .location = locationAt(std::string(uri),
                                                                                                      rangeAt(8, 14, 17))},
                                            .type_display = "logic [7:0]"}},
                          {"ready_1",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "ready_1",
                                                                               .name = "ready",
                                                                               .kind = "Variable",
                                                                               .location = locationAt(std::string(uri),
                                                                                                      rangeAt(1, 8, 13))},
                                            .type_display = "logic"}},
                          {"ready_2",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "ready_2",
                                                                               .name = "ready",
                                                                               .kind = "Variable",
                                                                               .location = locationAt(std::string(uri),
                                                                                                      rangeAt(2, 8, 13))},
                                            .type_display = "logic"}},
                          {"word_a",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "word_a",
                                                                               .name = "word_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = locationAt("file:///workspace/pkg_a.sv",
                                                                                                      rangeAt(1, 22, 28))}}},
                          {"word_b",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "word_b",
                                                                               .name = "word_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = locationAt("file:///workspace/pkg_b.sv",
                                                                                                      rangeAt(1, 23, 29))}}},
                          {"pkg_c",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg_c",
                                                                               .name = "pkg_c",
                                                                               .kind = "Package",
                                                                               .location = locationAt("file:///workspace/pkg_c.sv",
                                                                                                      rangeAt(0, 8, 13))}}},
                          {"pkg_only",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg_only",
                                                                               .name = "pkg_only_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = locationAt("file:///workspace/pkg_c.sv",
                                                                                                      rangeAt(1, 22, 32))}}}},
        .lookup_index =
            SnapshotDiagnosticLookupIndex{
                .package_definition_ids_by_name = {{"pkg_a", {"pkg_a"}},
                                                    {"pkg_b", {"pkg_b"}},
                                                    {"pkg_c", {"pkg_c"}}},
                .package_names_by_member = {{"word_t", {"pkg_a", "pkg_b"}},
                                            {"pkg_only_t", {"pkg_c"}}},
                .package_member_definition_counts = {{"pkg_a\x1fword_t", 1},
                                                      {"pkg_b\x1fword_t", 1}},
                .duplicate_symbols_by_uri =
                    {{std::string(uri),
                      {SemanticSymbolIdentity{.stable_id = "ready_1",
                                              .name = "ready",
                                              .kind = "Variable",
                                              .location = locationAt(std::string(uri), rangeAt(1, 8, 13))},
                       SemanticSymbolIdentity{.stable_id = "ready_2",
                                              .name = "ready",
                                              .kind = "Variable",
                                              .location = locationAt(std::string(uri), rangeAt(2, 8, 13))}}}}},
        .references = {DiagnosticReference{.stable_id = "lhs",
                                           .location = locationAt(std::string(uri), rangeAt(10, 9, 12))},
                       DiagnosticReference{.stable_id = "rhs",
                                           .location = locationAt(std::string(uri), rangeAt(10, 15, 18))}},
        .assignment_edges_by_uri = {{std::string(uri),
                                     {SnapshotAssignmentEdge{.from_symbol_id = "lhs",
                                                             .to_symbol_id = "rhs",
                                                             .location = locationAt(std::string(uri),
                                                                                    rangeAt(10, 2, 18)),
                                                             .expression_location = locationAt(std::string(uri),
                                                                                               rangeAt(10, 15, 18)),
                                                             .expression = "rhs"}}}},
        .type_references_by_uri = {{std::string(uri),
                                    {SnapshotTypeReference{.reference = locationAt(std::string(uri),
                                                                                   rangeAt(5, 2, 8)),
                                                           .type_name = "word_t",
                                                           .definitions = {locationAt("file:///workspace/pkg_a.sv",
                                                                                      rangeAt(1, 22, 28)),
                                                                           locationAt("file:///workspace/pkg_b.sv",
                                                                                      rangeAt(1, 23, 29))}},
                                     SnapshotTypeReference{.reference = locationAt(std::string(uri),
                                                                                   rangeAt(6, 2, 11)),
                                                           .type_name = "missing_t",
                                                           .definitions = {}},
                                     SnapshotTypeReference{.reference = locationAt(std::string(uri),
                                                                                   rangeAt(7, 2, 12)),
                                                           .type_name = "pkg_only_t",
                                                           .definitions = {}}}}},
        .include_directives_by_uri = {{std::string(uri),
                                       {IncludeDirective{.target = "missing.svh",
                                                         .range = rangeAt(11, 0, 22)}}}},
        .package_imports_by_uri = {{std::string(uri),
                                    {PackageImport{.package_name = "pkg_a",
                                                   .package_range = rangeAt(3, 9, 14),
                                                   .range = rangeAt(3, 2, 18)},
                                     PackageImport{.package_name = "pkg_b",
                                                   .package_range = rangeAt(4, 9, 14),
                                                   .range = rangeAt(4, 2, 18)}}}},
        .modules_by_name = {{"top", ModuleDefinition{.name = "top"}}},
        .module_instances_by_uri = {{std::string(uri),
                                     {SnapshotModuleInstance{.module_name = "missing_child",
                                                             .instance_name = "u_missing",
                                                             .uri = std::string(uri),
                                                             .range = rangeAt(12, 2, 28),
                                                             .selection_range = rangeAt(12, 16, 25),
                                                             .module_selection_range = rangeAt(12, 2, 15)}}}}};

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
        return diagnostic.code == "missingImport" &&
               diagnostic.message.find("pkg_c") != std::string::npos;
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
    CHECK(context.lookup_scanned_fact_count > 0);
}

TEST_CASE("DiagnosticProvider reports ambiguous missing imports without choosing a package",
          "[analysis][semantic][diagnostic-provider][diagnostics][imports]") {
    constexpr std::string_view uri = "file:///workspace/top.sv";
    DiagnosticContext context{
        .generation = 12,
        .snapshot_available = true,
        .workspace_root_uri = "file:///workspace",
        .document = SemanticEngineDocument{.uri = std::string(uri),
                                           .text = "module top;\n"
                                                   "  shared_t value;\n"
                                                   "endmodule\n"},
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
                          {"shared_a",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "shared_a",
                                                                               .name = "shared_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = locationAt("file:///workspace/pkg_a.sv",
                                                                                                      rangeAt(1, 22, 30))}}},
                          {"shared_b",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "shared_b",
                                                                               .name = "shared_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = locationAt("file:///workspace/pkg_b.sv",
                                                                                                      rangeAt(1, 22, 30))}}}},
        .lookup_index = SnapshotDiagnosticLookupIndex{
            .package_names_by_member = {{"shared_t", {"pkg_a", "pkg_b"}}}},
        .type_references_by_uri = {{std::string(uri),
                                    {SnapshotTypeReference{.reference = locationAt(std::string(uri),
                                                                                   rangeAt(1, 2, 10)),
                                                           .type_name = "shared_t",
                                                           .definitions = {}}}}}};

    const auto diagnostics = diagnosticsFor(context);

    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "ambiguousMissingImport" &&
               diagnostic.message.find("'pkg_a'") != std::string::npos &&
               diagnostic.message.find("'pkg_b'") != std::string::npos &&
               diagnostic.severity == 2;
    }));
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "missingImport";
    }));
    CHECK(context.lookup_scanned_fact_count > 0);
}

} // namespace pristine::analysis::semantic
