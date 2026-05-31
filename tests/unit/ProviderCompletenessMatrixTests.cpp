#include "../../src/analysis/semantic/CompletionProvider.h"
#include "../../src/analysis/semantic/QueryCache.h"
#include "../../src/analysis/semantic/SignatureInlayProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <unordered_map>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

ParseRange lineSpan(int start_line, int end_line) {
    return ParseRange{.start_line = start_line,
                      .start_character = 0,
                      .end_line = end_line,
                      .end_character = 0};
}

SemanticLocation locationAt(std::string uri, ParseRange range) {
    return SemanticLocation{.uri = std::move(uri), .range = range};
}

SemanticWorkspaceSymbolResult workspaceSymbolResult(std::string name) {
    SemanticWorkspaceSymbolResult result;
    result.generation = 11;
    result.symbols.push_back(SemanticWorkspaceSymbol{.name = std::move(name),
                                                     .kind = 0,
                                                     .location = SemanticLocation{},
                                                     .selection_range = ParseRange{},
                                                     .stable_id = "symbol|cached"});
    return result;
}

} // namespace

TEST_CASE("QueryCache keeps diagnostics generation and URI scoped",
          "[analysis][semantic][query-cache][diagnostics]") {
    QueryCache cache;
    cache.storeDiagnostics(1,
                           "file:///workspace/a.sv",
                           {SemanticEngineDiagnostic{.uri = "file:///workspace/a.sv",
                                                     .code = "duplicateSymbol"}});

    REQUIRE(cache.diagnostics(1, "file:///workspace/a.sv").has_value());
    CHECK(cache.diagnostics(2, "file:///workspace/a.sv") == std::nullopt);
    CHECK(cache.diagnostics(1, "file:///workspace/b.sv") == std::nullopt);
}

TEST_CASE("QueryCache keys workspace symbols by query and limit",
          "[analysis][semantic][query-cache][workspace-symbol]") {
    QueryCache cache;
    cache.storeWorkspaceSymbols(3, "child", 1, workspaceSymbolResult("child"));

    REQUIRE(cache.workspaceSymbols(3, "child", 1).has_value());
    CHECK(cache.workspaceSymbols(3, "child", 2) == std::nullopt);
    CHECK(cache.workspaceSymbols(3, "top", 1) == std::nullopt);
}

TEST_CASE("QueryCache keys references by include-declaration flag",
          "[analysis][semantic][query-cache][references]") {
    QueryCache cache;
    SemanticReferenceResult result;
    result.generation = 4;
    result.locations.push_back(locationAt("file:///workspace/top.sv", rangeAt(1, 8, 13)));
    cache.storeReferences(4, "file:///workspace/top.sv", 1, 9, true, result);

    REQUIRE(cache.references(4, "file:///workspace/top.sv", 1, 9, true).has_value());
    CHECK(cache.references(4, "file:///workspace/top.sv", 1, 9, false) == std::nullopt);
}

TEST_CASE("QueryCache keys rename by replacement text",
          "[analysis][semantic][query-cache][rename]") {
    QueryCache cache;
    SemanticRenameResult result;
    result.generation = 5;
    result.edits.push_back(SemanticTextEdit{.location = locationAt("file:///workspace/top.sv",
                                                                   rangeAt(1, 8, 13)),
                                            .new_text = "valid"});
    cache.storeRename(5, "file:///workspace/top.sv", 1, 9, "valid", result);

    REQUIRE(cache.rename(5, "file:///workspace/top.sv", 1, 9, "valid").has_value());
    CHECK(cache.rename(5, "file:///workspace/top.sv", 1, 9, "ready") == std::nullopt);
}

