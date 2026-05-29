#include "../../src/analysis/semantic/NavigationProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace pristine::analysis::semantic {
namespace {

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

} // namespace

TEST_CASE("NavigationProvider maps semantic token kinds and declaration modifiers",
          "[analysis][semantic][navigation-provider][tokens]") {
    constexpr std::string_view uri = "file:///workspace/top.sv";
    NavigationContext context{.generation = 13,
                              .snapshot_available = true,
                              .document_uri = std::string(uri),
                              .symbols_by_id = {{"symbol|pkg",
                                                 SemanticSymbolIdentity{.stable_id = "symbol|pkg",
                                                                        .name = "pkg",
                                                                        .kind = "Package",
                                                                        .location = locationAt(std::string(uri),
                                                                                               rangeAt(0, 8, 11))}},
                                                {"symbol|mod",
                                                 SemanticSymbolIdentity{.stable_id = "symbol|mod",
                                                                        .name = "child",
                                                                        .kind = "Definition",
                                                                        .location = locationAt(std::string(uri),
                                                                                               rangeAt(1, 7, 12))}},
                                                {"symbol|param",
                                                 SemanticSymbolIdentity{.stable_id = "symbol|param",
                                                                        .name = "WIDTH",
                                                                        .kind = "Parameter",
                                                                        .location = locationAt(std::string(uri),
                                                                                               rangeAt(2, 18, 23))}}},
                              .references = {NavigationReference{.stable_id = "symbol|mod",
                                                                 .location = locationAt(std::string(uri),
                                                                                        rangeAt(1, 7, 12)),
                                                                 .is_declaration = true},
                                             NavigationReference{.stable_id = "symbol|pkg",
                                                                 .location = locationAt(std::string(uri),
                                                                                        rangeAt(4, 2, 5))},
                                             NavigationReference{.stable_id = "symbol|param",
                                                                 .location = locationAt(std::string(uri),
                                                                                        rangeAt(5, 9, 14))}}};

    const auto result = semanticTokens(context);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 13);
    REQUIRE(result.tokens.size() == 3);
    CHECK(result.tokens[0].token_type == "type");
    CHECK(result.tokens[0].token_modifier == "declaration");
    CHECK(result.tokens[1].token_type == "namespace");
    CHECK(result.tokens[2].token_type == "parameter");
}

TEST_CASE("NavigationProvider reports unavailable snapshots for semantic tokens",
          "[analysis][semantic][navigation-provider][tokens]") {
    const NavigationContext context{.generation = 4, .snapshot_available = false};

    const auto result = semanticTokens(context);

    CHECK(result.generation == 4);
    CHECK(result.unresolved);
    REQUIRE_FALSE(result.messages.empty());
}

TEST_CASE("NavigationProvider builds selection parent chain from AST ranges and line range",
          "[analysis][semantic][navigation-provider][selection]") {
    constexpr std::string_view uri = "file:///workspace/select.sv";
    const std::string text = "module top;\n"
                             "  logic ready;\n"
                             "  assign ready = ready;\n"
                             "endmodule\n";
    const NavigationContext context{.generation = 17,
                                    .snapshot_available = true,
                                    .document_uri = std::string(uri),
                                    .document_text = &text,
                                    .selection_ranges = {rangeAt(2, 2, 2, 23),
                                                         rangeAt(0, 0, 3, 9)}};
    const SemanticLookupResult lookup{.generation = 17,
                                      .query_location = locationAt(std::string(uri),
                                                                   rangeAt(2, 9, 14))};

    const auto result = selectionRangesAt(context, lookup, 2, 10);

    REQUIRE_FALSE(result.unresolved);
    CHECK(result.generation == 17);
    REQUIRE(result.ranges.size() >= 3);
    CHECK(result.ranges[0].range.start_line == 2);
    CHECK(result.ranges[0].range.start_character == 9);
    CHECK(result.ranges[0].range.end_character == 14);
    CHECK(result.ranges[0].parent.has_value());
    CHECK(result.ranges.back().parent == std::nullopt);
    CHECK(std::any_of(result.ranges.begin(), result.ranges.end(), [](const SemanticSelectionRange& range) {
        return range.range.start_line == 0 && range.range.end_line == 3;
    }));
}

} // namespace pristine::analysis::semantic
