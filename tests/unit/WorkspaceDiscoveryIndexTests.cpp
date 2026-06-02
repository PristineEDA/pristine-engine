#include "pristine/analysis/SemanticEngine.h"
#include "../../src/analysis/semantic/WorkspaceDiscoveryIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <vector>

using namespace pristine::analysis;

namespace {

bool hasDeclaration(const SemanticWorkspaceDiscoverySnapshot& discovery,
                    std::string_view name,
                    std::string_view kind) {
    return std::any_of(discovery.declarations.begin(),
                       discovery.declarations.end(),
                       [&](const SemanticDiscoverySymbol& symbol) {
                           return symbol.name == name && symbol.kind == kind;
                       });
}

const SemanticDiscoveryClosureMetric* closureMetricFor(const SemanticWorkspaceDiscoverySnapshot& discovery,
                                                       std::string_view root_name) {
    const auto found = std::find_if(discovery.closure_metrics.begin(),
                                    discovery.closure_metrics.end(),
                                    [&](const SemanticDiscoveryClosureMetric& metric) {
                                        return metric.root_name == root_name;
                                    });
    return found == discovery.closure_metrics.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("WorkspaceDiscoveryIndex discovers top-level design candidates deterministically",
          "[analysis][semantic][discovery]") {
    auto index = semantic::buildWorkspaceDiscoveryIndex(
        7,
        {semantic::DiscoveryDocumentInput{.uri = "file:///workspace/b.sv",
                                          .text = R"(
package defs;
endpackage

module child(input logic clk);
endmodule
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/a.sv",
                                          .text = R"(
`define MAKE_CHILD child

interface bus_if;
endinterface

module top(input logic clk);
  child u_child(.clk(clk));
  import defs::*;
endmodule
)"}});

    CHECK(index.generation == 7);
    CHECK(index.file_count == 2);
    CHECK(index.declaration_count >= 4);
    CHECK(index.macro_count == 1);
    REQUIRE(index.files.size() == 2);
    CHECK(index.files[0].uri == "file:///workspace/a.sv");
    CHECK(index.files[1].uri == "file:///workspace/b.sv");
    REQUIRE(index.declarations_by_name.contains("top"));
    REQUIRE(index.files_by_declaration.contains("child"));
    CHECK(index.files_by_declaration.at("child") == std::vector<std::string>{"file:///workspace/b.sv"});
    REQUIRE(index.referenced_files_by_name.contains("child"));
    CHECK(index.referenced_files_by_name.at("child") == std::vector<std::string>{"file:///workspace/a.sv"});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/a.sv", "file:///workspace/b.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex includes direct include files in dependency closure",
          "[analysis][semantic][discovery]") {
    auto index = semantic::buildWorkspaceDiscoveryIndex(
        9,
        {semantic::DiscoveryDocumentInput{.uri = "file:///workspace/defs.svh",
                                          .text = "`define WIDTH 8\n"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/child.sv",
                                          .text = "module child; endmodule\n"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/top.sv",
                                          .text = R"(
`include "defs.svh"
module top;
  child u_child();
endmodule
)"}});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/child.sv",
                                              "file:///workspace/defs.svh",
                                              "file:///workspace/top.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex follows nested include files in dependency closure",
          "[analysis][semantic][discovery][include]") {
    auto index = semantic::buildWorkspaceDiscoveryIndex(
        10,
        {semantic::DiscoveryDocumentInput{.uri = "file:///workspace/a.svh",
                                          .text = R"(
`include "inc/b.svh"
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/inc/b.svh",
                                          .text = R"(
`include "../c.svh"
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/c.svh",
                                          .text = "`define DEPTH 4\n"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/top.sv",
                                          .text = R"(
`include "a.svh"
module top;
endmodule
)"}});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/a.svh",
                                              "file:///workspace/c.svh",
                                              "file:///workspace/inc/b.svh",
                                              "file:///workspace/top.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex follows package export references in dependency closure",
          "[analysis][semantic][discovery][package]") {
    auto index = semantic::buildWorkspaceDiscoveryIndex(
        11,
        {semantic::DiscoveryDocumentInput{.uri = "file:///workspace/defs.sv",
                                          .text = R"(
package defs;
  typedef logic [7:0] word_t;
endpackage
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/api.sv",
                                          .text = R"(
package api;
  export defs::*;
endpackage
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/top.sv",
                                          .text = R"(
module top;
  import api::*;
endmodule
)"}});

    REQUIRE(index.referenced_files_by_name.contains("api"));
    CHECK(index.referenced_files_by_name.at("api") == std::vector<std::string>{"file:///workspace/top.sv"});
    REQUIRE(index.referenced_files_by_name.contains("defs"));
    CHECK(index.referenced_files_by_name.at("defs") == std::vector<std::string>{"file:///workspace/api.sv"});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/api.sv",
                                              "file:///workspace/defs.sv",
                                              "file:///workspace/top.sv"});
}

TEST_CASE("SemanticEngine maintains a lightweight workspace discovery snapshot",
          "[analysis][semantic-engine][discovery]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/top.sv",
                          R"(
module child;
endmodule

module top;
  child u_child();
endmodule
)",
                          SemanticEngineDocumentState{.version = 1, .is_open = false, .dirty = false});

    auto discovery = engine.workspaceDiscovery();
    CHECK(discovery.generation == engine.generation());
    CHECK(discovery.cache_key != 0);
    CHECK_FALSE(discovery.cache_hit);
    CHECK(discovery.build_micros >= 0);
    CHECK(discovery.file_count == 1);
    CHECK(hasDeclaration(discovery, "top", "module"));
    CHECK(hasDeclaration(discovery, "child", "module"));
    CHECK(discovery.reference_count >= 1);
    CHECK(discovery.top_candidates == std::vector<std::string>{"top"});
    const auto* top_metric = closureMetricFor(discovery, "top");
    REQUIRE(top_metric != nullptr);
    CHECK(top_metric->candidate_document_count == 1);
    CHECK(top_metric->selected_document_count == 1);
    CHECK(top_metric->missing_candidate_count == 0);
    CHECK(top_metric->deduped_document_count == 0);
    CHECK(top_metric->selected_document_uris == std::vector<std::string>{"file:///workspace/top.sv"});

    const auto cached = engine.workspaceDiscovery();
    CHECK(cached.generation == discovery.generation);
    CHECK(cached.cache_key == discovery.cache_key);
    CHECK(cached.cache_hit);
    CHECK(cached.build_micros == 0);
    CHECK(cached.top_candidates == discovery.top_candidates);
    const auto* cached_metric = closureMetricFor(cached, "top");
    REQUIRE(cached_metric != nullptr);
    CHECK(cached_metric->selected_document_uris == top_metric->selected_document_uris);

    engine.updateDocument("file:///workspace/pkg.sv",
                          R"(
package defs;
  typedef logic value_t;
endpackage
)",
                          SemanticEngineDocumentState{.version = 1, .is_open = false, .dirty = false});
    auto updated = engine.workspaceDiscovery();
    CHECK(updated.generation == engine.generation());
    CHECK(updated.cache_key != discovery.cache_key);
    CHECK_FALSE(updated.cache_hit);
    CHECK(updated.file_count == 2);
    CHECK(hasDeclaration(updated, "defs", "package"));

    engine.removeDocument("file:///workspace/top.sv");
    auto removed = engine.workspaceDiscovery();
    CHECK(removed.generation == engine.generation());
    CHECK(removed.cache_key != updated.cache_key);
    CHECK_FALSE(removed.cache_hit);
    CHECK(removed.file_count == 1);
    CHECK_FALSE(hasDeclaration(removed, "top", "module"));
}

TEST_CASE("SemanticEngine discovery closure metrics describe package export closure",
          "[analysis][semantic-engine][discovery][closure][metrics]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/defs.sv",
                          R"(
package defs;
  typedef logic [7:0] word_t;
endpackage
)",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/api.sv",
                          R"(
package api;
  export defs::*;
endpackage
)",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/child.sv",
                          "module child; endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/top.sv",
                          R"(
module top;
  import api::*;
  child u_child();
endmodule
)",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/unrelated.sv",
                          "module unrelated; endmodule\n",
                          SemanticEngineDocumentState{.version = 1});

    const auto discovery = engine.workspaceDiscovery();
    const auto* top_metric = closureMetricFor(discovery, "top");
    REQUIRE(top_metric != nullptr);
    CHECK(top_metric->candidate_document_count == 4);
    CHECK(top_metric->selected_document_count == 4);
    CHECK(top_metric->missing_candidate_count == 0);
    CHECK(top_metric->selected_document_uris == std::vector<std::string>{"file:///workspace/api.sv",
                                                                         "file:///workspace/child.sv",
                                                                         "file:///workspace/defs.sv",
                                                                         "file:///workspace/top.sv"});
}
