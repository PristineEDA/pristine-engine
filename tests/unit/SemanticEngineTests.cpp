#include "pristine/analysis/SemanticEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis {

TEST_CASE("SemanticEngine lazily builds an AST compilation snapshot",
          "[analysis][semantic-engine]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    CHECK(engine.documentCount() == 1);
    CHECK(engine.snapshotDirty());

    const auto& snapshot = engine.snapshot();

    CHECK(snapshot.has_shallow_ast);
    CHECK_FALSE(snapshot.has_design_ast);
    CHECK(snapshot.document_uris.size() == 1);
    CHECK(snapshot.document_uris.front() == "file:///workspace/top.sv");
    CHECK_FALSE(engine.snapshotDirty());
}

TEST_CASE("SemanticEngine surfaces slang semantic diagnostics",
          "[analysis][semantic-engine][diagnostics]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/diagnostic.sv",
                          "module top;\n"
                          "  logic [3:0] value;\n"
                          "  assign value = missing_signal;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto diagnostics = engine.diagnosticsFor("file:///workspace/diagnostic.sv");

    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code.starts_with("slang:") &&
               diagnostic.message.find("missing_signal") != std::string::npos;
    }));
}

TEST_CASE("SemanticEngine refreshes snapshots after document updates",
          "[analysis][semantic-engine]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto first_generation = engine.snapshot().generation;

    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 2, .is_open = true, .dirty = true});

    CHECK(engine.snapshotDirty());
    const auto& snapshot = engine.snapshot();

    CHECK(snapshot.has_shallow_ast);
    CHECK(snapshot.generation > first_generation);
    CHECK(snapshot.dirty_document_uris == std::vector<std::string>{"file:///workspace/top.sv"});
    CHECK_FALSE(engine.snapshotDirty());
}

TEST_CASE("SemanticEngine tracks include dependencies and affected documents",
          "[analysis][semantic-engine][dependencies]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/rtl/top.sv",
                          "`include \"../include/defs.svh\"\n"
                          "module top;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});
    engine.updateDocument("file:///workspace/include/defs.svh",
                          "`define READY 1\n",
                          SemanticEngineDocumentState{.version = 1});

    CHECK(engine.includedUris("file:///workspace/rtl/top.sv") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh"});
    CHECK(engine.includingUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/rtl/top.sv"});
    CHECK(engine.affectedDocumentUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh",
                                   "file:///workspace/rtl/top.sv"});
}

TEST_CASE("SemanticEngine switches snapshot mode when design config is present",
          "[analysis][semantic-engine][config]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.build = std::string("rtl/top.f"),
                                          .top_modules = {"top", "top"}});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});

    const auto& snapshot = engine.snapshot();

    CHECK(snapshot.mode == SemanticEngineMode::Design);
    CHECK(snapshot.has_shallow_ast);
    CHECK(snapshot.has_design_ast);
    CHECK(snapshot.top_modules == std::vector<std::string>{"top"});
}

TEST_CASE("SemanticEngine exposes first-batch LSP-neutral query contracts",
          "[analysis][semantic-engine][query]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto lookup = engine.lookupAt("file:///workspace/top.sv", 1, 9);
    REQUIRE_FALSE(lookup.unresolved);
    REQUIRE(lookup.symbol.has_value());
    CHECK(lookup.symbol->name == "ready");
    CHECK(lookup.symbol->location.range.start_line == 1);
    CHECK(lookup.symbol->location.range.start_character == 8);

    const auto definitions = engine.definitionsAt("file:///workspace/top.sv", 1, 9);
    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().uri == "file:///workspace/top.sv");

    const auto references = engine.referencesAt("file:///workspace/top.sv", 1, 9, true);
    REQUIRE_FALSE(references.unresolved);
    CHECK(references.locations.size() == 3);

    const auto highlights = engine.documentHighlightsAt("file:///workspace/top.sv", 1, 9);
    REQUIRE_FALSE(highlights.unresolved);
    CHECK(highlights.locations.size() == 3);

    const auto prepare = engine.prepareRenameAt("file:///workspace/top.sv", 1, 9);
    REQUIRE_FALSE(prepare.unresolved);
    CHECK(prepare.placeholder == "ready");

    const auto rename = engine.renameAt("file:///workspace/top.sv", 1, 9, "valid");
    REQUIRE_FALSE(rename.unresolved);
    REQUIRE(rename.edits.size() == 3);
    CHECK(std::all_of(rename.edits.begin(), rename.edits.end(), [](const SemanticTextEdit& edit) {
        return edit.new_text == "valid";
    }));
}

