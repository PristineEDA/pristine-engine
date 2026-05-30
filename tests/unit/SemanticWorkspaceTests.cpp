#include "pristine/analysis/SemanticWorkspace.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis {

TEST_CASE("SemanticWorkspace resolves cross-file module definitions", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/child.sv", "module child; endmodule\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  child child_i();\n"
                             "endmodule\n");

    const auto definitions = workspace.engineDefinitionsAt("file:///workspace/top.sv", 1, 3);

    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().uri == "file:///workspace/child.sv");
}

TEST_CASE("SemanticWorkspace keeps same-name references scoped", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/shadowed.sv",
                             "module first;\n"
                             "  logic ready;\n"
                             "  assign ready = ready;\n"
                             "endmodule\n"
                             "module second;\n"
                             "  logic ready;\n"
                             "  assign ready = ready;\n"
                             "endmodule\n");

    const auto first_references = workspace.engineReferencesAt("file:///workspace/shadowed.sv", 1, 9, false);
    REQUIRE_FALSE(first_references.unresolved);
    REQUIRE(first_references.locations.size() == 2);
    CHECK(std::all_of(first_references.locations.begin(), first_references.locations.end(), [](const SemanticLocation& location) {
        return location.range.start_line == 2;
    }));

    const auto second_references = workspace.engineReferencesAt("file:///workspace/shadowed.sv", 5, 9, false);
    REQUIRE_FALSE(second_references.unresolved);
    REQUIRE(second_references.locations.size() == 2);
    CHECK(std::all_of(second_references.locations.begin(), second_references.locations.end(), [](const SemanticLocation& location) {
        return location.range.start_line == 6;
    }));
}

TEST_CASE("SemanticWorkspace resolves imported package symbols", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/defs.sv",
                             "package defs;\n"
                             "  typedef logic [7:0] word_t;\n"
                             "endpackage\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  import defs::*;\n"
                             "  word_t value;\n"
                             "endmodule\n");

    const auto definitions = workspace.engineTypeDefinitionsAt("file:///workspace/top.sv", 2, 3);

    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().uri == "file:///workspace/defs.sv");
}

TEST_CASE("SemanticWorkspace evaluates parameterized widths and hover metadata", "[analysis][semantic][types]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/typed.sv",
                             "module top #(parameter int WIDTH = 8) (\n"
                             "  input logic [WIDTH-1:0] data\n"
                             ");\n"
                             "endmodule\n");

    const auto hover = workspace.engineHoverAt("file:///workspace/typed.sv", 1, 28);
    REQUIRE_FALSE(hover.unresolved);
    CHECK(hover.contents.find("data") != std::string::npos);
    CHECK(hover.contents.find("logic") != std::string::npos);
}

TEST_CASE("SemanticWorkspace resolves typedef aliases into hover metadata", "[analysis][semantic][types]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/alias.sv",
                             "typedef logic [7:0] byte_t;\n"
                             "module top;\n"
                             "  byte_t value;\n"
                             "endmodule\n");

    const auto definitions = workspace.engineTypeDefinitionsAt("file:///workspace/alias.sv", 2, 3);
    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().range.start_line == 0);
    CHECK(definitions.locations.front().range.start_character == 20);

    const auto hover = workspace.engineHoverAt("file:///workspace/alias.sv", 2, 9);
    REQUIRE_FALSE(hover.unresolved);
    CHECK(hover.contents.find("value") != std::string::npos);
    CHECK(hover.contents.find("byte_t") != std::string::npos);
}

TEST_CASE("SemanticWorkspace scopes named generate blocks", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/generate.sv",
                             "module top;\n"
                             "  logic ready;\n"
                             "  if (1) begin : g\n"
                             "    logic ready;\n"
                             "    assign ready = ready;\n"
                             "  end\n"
                             "endmodule\n");

    const auto definitions = workspace.engineDefinitionsAt("file:///workspace/generate.sv", 4, 11);
    const auto references = workspace.engineReferencesAt("file:///workspace/generate.sv", 3, 11, false);

    REQUIRE_FALSE(definitions.unresolved);
    REQUIRE(definitions.locations.size() == 1);
    CHECK(definitions.locations.front().range.start_line == 3);
    REQUIRE_FALSE(references.unresolved);
    REQUIRE(references.locations.size() == 2);
    CHECK(std::all_of(references.locations.begin(), references.locations.end(), [](const SemanticLocation& location) {
        return location.range.start_line == 4;
    }));
}

