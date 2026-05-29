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

} // namespace pristine::analysis
