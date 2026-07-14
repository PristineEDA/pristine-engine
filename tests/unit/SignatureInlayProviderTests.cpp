#include "../../src/analysis/semantic/SignatureInlayProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <unordered_map>

namespace pristine::analysis::semantic {
namespace {

TEST_CASE("SignatureInlayProvider computes module signature active parameter",
          "[analysis][semantic][signature-inlay-provider][signature]") {
    const std::string text = "module top;\n"
                             "  child u_child(.clk(clk), .rst_n(rst_n));\n"
                             "endmodule\n";
    const ModuleDefinition child{.name = "child",
                                 .kind = "module",
                                 .range = ParseRange{},
                                 .selection_range = ParseRange{},
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
    const std::unordered_map<std::string, ModuleDefinition> modules{{"child", child}};

    const SignatureInlayContext context{
        .generation = 7,
        .document_uri = "file:///workspace/top.sv",
        .modules_by_name = &modules,
        .module_instances = {SignatureInlayModuleInstance{.module_name = "child",
                                                          .range = ParseRange{.start_line = 1,
                                                                              .start_character = 2,
                                                                              .end_line = 1,
                                                                              .end_character = 42},
                                                          .selection_range = ParseRange{.start_line = 1,
                                                                                        .start_character = 8,
                                                                                        .end_line = 1,
                                                                                        .end_character = 15},
                                                          .connections = {
                                                              SchematicConnection{.port_index = 0,
                                                                                  .range = ParseRange{.start_line = 1, .start_character = 16, .end_line = 1, .end_character = 25}},
                                                              SchematicConnection{.port_index = 1,
                                                                                  .range = ParseRange{.start_line = 1, .start_character = 27, .end_line = 1, .end_character = 40}}}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 1, 33);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 7);
    CHECK(result.label == "child(input logic clk, output logic rst_n)");
    REQUIRE(result.parameters.size() == 2);
    CHECK(result.parameters[1] == "output logic rst_n");
    CHECK(result.active_parameter == 1);
}

TEST_CASE("SignatureInlayProvider computes macro function active parameter",
          "[analysis][semantic][signature-inlay-provider][signature][macro][no-fallback]") {
    const std::string text = "`define ADD(lhs, rhs) ((lhs) + (rhs))\n"
                             "module top;\n"
                             "  assign value = `ADD(a, b);\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 13,
        .document_uri = "file:///workspace/top.sv",
        .macro_invocations = {MacroInvocationFact{
            .name = "ADD",
            .definition_uri = "file:///workspace/top.sv",
            .definition = MacroDefinition{.name = "ADD",
                                          .parameters = {"lhs", "rhs"},
                                          .body = "((lhs) + (rhs))",
                                          .range = ParseRange{.start_line = 0, .end_line = 0},
                                          .selection_range = ParseRange{.start_line = 0,
                                                                        .start_character = 8,
                                                                        .end_line = 0,
                                                                        .end_character = 11},
                                          .function_like = true},
            .range = ParseRange{.start_line = 2, .start_character = 17, .end_line = 2, .end_character = 27},
            .selection_range = ParseRange{.start_line = 2, .start_character = 18, .end_line = 2, .end_character = 21},
            .arguments = {"a", "b"},
            .argument_ranges = {ParseRange{.start_line = 2, .start_character = 22, .end_line = 2, .end_character = 23},
                                ParseRange{.start_line = 2, .start_character = 25, .end_line = 2, .end_character = 26}},
            .function_like = true,
            .resolved = true}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 2, 25);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 13);
    CHECK(result.label == "ADD(lhs, rhs)");
    REQUIRE(result.parameters.size() == 2);
    CHECK(result.parameters[0] == "lhs");
    CHECK(result.parameters[1] == "rhs");
    CHECK(result.active_parameter == 1);
}

TEST_CASE("SignatureInlayProvider computes function and task active parameters",
          "[analysis][semantic][signature-inlay-provider][signature][function][task]") {
    const std::string text = "module top;\n"
                             "  function automatic int add(input int lhs, input int rhs);\n"
                             "    return lhs + rhs;\n"
                             "  endfunction\n"
                             "  initial begin\n"
                             "    int value = add(a, b);\n"
                             "  end\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 17,
        .document_uri = "file:///workspace/top.sv",
        .callable_invocations = {CallableInvocationFact{.name = "add",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = ParseRange{.start_line = 5,
                                                         .start_character = 16,
                                                         .end_line = 5,
                                                         .end_character = 25},
                                     .selection_range = ParseRange{.start_line = 5,
                                                                   .start_character = 16,
                                                                   .end_line = 5,
                                                                   .end_character = 19},
                                     .parameters = {"input int lhs", "input int rhs"},
                                     .argument_ranges = {ParseRange{.start_line = 5, .start_character = 20, .end_line = 5, .end_character = 21},
                                                         ParseRange{.start_line = 5, .start_character = 23, .end_line = 5, .end_character = 24}}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 5, 24);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 17);
    CHECK(result.label == "function int add(input int lhs, input int rhs)");
    REQUIRE(result.parameters.size() == 2);
    CHECK(result.parameters[1] == "input int rhs");
    CHECK(result.active_parameter == 1);
}

TEST_CASE("SignatureInlayProvider chooses the innermost nested function call",
          "[analysis][semantic][signature-inlay-provider][signature][function][nested]") {
    const std::string text = "module top;\n"
                             "  int value = mix(pack(a, b), rhs_value);\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 21,
        .document_uri = "file:///workspace/top.sv",
        .callable_invocations = {CallableInvocationFact{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = ParseRange{.start_line = 1,
                                                         .start_character = 14,
                                                         .end_line = 1,
                                                         .end_character = 40},
                                     .selection_range = ParseRange{.start_line = 1,
                                                                   .start_character = 14,
                                                                   .end_line = 1,
                                                                   .end_character = 17},
                                     .parameters = {"input int lhs", "input int rhs"},
                                     .argument_ranges = {ParseRange{.start_line = 1, .start_character = 18, .end_line = 1, .end_character = 28},
                                                         ParseRange{.start_line = 1, .start_character = 30, .end_line = 1, .end_character = 39}}},
                  CallableInvocationFact{.name = "pack",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = ParseRange{.start_line = 1,
                                                         .start_character = 18,
                                                         .end_line = 1,
                                                         .end_character = 28},
                                     .selection_range = ParseRange{.start_line = 1,
                                                                   .start_character = 18,
                                                                   .end_line = 1,
                                                                   .end_character = 22},
                                     .parameters = {"input int a", "input int b"},
                                     .argument_ranges = {ParseRange{.start_line = 1, .start_character = 23, .end_line = 1, .end_character = 24},
                                                         ParseRange{.start_line = 1, .start_character = 26, .end_line = 1, .end_character = 27}}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 1, 27);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.label == "function int pack(input int a, input int b)");
    REQUIRE(result.parameters.size() == 2);
    CHECK(result.parameters[1] == "input int b");
    CHECK(result.active_parameter == 1);
}

TEST_CASE("SignatureInlayProvider emits type and instance inlay hints in location order",
          "[analysis][semantic][signature-inlay-provider][inlay]") {
    const ModuleDefinition child{.name = "child",
                                 .kind = "module",
                                 .range = ParseRange{},
                                 .selection_range = ParseRange{},
                                 .ports = {"clk"},
                                 .port_details = {},
                                 .instances = {}};
    const std::unordered_map<std::string, ModuleDefinition> modules{{"child", child}};
    const SignatureInlayContext context{
        .generation = 11,
        .document_uri = "file:///workspace/top.sv",
        .modules_by_name = &modules,
        .symbols = {SignatureInlaySymbol{
            .identity = SemanticSymbolIdentity{.stable_id = "symbol|value",
                                               .name = "value",
                                               .kind = "Variable",
                                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                            .range = ParseRange{.start_line = 2,
                                                                                                .start_character = 9,
                                                                                                .end_line = 2,
                                                                                                .end_character = 14}}},
            .type_display = "logic [3:0]"}},
        .module_instances = {SignatureInlayModuleInstance{.module_name = "child",
                                                          .range = ParseRange{.start_line = 1,
                                                                              .start_character = 2,
                                                                              .end_line = 1,
                                                                              .end_character = 18},
                                                          .selection_range = ParseRange{.start_line = 1,
                                                                                        .start_character = 8,
                                                                                        .end_line = 1,
                                                                                        .end_character = 15}}},
        .snapshot_available = true};

    const auto result = inlayHints(context,
                                  ParseRange{.start_line = 0,
                                             .start_character = 0,
                                             .end_line = 4,
                                             .end_character = 0});

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.hints.size() == 2);
    CHECK(result.hints[0].label == ": child");
    CHECK(result.hints[0].tooltip == "child(clk)");
    CHECK(result.hints[1].label == ": logic [3:0]");
    CHECK(result.hints[1].tooltip == "Resolved type");
}

TEST_CASE("SignatureInlayProvider emits constant value inlay hints",
          "[analysis][semantic][signature-inlay-provider][inlay][constant]") {
    const SignatureInlayContext context{
        .generation = 12,
        .document_uri = "file:///workspace/top.sv",
        .symbols = {SignatureInlaySymbol{
            .identity = SemanticSymbolIdentity{.stable_id = "symbol|WIDTH",
                                               .name = "WIDTH",
                                               .kind = "Parameter",
                                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                            .range = ParseRange{.start_line = 1,
                                                                                                .start_character = 17,
                                                                                                .end_line = 1,
                                                                                                .end_character = 22}}},
            .type_display = "int",
            .value_display = "8"}},
        .snapshot_available = true};