TEST_CASE("QueryCache keys completion by prefix and position",
          "[analysis][semantic][query-cache][completion]") {
    QueryCache cache;
    SemanticCompletionResult result;
    result.generation = 6;
    result.items.push_back(SemanticCompletionItem{.label = "ready"});
    cache.storeCompletions(6, "file:///workspace/top.sv", 2, 10, "rea", result);

    REQUIRE(cache.completions(6, "file:///workspace/top.sv", 2, 10, "rea").has_value());
    CHECK(cache.completions(6, "file:///workspace/top.sv", 2, 10, "val") == std::nullopt);
    CHECK(cache.completions(6, "file:///workspace/top.sv", 2, 11, "rea") == std::nullopt);
}

TEST_CASE("QueryCache keys module hierarchy by module and depth",
          "[analysis][semantic][query-cache][hierarchy]") {
    QueryCache cache;
    SemanticModuleHierarchyResult result;
    result.generation = 7;
    result.roots.push_back(SemanticHierarchyNode{.module_name = "top"});
    cache.storeModuleHierarchy(7, std::string_view("top"), 4, result);

    REQUIRE(cache.moduleHierarchy(7, std::string_view("top"), 4).has_value());
    CHECK(cache.moduleHierarchy(7, std::string_view("top"), 8) == std::nullopt);
    CHECK(cache.moduleHierarchy(7, std::nullopt, 4) == std::nullopt);
}

TEST_CASE("QueryCache keys schematic by module and depth",
          "[analysis][semantic][query-cache][schematic]") {
    QueryCache cache;
    SemanticSchematicResult result;
    result.generation = 8;
    result.root_module_id = "top";
    cache.storeSchematic(8, std::string_view("top"), 4, result);

    REQUIRE(cache.schematic(8, std::string_view("top"), 4).has_value());
    CHECK(cache.schematic(8, std::string_view("child"), 4) == std::nullopt);
    CHECK(cache.schematic(9, std::string_view("top"), 4) == std::nullopt);
}

TEST_CASE("QueryCache keys backward cone by cursor position",
          "[analysis][semantic][query-cache][cone]") {
    QueryCache cache;
    SemanticConeTrace trace;
    trace.generation = 9;
    trace.nodes.push_back(SemanticConeNode{.id = "symbol|out",
                                           .name = "out",
                                           .location = SemanticLocation{},
                                           .bit_width = std::nullopt});
    cache.storeBackwardCone(9, "file:///workspace/cone.sv", 4, 9, trace);

    REQUIRE(cache.backwardCone(9, "file:///workspace/cone.sv", 4, 9).has_value());
    CHECK(cache.backwardCone(9, "file:///workspace/cone.sv", 4, 10) == std::nullopt);
}

TEST_CASE("QueryCache keys code actions by requested range",
          "[analysis][semantic][query-cache][code-action]") {
    QueryCache cache;
    SemanticCodeActionResult result;
    result.generation = 10;
    result.actions.push_back(SemanticCodeAction{.title = "Create typedef 'missing_t'",
                                                .kind = "quickfix",
                                                .is_preferred = false,
                                                .diagnostics = {},
                                                .edits = {},
                                                .create_files = {}});
    cache.storeCodeActions(10, "file:///workspace/top.sv", rangeAt(3, 2, 11), result);

    REQUIRE(cache.codeActions(10, "file:///workspace/top.sv", rangeAt(3, 2, 11)).has_value());
    CHECK(cache.codeActions(10, "file:///workspace/top.sv", rangeAt(3, 2, 12)) == std::nullopt);
}

TEST_CASE("QueryCache clear removes all provider entries",
          "[analysis][semantic][query-cache]") {
    QueryCache cache;
    cache.storeDiagnostics(1, "file:///workspace/top.sv", {});
    cache.storeWorkspaceSymbols(1, "", 10, SemanticWorkspaceSymbolResult{});
    cache.storeCompletions(1, "file:///workspace/top.sv", 1, 2, "", SemanticCompletionResult{});

    cache.clear();

    CHECK(cache.diagnostics(1, "file:///workspace/top.sv") == std::nullopt);
    CHECK(cache.workspaceSymbols(1, "", 10) == std::nullopt);
    CHECK(cache.completions(1, "file:///workspace/top.sv", 1, 2, "") == std::nullopt);
}

