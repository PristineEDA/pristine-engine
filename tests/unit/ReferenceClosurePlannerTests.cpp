#include "pristine/analysis/SemanticEngine.h"
#include "../../src/analysis/semantic/DocumentClosurePlanner.h"
#include "../../src/analysis/semantic/SemanticSnapshotCache.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace pristine::analysis;

namespace {

void addClosure(SemanticWorkspaceDiscoverySnapshot& discovery,
                std::string uri,
                std::vector<std::string> documents = {},
                bool complete = true) {
    if (documents.empty()) {
        documents.push_back(uri);
    }
    std::sort(documents.begin(), documents.end());
    discovery.document_closures_by_uri.emplace(
        std::move(uri),
        SemanticWorkspaceDiscoverySnapshot::DocumentClosure{.uris = std::move(documents),
                                                             .fingerprint = 1,
                                                             .complete = complete});
}

semantic::ReferenceSearchSeed seed(std::string name = "value") {
    return semantic::ReferenceSearchSeed{.stable_id = "symbol:" + name,
                                         .declaration_uri = "file:///workspace/defs.sv",
                                         .spellings = {std::move(name)}};
}

SemanticWorkspaceDiscoverySnapshot baseDiscovery() {
    SemanticWorkspaceDiscoverySnapshot discovery;
    discovery.file_count = 8;
    addClosure(discovery, "file:///workspace/top.sv");
    addClosure(discovery, "file:///workspace/defs.sv");
    return discovery;
}

semantic::SemanticSnapshotCache::Entry entry(std::uint64_t key, std::string identity) {
    return semantic::SemanticSnapshotCache::Entry{.key = key,
                                                   .generation = 7,
                                                   .config_discovery_fingerprint = 11,
                                                   .closure_fingerprint = 13,
                                                   .scope_kind = "referenceClosure",
                                                   .root_uri = "file:///workspace/top.sv",
                                                   .identity = std::move(identity)};
}

} // namespace

TEST_CASE("SemanticSnapshotCache identity lookup never synthesizes an entry",
          "[analysis][semantic-snapshot-cache][reference-closure]") {
    semantic::SemanticSnapshotCache cache;
    CHECK(cache.findIdentity("") == nullptr);
    CHECK(cache.findIdentity("forged") == nullptr);
    CHECK(cache.size() == 0);
}

TEST_CASE("SemanticSnapshotCache retains explicit scope and fingerprint metadata",
          "[analysis][semantic-snapshot-cache][reference-closure]") {
    semantic::SemanticSnapshotCache cache;
    cache.insert(entry(1, "snapshot:a"));
    const auto* found = cache.findIdentity("snapshot:a");
    REQUIRE(found != nullptr);
    CHECK(found->scope_kind == "referenceClosure");
    CHECK(found->generation == 7);
    CHECK(found->config_discovery_fingerprint == 11);
    CHECK(found->closure_fingerprint == 13);
}

TEST_CASE("SemanticSnapshotCache evicts least recently used closure identities",
          "[analysis][semantic-snapshot-cache][reference-closure]") {
    semantic::SemanticSnapshotCache cache;
    cache.insert(entry(1, "snapshot:1"));
    cache.insert(entry(2, "snapshot:2"));
    cache.insert(entry(3, "snapshot:3"));
    cache.insert(entry(4, "snapshot:4"));
    REQUIRE(cache.findIdentity("snapshot:1") != nullptr);
    cache.insert(entry(5, "snapshot:5"));
    CHECK(cache.findIdentity("snapshot:1") != nullptr);
    CHECK(cache.findIdentity("snapshot:2") == nullptr);
    CHECK(cache.size() == 4);
}

TEST_CASE("Reference candidate plan includes lexical postings for a stable target",
          "[analysis][reference-closure][planner]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/use.sv");
    discovery.reference_candidate_uris_by_name["value"] = {"file:///workspace/use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.candidate_document_count == 2);
    CHECK(plan.closure.uris == std::vector<std::string>{"file:///workspace/defs.sv",
                                                        "file:///workspace/top.sv",
                                                        "file:///workspace/use.sv"});
}

