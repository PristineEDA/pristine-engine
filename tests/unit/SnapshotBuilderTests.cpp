#include "../../src/analysis/semantic/SnapshotBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

} // namespace

TEST_CASE("SnapshotBuilder owns movable snapshot data model",
          "[analysis][semantic][snapshot-builder]") {
    SnapshotData data;
    data.modules_by_name.emplace("top",
                                 ModuleDefinition{.name = "top",
                                                  .range = rangeAt(0, 0, 16),
                                                  .selection_range = rangeAt(0, 7, 10)});
    data.references.push_back(SnapshotIndexedReference{
        .stable_id = "symbol|top",
        .name = "top",
        .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                     .range = rangeAt(0, 7, 10)},
        .is_declaration = true});

    SnapshotData moved = std::move(data);

    REQUIRE(moved.modules_by_name.contains("top"));
    REQUIRE(moved.references.size() == 1);
    CHECK(moved.references.front().is_declaration);
}

TEST_CASE("SnapshotBuilder exposes build input/output value contracts",
          "[analysis][semantic][snapshot-builder]") {
    SnapshotBuildInput input{.generation = 11,
                             .dirty_document_uris = {"file:///workspace/top.sv"},
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                                                   .text = "module top; endmodule\n",
                                                                   .version = 1,
                                                                   .is_open = true}}}};
    SnapshotBuildOutput output{.snapshot = SemanticEngineSnapshot{.generation = input.generation,
                                                                  .document_uris = {"file:///workspace/top.sv"},
                                                                  .dirty_document_uris = input.dirty_document_uris},
                               .data = std::make_unique<SnapshotData>()};

    CHECK(output.snapshot.generation == 11);
    CHECK(output.snapshot.document_uris == input.dirty_document_uris);
    REQUIRE(output.data != nullptr);
}

} // namespace pristine::analysis::semantic
