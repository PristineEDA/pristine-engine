#include "pristine/analysis/SemanticEngine.h"
#include "../../src/analysis/semantic/DocumentClosurePlanner.h"
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

void addCompleteClosure(SemanticWorkspaceDiscoverySnapshot& discovery,
                        std::string uri,
                        std::vector<std::string> uris) {
    std::sort(uris.begin(), uris.end());
    discovery.document_closures_by_uri.emplace(
        std::move(uri),
        SemanticWorkspaceDiscoverySnapshot::DocumentClosure{.uris = std::move(uris),
                                                             .fingerprint = 1,
                                                             .complete = true});
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
  export mid::*;
endpackage
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/mid.sv",
                                          .text = R"(
package mid;
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
    REQUIRE(index.referenced_files_by_name.contains("mid"));
    CHECK(index.referenced_files_by_name.at("mid") == std::vector<std::string>{"file:///workspace/api.sv"});
    REQUIRE(index.referenced_files_by_name.contains("defs"));
    CHECK(index.referenced_files_by_name.at("defs") == std::vector<std::string>{"file:///workspace/mid.sv"});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/api.sv",
                                              "file:///workspace/defs.sv",
                                              "file:///workspace/mid.sv",
                                              "file:///workspace/top.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex parallel shallow scan is deterministic",
          "[analysis][semantic][workspace-discovery][parallel][deterministic]") {
    std::vector<semantic::DiscoveryDocumentInput> documents;
    for (int index = 0; index < 64; ++index) {
        documents.push_back(semantic::DiscoveryDocumentInput{
            .uri = "file:///workspace/unit_" + std::to_string(index) + ".sv",
            .text = "module unit_" + std::to_string(index) + "; endmodule\n"});
    }
    auto reversed = documents;
    std::reverse(reversed.begin(), reversed.end());

    const auto first = semantic::buildWorkspaceDiscoveryIndex(1, std::move(documents));
    const auto second = semantic::buildWorkspaceDiscoveryIndex(2, std::move(reversed));
    REQUIRE(first.files.size() == 64);
    REQUIRE(second.files.size() == first.files.size());
    REQUIRE(second.declarations.size() == first.declarations.size());
    for (size_t index = 0; index < first.files.size(); ++index) {
        CHECK(second.files[index].uri == first.files[index].uri);
    }
    for (size_t index = 0; index < first.declarations.size(); ++index) {
        CHECK(second.declarations[index].name == first.declarations[index].name);
        CHECK(second.declarations[index].location.uri == first.declarations[index].location.uri);
    }
}

TEST_CASE("DocumentClosurePlanner bounds workspace completion declarations deterministically",
          "[analysis][semantic][document-closure][completion]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    for (int index = 0; index < 130; ++index) {
        const auto uri = "file:///workspace/alpha_" + std::to_string(index) + ".sv";
        discovery.declarations.push_back(
            SemanticDiscoverySymbol{.name = "alpha_" + std::to_string(index),
                                    .kind = "module",
                                    .location = SemanticLocation{.uri = uri}});
        addCompleteClosure(discovery, uri, {uri});
    }

    const auto short_prefix = semantic::planWorkspaceCompletionClosure(
        discovery, {"file:///workspace/top.sv"}, {}, "a");
    CHECK(short_prefix.closure.complete);
    CHECK(short_prefix.matched_candidate_count == 130);
    CHECK(short_prefix.planned_candidate_count == 64);
    CHECK(short_prefix.skipped_candidate_count == 66);
    CHECK(short_prefix.closure.uris.size() == 65);
    CHECK(short_prefix.incomplete);

    const auto long_prefix = semantic::planWorkspaceCompletionClosure(
        discovery, {"file:///workspace/top.sv"}, {}, "al");
    CHECK(long_prefix.closure.complete);
    CHECK(long_prefix.matched_candidate_count == 130);
    CHECK(long_prefix.planned_candidate_count == 128);
    CHECK(long_prefix.skipped_candidate_count == 2);
    CHECK(long_prefix.closure.uris.size() == 129);
    CHECK(long_prefix.incomplete);
}