TEST_CASE("Reference candidate plan includes a declaration without postings",
          "[analysis][reference-closure][planner]") {
    const auto plan = semantic::planReferenceCandidateClosure(
        baseDiscovery(), {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.candidate_document_count == 1);
    CHECK(plan.selected_document_count == 2);
}

TEST_CASE("Reference candidate plan merges alias spellings deterministically",
          "[analysis][reference-closure][planner][alias]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/alias-use.sv");
    discovery.reference_candidate_uris_by_name["value"] = {"file:///workspace/value-use.sv"};
    addClosure(discovery, "file:///workspace/value-use.sv");
    discovery.reference_candidate_uris_by_name["value_alias"] = {"file:///workspace/alias-use.sv"};
    auto reference_seed = seed();
    reference_seed.spellings.push_back("value_alias");
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, reference_seed);
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.candidate_document_count == 3);
    CHECK(plan.selected_document_count == 4);
}

TEST_CASE("Reference candidate plan has no arbitrary candidate cap",
          "[analysis][reference-closure][planner][complete]") {
    auto discovery = baseDiscovery();
    discovery.file_count = 400;
    auto& postings = discovery.reference_candidate_uris_by_name["value"];
    for (int index = 0; index < 300; ++index) {
        const auto uri = "file:///workspace/use_" + std::to_string(index) + ".sv";
        postings.push_back(uri);
        addClosure(discovery, uri);
    }
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.candidate_document_count == 301);
    CHECK(plan.selected_document_count == 302);
}

TEST_CASE("Reference candidate plan includes each candidate dependency closure",
          "[analysis][reference-closure][planner][dependency]") {
    auto discovery = baseDiscovery();
    addClosure(discovery,
               "file:///workspace/use.sv",
               {"file:///workspace/use.sv", "file:///workspace/use_inc.svh"});
    discovery.reference_candidate_uris_by_name["value"] = {"file:///workspace/use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(std::find(plan.closure.uris.begin(), plan.closure.uris.end(), "file:///workspace/use_inc.svh") !=
          plan.closure.uris.end());
}

TEST_CASE("Reference candidate plan preselects full for an incomplete base closure",
          "[analysis][reference-closure][planner][incomplete]") {
    auto discovery = baseDiscovery();
    discovery.document_closures_by_uri["file:///workspace/top.sv"].complete = false;
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "incomplete-base-closure");
}

TEST_CASE("Reference candidate plan preselects full for an incomplete target seed",
          "[analysis][reference-closure][planner][incomplete]") {
    auto reference_seed = seed();
    reference_seed.complete = false;
    reference_seed.reasons.push_back("escaped-identifier");
    const auto plan = semantic::planReferenceCandidateClosure(
        baseDiscovery(), {"file:///workspace/top.sv"}, {}, reference_seed);
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "incomplete-seed");
}

TEST_CASE("Reference candidate plan preselects full for incomplete discovery postings",
          "[analysis][reference-closure][planner][incomplete]") {
    auto discovery = baseDiscovery();
    discovery.reference_candidate_incomplete_reasons = {"macro-token-paste"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "incomplete-discovery");
}

TEST_CASE("Reference candidate plan preselects full for an incomplete candidate closure",
          "[analysis][reference-closure][planner][incomplete]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/use.sv", {"file:///workspace/use.sv"}, false);
    discovery.reference_candidate_uris_by_name["value"] = {"file:///workspace/use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "incomplete-candidate-closure");
}

TEST_CASE("Reference candidate plan includes macro definition and invocation documents",
          "[analysis][reference-closure][planner][macro]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/macro_use.sv");
    discovery.macro_definitions.push_back(SemanticWorkspaceDiscoverySnapshot::MacroDefinition{
        .name = "READ_VALUE", .uri = "file:///workspace/defs.sv", .body_identifiers = {"value"}});
    discovery.macro_invocation_uris_by_name["READ_VALUE"] = {"file:///workspace/macro_use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.candidate_document_count == 2);
    CHECK(plan.selected_document_count == 3);
}

