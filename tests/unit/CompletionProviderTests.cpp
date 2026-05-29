#include "../../src/analysis/semantic/CompletionProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <unordered_map>

namespace pristine::analysis::semantic {
namespace {

TEST_CASE("CompletionProvider detects package, member, macro, and module contexts",
          "[analysis][semantic][completion-provider]") {
    {
        const auto context = detectCompletionContext("module top;\n  defs::thi\nendmodule\n",
                                                     1,
                                                     11,
                                                     "thi");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.package_qualifier == "defs");
        CHECK_FALSE(context.member_access);
        CHECK_FALSE(context.macro_invocation);
    }

    {
        const auto context = detectCompletionContext("module top;\n  u_child.da\nendmodule\n",
                                                     1,
                                                     12,
                                                     "da");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.member_access);
        CHECK(context.member_qualifier == "u_child");
    }

    {
        const auto context = detectCompletionContext("module top;\n  `AD\nendmodule\n",
                                                     1,
                                                     5,
                                                     "AD");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.macro_invocation);
    }

    {
        const auto context = detectCompletionContext("module top;\n  chi\nendmodule\n",
                                                     1,
                                                     5,
                                                     "chi");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.module_instantiation_position);
    }
}

TEST_CASE("CompletionProvider maps prefix start with UTF-16 positions",
          "[analysis][semantic][completion-provider][utf16]") {
    const std::string text = "module top;\n  logic smile_😀;\n  smile_😀\nendmodule\n";

    const auto start = completionPrefixStartOffset(text, 2, 8, "smile_");

    REQUIRE(start.has_value());
    CHECK(text.substr(*start, 6) == "smile_");
}

TEST_CASE("CompletionProvider builds module, port, macro, and symbol completion items",
          "[analysis][semantic][completion-provider]") {
    const ModuleDefinition module{.name = "child",
                                  .kind = "module",
                                  .range = ParseRange{.start_line = 0,
                                                      .start_character = 0,
                                                      .end_line = 0,
                                                      .end_character = 64},
                                  .selection_range = ParseRange{.start_line = 0,
                                                                .start_character = 7,
                                                                .end_line = 0,
                                                                .end_character = 12},
                                  .ports = {},
                                  .port_details = {SchematicPort{.name = "clk",
                                                                 .direction = "input",
                                                                 .width_text = "logic",
                                                                 .range = ParseRange{},
                                                                 .selection_range = ParseRange{}},
                                                   SchematicPort{.name = "rst_n",
                                                                 .direction = "output",
                                                                 .width_text = "logic",
                                                                 .range = ParseRange{},
                                                                 .selection_range = ParseRange{}}},
                                  .instances = {}};

    CHECK(moduleSignatureLabel(module) == "child(input logic clk, output logic rst_n)");
    CHECK(moduleInstantiationSnippet(module).find(".rst_n(${3:rst_n})") != std::string::npos);

    const MacroDefinition macro{.name = "ADD",
                                .parameters = {"lhs", "rhs"},
                                .body = "((lhs) + (rhs))",
                                .range = ParseRange{},
                                .selection_range = ParseRange{},
                                .function_like = true};
    CHECK(macroSignatureLabel(macro) == "ADD(lhs, rhs)");
    CHECK(macroInsertText(macro).find("${2:rhs}") != std::string::npos);
    CHECK(macroDocumentation(macro).find("Body:") != std::string::npos);

    std::vector<SemanticCompletionItem> items;
    std::set<std::string> emitted;
    bool truncated = false;
    appendModulePortCompletions(items,
                                emitted,
                                "module|child",
                                module,
                                "file:///workspace/child.sv",
                                "r",
                                {"clk"},
                                truncated);
    REQUIRE_FALSE(truncated);
    REQUIRE(items.size() == 1);
    CHECK(items.front().label == "rst_n");
    CHECK(items.front().stable_id == "module|child|port|rst_n");
    CHECK(items.front().documentation.find("Module: `child`") != std::string::npos);

    appendSymbolCompletion(items,
                           emitted,
                           SemanticSymbolIdentity{.stable_id = "symbol|ready",
                                                  .name = "ready",
                                                  .kind = "Variable",
                                                  .location = SemanticLocation{}},
                           "rea",
                           truncated);
    CHECK(std::any_of(items.begin(), items.end(), [](const SemanticCompletionItem& item) {
        return item.label == "ready" && item.detail == "Variable";
    }));

    appendCompletionItem(items,
                         emitted,
                         SemanticCompletionItem{.stable_id = "symbol|ready-copy",
                                                 .label = "ready",
                                                 .detail = "Variable",
                                                 .insert_text = "ready"},
                         "rea",
                         truncated);
    CHECK(std::count_if(items.begin(), items.end(), [](const SemanticCompletionItem& item) {
        return item.label == "ready";
    }) == 1);
}

