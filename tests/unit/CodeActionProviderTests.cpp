#include "../../src/analysis/semantic/CodeActionProvider.h"

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

} // namespace

TEST_CASE("CodeActionProvider creates include, module, port, and typedef fixes",
          "[analysis][semantic][code-action-provider]") {
    constexpr std::string_view uri = "file:///workspace/rtl/top.sv";
    const auto document_text = "`include \"missing.svh\"\n"
                               "module top;\n"
                               "  child u_child(.clk(clk));\n"
                               "  missing_child u_missing();\n"
                               "  import missing_pkg::*;\n"
                               "  missing_t value;\n"
                               "  pkg_only_t imported_later;\n"
                               "endmodule\n";
    CodeActionContext context{
        .generation = 9,
        .snapshot_available = true,
        .workspace_root_uri = "file:///workspace",
        .document = SemanticEngineDocument{.uri = std::string(uri), .text = document_text},
        .range = ParseRange{.start_line = 0,
                            .start_character = 0,
                            .end_line = 6,
                            .end_character = 17},
        .modules_by_name = {{"child",
                             ModuleDefinition{.name = "child",
                                              .kind = "module",
                                              .port_details = {SchematicPort{.name = "clk",
                                                                             .direction = "input",
                                                                             .width_text = "logic"},
                                                               SchematicPort{.name = "rst_n",
                                                                             .direction = "output",
                                                                             .width_text = "logic"},
                                                               SchematicPort{.name = "data",
                                                                             .direction = "input",
                                                                             .width_text = "logic"}}}}},
        .document_schematics = {ModuleSchematic{.name = "top",
                                                .cells = {SchematicCell{.id = "u_child",
                                                                        .name = "u_child",
                                                                        .type = "child",
                                                                        .kind = "module",
                                                                        .range = rangeAt(2, 2, 27),
                                                                        .selection_range = rangeAt(2, 8, 15),
                                                                        .connections = {SchematicConnection{
                                                                            .port_name = "clk",
                                                                            .signal = "clk",
                                                                            .range = rangeAt(2, 17, 26)}}}}}},
        .include_directives = {IncludeDirective{.target = "missing.svh",
                                                .range = rangeAt(0, 0, 22)}},
        .module_instances = {SnapshotModuleInstance{.module_name = "missing_child",
                                                    .instance_name = "u_missing",
                                                    .uri = std::string(uri),
                                                    .range = rangeAt(3, 2, 28),
                                                    .selection_range = rangeAt(3, 16, 25),
                                                    .module_selection_range = rangeAt(3, 2, 15)}},
        .symbols_by_id = {{"pkg",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg",
                                                                               .name = "defs",
                                                                               .kind = "Package",
                                                                               .location = SemanticLocation{
                                                                                   .uri = "file:///workspace/rtl/defs.sv",
                                                                                   .range = rangeAt(0, 8, 12)}}}},
                          {"pkg_only",
                           DiagnosticSymbol{.identity = SemanticSymbolIdentity{.stable_id = "pkg_only",
                                                                               .name = "pkg_only_t",
                                                                               .kind = "TypeAlias",
                                                                               .location = SemanticLocation{
                                                                                   .uri = "file:///workspace/rtl/defs.sv",
                                                                                   .range = rangeAt(1, 22, 32)}}}}},
        .diagnostics = {SemanticEngineDiagnostic{.uri = std::string(uri),
                                                 .code = "unresolvedType",
                                                 .message = "Type 'missing_t' could not be resolved.",
                                                 .range = rangeAt(5, 2, 11),
                                                 .severity = 1},
                        SemanticEngineDiagnostic{.uri = std::string(uri),
                                                 .code = "unresolvedPackage",
                                                 .message = "Package 'missing_pkg' could not be resolved.",
                                                 .range = rangeAt(4, 9, 20),
                                                 .severity = 1},
                        SemanticEngineDiagnostic{.uri = std::string(uri),
                                                 .code = "missingImport",
                                                 .message = "Type 'pkg_only_t' is available from package 'defs' but is not imported.",
                                                 .range = rangeAt(6, 2, 12),
                                                 .severity = 1}}};

    const auto result = codeActionsAt(context);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 9);
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Create include file 'missing.svh'" &&
               action.create_files.size() == 1 &&
               action.create_files.front().uri == "file:///workspace/rtl/missing.svh";
    }));
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Create stub module 'missing_child'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text.find("module missing_child;") != std::string::npos;
    }));
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Add missing port connections to 'u_child'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text == ", .rst_n(rst_n), .data(data)";
    }));
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Create typedef 'missing_t'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text.find("typedef logic missing_t;") != std::string::npos;
    }));
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Create package 'missing_pkg'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text.find("package missing_pkg;") != std::string::npos;
    }));
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Import package 'defs'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text.find("import defs::*;") != std::string::npos;
    }));
}

TEST_CASE("CodeActionProvider reports unavailable snapshot",
          "[analysis][semantic][code-action-provider]") {
    const CodeActionContext context{.generation = 5, .snapshot_available = false};

    const auto result = codeActionsAt(context);

    CHECK(result.generation == 5);
    CHECK(result.unresolved);
    REQUIRE_FALSE(result.messages.empty());
}

} // namespace pristine::analysis::semantic
