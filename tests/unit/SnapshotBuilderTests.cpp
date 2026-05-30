#include "../../src/analysis/semantic/SnapshotBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("SnapshotBuilder builds syntax, diagnostics, and include dependency edges",
          "[analysis][semantic][snapshot-builder]") {
    SnapshotBuildInput input{.generation = 12,
                             .dirty_document_uris = {"file:///workspace/rtl/top.sv"},
                             .documents = {{"file:///workspace/rtl/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/rtl/top.sv",
                                                .text = "`include \"../include/defs.svh\"\n"
                                                        "module top;\n"
                                                        "  logic [3:0] lhs;\n"
                                                        "  logic [7:0] rhs;\n"
                                                        "  assign lhs = rhs;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true,
                                                .dirty = true}},
                                           {"file:///workspace/include/defs.svh",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/include/defs.svh",
                                                .text = "`define READY 1\n",
                                                .version = 1}}}};

    const auto output = SnapshotBuilder{}.build(std::move(input));

    CHECK(output.snapshot.generation == 12);
    CHECK(output.snapshot.mode == SemanticEngineMode::Shallow);
    CHECK(output.snapshot.has_shallow_ast);
    CHECK_FALSE(output.snapshot.has_design_ast);
    CHECK(output.snapshot.document_uris == std::vector<std::string>{"file:///workspace/include/defs.svh",
                                                                    "file:///workspace/rtl/top.sv"});
    CHECK(output.includes.at("file:///workspace/rtl/top.sv") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh"});
    CHECK(output.reverse_includes.at("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/rtl/top.sv"});
    REQUIRE(output.data != nullptr);
    CHECK(output.data->module_entries.size() == 1);
    CHECK(output.data->modules_by_name.contains("top"));
    CHECK(std::any_of(output.snapshot.diagnostics.begin(),
                      output.snapshot.diagnostics.end(),
                      [](const SemanticEngineDiagnostic& diagnostic) {
                          return diagnostic.code.starts_with("slang:");
                      }));
}

TEST_CASE("SnapshotBuilder enters design mode when build or top config is present",
          "[analysis][semantic][snapshot-builder]") {
    SnapshotBuildInput input{.generation = 13,
                             .config = SemanticEngineConfig{.top_modules = {"top"}},
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                                                   .text = "module top; endmodule\n",
                                                                   .version = 1}}}};

    const auto output = SnapshotBuilder{}.build(std::move(input));

    CHECK(output.snapshot.mode == SemanticEngineMode::Design);
    CHECK(output.snapshot.top_modules == std::vector<std::string>{"top"});
    CHECK(output.snapshot.has_shallow_ast);
    CHECK(output.snapshot.has_design_ast);
}

} // namespace pristine::analysis::semantic