TEST_CASE("CompletionProvider detects array and hierarchical member contexts",
          "[analysis][semantic][completion-provider][member]") {
    const auto array_context = detectCompletionContext("module top;\n  lanes[0].sta\nendmodule\n",
                                                       1,
                                                       14,
                                                       "sta");
    REQUIRE(array_context.prefix_start.has_value());
    CHECK(array_context.member_access);
    CHECK_FALSE(array_context.member_qualifier.has_value());

    const auto instance_context = detectCompletionContext("module top;\n  u_child.data_\nendmodule\n",
                                                          1,
                                                          15,
                                                          "data_");
    REQUIRE(instance_context.prefix_start.has_value());
    CHECK(instance_context.member_access);
    CHECK(instance_context.member_qualifier == "u_child");
}

TEST_CASE("CompletionProvider detects package-qualified enum prefixes",
          "[analysis][semantic][completion-provider][package]") {
    const auto context = detectCompletionContext("module top;\n  defs::STATE_\nendmodule\n",
                                                 1,
                                                 14,
                                                 "STATE_");

    REQUIRE(context.prefix_start.has_value());
    CHECK(context.package_qualifier == "defs");
    CHECK_FALSE(context.member_access);
}

TEST_CASE("CompletionProvider builds three-argument macro snippets",
          "[analysis][semantic][completion-provider][macro]") {
    const MacroDefinition macro{.name = "MUX",
                                .parameters = {"sel", "lhs", "rhs"},
                                .body = "((sel) ? (lhs) : (rhs))",
                                .function_like = true};

    CHECK(macroSignatureLabel(macro) == "MUX(sel, lhs, rhs)");
    CHECK(macroInsertText(macro).find("${3:rhs}") != std::string::npos);
    CHECK(macroDocumentation(macro).find("Parameters: `sel, lhs, rhs`") != std::string::npos);
}

TEST_CASE("CompletionProvider builds parameterized module port snippets",
          "[analysis][semantic][completion-provider][module]") {
    const ModuleDefinition module{.name = "child",
                                  .kind = "module",
                                  .port_details = {SchematicPort{.name = "clk",
                                                                 .direction = "input",
                                                                 .width_text = "logic"},
                                                   SchematicPort{.name = "data",
                                                                 .direction = "input",
                                                                 .width_text = "logic [WIDTH-1:0]"},
                                                   SchematicPort{.name = "valid",
                                                                 .direction = "output",
                                                                 .width_text = "logic"}}};

    CHECK(moduleSignatureLabel(module).find("input logic [WIDTH-1:0] data") != std::string::npos);
    CHECK(moduleInstantiationSnippet(module).find(".valid(${4:valid})") != std::string::npos);
}

TEST_CASE("CompletionProvider excludes already-connected ports",
          "[analysis][semantic][completion-provider][ports]") {
    const ModuleDefinition module{.name = "child",
                                  .kind = "module",
                                  .port_details = {SchematicPort{.name = "clk",
                                                                 .direction = "input",
                                                                 .width_text = "logic"},
                                                   SchematicPort{.name = "data",
                                                                 .direction = "input",
                                                                 .width_text = "logic"}}};
    std::vector<SemanticCompletionItem> items;
    std::set<std::string> emitted;
    bool truncated = false;

    appendModulePortCompletions(items,
                                emitted,
                                "module|child",
                                module,
                                "file:///workspace/child.sv",
                                "",
                                {"clk"},
                                truncated);

    REQUIRE_FALSE(truncated);
    REQUIRE(items.size() == 1);
    CHECK(items.front().label == "data");
}

