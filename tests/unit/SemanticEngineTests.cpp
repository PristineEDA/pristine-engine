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

} // namespace pristine::analysis
