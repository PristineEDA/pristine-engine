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

TEST_CASE("SemanticEngine owns UX diagnostics formerly produced by workspace metadata",
          "[analysis][semantic-engine][diagnostics]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/pkg_a.sv",
                          "package pkg_a;\n"
                          "  typedef logic [7:0] word_t;\n"
                          "endpackage\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/pkg_b.sv",
                          "package pkg_b;\n"
                          "  typedef logic [15:0] word_t;\n"
                          "endpackage\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/diagnostics.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  logic ready;\n"
                          "  import pkg_a::*;\n"
                          "  import pkg_b::*;\n"
                          "  word_t value;\n"
                          "  missing_t missing;\n"
                          "  logic [3:0] lhs;\n"
                          "  logic [7:0] rhs;\n"
                          "  assign lhs = rhs;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto diagnostics = engine.diagnosticsFor("file:///workspace/diagnostics.sv");

    const auto find_diagnostic = [&](std::string_view code) {
        return std::find_if(diagnostics.begin(),
                            diagnostics.end(),
                            [code](const SemanticEngineDiagnostic& diagnostic) {
                                return diagnostic.code == code;
                            });
    };

    const auto duplicate = find_diagnostic("duplicateSymbol");
    REQUIRE(duplicate != diagnostics.end());
    CHECK(duplicate->message == "Duplicate symbol 'ready' in the same scope.");
    CHECK(duplicate->range.start_line == 2);

    const auto ambiguous = find_diagnostic("ambiguousReference");
    REQUIRE(ambiguous != diagnostics.end());
    CHECK(ambiguous->message == "Symbol 'word_t' has 2 possible definitions in scope.");
    CHECK(ambiguous->severity == 2);

    const auto unresolved_type = find_diagnostic("unresolvedType");
    REQUIRE(unresolved_type != diagnostics.end());
    CHECK(unresolved_type->message == "Type 'missing_t' could not be resolved.");
    CHECK(unresolved_type->range.start_line == 6);

    const auto width_mismatch = find_diagnostic("widthMismatch");
    REQUIRE(width_mismatch != diagnostics.end());
    CHECK(width_mismatch->message == "Width mismatch: assigning 8-bit 'rhs' to 4-bit 'lhs'.");
    CHECK(width_mismatch->severity == 2);
}

TEST_CASE("SemanticEngine diagnoses non-adjacent duplicate symbols without module-body leakage",
          "[analysis][semantic-engine][diagnostics][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/non-adjacent-duplicate.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  logic valid;\n"
                          "  logic ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto diagnostics = engine.diagnosticsFor("file:///workspace/non-adjacent-duplicate.sv");

    CHECK(std::count_if(diagnostics.begin(),
                        diagnostics.end(),
                        [](const SemanticEngineDiagnostic& diagnostic) {
                            return diagnostic.code == "duplicateSymbol" &&
                                   diagnostic.message == "Duplicate symbol 'ready' in the same scope." &&
                                   diagnostic.range.start_line == 3;
                        }) == 1);
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& diagnostic) {
        return diagnostic.code == "duplicateSymbol" && diagnostic.message.find("'top'") != std::string::npos;
    }));
}

