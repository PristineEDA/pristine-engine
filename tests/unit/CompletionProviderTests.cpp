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

TEST_CASE("CompletionProvider detects array and hierarchical member qualifiers",
          "[analysis][semantic][completion-provider][member]") {
    {
        const auto context = detectCompletionContext("module top;\n  lanes[0].status_\nendmodule\n",
                                                     1,
                                                     18,
                                                     "status_");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.member_access);
        CHECK(context.member_qualifier == "lanes[0]");
    }

    {
        const auto context = detectCompletionContext("module top;\n  bus.master.status_\nendmodule\n",
                                                     1,
                                                     20,
                                                     "status_");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.member_access);
        CHECK(context.member_qualifier == "bus.master");
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

TEST_CASE("CompletionProvider appends member completions from provider-facing candidates",
          "[analysis][semantic][completion-provider][member]") {
    CompletionMemberContext context;
    context.qualifier = "lanes[0]";
    context.candidates = {CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|lanes",
                         .name = "lanes",
                         .kind = "Variable",
                         .location = SemanticLocation{}}},
                          CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|status_ready",
                         .name = "status_ready",
                         .kind = "Field",
                         .location = SemanticLocation{}}},
                          CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|payload",
                         .name = "payload",
                         .kind = "Field",
                         .location = SemanticLocation{}}},
                          CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|child",
                         .name = "child",
                         .kind = "Definition",
                         .location = SemanticLocation{}}}};

    std::vector<SemanticCompletionItem> items;
    std::set<std::string> emitted;
    bool truncated = false;

    appendMemberCompletions(items, emitted, context, "status_", truncated);

    REQUIRE_FALSE(truncated);
    REQUIRE(items.size() == 1);
    CHECK(items.front().label == "status_ready");
    CHECK(items.front().detail == "Variable");
}

TEST_CASE("CompletionProvider computes connected named ports within an instance range",
          "[analysis][semantic][completion-provider][ports]") {
    const std::string text = "module top;\n  child u_child(.clk(clk), .ready(ready), .da);\nendmodule\n";

    const auto connected = connectedNamedPortsForInstance(text,
                                                          1,
                                                          45,
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 8,
                                                                     .end_line = 1,
                                                                     .end_character = 15},
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 2,
                                                                     .end_line = 1,
                                                                     .end_character = 47});

    CHECK(connected.contains("clk"));
    CHECK(connected.contains("ready"));
    CHECK_FALSE(connected.contains("da"));
}

TEST_CASE("CompletionProvider ignores nested named ports after the cursor",
          "[analysis][semantic][completion-provider][ports]") {
    const std::string text = "module top;\n  child u_child(.clk(clk), .ready(ready), .data(data));\nendmodule\n";

    const auto connected = connectedNamedPortsForInstance(text,
                                                          1,
                                                          27,
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 8,
                                                                     .end_line = 1,
                                                                     .end_character = 15},
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 2,
                                                                     .end_line = 1,
                                                                     .end_character = 55});

    CHECK(connected.contains("clk"));
    CHECK_FALSE(connected.contains("ready"));
    CHECK_FALSE(connected.contains("data"));
}

TEST_CASE("CompletionProvider returns no connected ports for invalid instance ranges",
          "[analysis][semantic][completion-provider][ports]") {
    const std::string text = "module top;\n  child u_child(.clk(clk));\nendmodule\n";

    const auto connected = connectedNamedPortsForInstance(text,
                                                          1,
                                                          31,
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 8,
                                                                     .end_line = 1,
                                                                     .end_character = 15},
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 2,
                                                                     .end_line = 1,
                                                                     .end_character = 200});

    CHECK(connected.empty());
}

TEST_CASE("CompletionProvider ignores nested function call named arguments in port exclusion",
          "[analysis][semantic][completion-provider][ports]") {
    const std::string text =
        "module top;\n  child u_child(.clk(clk), .data(func(.inner(sig))), .re);\nendmodule\n";

    const auto connected = connectedNamedPortsForInstance(text,
                                                          1,
                                                          53,
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 8,
                                                                     .end_line = 1,
                                                                     .end_character = 15},
                                                          ParseRange{.start_line = 1,
                                                                     .start_character = 2,
                                                                     .end_line = 1,
                                                                     .end_character = 58});

    CHECK(connected.contains("clk"));
    CHECK(connected.contains("data"));
    CHECK_FALSE(connected.contains("inner"));
    CHECK_FALSE(connected.contains("re"));
}

TEST_CASE("CompletionProvider member completions are case insensitive and deduped",
          "[analysis][semantic][completion-provider][member]") {
    CompletionMemberContext context;
    context.qualifier = "bus.master";
    context.candidates = {CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|status_ready",
                         .name = "status_ready",
                         .kind = "Field"}},
                          CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|status_ready_duplicate",
                         .name = "status_ready",
                         .kind = "Field"}},
                          CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                         .stable_id = "symbol|STATUS_ERROR",
                         .name = "STATUS_ERROR",
                         .kind = "Field"}}};
    std::vector<SemanticCompletionItem> items;
    std::set<std::string> emitted;
    bool truncated = false;

    appendMemberCompletions(items, emitted, context, "status_", truncated);

    REQUIRE_FALSE(truncated);
    CHECK(items.size() == 2);
    CHECK(std::count_if(items.begin(), items.end(), [](const auto& item) {
        return item.label == "status_ready";
    }) == 1);
    CHECK(std::any_of(items.begin(), items.end(), [](const auto& item) {
        return item.label == "STATUS_ERROR";
    }));
}

TEST_CASE("CompletionProvider stops member completions at the provider cap",
          "[analysis][semantic][completion-provider][member][truncated]") {
    CompletionMemberContext context;
    context.qualifier = "bus";
    for (int index = 0; index < 2010; ++index) {
        context.candidates.push_back(CompletionMemberCandidate{.identity = SemanticSymbolIdentity{
                                      .stable_id = "symbol|field" + std::to_string(index),
                                      .name = "field" + std::to_string(index),
                                      .kind = "Field"}});
    }
    std::vector<SemanticCompletionItem> items;
    std::set<std::string> emitted;
    bool truncated = false;

    appendMemberCompletions(items, emitted, context, "field", truncated);

    CHECK(truncated);
    CHECK(items.size() == 2000);
}

TEST_CASE("CompletionProvider preserves unresolved resolve result for missing symbols",
          "[analysis][semantic][completion-provider][resolve]") {
    const CompletionResolveContext context;

    const auto resolved = resolveCompletionItem("symbol|missing", "missing", context);

    CHECK(resolved.unresolved);
    CHECK(resolved.label == "missing");
    CHECK(resolved.insert_text == "missing");
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
