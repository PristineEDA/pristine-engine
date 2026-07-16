#include "../../src/analysis/semantic/NavigationProvider.h"
#include "../../src/analysis/semantic/QueryCache.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {
namespace {

constexpr std::string_view kTopUri = "file:///workspace/top.sv";
constexpr std::string_view kChildUri = "file:///workspace/child.sv";

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

ParseRange rangeAt(int start_line, int start_character, int end_line, int end_character) {
    return ParseRange{.start_line = start_line,
                      .start_character = start_character,
                      .end_line = end_line,
                      .end_character = end_character};
}

SemanticLocation locationAt(std::string uri, ParseRange range) {
    return SemanticLocation{.uri = std::move(uri), .range = range};
}

SemanticSymbolIdentity identity(std::string stable_id,
                                std::string name,
                                std::string kind,
                                std::string uri,
                                ParseRange range) {
    return SemanticSymbolIdentity{.stable_id = std::move(stable_id),
                                  .name = std::move(name),
                                  .kind = std::move(kind),
                                  .location = locationAt(std::move(uri), range)};
}

SnapshotNavigationOccurrenceIndex occurrenceIndex(std::vector<SnapshotNavigationOccurrence> occurrences) {
    std::sort(occurrences.begin(), occurrences.end(), [](const auto& left, const auto& right) {
        if (left.location.range.start_line != right.location.range.start_line) {
            return left.location.range.start_line < right.location.range.start_line;
        }
        if (left.location.range.start_character != right.location.range.start_character) {
            return left.location.range.start_character < right.location.range.start_character;
        }
        return left.stable_id < right.stable_id;
    });
    SnapshotNavigationOccurrenceIndex index;
    index.occurrences = std::move(occurrences);
    for (const auto& occurrence : index.occurrences) {
        if (index.prefix_max_end_ranges.empty() ||
            index.prefix_max_end_ranges.back().end_line < occurrence.location.range.end_line ||
            (index.prefix_max_end_ranges.back().end_line == occurrence.location.range.end_line &&
             index.prefix_max_end_ranges.back().end_character < occurrence.location.range.end_character)) {
            index.prefix_max_end_ranges.push_back(occurrence.location.range);
        }
        else {
            index.prefix_max_end_ranges.push_back(index.prefix_max_end_ranges.back());
        }
    }
    return index;
}

NavigationContext makeContext(
    const SnapshotNavigationOccurrenceIndex& occurrences,
    const std::unordered_map<std::string, SnapshotNavigationTargetFact>& targets,
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>>& by_symbol,
    const std::unordered_map<std::string, std::vector<std::string>>& aliases,
    const SnapshotImplementationEdgeIndex* implementation_edges = nullptr,
    const std::vector<SnapshotTypeReference>* type_references = nullptr,
    const std::vector<MacroInvocationFact>* macros = nullptr,
    const std::vector<CallableInvocationFact>* callables = nullptr,
    const SnapshotSelectionRangeIndex* selection_ranges = nullptr) {
    return NavigationContext{.generation = 9,
                             .snapshot_available = true,
                             .document_uri = std::string(kTopUri),
                             .occurrence_index = &occurrences,
                             .occurrences_by_symbol = &by_symbol,
                             .reference_aliases_by_id = &aliases,
                             .targets_by_id = &targets,
                             .implementation_edges = implementation_edges,
                             .type_references = type_references,
                             .macro_invocations = macros,
                             .callable_invocations = callables,
                             .selection_range_index = selection_ranges};
}

} // namespace

TEST_CASE("Navigation target lookup consumes URI-local occurrence facts",
          "[analysis][semantic][navigation-index][lookup]") {
    const auto ready = identity("symbol|ready", "ready", "Variable", std::string(kTopUri), rangeAt(1, 8, 13));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {ready.stable_id, SnapshotNavigationTargetFact{.identity = ready, .type_display = "logic"}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = ready.stable_id,
                                                                            .location = ready.location,
                                                                            .has_type_display = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {ready.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = lookupAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 10);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.symbol.has_value());
    CHECK(result.symbol->stable_id == ready.stable_id);
    CHECK(result.scanned_occurrence_count == 1);
    CHECK(result.scanned_target_count == 1);
}