TEST_CASE("SemanticEngine resolves packages and local shadowing without legacy workspace diagnostics",
          "[analysis][semantic-engine][diagnostics][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/defs.sv",
                          "package defs; endpackage\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic defs;\n"
                          "  import defs::*;\n"
                          "  logic flag;\n"
                          "  if (1) begin : g\n"
                          "    logic flag;\n"
                          "    assign flag = flag;\n"
                          "  end\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto diagnostics = engine.diagnosticsFor("file:///workspace/top.sv");

    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticEngineDiagnostic& item) {
        return item.code == "unresolvedPackage" || item.code == "ambiguousReference";
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

TEST_CASE("SemanticEngine invalidates generation caches after document updates",
          "[analysis][semantic-engine][cache]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/cache.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto first_diagnostics = engine.diagnosticsFor("file:///workspace/cache.sv");
    const auto first_symbols = engine.workspaceSymbols("ready");
    REQUIRE(first_diagnostics.empty());
    REQUIRE_FALSE(first_symbols.unresolved);
    REQUIRE(first_symbols.symbols.size() == 1);
    const auto first_generation = first_symbols.generation;

    engine.updateDocument("file:///workspace/cache.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  logic ready;\n"
                          "  logic valid;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 2, .is_open = true});

    const auto updated_diagnostics = engine.diagnosticsFor("file:///workspace/cache.sv");
    const auto updated_symbols = engine.workspaceSymbols("valid");

    CHECK(std::any_of(updated_diagnostics.begin(),
                      updated_diagnostics.end(),
                      [](const SemanticEngineDiagnostic& diagnostic) {
                          return diagnostic.code == "duplicateSymbol";
                      }));
    REQUIRE_FALSE(updated_symbols.unresolved);
    CHECK(updated_symbols.generation > first_generation);
    CHECK(std::any_of(updated_symbols.symbols.begin(),
                      updated_symbols.symbols.end(),
                      [](const SemanticWorkspaceSymbol& symbol) {
                          return symbol.name == "valid";
                      }));
}

TEST_CASE("SemanticEngine invalidates query caches for semantic providers after document updates",
          "[analysis][semantic-engine][cache]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/cache-providers.sv",
                          "module child(input logic clk, output logic rst_n);\n"
                          "endmodule\n"
                          "module top;\n"
                          "  logic ready;\n"
                          "  assign ready = ready;\n"
                          "  child u_child(.clk(clk), .r);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto first_references = engine.referencesAt("file:///workspace/cache-providers.sv", 3, 9, true);
    const auto first_rename = engine.renameAt("file:///workspace/cache-providers.sv", 3, 9, "valid");
    const auto first_completion = engine.completionsAt("file:///workspace/cache-providers.sv", 5, 28, "");
    const auto first_hierarchy = engine.moduleHierarchy("top", 4);
    const auto first_schematic = engine.schematic("top", 4);
    const auto first_cone = engine.backwardConeAt("file:///workspace/cache-providers.sv", 3, 9);
    const auto first_actions = engine.codeActionsAt(
        "file:///workspace/cache-providers.sv",
        ParseRange{.start_line = 5, .start_character = 2, .end_line = 5, .end_character = 20});

    REQUIRE_FALSE(first_references.unresolved);
    REQUIRE_FALSE(first_rename.unresolved);
    REQUIRE_FALSE(first_completion.unresolved);
    REQUIRE_FALSE(first_hierarchy.unresolved);
    REQUIRE_FALSE(first_schematic.unresolved);
    REQUIRE_FALSE(first_cone.unresolved);
    REQUIRE_FALSE(first_actions.unresolved);
    REQUIRE(first_references.locations.size() == 3);
    REQUIRE(first_rename.edits.size() == 3);
    CHECK(std::any_of(first_completion.items.begin(),
                      first_completion.items.end(),
                      [](const SemanticCompletionItem& item) {
                          return item.label == "rst_n";
                      }));
    const auto first_generation = first_references.generation;

    engine.updateDocument("file:///workspace/cache-providers.sv",
                          "module child(input logic clk, output logic rst_n, input logic data);\n"
                          "endmodule\n"
                          "module top;\n"
                          "  logic ready;\n"
                          "  logic extra;\n"
                          "  assign ready = extra;\n"
                          "  child u_child(.clk(clk), .r);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 2, .is_open = true});

    const auto updated_references = engine.referencesAt("file:///workspace/cache-providers.sv", 3, 9, true);
    const auto updated_rename = engine.renameAt("file:///workspace/cache-providers.sv", 3, 9, "valid");
    const auto updated_completion = engine.completionsAt("file:///workspace/cache-providers.sv", 6, 28, "");
    const auto updated_hierarchy = engine.moduleHierarchy("top", 4);
    const auto updated_schematic = engine.schematic("top", 4);
    const auto updated_cone = engine.backwardConeAt("file:///workspace/cache-providers.sv", 3, 9);
    const auto updated_actions = engine.codeActionsAt(
        "file:///workspace/cache-providers.sv",
        ParseRange{.start_line = 6, .start_character = 2, .end_line = 6, .end_character = 20});

    CHECK(updated_references.generation > first_generation);
    CHECK(updated_references.locations.size() == 2);
    CHECK(updated_rename.edits.size() == 2);
    CHECK(std::any_of(updated_completion.items.begin(),
                      updated_completion.items.end(),
                      [](const SemanticCompletionItem& item) {
                          return item.label == "data";
                      }));
    CHECK(updated_hierarchy.generation == updated_references.generation);
    CHECK(updated_schematic.generation == updated_references.generation);
    CHECK(updated_cone.generation == updated_references.generation);
    CHECK(updated_actions.generation == updated_references.generation);
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

TEST_CASE("SemanticEngine references ignore comment and string tokens without identifier-scan fallback",
          "[analysis][semantic-engine][ast-identity][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/no-text-scan.sv",
                          "module top;\n"
                          "  logic ready;\n"
                          "  // ready in a comment is not a reference\n"
                          "  string label = \"ready\";\n"
                          "  assign ready = ready;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto references = engine.referencesAt("file:///workspace/no-text-scan.sv", 1, 9, true);
    REQUIRE_FALSE(references.unresolved);
    REQUIRE(references.locations.size() == 3);
    CHECK(std::all_of(references.locations.begin(),
                      references.locations.end(),
                      [](const SemanticLocation& location) {
                          return location.range.start_line == 1 || location.range.start_line == 4;
                      }));

    const auto rename = engine.renameAt("file:///workspace/no-text-scan.sv", 1, 9, "valid");
    REQUIRE_FALSE(rename.unresolved);
    REQUIRE(rename.edits.size() == 3);
    CHECK(std::none_of(rename.edits.begin(), rename.edits.end(), [](const SemanticTextEdit& edit) {
        return edit.location.range.start_line == 2 || edit.location.range.start_line == 3;
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

TEST_CASE("SemanticEngine provides AST-backed function and task signature help",
          "[analysis][semantic-engine][signature][function][task][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/calls.sv",
                          "module top;\n"
                          "  function automatic int add(input int lhs, input int rhs);\n"
                          "    return lhs + rhs;\n"
                          "  endfunction\n"
                          "  task automatic emit(input logic ready);\n"
                          "  endtask\n"
                          "  initial begin\n"
                          "    int value = add(1, 2);\n"
                          "    emit(1'b1);\n"
                          "  end\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto function_signature = engine.signatureHelpAt("file:///workspace/calls.sv", 7, 23);
    REQUIRE_FALSE(function_signature.unresolved);
    CHECK(function_signature.label == "function int add(input int lhs, input int rhs)");
    REQUIRE(function_signature.parameters.size() == 2);
    CHECK(function_signature.parameters[1] == "input int rhs");
    CHECK(function_signature.active_parameter == 1);

    const auto task_signature = engine.signatureHelpAt("file:///workspace/calls.sv", 8, 13);
    REQUIRE_FALSE(task_signature.unresolved);
    CHECK(task_signature.label == "task emit(input logic ready)");
    REQUIRE(task_signature.parameters.size() == 1);
    CHECK(task_signature.parameters[0] == "input logic ready");
    CHECK(task_signature.active_parameter == 0);
}

TEST_CASE("SemanticEngine emits AST-backed port inlay hints for module instances",
          "[analysis][semantic-engine][inlay][ports]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(input logic clk, output logic rst_n);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic clk;\n"
                          "  logic rst_n;\n"
                          "  child u_named(.clk(clk), .rst_n(rst_n));\n"
                          "  child u_ordered(clk, rst_n);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto hints = engine.inlayHints("file:///workspace/top.sv",
                                        ParseRange{.start_line = 0,
                                                   .start_character = 0,
                                                   .end_line = 6,
                                                   .end_character = 0});

    REQUIRE_FALSE(hints.unresolved);
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == ".clk" &&
               hint.tooltip == "input logic clk";
    }));
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == ".rst_n" &&
               hint.tooltip == "output logic rst_n";
    }));
}

TEST_CASE("SemanticEngine emits AST-backed function and task argument inlay hints",
          "[analysis][semantic-engine][inlay][function][task][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/call-hints.sv",
                          "module top;\n"
                          "  function automatic int add(input int lhs, input int rhs);\n"
                          "    return lhs + rhs;\n"
                          "  endfunction\n"
                          "  task automatic emit(input logic ready);\n"
                          "  endtask\n"
                          "  initial begin\n"
                          "    int value = add(1, 2);\n"
                          "    emit(1'b1);\n"
                          "  end\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto hints = engine.inlayHints("file:///workspace/call-hints.sv",
                                        ParseRange{.start_line = 0,
                                                   .start_character = 0,
                                                   .end_line = 10,
                                                   .end_character = 0});

    REQUIRE_FALSE(hints.unresolved);
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input int lhs:" &&
               hint.tooltip == "function int add(input int lhs, input int rhs)";
    }));
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input int rhs:" &&
               hint.tooltip == "function int add(input int lhs, input int rhs)";
    }));
    CHECK(std::any_of(hints.hints.begin(), hints.hints.end(), [](const SemanticInlayHint& hint) {
        return hint.kind == "parameter" && hint.label == "input logic ready:" &&
               hint.tooltip == "task emit(input logic ready)";
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

TEST_CASE("SemanticEngine uses discovery closure for hierarchy and schematic cold graph builds",
          "[analysis][semantic-engine][discovery][hierarchy][schematic][cache]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.top_modules = {"top"}});
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(output logic q);\n"
                          "  assign q = 1'b1;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic q;\n"
                          "  child u_child(.q(q));\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/unrelated.sv",
                          "module unrelated;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});

    const auto hierarchy = engine.moduleHierarchy(std::string_view("top"), 4);
    REQUIRE_FALSE(hierarchy.unresolved);
    CHECK(hierarchy.discovery_closure_used);
    CHECK(hierarchy.discovery_closure_root_name == "top");
    CHECK(hierarchy.discovery_closure_candidate_document_count == 2);
    CHECK(hierarchy.discovery_closure_document_count == 2);
    CHECK(hierarchy.discovery_closure_missing_candidate_count == 0);
    CHECK(hierarchy.discovery_closure_deduped_document_count == 0);
    CHECK(hierarchy.discovery_closure_build_micros >= 0);
    REQUIRE(hierarchy.roots.size() == 1);
    REQUIRE(hierarchy.roots.front().children.size() == 1);
    CHECK(hierarchy.roots.front().children.front().module_name == "child");

    const auto cached_hierarchy = engine.moduleHierarchy(std::string_view("top"), 4);
    CHECK(cached_hierarchy.discovery_closure_used);
    CHECK(cached_hierarchy.discovery_closure_root_name == hierarchy.discovery_closure_root_name);
    CHECK(cached_hierarchy.discovery_closure_candidate_document_count ==
          hierarchy.discovery_closure_candidate_document_count);
    CHECK(cached_hierarchy.discovery_closure_document_count == hierarchy.discovery_closure_document_count);
    CHECK(cached_hierarchy.discovery_closure_missing_candidate_count ==
          hierarchy.discovery_closure_missing_candidate_count);
    CHECK(cached_hierarchy.discovery_closure_deduped_document_count ==
          hierarchy.discovery_closure_deduped_document_count);
    CHECK(cached_hierarchy.discovery_closure_build_micros == 0);

    const auto schematic = engine.schematic(std::string_view("top"), 4);
    REQUIRE_FALSE(schematic.unresolved);
    CHECK(schematic.discovery_closure_used);
    CHECK(schematic.discovery_closure_root_name == "top");
    CHECK(schematic.discovery_closure_candidate_document_count == 2);
    CHECK(schematic.discovery_closure_document_count == 2);
    CHECK(schematic.discovery_closure_missing_candidate_count == 0);
    CHECK(schematic.discovery_closure_deduped_document_count == 0);
    CHECK(schematic.discovery_closure_build_micros >= 0);
    CHECK(std::any_of(schematic.modules.begin(),
                      schematic.modules.end(),
                      [](const SemanticSchematicModuleView& view) {
                          return view.module.name == "top";
                      }));
}

TEST_CASE("SemanticEngine routes module signatures and schematic cells through AstIndex views",
          "[analysis][semantic-engine][ast-index][schematic][signature]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.top_modules = {"top"}});
    engine.updateDocument("file:///workspace/child.sv",
                          "module child(input logic clk, output logic [3:0] data);\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          "module top;\n"
                          "  logic clk;\n"
                          "  logic [3:0] data;\n"
                          "  child u_child(.clk(clk), .data(data));\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto signature = engine.signatureHelpAt("file:///workspace/top.sv", 3, 26);
    REQUIRE_FALSE(signature.unresolved);
    CHECK(signature.label.find("child(") == 0);
    CHECK(std::any_of(signature.parameters.begin(), signature.parameters.end(), [](const std::string& parameter) {
        return parameter.find("data") != std::string::npos &&
               parameter.find("output") != std::string::npos;
    }));

    const auto schematic = engine.schematic("top", 4);
    REQUIRE_FALSE(schematic.unresolved);
    const auto top = std::find_if(schematic.modules.begin(),
                                  schematic.modules.end(),
                                  [](const SemanticSchematicModuleView& view) {
                                      return view.module.name == "top";
                                  });
    REQUIRE(top != schematic.modules.end());
    REQUIRE(top->module.cells.size() == 1);
    CHECK(top->module.cells.front().name == "u_child");
    CHECK(top->module.cells.front().type == "child");
    CHECK(std::any_of(top->module.cells.front().connections.begin(),
                      top->module.cells.front().connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "data";
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

TEST_CASE("SemanticEngine traces backward cone through AstIndex identifiers without same-name leakage",
          "[analysis][semantic-engine][design][cone][no-fallback]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.top_modules = {"top"}});
    engine.updateDocument("file:///workspace/cones.sv",
                          "module top;\n"
                          "  logic mid;\n"
                          "  logic out;\n"
                          "  assign out = mid;\n"
                          "endmodule\n"
                          "module other;\n"
                          "  logic mid;\n"
                          "  logic out;\n"
                          "  assign out = mid;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto cone = engine.backwardConeAt("file:///workspace/cones.sv", 2, 9);

    REQUIRE_FALSE(cone.unresolved);
    REQUIRE(cone.root_symbol_id.has_value());
    REQUIRE(cone.nodes.size() == 2);
    CHECK(std::all_of(cone.nodes.begin(), cone.nodes.end(), [](const SemanticConeNode& node) {
        return node.location.range.start_line < 4;
    }));
    REQUIRE(cone.edges.size() == 1);
    CHECK(cone.edges.front().expression == "mid");
    CHECK(cone.edges.front().location.range.start_line == 3);
}

TEST_CASE("SemanticEngine resolves type definitions through AstIndex type references",
          "[analysis][semantic-engine][type-definition][no-fallback]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/types.sv",
                          "module top;\n"
                          "  typedef logic [3:0] nibble_t;\n"
                          "  nibble_t value;\n"
                          "endmodule\n",
                          SemanticEngineDocumentState{.version = 1, .is_open = true});

    const auto type_definitions = engine.typeDefinitionsAt("file:///workspace/types.sv", 2, 3);

    REQUIRE_FALSE(type_definitions.unresolved);
    REQUIRE(type_definitions.locations.size() == 1);
    CHECK(type_definitions.locations.front().uri == "file:///workspace/types.sv");
    CHECK(type_definitions.locations.front().range.start_line == 1);
    CHECK(type_definitions.locations.front().range.start_character == 22);
    CHECK(type_definitions.locations.front().range.end_character == 30);
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