TEST_CASE("SemanticWorkspace traces local backward assignment cones",
          "[analysis][semantic][cone]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/cone.sv",
                             "module top;\n"
                             "  logic a;\n"
                             "  logic b;\n"
                             "  logic mid;\n"
                             "  logic out;\n"
                             "  assign mid = a & b;\n"
                             "  assign out = mid;\n"
                             "endmodule\n");

    const auto trace = workspace.engineBackwardConeAt("file:///workspace/cone.sv", 4, 9);

    REQUIRE(trace.root_symbol_id.has_value());
    CHECK(trace.messages.empty());
    const auto find_node = [&](std::string_view name) {
        return std::find_if(trace.nodes.begin(), trace.nodes.end(), [&](const SemanticConeNode& node) {
            return node.name == name;
        });
    };
    const auto out = find_node("out");
    const auto mid = find_node("mid");
    const auto a = find_node("a");
    const auto b = find_node("b");
    REQUIRE(out != trace.nodes.end());
    REQUIRE(mid != trace.nodes.end());
    REQUIRE(a != trace.nodes.end());
    REQUIRE(b != trace.nodes.end());
    CHECK(*trace.root_symbol_id == out->id);

    const auto has_edge = [&](const SemanticConeNode& from, const SemanticConeNode& to) {
        return std::any_of(trace.edges.begin(), trace.edges.end(), [&](const SemanticConeEdge& edge) {
            return edge.from_symbol_id == from.id && edge.to_symbol_id == to.id;
        });
    };
    CHECK(has_edge(*out, *mid));
    CHECK(has_edge(*mid, *a));
    CHECK(has_edge(*mid, *b));
}

TEST_CASE("SemanticWorkspace tracks include directives per document", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/includes.sv",
                             "`include \"defs.svh\"\n"
                             "`include \"rtl/types.svh\"\n"
                             "module top; endmodule\n",
                             SemanticDocumentState{.version = 7,
                                                   .is_open = true,
                                                   .dirty = true,
                                                   .invalidate_dependents = false});

    const auto* document = workspace.document("file:///workspace/includes.sv");

    REQUIRE(document != nullptr);
    CHECK(document->version == 7);
    CHECK(document->is_open);
    CHECK(document->dirty);
    REQUIRE(document->includes.size() == 2);
    CHECK(document->includes.front().target == "defs.svh");
    CHECK(document->includes.front().range.start_line == 0);
    REQUIRE(document->included_uris.size() == 2);
    CHECK(document->included_uris.at(0) == "file:///workspace/defs.svh");
    CHECK(document->included_uris.at(1) == "file:///workspace/rtl/types.svh");
}

TEST_CASE("SemanticWorkspace builds reverse include edges and marks dependents stale", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/rtl/top.sv",
                             "`include \"defs.svh\"\n"
                             "module top; endmodule\n");
    workspace.updateDocument("file:///workspace/rtl/defs.svh", "typedef logic bit_t;\n");

    const auto including_top = workspace.includingUris("file:///workspace/rtl/defs.svh");
    REQUIRE(including_top.size() == 1);
    CHECK(including_top.front() == "file:///workspace/rtl/top.sv");
    CHECK(workspace.staleDocumentUris().empty());

    workspace.updateDocument("file:///workspace/rtl/defs.svh", "typedef bit bit_t;\n",
                             SemanticDocumentState{.version = -1,
                                                   .is_open = false,
                                                   .dirty = false,
                                                   .invalidate_dependents = true});

    const auto stale_documents = workspace.staleDocumentUris();
    REQUIRE(stale_documents.size() == 1);
    CHECK(stale_documents.front() == "file:///workspace/rtl/top.sv");
}

TEST_CASE("SemanticWorkspace clears dependency graph when documents are removed", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/top.sv",
                             "`include \"defs.svh\"\n"
                             "module top; endmodule\n");

    REQUIRE_FALSE(workspace.includingUris("file:///workspace/defs.svh").empty());

    workspace.removeDocument("file:///workspace/top.sv");

    CHECK(workspace.includingUris("file:///workspace/defs.svh").empty());
    CHECK(workspace.documentCount() == 0);
}

} // namespace pristine::analysis