TEST_CASE("SemanticEngine owns workspace symbol lookup",
          "[analysis][semantic-engine][workspace-symbol]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/pkg.sv",
                          "package control_pkg;\n"
                          "  typedef logic [3:0] nibble_t;\n"
                          "endpackage\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});

    const auto symbols = engine.workspaceSymbols("ct");

    REQUIRE_FALSE(symbols.unresolved);
    CHECK(std::any_of(symbols.symbols.begin(), symbols.symbols.end(), [](const SemanticWorkspaceSymbol& symbol) {
        return symbol.name == "control_pkg" && symbol.kind == 4 &&
               symbol.location.uri == "file:///workspace/pkg.sv";
    }));
    CHECK(std::none_of(symbols.symbols.begin(), symbols.symbols.end(), [](const SemanticWorkspaceSymbol& symbol) {
        return symbol.name == "ready";
    }));

    const auto type_symbols = engine.workspaceSymbols("nt");
    CHECK(std::any_of(type_symbols.symbols.begin(), type_symbols.symbols.end(), [](const SemanticWorkspaceSymbol& symbol) {
        return symbol.name == "nibble_t" && symbol.location.uri == "file:///workspace/pkg.sv";
    }));

    const auto truncated = engine.workspaceSymbols("", 1);
    CHECK(truncated.truncated);
    CHECK(truncated.symbols.size() == 1);
}

TEST_CASE("SemanticEngine uses AST symbol identity to avoid same-name false references",
          "[analysis][semantic-engine][ast-identity]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/shadow.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "endmodule\n"
                          "module other;\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto first = engine.referencesAt("file:///workspace/shadow.sv", 1, 9, true);
    REQUIRE_FALSE(first.unresolved);
    CHECK(first.locations.size() == 3);
    CHECK(std::all_of(first.locations.begin(), first.locations.end(), [](const SemanticLocation& location) {
        return location.range.start_line < 4;
    }));

    const auto second = engine.referencesAt("file:///workspace/shadow.sv", 5, 9, true);
    REQUIRE_FALSE(second.unresolved);
    CHECK(second.locations.size() == 3);
    CHECK(std::all_of(second.locations.begin(), second.locations.end(), [](const SemanticLocation& location) {
        return location.range.start_line > 4;
    }));
}

TEST_CASE("SemanticEngine resolves cross-file module definitions through AST identity",
          "[analysis][semantic-engine][ast-identity]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/child.sv",
                          "module child;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  child u_child();\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto definitions = engine.definitionsAt("file:///workspace/top.sv", 1, 4);

    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().uri == "file:///workspace/child.sv");
    CHECK(definitions.locations.front().range.start_line == 0);
}

