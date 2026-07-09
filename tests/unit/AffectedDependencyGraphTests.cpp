#include "../../src/analysis/semantic/AffectedDependencyGraph.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using pristine::analysis::semantic::AffectedDependencyGraph;

TEST_CASE("AffectedDependencyGraph traverses include and semantic reverse dependencies",
          "[analysis][semantic][affected]") {
    AffectedDependencyGraph graph;

    graph.setIncludedUris("file:///workspace/top.sv",
                          {"file:///workspace/include/defs.svh"});
    graph.setIncludedUris("file:///workspace/wrapper.sv",
                          {"file:///workspace/include/defs.svh"});
    graph.addSemanticDependency("file:///workspace/pkg.sv", "file:///workspace/top.sv");
    graph.addSemanticDependency("file:///workspace/top.sv", "file:///workspace/tb.sv");

    CHECK(graph.includedUris("file:///workspace/top.sv") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh"});
    CHECK(graph.includingUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/top.sv",
                                   "file:///workspace/wrapper.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/pkg.sv") ==
          std::vector<std::string>{"file:///workspace/pkg.sv",
                                   "file:///workspace/tb.sv",
                                   "file:///workspace/top.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh",
                                   "file:///workspace/tb.sv",
                                   "file:///workspace/top.sv",
                                   "file:///workspace/wrapper.sv"});
}

TEST_CASE("AffectedDependencyGraph replaces include edges and removes documents cleanly",
          "[analysis][semantic][affected]") {
    AffectedDependencyGraph graph;

    graph.setIncludedUris("file:///workspace/top.sv",
                          {"file:///workspace/a.svh", "file:///workspace/a.svh"});
    CHECK(graph.includingUris("file:///workspace/a.svh") ==
          std::vector<std::string>{"file:///workspace/top.sv"});

    graph.setIncludedUris("file:///workspace/top.sv", {"file:///workspace/b.svh"});
    CHECK(graph.includingUris("file:///workspace/a.svh").empty());
    CHECK(graph.includingUris("file:///workspace/b.svh") ==
          std::vector<std::string>{"file:///workspace/top.sv"});

    graph.addSemanticDependency("file:///workspace/pkg.sv", "file:///workspace/top.sv");
    graph.removeDocument("file:///workspace/top.sv");

    CHECK(graph.includedUris("file:///workspace/top.sv").empty());
    CHECK(graph.includingUris("file:///workspace/b.svh").empty());
    CHECK(graph.affectedDocumentUris("file:///workspace/pkg.sv") ==
          std::vector<std::string>{"file:///workspace/pkg.sv"});
}