TEST_CASE("Navigation target lookup does not guess a missing target",
          "[analysis][semantic][navigation-index][lookup][no-fallback]") {
    const auto occurrence = SnapshotNavigationOccurrence{.stable_id = "symbol|missing",
                                                           .location = locationAt(std::string(kTopUri), rangeAt(2, 4, 11))};
    const auto occurrences = occurrenceIndex({occurrence});
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {occurrence.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = lookupAt(makeContext(occurrences, targets, by_symbol, aliases), 2, 6);

    CHECK(result.unresolved);
    REQUIRE_FALSE(result.messages.empty());
    CHECK(result.messages.front().find("target") != std::string::npos);
}

TEST_CASE("Navigation lookup deterministically prefers a declaration at the same range",
          "[analysis][semantic][navigation-index][tie-break]") {
    const auto declaration = identity("symbol|decl", "value", "Variable", std::string(kTopUri), rangeAt(3, 2, 7));
    const auto reference = identity("symbol|ref", "value", "Variable", std::string(kTopUri), rangeAt(3, 2, 7));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {declaration.stable_id, SnapshotNavigationTargetFact{.identity = declaration}},
        {reference.stable_id, SnapshotNavigationTargetFact{.identity = reference}}};
    const auto occurrences = occurrenceIndex(
        {SnapshotNavigationOccurrence{.stable_id = reference.stable_id, .location = reference.location},
         SnapshotNavigationOccurrence{.stable_id = declaration.stable_id,
                                      .location = declaration.location,
                                      .is_declaration = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = lookupAt(makeContext(occurrences, targets, by_symbol, aliases), 3, 4);

    REQUIRE(result.symbol.has_value());
    CHECK(result.symbol->stable_id == declaration.stable_id);
}

TEST_CASE("Navigation definitions return indexed macro definitions", "[analysis][semantic][navigation-index][macro]") {
    const auto occurrences = occurrenceIndex({});
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const std::vector<MacroInvocationFact> macros{
        MacroInvocationFact{.name = "READY",
                            .definition_uri = std::string(kChildUri),
                            .definition = MacroDefinition{.name = "READY"},
                            .range = rangeAt(0, 2, 8),
                            .selection_range = rangeAt(0, 3, 8),
                            .resolved = true}};

    const auto result = definitionsAt(makeContext(occurrences, targets, by_symbol, aliases, nullptr, nullptr, &macros),
                                      0,
                                      4);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.locations.size() == 1);
    CHECK(result.locations.front().uri == kChildUri);
}

TEST_CASE("Navigation definitions keep unresolved macros unresolved", "[analysis][semantic][navigation-index][macro][no-fallback]") {
    const auto occurrences = occurrenceIndex({});
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const std::vector<MacroInvocationFact> macros{
        MacroInvocationFact{.name = "MISSING", .range = rangeAt(0, 2, 10), .selection_range = rangeAt(0, 3, 10)}};

    const auto result = definitionsAt(makeContext(occurrences, targets, by_symbol, aliases, nullptr, nullptr, &macros),
                                      0,
                                      4);

    CHECK(result.unresolved);
    REQUIRE_FALSE(result.messages.empty());
}

TEST_CASE("Navigation type definition uses copied target facts without declaration fallback",
          "[analysis][semantic][navigation-index][type-definition][no-fallback]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(4, 6, 11));
    const auto type = locationAt(std::string(kChildUri), rangeAt(1, 8, 14));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id,
         SnapshotNavigationTargetFact{.identity = value, .type_display = "payload_t", .type_definition_locations = {type}}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location,
                                                                            .has_type_display = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = typeDefinitionsAt(makeContext(occurrences, targets, by_symbol, aliases), 4, 8);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.locations.size() == 1);
    CHECK(result.locations.front().uri == kChildUri);
}

TEST_CASE("Navigation type definition reports absent indexed type facts", "[analysis][semantic][navigation-index][type-definition][no-fallback]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(4, 6, 11));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = typeDefinitionsAt(makeContext(occurrences, targets, by_symbol, aliases), 4, 8);

    CHECK(result.unresolved);
    CHECK(result.locations.empty());
}

TEST_CASE("Navigation references merge same-range alias occurrence facts",
          "[analysis][semantic][navigation-index][references]") {
    const auto port = identity("symbol|port", "data", "Variable", std::string(kTopUri), rangeAt(1, 6, 10));
    const auto alias = identity("symbol|alias", "data", "Variable", std::string(kTopUri), rangeAt(1, 6, 10));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {port.stable_id, SnapshotNavigationTargetFact{.identity = port}},
        {alias.stable_id, SnapshotNavigationTargetFact{.identity = alias}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = port.stable_id,
                                                                            .location = port.location,
                                                                            .is_declaration = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {port.stable_id, {SnapshotNavigationOccurrence{.stable_id = port.stable_id,
                                                        .location = port.location,
                                                        .is_declaration = true}}},
        {alias.stable_id, {SnapshotNavigationOccurrence{.stable_id = alias.stable_id,
                                                         .location = locationAt(std::string(kChildUri), rangeAt(2, 4, 8))}}}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases{{port.stable_id,
                                                                              {alias.stable_id, port.stable_id}}};

    const auto result = referencesAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 7, true, 20);

    CHECK_FALSE(result.unresolved);
    CHECK(result.locations.size() == 2);
}

TEST_CASE("Navigation references honor the declaration filter", "[analysis][semantic][navigation-index][references]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location,
                                                                            .is_declaration = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id,
         {SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                       .location = value.location,
                                       .is_declaration = true},
          SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                       .location = locationAt(std::string(kTopUri), rangeAt(2, 8, 13))}}}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = referencesAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 5, false, 20);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.locations.size() == 1);
    CHECK(result.locations.front().range.start_line == 2);
}

