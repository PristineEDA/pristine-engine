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

    CHECK(snapshot.has_ast);
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

    CHECK(snapshot.has_ast);
    CHECK(snapshot.generation > first_generation);
    CHECK_FALSE(engine.snapshotDirty());
}

} // namespace pristine::analysis
