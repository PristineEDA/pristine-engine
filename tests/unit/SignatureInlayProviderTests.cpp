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
        .document_text = &text,
        .modules_by_name = &modules,
        .module_instances = {SignatureInlayModuleInstance{.module_name = "child",
                                                          .range = ParseRange{.start_line = 1,
                                                                              .start_character = 2,
                                                                              .end_line = 1,
                                                                              .end_character = 42},
                                                          .selection_range = ParseRange{.start_line = 1,
                                                                                        .start_character = 8,
                                                                                        .end_line = 1,
                                                                                        .end_character = 15}}},
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
        .document_text = &text,
        .macros = {MacroDefinition{.name = "ADD",
                                   .parameters = {"lhs", "rhs"},
                                   .body = "((lhs) + (rhs))",
                                   .range = ParseRange{.start_line = 0,
                                                       .start_character = 0,
                                                       .end_line = 0,
                                                       .end_character = 37},
                                   .selection_range = ParseRange{.start_line = 0,
                                                                 .start_character = 8,
                                                                 .end_line = 0,
                                                                 .end_character = 11},
                                   .function_like = true}},
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
        CHECK(result.messages.front().find("document is not indexed") != std::string::npos);
    }
}

} // namespace
} // namespace pristine::analysis::semantic