TEST_CASE("DocumentClosurePlanner skips incomplete and over-cap candidate closures",
          "[analysis][semantic][document-closure][completion]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    for (int index = 0; index < 128; ++index) {
        const auto root_uri = "file:///workspace/pkg_" + std::to_string(index) + ".sv";
        discovery.declarations.push_back(
            SemanticDiscoverySymbol{.name = "pkg_" + std::to_string(index),
                                    .kind = "package",
                                    .location = SemanticLocation{.uri = root_uri}});
        std::vector<std::string> closure{root_uri};
        closure.push_back("file:///workspace/inc_" + std::to_string(index) + ".svh");
        closure.push_back("file:///workspace/impl_" + std::to_string(index) + ".sv");
        addCompleteClosure(discovery, root_uri, std::move(closure));
    }
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "pkg_missing",
                                .kind = "package",
                                .location = SemanticLocation{.uri = "file:///workspace/missing.sv"}});

    const auto plan = semantic::planWorkspaceCompletionClosure(
        discovery, {"file:///workspace/top.sv"}, {}, "pkg");
    CHECK(plan.closure.complete);
    CHECK(plan.incomplete);
    CHECK(plan.planned_candidate_count == 128);
    CHECK(plan.skipped_candidate_count > 0);
    CHECK(plan.added_document_count <= semantic::kWorkspaceCompletionAdditionalDocumentLimit);
    CHECK(plan.closure.uris.size() <= semantic::kWorkspaceCompletionAdditionalDocumentLimit + 1);
    CHECK(plan.closure.fingerprint != 0);
}

TEST_CASE("DocumentClosurePlanner canonicalizes document closure roots and fingerprints",
          "[analysis][semantic][document-closure][canonical]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/a.sv", {"file:///workspace/a.sv"});
    addCompleteClosure(discovery, "file:///workspace/b.sv", {"file:///workspace/b.sv"});

    const auto first = semantic::planDocumentClosure(
        discovery, {"file:///workspace/b.sv", "file:///workspace/a.sv", "file:///workspace/a.sv"}, {});
    const auto second =
        semantic::planDocumentClosure(discovery, {"file:///workspace/a.sv", "file:///workspace/b.sv"}, {});
    CHECK(first.complete);
    CHECK(first.root_key == "file:///workspace/a.sv\nfile:///workspace/b.sv");
    CHECK(first.uris == std::vector<std::string>{"file:///workspace/a.sv", "file:///workspace/b.sv"});
    CHECK(first.fingerprint == second.fingerprint);
}

TEST_CASE("DocumentClosurePlanner records a missing base root",
          "[analysis][semantic][document-closure][missing-root]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    const auto plan = semantic::planDocumentClosure(discovery, {"file:///workspace/missing.sv"}, {});
    CHECK_FALSE(plan.complete);
    CHECK(plan.uris.empty());
    CHECK(plan.reasons == std::vector<std::string>{"closure-not-discovered:file:///workspace/missing.sv"});
    CHECK(plan.fingerprint != 0);
}

TEST_CASE("DocumentClosurePlanner includes configured top closures",
          "[analysis][semantic][document-closure][configured-top]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/open.sv", {"file:///workspace/open.sv"});
    discovery.closure_uris_by_name["configured_top"] = {"file:///workspace/child.sv",
                                                         "file:///workspace/configured.sv"};

    const auto plan = semantic::planDocumentClosure(
        discovery, {"file:///workspace/open.sv"}, {"configured_top"});
    CHECK(plan.complete);
    CHECK(plan.uris == std::vector<std::string>{"file:///workspace/child.sv",
                                                 "file:///workspace/configured.sv",
                                                 "file:///workspace/open.sv"});
}

TEST_CASE("DocumentClosurePlanner records missing configured top closures",
          "[analysis][semantic][document-closure][configured-top]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/open.sv", {"file:///workspace/open.sv"});

    const auto plan = semantic::planDocumentClosure(
        discovery, {"file:///workspace/open.sv"}, {"missing_top"});
    CHECK_FALSE(plan.complete);
    CHECK(plan.reasons == std::vector<std::string>{"configured-top-not-discovered:missing_top"});
}

TEST_CASE("DocumentClosurePlanner accepts exactly the short candidate cap",
          "[analysis][semantic][document-closure][completion][short-cap]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    for (size_t index = 0; index < semantic::kShortWorkspaceCompletionCandidateLimit; ++index) {
        const auto uri = "file:///workspace/a" + std::to_string(index) + ".sv";
        discovery.declarations.push_back(
            SemanticDiscoverySymbol{.name = "a" + std::to_string(index), .location = {.uri = uri}});
        addCompleteClosure(discovery, uri, {uri});
    }

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "a");
    CHECK_FALSE(plan.incomplete);
    CHECK(plan.planned_candidate_count == semantic::kShortWorkspaceCompletionCandidateLimit);
    CHECK(plan.skipped_candidate_count == 0);
}

