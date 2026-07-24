#include "../../src/analysis/semantic/CodeActionProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <unordered_map>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

std::string bindingKey(std::string_view uri, const ParseRange& range) {
    return std::string(uri) + "\x1f" + std::to_string(range.start_line) + ":" +
           std::to_string(range.start_character) + ":" + std::to_string(range.end_line) + ":" +
           std::to_string(range.end_character);
}

void addPortBinding(SnapshotDesignGraphBindingIndex& bindings,
                    std::string_view uri,
                    const ParseRange& instance_selection_range,
                    std::string instance_id,
                    std::string endpoint_id,
                    std::string endpoint_name) {
    bindings.instance_ids_by_uri_range.insert_or_assign(bindingKey(uri, instance_selection_range), instance_id);
    bindings.endpoints_by_stable_id.insert_or_assign(
        endpoint_id,
        SnapshotGraphEndpointFact{.stable_id = endpoint_id,
                                  .module_name = "child",
                                  .name = endpoint_name,
                                  .kind = SnapshotGraphEndpointKind::Port,
                                  .direction = SnapshotGraphPortDirection::Input,
                                  .location = SemanticLocation{.uri = std::string(uri),
                                                               .range = instance_selection_range}});
    bindings.connection_bindings_by_instance_id[instance_id].push_back(bindings.connection_bindings.size());
    bindings.connection_bindings.push_back(SnapshotGraphConnectionBindingFact{
        .instance_stable_id = std::move(instance_id),
        .endpoint_stable_id = std::move(endpoint_id),
        .location = SemanticLocation{.uri = std::string(uri), .range = instance_selection_range},
        .kind = SnapshotConeEdgeKind::InstancePort});
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

    SnapshotDesignGraphBindingIndex bindings;
    addPortBinding(bindings, uri, rangeAt(2, 8, 15), "instance|u_child", "endpoint|child|clk", "clk");

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
                                                    .instance_stable_id = {},
                                                    .uri = std::string(uri),
                                                    .range = rangeAt(3, 2, 28),
                                                    .selection_range = rangeAt(3, 16, 25),
                                                    .module_selection_range = rangeAt(3, 2, 15)}},
        .design_graph_bindings = &bindings,
        .packages_by_name = {{"defs",
                              SnapshotPackageVisibility{
                                  .package_name = "defs",
                                  .uri = "file:///workspace/rtl/defs.sv",
                                  .candidates = {SnapshotVisibilityCandidate{
                                      .identity = SemanticSymbolIdentity{
                                          .stable_id = "pkg_only",
                                          .name = "pkg_only_t",
                                          .kind = "TypeAlias",
                                          .location = SemanticLocation{
                                              .uri = "file:///workspace/rtl/defs.sv",
                                              .range = rangeAt(1, 22, 32)}}}}}}},
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