    const auto result = inlayHints(context,
                                  ParseRange{.start_line = 0,
                                             .start_character = 0,
                                             .end_line = 3,
                                             .end_character = 0});

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.hints.size() == 2);
    CHECK(result.hints[0].label == ": int");
    CHECK(result.hints[0].tooltip == "Resolved type");
    CHECK(result.hints[1].label == " = 8");
    CHECK(result.hints[1].kind == "parameter");
    CHECK(result.hints[1].tooltip == "Resolved constant value");
}

TEST_CASE("SignatureInlayProvider emits AST-derived named and ordered port inlay hints",
          "[analysis][semantic][signature-inlay-provider][inlay][ports]") {
    const ModuleDefinition child{.name = "child",
                                 .kind = "module",
                                 .range = ParseRange{},
                                 .selection_range = ParseRange{},
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
                                                                .selection_range = ParseRange{}}}};
    const std::unordered_map<std::string, ModuleDefinition> modules{{"child", child}};
    const SignatureInlayContext context{
        .generation = 12,
        .document_uri = "file:///workspace/top.sv",
        .modules_by_name = &modules,
        .module_instances = {SignatureInlayModuleInstance{
            .module_name = "child",
            .range = ParseRange{.start_line = 1, .start_character = 2, .end_line = 1, .end_character = 34},
            .selection_range = ParseRange{.start_line = 1,
                                          .start_character = 8,
                                          .end_line = 1,
                                          .end_character = 15},
            .connections = {SchematicConnection{.port_name = "clk",
                                                .port_index = 0,
                                                .signal = "clk",
                                                .range = ParseRange{.start_line = 1,
                                                                    .start_character = 17,
                                                                    .end_line = 1,
                                                                    .end_character = 25}},
                            SchematicConnection{.port_index = 1,
                                                .signal = "rst_n",
                                                .range = ParseRange{.start_line = 1,
                                                                    .start_character = 27,
                                                                    .end_line = 1,
                                                                    .end_character = 32}}}}},
        .snapshot_available = true};

    const auto result = inlayHints(context,
                                  ParseRange{.start_line = 1,
                                             .start_character = 0,
                                             .end_line = 1,
                                             .end_character = 40});

    REQUIRE_FALSE(result.unresolved);
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == ".clk" &&
               hint.location.range.start_character == 17 &&
               hint.tooltip == "input logic clk";
    }));
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == ".rst_n" &&
               hint.location.range.start_character == 27 &&
               hint.tooltip == "output logic rst_n";
    }));
}