TEST_CASE("Navigation highlights keep URI-local occurrences", "[analysis][semantic][navigation-index][highlight]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id,
         {SnapshotNavigationOccurrence{.stable_id = value.stable_id, .location = value.location},
          SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                       .location = locationAt(std::string(kChildUri), rangeAt(1, 4, 9))}}}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = documentHighlightsAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 5, 20);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.locations.size() == 1);
    CHECK(result.locations.front().uri == kTopUri);
}

TEST_CASE("Navigation implementations consume direct module instance edges",
          "[analysis][semantic][navigation-index][implementation]") {
    const auto module = identity("symbol|child", "child", "Definition", std::string(kChildUri), rangeAt(0, 7, 12));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {module.stable_id, SnapshotNavigationTargetFact{.identity = module}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = module.stable_id,
                                                                            .location = module.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {module.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    SnapshotImplementationEdgeIndex edges;
    edges.edges = {SnapshotImplementationEdge{.target_stable_id = module.stable_id,
                                              .implementation_stable_id = "instance|top|u0",
                                              .location = locationAt(std::string(kTopUri), rangeAt(3, 2, 7)),
                                              .kind = "moduleInstance"}};
    edges.edges_by_target_stable_id[module.stable_id] = {0};

    const auto result = implementationsAt(makeContext(occurrences, targets, by_symbol, aliases, &edges), 0, 8, 20);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.locations.size() == 1);
    CHECK(result.scanned_implementation_edge_count == 1);
}

TEST_CASE("Navigation implementations never scan absent target edges",
          "[analysis][semantic][navigation-index][implementation][no-fallback]") {
    const auto module = identity("symbol|child", "child", "Definition", std::string(kChildUri), rangeAt(0, 7, 12));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {module.stable_id, SnapshotNavigationTargetFact{.identity = module}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = module.stable_id,
                                                                            .location = module.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {module.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const SnapshotImplementationEdgeIndex edges;

    const auto result = implementationsAt(makeContext(occurrences, targets, by_symbol, aliases, &edges), 0, 8, 20);

    CHECK(result.unresolved);
    CHECK(result.scanned_implementation_edge_count == 0);
}

TEST_CASE("Navigation hover formats copied type display", "[analysis][semantic][navigation-index][hover]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value, .type_display = "logic [3:0]"}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location,
                                                                            .has_type_display = true}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = hoverAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 5);

    CHECK_FALSE(result.unresolved);
    CHECK(result.contents.find("logic [3:0]") != std::string::npos);
}

TEST_CASE("Navigation prepare rename accepts indexed source-backed identifiers",
          "[analysis][semantic][navigation-index][rename]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value, .rename_eligible = true}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id, occurrences.occurrences}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = prepareRenameAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 5);

    CHECK_FALSE(result.unresolved);
    CHECK(result.placeholder == "value");
}

TEST_CASE("Navigation prepare rename rejects macro virtual positions",
          "[analysis][semantic][navigation-index][rename][macro][no-fallback]") {
    const auto occurrences = occurrenceIndex({});
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const std::vector<MacroInvocationFact> macros{
        MacroInvocationFact{.name = "VALUE", .range = rangeAt(2, 2, 8), .selection_range = rangeAt(2, 3, 8)}};

    const auto result = prepareRenameAt(makeContext(occurrences, targets, by_symbol, aliases, nullptr, nullptr, &macros),
                                        2,
                                        4);

    CHECK(result.unresolved);
    REQUIRE_FALSE(result.messages.empty());
    CHECK(result.messages.front().find("macro rename") != std::string::npos);
}

TEST_CASE("Navigation rename emits value-type occurrence edits", "[analysis][semantic][navigation-index][rename]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value, .rename_eligible = true}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol{
        {value.stable_id,
         {SnapshotNavigationOccurrence{.stable_id = value.stable_id, .location = value.location},
          SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                       .location = locationAt(std::string(kTopUri), rangeAt(2, 8, 13))}}}};
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = renameAt(makeContext(occurrences, targets, by_symbol, aliases), 1, 5, "renamed", 20);

    REQUIRE_FALSE(result.unresolved);
    REQUIRE(result.edits.size() == 2);
    CHECK(std::all_of(result.edits.begin(), result.edits.end(), [](const SemanticTextEdit& edit) {
        return edit.new_text == "renamed";
    }));
}