TEST_CASE("SemanticEngine exposes second-batch value-type semantic query contracts",
          "[analysis][semantic-engine][query]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/types.sv",
                          "module top;\n"
                          "  typedef logic [3:0] nibble_t;\n"
                          "  nibble_t value;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto type_definitions = engine.typeDefinitionsAt("file:///workspace/types.sv", 2, 11);
    REQUIRE_FALSE(type_definitions.unresolved);
    REQUIRE_FALSE(type_definitions.locations.empty());

    const auto completions = engine.completionsAt("file:///workspace/types.sv", 2, 4, "val");
    REQUIRE_FALSE(completions.unresolved);
    CHECK(std::any_of(completions.items.begin(), completions.items.end(), [](const auto& item) {
        return item.label == "value";
    }));

    const auto hints = engine.inlayHints("file:///workspace/types.sv",
                                        ParseRange{.start_line = 0,
                                                   .start_character = 0,
                                                   .end_line = 4,
                                                   .end_character = 0});
    REQUIRE_FALSE(hints.unresolved);
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.label.find("nibble_t") != std::string::npos ||
               hint.label.find("logic") != std::string::npos;
    }));

    const auto tokens = engine.semanticTokens("file:///workspace/types.sv");
    REQUIRE_FALSE(tokens.unresolved);
    CHECK(std::any_of(tokens.tokens.begin(), tokens.tokens.end(), [](const SemanticToken& token) {
        return token.token_modifier == "declaration";
    }));

    const auto selection = engine.selectionRangesAt("file:///workspace/types.sv", 2, 11);
    REQUIRE_FALSE(selection.unresolved);
    CHECK_FALSE(selection.ranges.empty());
}

TEST_CASE("SemanticEngine provides AST-backed module signature help and port completions",
          "[analysis][semantic-engine][completion][signature]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(input logic clk, output logic rst_n, input logic [3:0] data);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  child u_child(.clk(clk), .r);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto signature = engine.signatureHelpAt("file:///workspace/top.sv", 1, 28);
    REQUIRE_FALSE(signature.unresolved);
    CHECK(signature.label == "child(input logic clk, output logic rst_n, input logic [3:0] data)");
    REQUIRE(signature.parameters.size() == 3);
    CHECK(signature.parameters[1] == "output logic rst_n");
    CHECK(signature.active_parameter == 1);

    const auto completions = engine.completionsAt("file:///workspace/top.sv", 1, 28, "");
    REQUIRE_FALSE(completions.unresolved);
    CHECK(std::any_of(completions.items.begin(), completions.items.end(), [](const auto& item) {
        return item.label == "rst_n" && item.detail == "output logic rst_n" &&
               item.insert_text.find("rst_n(") != std::string::npos &&
               !item.stable_id.empty();
    }));
}

TEST_CASE("SemanticEngine provides AST-backed macro completions and resolve docs",
          "[analysis][semantic-engine][completion][macro]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/macros.sv",
                          "`define ADD(lhs, rhs) ((lhs) + (rhs))\n"
                          "module top;\n"
                          "  logic value = `AD\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto completions = engine.completionsAt("file:///workspace/macros.sv", 2, 19, "AD");
    REQUIRE_FALSE(completions.unresolved);
    const auto completion = std::find_if(completions.items.begin(), completions.items.end(), [](const auto& item) {
        return item.label == "ADD";
    });
    REQUIRE(completion != completions.items.end());
    CHECK(completion->detail == "Macro function");
    CHECK(completion->insert_text.find("${1:lhs}") != std::string::npos);
    CHECK(completion->documentation.find("Parameters") != std::string::npos);
}

TEST_CASE("SemanticEngine builds selection parent chains from AST and syntax ranges",
          "[analysis][semantic-engine][selection]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/select.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto selection = engine.selectionRangesAt("file:///workspace/select.sv", 2, 10);
    REQUIRE_FALSE(selection.unresolved);
    REQUIRE(selection.ranges.size() >= 3);
    CHECK(selection.ranges.front().range.start_line == 2);
    CHECK(selection.ranges.front().range.start_character == 9);
    REQUIRE(selection.ranges.front().parent.has_value());
    CHECK(*selection.ranges.front().parent == 1);
}

TEST_CASE("SemanticEngine reports UTF-16 ranges for AST references",
          "[analysis][semantic-engine][utf16]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/utf.sv",
                          "module top;\n"
                          "  // smile \xF0\x9F\x98\x80\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto lookup = engine.lookupAt("file:///workspace/utf.sv", 2, 9);

    REQUIRE_FALSE(lookup.unresolved);
    REQUIRE(lookup.symbol.has_value());
    CHECK(lookup.symbol->location.range.start_line == 2);
    CHECK(lookup.symbol->location.range.start_character == 8);
}

