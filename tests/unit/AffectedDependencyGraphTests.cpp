#include "../../src/analysis/semantic/AffectedDependencyGraph.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using pristine::analysis::semantic::AffectedDependencyGraph;
using pristine::analysis::semantic::AffectedDependencyEdgeKind;

TEST_CASE("AffectedDependencyGraph traverses include and semantic reverse dependencies",
          "[analysis][semantic][affected]") {
    AffectedDependencyGraph graph;

    graph.setIncludedUris("file:///workspace/top.sv",
                          {"file:///workspace/include/defs.svh"});
    graph.setIncludedUris("file:///workspace/wrapper.sv",
                          {"file:///workspace/include/defs.svh"});
    graph.addSemanticDependency(AffectedDependencyEdgeKind::SemanticImport,
                                "file:///workspace/pkg.sv",
                                "file:///workspace/top.sv");
    graph.addSemanticDependency(AffectedDependencyEdgeKind::ModuleInstance,
                                "file:///workspace/top.sv",
                                "file:///workspace/tb.sv");

    CHECK(graph.includedUris("file:///workspace/top.sv") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh"});
    CHECK(graph.includingUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/top.sv",
                                   "file:///workspace/wrapper.sv"});
    CHECK(graph.dependentUris("file:///workspace/pkg.sv",
                              AffectedDependencyEdgeKind::SemanticImport) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
    CHECK(graph.dependentUris("file:///workspace/top.sv",
                              AffectedDependencyEdgeKind::ModuleInstance) ==
          std::vector<std::string>{"file:///workspace/tb.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/pkg.sv") ==
          std::vector<std::string>{"file:///workspace/pkg.sv",
                                   "file:///workspace/tb.sv",
                                   "file:///workspace/top.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/include/defs.svh") ==
          std::vector<std::string>{"file:///workspace/include/defs.svh",
                                   "file:///workspace/tb.sv",
                                   "file:///workspace/top.sv",
                                   "file:///workspace/wrapper.sv"});

    const auto stats = graph.stats();
    CHECK(stats.documents_with_includes == 2);
    CHECK(stats.include_edges == 2);
    CHECK(stats.semantic_import_edges == 1);
    CHECK(stats.module_instance_edges == 1);
    CHECK(stats.config_edges == 0);
    CHECK(stats.total_edges == 4);

    const auto edges = graph.edges();
    REQUIRE(edges.size() == 4);
    CHECK(edges[0].kind == AffectedDependencyEdgeKind::Include);
    CHECK(edges[0].dependency_uri == "file:///workspace/include/defs.svh");
    CHECK(edges[0].dependent_uri == "file:///workspace/top.sv");
    CHECK(edges[1].kind == AffectedDependencyEdgeKind::Include);
    CHECK(edges[1].dependency_uri == "file:///workspace/include/defs.svh");
    CHECK(edges[1].dependent_uri == "file:///workspace/wrapper.sv");
    CHECK(edges[2].kind == AffectedDependencyEdgeKind::SemanticImport);
    CHECK(edges[2].dependency_uri == "file:///workspace/pkg.sv");
    CHECK(edges[2].dependent_uri == "file:///workspace/top.sv");
    CHECK(edges[3].kind == AffectedDependencyEdgeKind::ModuleInstance);
    CHECK(edges[3].dependency_uri == "file:///workspace/top.sv");
    CHECK(edges[3].dependent_uri == "file:///workspace/tb.sv");
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

TEST_CASE("AffectedDependencyGraph tracks config edges without changing include views",
          "[analysis][semantic][affected]") {
    AffectedDependencyGraph graph;

    graph.setIncludedUris("file:///workspace/top.sv", {"file:///workspace/defs.svh"});
    graph.addSemanticDependency(AffectedDependencyEdgeKind::Config,
                                "file:///workspace/.slang/server.json",
                                "file:///workspace/top.sv");

    CHECK(graph.includingUris("file:///workspace/.slang/server.json").empty());
    CHECK(graph.dependentUris("file:///workspace/.slang/server.json",
                              AffectedDependencyEdgeKind::Config) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/.slang/server.json") ==
          std::vector<std::string>{"file:///workspace/.slang/server.json",
                                   "file:///workspace/top.sv"});

    const auto stats = graph.stats();
    CHECK(stats.include_edges == 1);
    CHECK(stats.config_edges == 1);
    CHECK(stats.total_edges == 2);
}

TEST_CASE("AffectedDependencyGraph traverses semantic package export chains",
          "[analysis][semantic][affected][package-export]") {
    AffectedDependencyGraph graph;
    graph.addSemanticDependency(AffectedDependencyEdgeKind::SemanticExport,
                                "file:///workspace/defs.sv",
                                "file:///workspace/api.sv");
    graph.addSemanticDependency(AffectedDependencyEdgeKind::SemanticImport,
                                "file:///workspace/api.sv",
                                "file:///workspace/top.sv");

    CHECK(graph.dependentUris("file:///workspace/defs.sv",
                              AffectedDependencyEdgeKind::SemanticExport) ==
          std::vector<std::string>{"file:///workspace/api.sv"});
    CHECK(graph.affectedDocumentUris("file:///workspace/defs.sv") ==
          std::vector<std::string>{"file:///workspace/api.sv",
                                   "file:///workspace/defs.sv",
                                   "file:///workspace/top.sv"});
    const auto stats = graph.stats();
    CHECK(stats.semantic_export_edges == 1);
    CHECK(stats.semantic_import_edges == 1);
    CHECK(stats.total_edges == 2);
}

TEST_CASE("AffectedDependencyGraph terminates deterministic package export cycles",
          "[analysis][semantic][affected][package-export][cycle]") {
    AffectedDependencyGraph graph;
    graph.addSemanticDependency(AffectedDependencyEdgeKind::SemanticExport,
                                "file:///workspace/a.sv",
                                "file:///workspace/b.sv");
    graph.addSemanticDependency(AffectedDependencyEdgeKind::SemanticExport,
                                "file:///workspace/b.sv",
                                "file:///workspace/a.sv");

    CHECK(graph.affectedDocumentUris("file:///workspace/a.sv") ==
          std::vector<std::string>{"file:///workspace/a.sv", "file:///workspace/b.sv"});
    CHECK(graph.stats().semantic_export_edges == 2);
}