TEST_CASE("SignatureInlayProvider emits wildcard port labels from indexed port names",
          "[analysis][semantic][signature-inlay-provider][inlay][ports][wildcard]") {
    const ModuleDefinition child{.name = "child",
                                 .kind = "module",
                                 .port_details = {SchematicPort{.name = "clk",
                                                                .direction = "input",
                                                                .width_text = "logic"},
                                                  SchematicPort{.name = "ready",
                                                                .direction = "output",
                                                                .width_text = "logic"}}};
    const std::unordered_map<std::string, ModuleDefinition> modules{{"child", child}};
    const SignatureInlayContext context{
        .generation = 19,
        .document_uri = "file:///workspace/top.sv",
        .modules_by_name = &modules,
        .module_instances = {SignatureInlayModuleInstance{
            .module_name = "child",
            .range = ParseRange{.start_line = 1, .start_character = 2, .end_line = 1, .end_character = 19},
            .selection_range = ParseRange{.start_line = 1,
                                          .start_character = 8,
                                          .end_line = 1,
                                          .end_character = 15},
            .connections = {SchematicConnection{.port_name = "clk",
                                                .signal = "clk",
                                                .range = ParseRange{.start_line = 1,
                                                                    .start_character = 16,
                                                                    .end_line = 1,
                                                                    .end_character = 18}},
                            SchematicConnection{.port_name = "ready",
                                                .signal = "ready",
                                                .range = ParseRange{.start_line = 1,
                                                                    .start_character = 16,
                                                                    .end_line = 1,
                                                                    .end_character = 18}}}}},
        .snapshot_available = true};

    const auto result = inlayHints(context,
                                  ParseRange{.start_line = 1,
                                             .start_character = 0,
                                             .end_line = 1,
                                             .end_character = 24});

    REQUIRE_FALSE(result.unresolved);
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.label == ".clk" && hint.tooltip == "input logic clk";
    }));
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.label == ".ready" && hint.tooltip == "output logic ready";
    }));
}