TEST_CASE("SemanticEngine builds design snapshot hierarchy, schematic, call hierarchy, and cones",
          "[analysis][semantic-engine][design][hierarchy][schematic][cone]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.build = std::string("rtl/files.f"),
                                          .top_modules = {"top"}});
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(input logic clk, output logic q);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic clk;\n"
                          "  logic mid;\n"
                          "  logic out;\n"
                          "  child u_child(.clk(clk), .q(mid));\n"
                          "  assign out = mid;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto hierarchy = engine.moduleHierarchy(std::nullopt, 8);
    REQUIRE_FALSE(hierarchy.unresolved);
    REQUIRE(hierarchy.roots.size() == 1);
    CHECK(hierarchy.roots.front().module_name == "top");
    REQUIRE(hierarchy.roots.front().children.size() == 1);
    CHECK(hierarchy.roots.front().children.front().module_name == "child");
    CHECK(hierarchy.roots.front().children.front().instance_name == "u_child");

    const auto schematic = engine.schematic(std::nullopt, 8);
    REQUIRE_FALSE(schematic.unresolved);
    REQUIRE(schematic.root_module_id.has_value());
    CHECK(*schematic.root_module_id == "top");
    CHECK(std::any_of(schematic.modules.begin(), schematic.modules.end(), [](const SemanticSchematicModuleView& view) {
        return view.module.name == "top" &&
               std::any_of(view.nets.begin(), view.nets.end(), [](const SemanticSchematicNet& net) {
                   return net.name == "mid" && !net.drivers.empty() && !net.loads.empty();
               });
    }));

    const auto prepared_from_definition = engine.prepareCallHierarchy("file:///workspace/top.sv", 0, 8);
    REQUIRE_FALSE(prepared_from_definition.unresolved);
    REQUIRE(prepared_from_definition.items.size() == 1);
    CHECK(prepared_from_definition.items.front().name == "top");

    const auto outgoing = engine.outgoingCalls(prepared_from_definition.items.front());
    REQUIRE_FALSE(outgoing.unresolved);
    REQUIRE(outgoing.calls.size() == 1);
    CHECK(outgoing.calls.front().item.name == "child");
    REQUIRE(outgoing.calls.front().from_ranges.size() == 1);

    const auto prepared_from_instance = engine.prepareCallHierarchy("file:///workspace/top.sv", 4, 4);
    REQUIRE_FALSE(prepared_from_instance.unresolved);
    REQUIRE(prepared_from_instance.items.size() == 1);
    CHECK(prepared_from_instance.items.front().name == "child");

    const auto incoming = engine.incomingCalls(prepared_from_instance.items.front());
    REQUIRE_FALSE(incoming.unresolved);
    REQUIRE(incoming.calls.size() == 1);
    CHECK(incoming.calls.front().item.name == "top");

    const auto cone = engine.backwardConeAt("file:///workspace/top.sv", 3, 9);
    REQUIRE_FALSE(cone.unresolved);
    REQUIRE(cone.root_symbol_id.has_value());
    CHECK(std::any_of(cone.nodes.begin(), cone.nodes.end(), [](const SemanticConeNode& node) {
        return node.name == "out";
    }));
    CHECK(std::any_of(cone.nodes.begin(), cone.nodes.end(), [](const SemanticConeNode& node) {
        return node.name == "mid";
    }));
    CHECK(std::any_of(cone.edges.begin(), cone.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.expression == "mid";
    }));
}