TEST_CASE("DocumentClosurePlanner accepts exactly the long candidate cap",
          "[analysis][semantic][document-closure][completion][long-cap]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    for (size_t index = 0; index < semantic::kLongWorkspaceCompletionCandidateLimit; ++index) {
        const auto uri = "file:///workspace/ab" + std::to_string(index) + ".sv";
        discovery.declarations.push_back(
            SemanticDiscoverySymbol{.name = "ab" + std::to_string(index), .location = {.uri = uri}});
        addCompleteClosure(discovery, uri, {uri});
    }

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "ab");
    CHECK_FALSE(plan.incomplete);
    CHECK(plan.planned_candidate_count == semantic::kLongWorkspaceCompletionCandidateLimit);
    CHECK(plan.skipped_candidate_count == 0);
}

TEST_CASE("DocumentClosurePlanner applies the short cap to an empty prefix",
          "[analysis][semantic][document-closure][completion][empty-prefix]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    for (size_t index = 0; index <= semantic::kShortWorkspaceCompletionCandidateLimit; ++index) {
        const auto uri = "file:///workspace/any" + std::to_string(index) + ".sv";
        discovery.declarations.push_back(
            SemanticDiscoverySymbol{.name = "any" + std::to_string(index), .location = {.uri = uri}});
        addCompleteClosure(discovery, uri, {uri});
    }

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "");
    CHECK(plan.incomplete);
    CHECK(plan.matched_candidate_count == semantic::kShortWorkspaceCompletionCandidateLimit + 1);
    CHECK(plan.planned_candidate_count == semantic::kShortWorkspaceCompletionCandidateLimit);
}

TEST_CASE("DocumentClosurePlanner deduplicates repeated candidate facts",
          "[analysis][semantic][document-closure][completion][dedupe]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    const auto candidate = SemanticDiscoverySymbol{.name = "child",
                                                    .kind = "Definition",
                                                    .location = {.uri = "file:///workspace/child.sv"}};
    discovery.declarations = {candidate, candidate};
    addCompleteClosure(discovery, "file:///workspace/child.sv", {"file:///workspace/child.sv"});

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "child");
    CHECK(plan.matched_candidate_count == 1);
    CHECK(plan.planned_candidate_count == 1);
    CHECK(plan.added_document_count == 1);
}