TEST_CASE("SignatureInlayProvider emits AST-derived function and task argument hints",
          "[analysis][semantic][signature-inlay-provider][inlay][function][task][no-fallback]") {
    const std::string text = "module top;\n"
                             "  initial begin\n"
                             "    int value = add(lhs_value, rhs_value);\n"
                             "    emit(ready);\n"
                             "  end\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 18,
        .document_uri = "file:///workspace/top.sv",
        .callable_invocations = {CallableInvocationFact{.name = "add",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = ParseRange{.start_line = 2,
                                                         .start_character = 16,
                                                         .end_line = 2,
                                                         .end_character = 41},
                                     .selection_range = ParseRange{.start_line = 2,
                                                                   .start_character = 16,
                                                                   .end_line = 2,
                                                                   .end_character = 19},
                                     .parameters = {"input int lhs", "input int rhs"},
                                     .argument_ranges = {ParseRange{.start_line = 2, .start_character = 20, .end_line = 2, .end_character = 29},
                                                         ParseRange{.start_line = 2, .start_character = 31, .end_line = 2, .end_character = 40}}},
                  CallableInvocationFact{.name = "emit",
                                     .kind = "task",
                                     .range = ParseRange{.start_line = 3,
                                                         .start_character = 4,
                                                         .end_line = 3,
                                                         .end_character = 15},
                                     .selection_range = ParseRange{.start_line = 3,
                                                                   .start_character = 4,
                                                                   .end_line = 3,
                                                                   .end_character = 8},
                                     .parameters = {"input logic ready"},
                                     .argument_ranges = {ParseRange{.start_line = 3, .start_character = 9, .end_line = 3, .end_character = 14}}}},
        .snapshot_available = true};

    const auto result = inlayHints(context,
                                  ParseRange{.start_line = 0,
                                             .start_character = 0,
                                             .end_line = 5,
                                             .end_character = 0});

    REQUIRE_FALSE(result.unresolved);
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input int lhs:" &&
               hint.location.range.start_line == 2 &&
               hint.location.range.start_character == 20 &&
               hint.tooltip == "function int add(input int lhs, input int rhs)";
    }));
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input int rhs:" &&
               hint.location.range.start_line == 2 &&
               hint.location.range.start_character == 31 &&
               hint.tooltip == "function int add(input int lhs, input int rhs)";
    }));
    CHECK(std::any_of(result.hints.begin(), result.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input logic ready:" &&
               hint.location.range.start_line == 3 &&
               hint.location.range.start_character == 9 &&
               hint.tooltip == "task emit(input logic ready)";
    }));
}

