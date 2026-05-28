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

    const auto definitions = workspace.definitionsAt("file:///workspace/top.sv", 1, 3);

    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().name == "child");
    CHECK(definitions.front().location.uri == "file:///workspace/child.sv");
    CHECK(definitions.front().scope_path == "$root");
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

    const auto first_references = workspace.referencesAt("file:///workspace/shadowed.sv", 1, 9, false);
    REQUIRE(first_references.size() == 2);
    CHECK(std::all_of(first_references.begin(), first_references.end(), [](const SemanticReference& reference) {
        return reference.location.range.start_line == 2;
    }));

    const auto second_references = workspace.referencesAt("file:///workspace/shadowed.sv", 5, 9, false);
    REQUIRE(second_references.size() == 2);
    CHECK(std::all_of(second_references.begin(), second_references.end(), [](const SemanticReference& reference) {
        return reference.location.range.start_line == 6;
    }));
}

TEST_CASE("SemanticWorkspace reports visible symbols from nearest scope outward", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/visible.sv",
                             "module visible;\n"
                             "  logic local_ready;\n"
                             "  function void sample();\n"
                             "  endfunction\n"
                             "endmodule\n");

    const auto visible = workspace.visibleSymbolsAt("file:///workspace/visible.sv", 3, 4, "");

    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "local_ready";
    }));
    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "sample";
    }));
    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "visible";
    }));

    const auto local_it = std::find_if(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "local_ready";
    });
    const auto root_it = std::find_if(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "visible";
    });
    REQUIRE(local_it != visible.end());
    REQUIRE(root_it != visible.end());
    CHECK(local_it < root_it);
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