TEST_CASE("SemanticEngine reports partial design results for unresolved modules and hierarchy caps",
          "[analysis][semantic-engine][design][negative]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.top_modules = {"top"}});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  missing_child u_missing();\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto hierarchy = engine.moduleHierarchy(std::nullopt, 0);
    REQUIRE_FALSE(hierarchy.roots.empty());
    CHECK(hierarchy.partial);
    CHECK(hierarchy.truncated);
    CHECK(std::any_of(hierarchy.messages.begin(), hierarchy.messages.end(), [](const std::string& message) {
        return message.find("maxDepth") != std::string::npos;
    }));

    const auto schematic = engine.schematic(std::string_view("missing_child"), 8);
    CHECK(schematic.partial);
    CHECK(schematic.modules.empty());
    CHECK(std::any_of(schematic.messages.begin(), schematic.messages.end(), [](const std::string& message) {
        return message.find("No schematic data") != std::string::npos;
    }));
}

TEST_CASE("SemanticEngine owns code actions for unresolved modules, ports, and types",
          "[analysis][semantic-engine][code-action]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(input logic clk, output logic rst_n, input logic data);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/actions.sv",
                          "module top;\n"
                          "  child u_child(.clk(clk));\n"
                          "  missing_child u_missing();\n"
                          "  missing_t value;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto port_actions = engine.codeActionsAt(
        "file:///workspace/actions.sv",
        ParseRange{.start_line = 1, .start_character = 2, .end_line = 1, .end_character = 25});
    REQUIRE_FALSE(port_actions.unresolved);
    REQUIRE(std::any_of(port_actions.actions.begin(), port_actions.actions.end(), [](const SemanticCodeAction& action) {
        return action.title == "Add missing port connections to 'u_child'" &&
               action.edits.size() == 1 &&
               action.edits.front().new_text == ", .rst_n(rst_n), .data(data)";
    }));

    const auto module_actions = engine.codeActionsAt(
        "file:///workspace/actions.sv",
        ParseRange{.start_line = 2, .start_character = 2, .end_line = 2, .end_character = 15});
    REQUIRE(std::any_of(module_actions.actions.begin(),
                        module_actions.actions.end(),
                        [](const SemanticCodeAction& action) {
                            return action.title == "Create stub module 'missing_child'" &&
                                   action.diagnostics.size() == 1 &&
                                   action.diagnostics.front().code == "unresolvedModule" &&
                                   action.edits.size() == 1 &&
                                   action.edits.front().new_text.find("module missing_child;") !=
                                       std::string::npos;
                        }));

    const auto type_actions = engine.codeActionsAt(
        "file:///workspace/actions.sv",
        ParseRange{.start_line = 3, .start_character = 2, .end_line = 3, .end_character = 11});
    REQUIRE(std::any_of(type_actions.actions.begin(),
                        type_actions.actions.end(),
                        [](const SemanticCodeAction& action) {
                            return action.title == "Create typedef 'missing_t'" &&
                                   action.diagnostics.size() == 1 &&
                                   action.diagnostics.front().code == "unresolvedType" &&
                                   action.edits.size() == 1 &&
                                   action.edits.front().new_text.find("typedef logic missing_t;") !=
                                       std::string::npos;
                        }));
}

TEST_CASE("SemanticEngine owns include creation code actions",
          "[analysis][semantic-engine][code-action][include]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.workspace_root_uri = std::string("file:///workspace")});
    engine.updateDocument("file:///workspace/rtl/top.sv",
                          "`include \"missing.svh\"\n"
                          "module top;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto actions = engine.codeActionsAt(
        "file:///workspace/rtl/top.sv",
        ParseRange{.start_line = 0, .start_character = 10, .end_line = 0, .end_character = 21});

    REQUIRE_FALSE(actions.unresolved);
    REQUIRE(actions.actions.size() == 1);
    CHECK(actions.actions.front().title == "Create include file 'missing.svh'");
    REQUIRE(actions.actions.front().diagnostics.size() == 1);
    CHECK(actions.actions.front().diagnostics.front().code == "unknownInclude");
    REQUIRE(actions.actions.front().create_files.size() == 1);
    CHECK(actions.actions.front().create_files.front().uri == "file:///workspace/rtl/missing.svh");
}

} // namespace pristine::analysis