TEST_CASE("SignatureInlayProvider clamps function active parameter at the indexed parameter count",
          "[analysis][semantic][signature-inlay-provider][signature][function]") {
    const std::string text = "module top;\n"
                             "  int value = mix(lhs, rhs, extra);\n"
                             "endmodule\n";
    const SignatureInlayContext context{
        .generation = 20,
        .document_uri = "file:///workspace/top.sv",
        .callable_invocations = {CallableInvocationFact{.name = "mix",
                                     .kind = "function",
                                     .return_type = "int",
                                     .range = ParseRange{.start_line = 1,
                                                         .start_character = 14,
                                                         .end_line = 1,
                                                         .end_character = 34},
                                     .selection_range = ParseRange{.start_line = 1,
                                                                   .start_character = 14,
                                                                   .end_line = 1,
                                                                   .end_character = 17},
                                     .parameters = {"input int lhs", "input int rhs"},
                                     .argument_ranges = {ParseRange{.start_line = 1, .start_character = 18, .end_line = 1, .end_character = 21},
                                                         ParseRange{.start_line = 1, .start_character = 23, .end_line = 1, .end_character = 26},
                                                         ParseRange{.start_line = 1, .start_character = 28, .end_line = 1, .end_character = 33}}}},
        .snapshot_available = true};

    const auto result = signatureHelpAt(context, 1, 33);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.active_parameter == 1);
}

TEST_CASE("SignatureInlayProvider reports unresolved snapshot and document states",
          "[analysis][semantic][signature-inlay-provider][unresolved]") {
    {
        const SignatureInlayContext context{.generation = 3, .snapshot_available = false};
        const auto result = signatureHelpAt(context, 0, 0);
        CHECK(result.unresolved);
        REQUIRE_FALSE(result.messages.empty());
        CHECK(result.messages.front().find("snapshot is unavailable") != std::string::npos);
    }

    {
        const SignatureInlayContext context{.generation = 4, .snapshot_available = true};
        const auto result = signatureHelpAt(context, 0, 0);
        CHECK(result.unresolved);
        REQUIRE_FALSE(result.messages.empty());
        CHECK(result.messages.front().find("no AST-backed signature invocation") != std::string::npos);
    }
}

TEST_CASE("SignatureInlayProvider reports URI-local callable scan counts",
          "[analysis][semantic][signature-inlay-provider][telemetry][callable]") {
    const SignatureInlayContext context{
        .generation = 80,
        .document_uri = "file:///workspace/top.sv",
        .callable_invocations = {CallableInvocationFact{
            .target_stable_id = "fn:add",
            .name = "add",
            .kind = "function",
            .return_type = "int",
            .range = ParseRange{.start_line = 2, .start_character = 10, .end_line = 2, .end_character = 19},
            .parameters = {"input int value"},
            .argument_ranges = {ParseRange{.start_line = 2, .start_character = 14, .end_line = 2, .end_character = 18}},
            .resolved = true}},
        .snapshot_available = true};
    const auto result = signatureHelpAt(context, 2, 16);
    CHECK_FALSE(result.unresolved);
    CHECK(result.scanned_invocation_count == 1);
    CHECK(result.scanned_macro_definition_count == 0);
    CHECK(result.scanned_global_symbol_count == 0);
}

TEST_CASE("SignatureInlayProvider reports visible macro scan counts",
          "[analysis][semantic][signature-inlay-provider][telemetry][macro]") {
    const SignatureInlayContext context{
        .generation = 81,
        .document_uri = "file:///workspace/top.sv",
        .macro_invocations = {
            MacroInvocationFact{.name = "FIRST",
                                .definition = MacroDefinition{.name = "FIRST", .parameters = {"x"}, .function_like = true},
                                .range = ParseRange{.start_line = 1, .start_character = 0, .end_line = 1, .end_character = 9},
                                .argument_ranges = {ParseRange{.start_line = 1, .start_character = 7, .end_line = 1, .end_character = 8}},
                                .function_like = true,
                                .resolved = true},
            MacroInvocationFact{.name = "SECOND",
                                .definition = MacroDefinition{.name = "SECOND", .parameters = {"x"}, .function_like = true},
                                .range = ParseRange{.start_line = 2, .start_character = 0, .end_line = 2, .end_character = 10},
                                .argument_ranges = {ParseRange{.start_line = 2, .start_character = 8, .end_line = 2, .end_character = 9}},
                                .function_like = true,
                                .resolved = true}},
        .snapshot_available = true};
    const auto result = signatureHelpAt(context, 2, 8);
    CHECK(result.label == "SECOND(x)");
    CHECK(result.scanned_macro_definition_count == 2);
    CHECK(result.scanned_global_symbol_count == 0);
}