TEST_CASE("DocumentClosurePlanner does not charge the base document twice",
          "[analysis][semantic][document-closure][completion][base-dedupe]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "top", .location = {.uri = "file:///workspace/top.sv"}});

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "top");
    CHECK_FALSE(plan.incomplete);
    CHECK(plan.added_document_count == 0);
    CHECK(plan.closure.uris == std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("DocumentClosurePlanner marks a missing candidate closure incomplete",
          "[analysis][semantic][document-closure][completion][missing-candidate]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "child", .location = {.uri = "file:///workspace/child.sv"}});

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "child");
    CHECK(plan.closure.complete);
    CHECK(plan.incomplete);
    CHECK(plan.planned_candidate_count == 1);
    CHECK(plan.skipped_candidate_count == 1);
    CHECK(plan.closure.uris == std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("DocumentClosurePlanner marks an incomplete candidate closure incomplete",
          "[analysis][semantic][document-closure][completion][incomplete-candidate]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "child", .location = {.uri = "file:///workspace/child.sv"}});
    discovery.document_closures_by_uri.emplace(
        "file:///workspace/child.sv",
        SemanticWorkspaceDiscoverySnapshot::DocumentClosure{
            .uris = {"file:///workspace/child.sv"}, .reasons = {"missing:child.svh"}, .complete = false});

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "child");
    CHECK(plan.incomplete);
    CHECK(plan.skipped_candidate_count == 1);
    CHECK(plan.closure.reasons ==
          std::vector<std::string>{"workspace-candidate-closure-incomplete:file:///workspace/child.sv"});
}

TEST_CASE("DocumentClosurePlanner skips candidate closures that exceed the document budget",
          "[analysis][semantic][document-closure][completion][document-cap]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    addCompleteClosure(discovery, "file:///workspace/top.sv", {"file:///workspace/top.sv"});
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "large", .location = {.uri = "file:///workspace/large.sv"}});
    std::vector<std::string> large_closure;
    for (size_t index = 0; index <= semantic::kWorkspaceCompletionAdditionalDocumentLimit; ++index) {
        large_closure.push_back("file:///workspace/large_" + std::to_string(index) + ".sv");
    }
    addCompleteClosure(discovery, "file:///workspace/large.sv", std::move(large_closure));

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "large");
    CHECK(plan.incomplete);
    CHECK(plan.added_document_count == 0);
    CHECK(plan.skipped_candidate_count == 1);
    CHECK(plan.closure.uris == std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("DocumentClosurePlanner preserves a base failure instead of planning candidates",
          "[analysis][semantic][document-closure][completion][base-incomplete]") {
    SemanticWorkspaceDiscoverySnapshot discovery;
    discovery.document_closures_by_uri.emplace(
        "file:///workspace/top.sv",
        SemanticWorkspaceDiscoverySnapshot::DocumentClosure{
            .uris = {"file:///workspace/top.sv"}, .reasons = {"missing:top.svh"}, .complete = false});
    discovery.declarations.push_back(
        SemanticDiscoverySymbol{.name = "child", .location = {.uri = "file:///workspace/child.sv"}});
    addCompleteClosure(discovery, "file:///workspace/child.sv", {"file:///workspace/child.sv"});

    const auto plan =
        semantic::planWorkspaceCompletionClosure(discovery, {"file:///workspace/top.sv"}, {}, "child");
    CHECK_FALSE(plan.closure.complete);
    CHECK_FALSE(plan.incomplete);
    CHECK(plan.matched_candidate_count == 0);
    CHECK(plan.planned_candidate_count == 0);
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

TEST_CASE("SemanticEngine workspace discovery respects configured index dirs and excludes",
          "[analysis][semantic-engine][discovery][config][cache]") {
    SemanticEngine engine;
    engine.configure(SemanticEngineConfig{.workspace_root_uri = std::string("file:///workspace"),
                                          .index = {SemanticEngineConfig::IndexConfig{
                                              .dirs = {"rtl"},
                                              .exclude_dirs = {"rtl/vendor"}}}});
    engine.updateDocument("file:///workspace/rtl/top.sv",
                          "module top; child u_child(); endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/rtl/child.sv",
                          "module child; endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/rtl/vendor/hidden.sv",
                          "module hidden; endmodule\n",
                          SemanticEngineDocumentState{.version = 1});
    engine.updateDocument("file:///workspace/tb/tb_top.sv",
                          "module tb_top; endmodule\n",
                          SemanticEngineDocumentState{.version = 1});

    const auto discovery = engine.workspaceDiscovery();
    CHECK_FALSE(discovery.cache_hit);
    CHECK(discovery.file_count == 2);
    CHECK(hasDeclaration(discovery, "top", "module"));
    CHECK(hasDeclaration(discovery, "child", "module"));
    CHECK_FALSE(hasDeclaration(discovery, "hidden", "module"));
    CHECK_FALSE(hasDeclaration(discovery, "tb_top", "module"));
    const auto* top_metric = closureMetricFor(discovery, "top");
    REQUIRE(top_metric != nullptr);
    CHECK(top_metric->selected_document_uris == std::vector<std::string>{"file:///workspace/rtl/child.sv",
                                                                         "file:///workspace/rtl/top.sv"});

    const auto cached = engine.workspaceDiscovery();
    CHECK(cached.cache_hit);
    CHECK(cached.file_count == discovery.file_count);

    engine.configure(SemanticEngineConfig{.workspace_root_uri = std::string("file:///workspace"),
                                          .index = {SemanticEngineConfig::IndexConfig{
                                              .dirs = {"rtl", "tb"},
                                              .exclude_dirs = {"rtl/vendor"}}}});
    const auto reconfigured = engine.workspaceDiscovery();
    CHECK_FALSE(reconfigured.cache_hit);
    CHECK(reconfigured.file_count == 3);
    CHECK(hasDeclaration(reconfigured, "tb_top", "module"));
    CHECK_FALSE(hasDeclaration(reconfigured, "hidden", "module"));
}

TEST_CASE("WorkspaceDiscoveryIndex builds a complete URI-local identifier closure",
          "[analysis][semantic][discovery][document-closure]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/pkg.sv",
          .text = "package defs; typedef logic word_t; endpackage\n"},
         {.uri = "file:///workspace/top.sv",
          .text = "module top; import defs::*; word_t value; endmodule\n"},
         {.uri = "file:///workspace/unrelated.sv",
          .text = "module unrelated; endmodule\n"}});

    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Complete);
    CHECK(closure.uris == std::vector<std::string>{"file:///workspace/pkg.sv",
                                                   "file:///workspace/top.sv"});
    CHECK(closure.reasons.empty());
    CHECK(closure.fingerprint != 0);
}

TEST_CASE("WorkspaceDiscoveryIndex marks missing includes incomplete before semantic build",
          "[analysis][semantic][discovery][document-closure]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/top.sv",
          .text = "`include \"missing.svh\"\nmodule top; endmodule\n"}});
    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Incomplete);
    REQUIRE(closure.reasons.size() == 1);
    CHECK(closure.reasons.front().starts_with("missing-include:"));
}