TEST_CASE("Reference candidate plan follows nested macro propagation",
          "[analysis][reference-closure][planner][macro]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/macro_use.sv");
    discovery.macro_definitions = {
        {.name = "READ_VALUE", .uri = "file:///workspace/defs.sv", .body_identifiers = {"value"}},
        {.name = "WRAP_VALUE", .uri = "file:///workspace/defs.sv", .body_identifiers = {"READ_VALUE"}}};
    discovery.macro_invocation_uris_by_name["WRAP_VALUE"] = {"file:///workspace/macro_use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(plan.selected_document_count == 3);
}

TEST_CASE("Reference candidate plan preselects full for incomplete macro propagation",
          "[analysis][reference-closure][planner][macro][incomplete]") {
    auto discovery = baseDiscovery();
    discovery.macro_definitions.push_back(SemanticWorkspaceDiscoverySnapshot::MacroDefinition{
        .name = "READ_VALUE",
        .uri = "file:///workspace/defs.sv",
        .body_identifiers = {"value"},
        .complete = false,
        .reasons = {"macro-token-paste"}});
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "incomplete-macro-propagation");
}

TEST_CASE("Reference candidate plan uses full scope when candidates cover the workspace",
          "[analysis][reference-closure][planner][scope]") {
    auto discovery = baseDiscovery();
    discovery.file_count = 3;
    addClosure(discovery, "file:///workspace/use.sv");
    discovery.reference_candidate_uris_by_name["value"] = {"file:///workspace/use.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(plan.requires_full_snapshot);
    CHECK(plan.confidence == "candidate-closure-covers-workspace");
}

TEST_CASE("Reference candidate plan fingerprint is independent of posting order",
          "[analysis][reference-closure][planner][deterministic]") {
    auto first = baseDiscovery();
    auto second = baseDiscovery();
    addClosure(first, "file:///workspace/a.sv");
    addClosure(first, "file:///workspace/b.sv");
    addClosure(second, "file:///workspace/a.sv");
    addClosure(second, "file:///workspace/b.sv");
    first.reference_candidate_uris_by_name["value"] = {"file:///workspace/b.sv", "file:///workspace/a.sv"};
    second.reference_candidate_uris_by_name["value"] = {"file:///workspace/a.sv", "file:///workspace/b.sv"};
    const auto first_plan = semantic::planReferenceCandidateClosure(
        first, {"file:///workspace/top.sv"}, {}, seed());
    const auto second_plan = semantic::planReferenceCandidateClosure(
        second, {"file:///workspace/top.sv"}, {}, seed());
    CHECK(first_plan.closure.uris == second_plan.closure.uris);
    CHECK(first_plan.closure.fingerprint == second_plan.closure.fingerprint);
}

TEST_CASE("Reference candidate plan includes configured top closures before searching",
          "[analysis][reference-closure][planner][config]") {
    auto discovery = baseDiscovery();
    addClosure(discovery, "file:///workspace/config_top.sv");
    discovery.closure_uris_by_name["configured"] = {"file:///workspace/config_top.sv"};
    const auto plan = semantic::planReferenceCandidateClosure(
        discovery, {"file:///workspace/top.sv"}, {"configured"}, seed());
    CHECK_FALSE(plan.requires_full_snapshot);
    CHECK(std::find(plan.closure.uris.begin(), plan.closure.uris.end(), "file:///workspace/config_top.sv") !=
          plan.closure.uris.end());
}

TEST_CASE("Reference candidate plan reports a deterministic policy fingerprint",
          "[analysis][reference-closure][planner][deterministic]") {
    const auto first = semantic::planReferenceCandidateClosure(
        baseDiscovery(), {"file:///workspace/top.sv"}, {}, seed());
    const auto second = semantic::planReferenceCandidateClosure(
        baseDiscovery(), {"file:///workspace/top.sv"}, {}, seed());
    CHECK(first.closure.fingerprint != 0);
    CHECK(first.closure.fingerprint == second.closure.fingerprint);
}