TEST_CASE("CodeActionProvider scopes missing port fixes to the requested instance",
          "[analysis][semantic][code-action-provider][ports]") {
    constexpr std::string_view uri = "file:///workspace/rtl/top.sv";
    const auto document_text = "module top;\n"
                               "  logic clk;\n"
                               "  child u_a(.clk(clk));\n"
                               "  child u_b(.clk(clk));\n"
                               "endmodule\n";


    SnapshotDesignGraphBindingIndex scoped_bindings;
    addPortBinding(scoped_bindings, uri, rangeAt(3, 8, 11), "instance|u_b", "endpoint|child|clk", "clk");

    CodeActionContext context{
        .generation = 11,
        .snapshot_available = true,
        .document = SemanticEngineDocument{.uri = std::string(uri), .text = document_text},
        .range = rangeAt(3, 2, 23),
        .modules_by_name = {{"child",
                             ModuleDefinition{.name = "child",
                                              .kind = "module",
                                              .port_details = {SchematicPort{.name = "clk",
                                                                             .direction = "input",
                                                                             .width_text = "logic"},
                                                               SchematicPort{.name = "rst_n",
                                                                             .direction = "input",
                                                                             .width_text = "logic"},
                                                               SchematicPort{.name = "data",
                                                                             .direction = "input",
                                                                             .width_text = "logic"}}}}},
        .document_schematics = {ModuleSchematic{.name = "top",
                                                .cells = {SchematicCell{.id = "u_a",
                                                                        .name = "u_a",
                                                                        .type = "child",
                                                                        .kind = "module",
                                                                        .range = rangeAt(2, 2, 23),
                                                                        .selection_range = rangeAt(2, 8, 11),
                                                                        .connections = {SchematicConnection{
                                                                            .port_name = "clk",
                                                                            .signal = "clk",
                                                                            .range = rangeAt(2, 12, 21)}}},
                                                          SchematicCell{.id = "u_b",
                                                                        .name = "u_b",
                                                                        .type = "child",
                                                                        .kind = "module",
                                                                        .range = rangeAt(3, 2, 23),
                                                                        .selection_range = rangeAt(3, 8, 11),
                                                                        .connections = {SchematicConnection{
                                                                            .port_name = "clk",
                                                                            .signal = "clk",
                                                                            .range = rangeAt(3, 12, 21)}}}}}},
        .design_graph_bindings = &scoped_bindings};
    const auto result = codeActionsAt(context);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.actions.size() == 1);
    const auto& action = result.actions.front();
    CHECK(action.title == "Add missing port connections to 'u_b'");
    REQUIRE(action.edits.size() == 1);
    CHECK(action.edits.front().range.start_line == 3);
    CHECK(action.edits.front().range.start_character == 21);
    CHECK(action.edits.front().new_text == ", .rst_n(rst_n), .data(data)");
}

TEST_CASE("CodeActionProvider requires typed bindings for missing-port fixes",
           "[analysis][semantic][code-action-provider][ports][no-fallback]") {
    constexpr std::string_view uri = "file:///workspace/rtl/top.sv";
    const auto document_text = "module top;\n"
                               "  child u_child(.clk(clk));\n"
                               "endmodule\n";
    ModuleDefinition child{.name = "child",
                           .kind = "module",
                           .port_details = {SchematicPort{.name = "clk"},
                                            SchematicPort{.name = "rst_n"}}};
    ModuleSchematic top{.name = "top"};
    top.cells.push_back(SchematicCell{.id = "u_child",
                                      .name = "u_child",
                                      .type = "child",
                                      .kind = "module",
                                      .range = rangeAt(1, 2, 27),
                                      .selection_range = rangeAt(1, 8, 15),
                                      .connections = {SchematicConnection{.port_name = "clk",
                                                                           .signal = "clk",
                                                                           .range = rangeAt(1, 17, 26)}}});
    const CodeActionContext context{.generation = 12,
                                    .snapshot_available = true,
                                    .document = SemanticEngineDocument{.uri = std::string(uri),
                                                                       .text = document_text},
                                    .range = rangeAt(1, 2, 27),
                                    .modules_by_name = {{"child", child}},
                                    .document_schematics = {top}};

    const auto result = codeActionsAt(context);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.actions.empty());
}
TEST_CASE("CodeActionProvider creates macro definitions from slang macro diagnostics",
          "[analysis][semantic][code-action-provider][macro]") {
    constexpr std::string_view uri = "file:///workspace/rtl/top.sv";
    const auto document_text = "module top;\n"
                               "  assign value = `MISSING(ready, valid);\n"
                               "endmodule\n";

    CodeActionContext context{.generation = 10,
                              .snapshot_available = true,
                              .document = SemanticEngineDocument{.uri = std::string(uri),
                                                                 .text = document_text},
                              .range = rangeAt(1, 17, 25),
                              .macro_invocations = {MacroInvocationFact{
                                  .name = "MISSING",
                                  .range = rangeAt(1, 17, 39),
                                  .selection_range = rangeAt(1, 18, 25),
                                  .arguments = {"ready", "valid"},
                                  .argument_ranges = {rangeAt(1, 26, 31), rangeAt(1, 33, 38)},
                                  .function_like = true,
                                  .resolved = false}},
                              .diagnostics = {SemanticEngineDiagnostic{
                                  .uri = std::string(uri),
                                  .code = "slang:UnknownDirective",
                                  .message = "unknown macro or compiler directive '`MISSING'",
                                  .range = rangeAt(1, 18, 25),
                                  .severity = 1}}};

    const auto result = codeActionsAt(context);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.actions.size() == 1);
    const auto& action = result.actions.front();
    CHECK(action.title == "Define macro 'MISSING'");
    REQUIRE(action.diagnostics.size() == 1);
    CHECK(action.diagnostics.front().code == "slang:UnknownDirective");
    REQUIRE(action.edits.size() == 1);
    CHECK(action.edits.front().range.start_line == 0);
    CHECK(action.edits.front().new_text.find("`define MISSING(arg0, arg1) arg0") !=
          std::string::npos);
}