TEST_CASE("WorkspaceDiscoveryIndex marks escaped identifiers incomplete",
          "[analysis][semantic][discovery][document-closure]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/top.sv",
          .text = "module \\escaped.name ; endmodule\n"}});
    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Incomplete);
    CHECK(std::find(closure.reasons.begin(), closure.reasons.end(), "escaped-identifier") !=
          closure.reasons.end());
}

TEST_CASE("WorkspaceDiscoveryIndex marks macro token paste incomplete",
          "[analysis][semantic][discovery][document-closure]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/top.sv",
          .text = "`define JOIN(a,b) a``b\nmodule top; endmodule\n"}});
    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Incomplete);
    CHECK(std::find(closure.reasons.begin(), closure.reasons.end(), "macro-token-paste") !=
          closure.reasons.end());
}

TEST_CASE("WorkspaceDiscoveryIndex document closure terminates include cycles",
          "[analysis][semantic][discovery][document-closure][cycle]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/a.svh", .text = "`include \"b.svh\"\n"},
         {.uri = "file:///workspace/b.svh", .text = "`include \"a.svh\"\n"}});
    const auto closure = semantic::discoveryDocumentClosure(index, "file:///workspace/a.svh");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Complete);
    CHECK(closure.uris == std::vector<std::string>{"file:///workspace/a.svh",
                                                   "file:///workspace/b.svh"});
}

TEST_CASE("WorkspaceDiscoveryIndex document closure fingerprint ignores file enumeration order",
          "[analysis][semantic][discovery][document-closure][deterministic]") {
    std::vector<semantic::DiscoveryDocumentInput> documents{
        {.uri = "file:///workspace/pkg.sv", .text = "package p; typedef logic t; endpackage\n"},
        {.uri = "file:///workspace/top.sv", .text = "module top; import p::*; t x; endmodule\n"}};
    auto reversed = documents;
    std::reverse(reversed.begin(), reversed.end());
    const auto first = semantic::buildWorkspaceDiscoveryIndex(1, documents);
    const auto second = semantic::buildWorkspaceDiscoveryIndex(2, reversed);
    const auto first_closure =
        semantic::discoveryDocumentClosure(first, "file:///workspace/top.sv");
    const auto second_closure =
        semantic::discoveryDocumentClosure(second, "file:///workspace/top.sv");
    CHECK(first_closure.uris == second_closure.uris);
    CHECK(first_closure.fingerprint == second_closure.fingerprint);
}

TEST_CASE("WorkspaceDiscoveryIndex comments and strings do not add closure dependencies",
          "[analysis][semantic][discovery][document-closure]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/child.sv", .text = "module child; endmodule\n"},
         {.uri = "file:///workspace/top.sv",
          .text = "module top; string s = \"child\"; // child ignored\nendmodule\n"}});
    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Complete);
    CHECK(closure.uris == std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex local names do not expand a document closure",
          "[analysis][semantic][discovery][document-closure][local-name]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/child.sv",
          .text = "module child #(parameter int WIDTH = 1); endmodule\n"},
         {.uri = "file:///workspace/top.sv",
          .text = "module top #(parameter int WIDTH = 4); child #(.WIDTH(WIDTH)) u(); endmodule\n"},
         {.uri = "file:///workspace/unrelated.sv",
          .text = "module unrelated #(parameter int WIDTH = 8); endmodule\n"}});

    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Complete);
    CHECK(closure.uris == std::vector<std::string>{"file:///workspace/child.sv",
                                                   "file:///workspace/top.sv"});
}

TEST_CASE("WorkspaceDiscoveryIndex package members remain visible closure candidates",
          "[analysis][semantic][discovery][document-closure][package-member]") {
    const auto index = semantic::buildWorkspaceDiscoveryIndex(
        1,
        {{.uri = "file:///workspace/defs.sv",
          .text = "package defs; typedef logic word_t; endpackage\n"},
         {.uri = "file:///workspace/top.sv",
          .text = "module top; word_t value; endmodule\n"},
         {.uri = "file:///workspace/unrelated.sv",
          .text = "module unrelated; logic value; endmodule\n"}});

    const auto closure =
        semantic::discoveryDocumentClosure(index, "file:///workspace/top.sv");
    CHECK(closure.confidence == semantic::DiscoveryClosureConfidence::Complete);
    CHECK(closure.uris == std::vector<std::string>{"file:///workspace/defs.sv",
                                                   "file:///workspace/top.sv"});
}