TEST_CASE("SignatureInlayProvider unresolved callable never scans global symbols",
          "[analysis][semantic][signature-inlay-provider][unresolved][no-global-scan]") {
    const SignatureInlayContext context{
        .generation = 82,
        .callable_invocations = {CallableInvocationFact{
            .name = "missing",
            .range = ParseRange{.start_line = 3, .start_character = 2, .end_line = 3, .end_character = 12},
            .resolved = false}},
        .snapshot_available = true};
    const auto result = signatureHelpAt(context, 3, 7);
    CHECK(result.unresolved);
    CHECK(result.scanned_invocation_count == 1);
    CHECK(result.scanned_global_symbol_count == 0);
}

TEST_CASE("SignatureInlayProvider chooses the narrowest nested invocation fact",
          "[analysis][semantic][signature-inlay-provider][nested]") {
    const SignatureInlayContext context{
        .generation = 83,
        .callable_invocations = {
            CallableInvocationFact{.name = "outer",
                                   .kind = "function",
                                   .range = ParseRange{.start_line = 4, .start_character = 0, .end_line = 4, .end_character = 20},
                                   .parameters = {"input int value"},
                                   .resolved = true},
            CallableInvocationFact{.name = "inner",
                                   .kind = "function",
                                   .range = ParseRange{.start_line = 4, .start_character = 6, .end_line = 4, .end_character = 14},
                                   .parameters = {"input int value"},
                                   .resolved = true}},
        .snapshot_available = true};
    const auto result = signatureHelpAt(context, 4, 10);
    CHECK(result.label.find("inner") != std::string::npos);
    CHECK(result.scanned_invocation_count == 2);
}

TEST_CASE("SignatureInlayProvider macro active parameter uses indexed argument ranges",
          "[analysis][semantic][signature-inlay-provider][macro][active-parameter]") {
    const SignatureInlayContext context{
        .generation = 84,
        .macro_invocations = {MacroInvocationFact{
            .name = "PAIR",
            .definition = MacroDefinition{.name = "PAIR", .parameters = {"left", "right"}, .function_like = true},
            .range = ParseRange{.start_line = 5, .start_character = 2, .end_line = 5, .end_character = 18},
            .argument_ranges = {ParseRange{.start_line = 5, .start_character = 8, .end_line = 5, .end_character = 11},
                                ParseRange{.start_line = 5, .start_character = 13, .end_line = 5, .end_character = 17}},
            .function_like = true,
            .resolved = true}},
        .snapshot_available = true};
    const auto result = signatureHelpAt(context, 5, 15);
    CHECK(result.active_parameter == 1);
    CHECK(result.parameters == std::vector<std::string>{"left", "right"});
}

TEST_CASE("SignatureInlayProvider inlay telemetry counts only indexed invocation facts",
          "[analysis][semantic][signature-inlay-provider][inlay][telemetry]") {
    const SignatureInlayContext context{
        .generation = 85,
        .document_uri = "file:///workspace/top.sv",
        .module_instances = {SignatureInlayModuleInstance{
            .module_name = "child",
            .range = ParseRange{.start_line = 1, .start_character = 0, .end_line = 1, .end_character = 10},
            .selection_range = ParseRange{.start_line = 1, .start_character = 0, .end_line = 1, .end_character = 5}}},
        .callable_invocations = {CallableInvocationFact{
            .name = "add",
            .kind = "function",
            .range = ParseRange{.start_line = 2, .start_character = 0, .end_line = 2, .end_character = 8},
            .parameters = {"value"},
            .argument_ranges = {ParseRange{.start_line = 2, .start_character = 4, .end_line = 2, .end_character = 7}},
            .resolved = true}},
        .macro_invocations = {MacroInvocationFact{
            .name = "ONE",
            .definition = MacroDefinition{.name = "ONE", .parameters = {"value"}, .function_like = true},
            .range = ParseRange{.start_line = 3, .start_character = 0, .end_line = 3, .end_character = 8},
            .argument_ranges = {ParseRange{.start_line = 3, .start_character = 5, .end_line = 3, .end_character = 7}},
            .function_like = true,
            .resolved = true}},
        .snapshot_available = true};
    const auto result = inlayHints(
        context,
        ParseRange{.start_line = 0, .start_character = 0, .end_line = 4, .end_character = 0});
    CHECK(result.scanned_invocation_count == 2);
    CHECK(result.scanned_macro_definition_count == 1);
    CHECK(result.scanned_global_symbol_count == 0);
}

} // namespace
} // namespace pristine::analysis::semantic