TEST_CASE("CodeActionProvider expands indexed object and function macros",
          "[analysis][semantic][code-action-provider][macro]") {
    constexpr std::string_view uri = "file:///workspace/rtl/top.sv";
    const auto document_text = "`define READY 1'b1\n"
                               "`define ADD(lhs, rhs) ((lhs) + (rhs))\n"
                               "module top;\n"
                               "  assign ready = `READY;\n"
                               "  assign sum = `ADD(lhs_value, rhs_value);\n"
                               "endmodule\n";

    CodeActionContext object_context{.generation = 12,
                                     .snapshot_available = true,
                                     .document = SemanticEngineDocument{.uri = std::string(uri),
                                                                        .text = document_text},
                                     .range = rangeAt(3, 18, 24),
                                     .macro_invocations = {MacroInvocationFact{
                                         .name = "READY",
                                         .definition_uri = std::string(uri),
                                         .definition = MacroDefinition{.name = "READY", .body = "1'b1"},
                                         .range = rangeAt(3, 17, 23),
                                         .selection_range = rangeAt(3, 18, 23),
                                         .expansion_text = "1'b1",
                                         .function_like = false,
                                         .resolved = true}}};
    const auto object_result = codeActionsAt(object_context);

    REQUIRE_FALSE(object_result.unresolved);
    REQUIRE(object_result.actions.size() == 1);
    CHECK(object_result.actions.front().title == "Expand macro 'READY'");
    REQUIRE(object_result.actions.front().edits.size() == 1);
    CHECK(object_result.actions.front().edits.front().new_text == "1'b1");

    CodeActionContext function_context{.generation = 12,
                                       .snapshot_available = true,
                                       .document = SemanticEngineDocument{.uri = std::string(uri),
                                                                          .text = document_text},
                                       .range = rangeAt(4, 15, 19),
                                       .macro_invocations = {MacroInvocationFact{
                                           .name = "ADD",
                                           .definition_uri = std::string(uri),
                                           .definition = MacroDefinition{.name = "ADD",
                                                                         .parameters = {"lhs", "rhs"},
                                                                         .body = "((lhs) + (rhs))",
                                                                         .function_like = true},
                                           .range = rangeAt(4, 15, 42),
                                           .selection_range = rangeAt(4, 16, 19),
                                           .arguments = {"lhs_value", "rhs_value"},
                                           .expansion_text = "((lhs_value) + (rhs_value))",
                                           .function_like = true,
                                           .resolved = true}}};
    const auto function_result = codeActionsAt(function_context);

    REQUIRE_FALSE(function_result.unresolved);
    REQUIRE(function_result.actions.size() == 1);
    CHECK(function_result.actions.front().title == "Expand macro 'ADD'");
    REQUIRE(function_result.actions.front().edits.size() == 1);
    CHECK(function_result.actions.front().edits.front().new_text == "((lhs_value) + (rhs_value))");
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