TEST_CASE("SignatureInlayProvider computes first function argument",
          "[analysis][semantic][signature-inlay-provider][signature][function]") {
    const std::string text = "module top;\n  int value = mix(lhs_value, rhs_value, mask_value);\nendmodule\n";
    const SignatureInlayContext context{
        .generation = 21,
        .document_uri = "file:///workspace/top.sv",
        .document_text = &text,
        .calls = {SignatureInlayCall{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = rangeAt(1, 14, 49),
                                     .selection_range = rangeAt(1, 14, 17),
                                     .parameters = {"input int lhs",
                                                    "input int rhs",
                                                    "input int mask"}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 1, 20);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.active_parameter == 0);
    CHECK(result.label == "function int mix(input int lhs, input int rhs, input int mask)");
}

TEST_CASE("SignatureInlayProvider computes third function argument",
          "[analysis][semantic][signature-inlay-provider][signature][function]") {
    const std::string text = "module top;\n  int value = mix(lhs_value, rhs_value, mask_value);\nendmodule\n";
    const SignatureInlayContext context{
        .generation = 22,
        .document_uri = "file:///workspace/top.sv",
        .document_text = &text,
        .calls = {SignatureInlayCall{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = rangeAt(1, 14, 49),
                                     .selection_range = rangeAt(1, 14, 17),
                                     .parameters = {"input int lhs",
                                                    "input int rhs",
                                                    "input int mask"}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 1, 47);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.active_parameter == 2);
}

TEST_CASE("SignatureInlayProvider filters argument hints by requested range",
          "[analysis][semantic][signature-inlay-provider][inlay]") {
    const std::string text = "module top;\n"
                             "  int a = mix(lhs_value, rhs_value);\n"
                             "  int b = mix(lhs_value, rhs_value);\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 23,
        .document_uri = "file:///workspace/top.sv",
        .document_text = &text,
        .calls = {SignatureInlayCall{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = rangeAt(1, 10, 35),
                                     .selection_range = rangeAt(1, 10, 13),
                                     .parameters = {"input int lhs", "input int rhs"}},
                  SignatureInlayCall{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = rangeAt(2, 10, 35),
                                     .selection_range = rangeAt(2, 10, 13),
                                     .parameters = {"input int lhs", "input int rhs"}}},
        .snapshot_available = true};

    const auto result = inlayHints(context, lineSpan(2, 3));

    REQUIRE_FALSE(result.unresolved);
    CHECK(std::all_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.location.range.start_line == 2;
    }));
}

TEST_CASE("SignatureInlayProvider emits ordered third port hints",
          "[analysis][semantic][signature-inlay-provider][inlay][ports]") {
    const ModuleDefinition child{.name = "child",
                                 .kind = "module",
                                 .port_details = {SchematicPort{.name = "clk",
                                                                .direction = "input",
                                                                .width_text = "logic"},
                                                  SchematicPort{.name = "ready",
                                                                .direction = "input",
                                                                .width_text = "logic"},
                                                  SchematicPort{.name = "data",
                                                                .direction = "output",
                                                                .width_text = "logic"}}};
    const std::unordered_map<std::string, ModuleDefinition> modules{{"child", child}};
    const SignatureInlayContext context{
        .generation = 24,
        .document_uri = "file:///workspace/top.sv",
        .modules_by_name = &modules,
        .module_instances = {SignatureInlayModuleInstance{
            .module_name = "child",
            .range = rangeAt(1, 2, 34),
            .selection_range = rangeAt(1, 8, 15),
            .connections = {SchematicConnection{.port_index = 0,
                                                .signal = "clk",
                                                .range = rangeAt(1, 16, 19)},
                            SchematicConnection{.port_index = 1,
                                                .signal = "ready",
                                                .range = rangeAt(1, 21, 26)},
                            SchematicConnection{.port_index = 2,
                                                .signal = "data",
                                                .range = rangeAt(1, 28, 32)}}}},
        .snapshot_available = true};

    const auto result = inlayHints(context, lineSpan(1, 2));

    REQUIRE_FALSE(result.unresolved);
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.label == ".data" && hint.tooltip == "output logic data";
    }));
}

} // namespace pristine::analysis::semantic