TEST_CASE("Navigation semantic tokens scan only the URI-local occurrence index",
          "[analysis][semantic][navigation-index][tokens]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(1, 4, 9));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value}}};
    const auto occurrences = occurrenceIndex(
        {SnapshotNavigationOccurrence{.stable_id = value.stable_id, .location = value.location},
         SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                      .location = locationAt(std::string(kTopUri), rangeAt(2, 8, 13))}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;

    const auto result = semanticTokens(makeContext(occurrences, targets, by_symbol, aliases));

    CHECK_FALSE(result.unresolved);
    CHECK(result.scanned_occurrence_count == 2);
    CHECK(result.tokens.size() == 2);
}

TEST_CASE("Navigation selection ranges use the URI-local interval index",
          "[analysis][semantic][navigation-index][selection]") {
    const auto value = identity("symbol|value", "value", "Variable", std::string(kTopUri), rangeAt(2, 9, 14));
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets{
        {value.stable_id, SnapshotNavigationTargetFact{.identity = value}}};
    const auto occurrences = occurrenceIndex({SnapshotNavigationOccurrence{.stable_id = value.stable_id,
                                                                            .location = value.location}});
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const SnapshotSelectionRangeIndex selection_ranges{
        .ranges = {rangeAt(0, 0, 3, 9), rangeAt(2, 2, 2, 23)},
        .prefix_max_end_ranges = {rangeAt(0, 0, 3, 9), rangeAt(0, 0, 3, 9)}};

    const auto result = selectionRangesAt(
        makeContext(occurrences, targets, by_symbol, aliases, nullptr, nullptr, nullptr, nullptr, &selection_ranges),
        2,
        10);

    CHECK_FALSE(result.unresolved);
    CHECK(result.scanned_candidate_count <= 2);
    CHECK(result.ranges.size() >= 2);
}

TEST_CASE("Navigation selection range recovers an indexed callable selection",
          "[analysis][semantic][navigation-index][selection][callable]") {
    const auto occurrences = occurrenceIndex({});
    const std::unordered_map<std::string, SnapshotNavigationTargetFact> targets;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>> by_symbol;
    const std::unordered_map<std::string, std::vector<std::string>> aliases;
    const std::vector<CallableInvocationFact> calls{
        CallableInvocationFact{.name = "work", .selection_range = rangeAt(2, 4, 13), .resolved = true}};

    const auto result = selectionRangesAt(makeContext(occurrences, targets, by_symbol, aliases, nullptr, nullptr, nullptr, &calls),
                                          2,
                                          7);

    CHECK_FALSE(result.unresolved);
    REQUIRE_FALSE(result.ranges.empty());
    CHECK(result.ranges.front().range.start_character == 4);
}

TEST_CASE("Navigation cache scopes hover entries by generation", "[analysis][semantic][navigation-index][cache]") {
    QueryCache cache;
    cache.storeHover(4, kTopUri, 1, 4, SemanticHoverResult{.contents = "value"});

    REQUIRE(cache.hover(4, kTopUri, 1, 4).has_value());
    CHECK(cache.hover(5, kTopUri, 1, 4) == std::nullopt);
}

TEST_CASE("Navigation cache keeps definition and type-definition entries distinct",
          "[analysis][semantic][navigation-index][cache]") {
    QueryCache cache;
    cache.storeDefinitions(4, kTopUri, 1, 4, SemanticReferenceResult{});
    cache.storeTypeDefinitions(4, kTopUri, 1, 4, SemanticReferenceResult{});

    REQUIRE(cache.definitions(4, kTopUri, 1, 4).has_value());
    REQUIRE(cache.typeDefinitions(4, kTopUri, 1, 4).has_value());
}

TEST_CASE("Navigation cache clears prepare-rename and document-highlight entries",
          "[analysis][semantic][navigation-index][cache]") {
    QueryCache cache;
    cache.storePrepareRename(4, kTopUri, 1, 4, SemanticPrepareRenameResult{});
    cache.storeDocumentHighlights(4, kTopUri, 1, 4, SemanticReferenceResult{});
    cache.clear();

    CHECK(cache.prepareRename(4, kTopUri, 1, 4) == std::nullopt);
    CHECK(cache.documentHighlights(4, kTopUri, 1, 4) == std::nullopt);
}

TEST_CASE("Navigation cache evicts bounded implementation entries", "[analysis][semantic][navigation-index][cache]") {
    QueryCache cache;
    cache.setMaxEntriesPerQuery(1);
    cache.storeImplementations(4, kTopUri, 1, 4, SemanticReferenceResult{});
    cache.storeImplementations(4, kTopUri, 2, 4, SemanticReferenceResult{});

    CHECK(cache.implementations(4, kTopUri, 1, 4) == std::nullopt);
    REQUIRE(cache.implementations(4, kTopUri, 2, 4).has_value());
}

TEST_CASE("Navigation cache records and resets local navigation telemetry", "[analysis][semantic][navigation-index][cache][telemetry]") {
    QueryCache cache;
    cache.recordNavigationScan(3, 1, 2, 4, 5);

    const auto stats = cache.snapshotAndResetStats();
    CHECK(stats.navigation_occurrence_scanned == 3);
    CHECK(stats.navigation_target_lookup_scanned == 1);
    CHECK(stats.implementation_edge_scanned == 2);
    CHECK(stats.semantic_token_scanned_occurrences == 4);
    CHECK(stats.selection_range_scanned_candidates == 5);
    CHECK(cache.stats().navigation_occurrence_scanned == 0);
}

} // namespace pristine::analysis::semantic