TEST_CASE("CompletionProvider resolves completion documentation and snippets",
          "[analysis][semantic][completion-provider][resolve]") {
    const ModuleDefinition module{.name = "child",
                                  .kind = "module",
                                  .range = ParseRange{.start_line = 0,
                                                      .start_character = 0,
                                                      .end_line = 0,
                                                      .end_character = 64},
                                  .selection_range = ParseRange{.start_line = 0,
                                                                .start_character = 7,
                                                                .end_line = 0,
                                                                .end_character = 12},
                                  .ports = {},
                                  .port_details = {SchematicPort{.name = "clk",
                                                                 .direction = "input",
                                                                 .width_text = "logic",
                                                                 .range = ParseRange{},
                                                                 .selection_range = ParseRange{}},
                                                   SchematicPort{.name = "rst_n",
                                                                 .direction = "output",
                                                                 .width_text = "logic",
                                                                 .range = ParseRange{},
                                                                 .selection_range = ParseRange{}}},
                                  .instances = {}};
    const MacroDefinition macro{.name = "ADD",
                                .parameters = {"lhs", "rhs"},
                                .body = "((lhs) + (rhs))",
                                .range = ParseRange{},
                                .selection_range = ParseRange{},
                                .function_like = true};
    const std::unordered_map<std::string, ModuleDefinition> modules_by_name{{"child", module}};
    const std::unordered_map<std::string, std::string> module_uris_by_name{
        {"child", "file:///workspace/child.sv"}};
    const std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri{
        {"file:///workspace/macros.sv", {macro}}};

    CompletionResolveContext context{.modules_by_name = &modules_by_name,
                                     .module_uris_by_name = &module_uris_by_name,
                                     .macros_by_uri = &macros_by_uri};

    const auto port = resolveCompletionItem("module|child|port|rst_n", "rst_n", context);
    CHECK(port.detail == "output logic rst_n");
    CHECK(port.documentation.find("Module: `child`") != std::string::npos);
    CHECK(port.insert_text.find("rst_n(") != std::string::npos);

    const auto resolved_macro = resolveCompletionItem("file:///workspace/macros.sv|macro|ADD",
                                                      "ADD",
                                                      context);
    CHECK(resolved_macro.detail.find("Macro function") != std::string::npos);
    CHECK(resolved_macro.documentation.find("Parameters: `lhs, rhs`") != std::string::npos);
    CHECK(resolved_macro.insert_text.find("${2:rhs}") != std::string::npos);

    context.symbol = CompletionResolveSymbol{
        .identity = SemanticSymbolIdentity{.stable_id = "symbol|child",
                                           .name = "child",
                                           .kind = "Definition",
                                           .location = SemanticLocation{}},
        .type_display = {}};
    const auto resolved_module = resolveCompletionItem("symbol|child", "child", context);
    CHECK(resolved_module.detail == "child(input logic clk, output logic rst_n)");
    CHECK(resolved_module.documentation.find("Declared: `file:///workspace/child.sv:1:8`") !=
          std::string::npos);
    CHECK(resolved_module.insert_text.find(".rst_n(${3:rst_n})") != std::string::npos);

    context.symbol = CompletionResolveSymbol{
        .identity = SemanticSymbolIdentity{.stable_id = "symbol|value",
                                           .name = "value",
                                           .kind = "Variable",
                                           .location = SemanticLocation{}},
        .type_display = "logic [7:0]"};
    const auto resolved_symbol = resolveCompletionItem("symbol|value", "value", context);
    CHECK(resolved_symbol.detail == "Variable");
    CHECK(resolved_symbol.documentation.find("Type: `logic [7:0]`") != std::string::npos);

    context.symbol.reset();
    const auto missing = resolveCompletionItem("symbol|missing", "missing", context);
    CHECK(missing.unresolved);
}

} // namespace
} // namespace pristine::analysis::semantic
