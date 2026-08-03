#include "pristine/server/SemanticTokenService.h"

#include <catch2/catch_test_macros.hpp>

namespace pristine::server {
namespace {

analysis::SemanticToken token(int line, int start, int end, std::string type) {
    return analysis::SemanticToken{.location = analysis::SemanticLocation{
                                       .uri = "file:///top.sv",
                                       .range = analysis::ParseRange{.start_line = line,
                                                                    .start_character = start,
                                                                    .end_line = line,
                                                                    .end_character = end}},
                                   .token_type = std::move(type),
                                   .token_modifier = {}};
}

std::vector<int> applyDelta(std::vector<int> previous, const SemanticTokenService::DeltaEdit& edit) {
    previous.erase(previous.begin() + static_cast<std::ptrdiff_t>(edit.start),
                   previous.begin() + static_cast<std::ptrdiff_t>(edit.start + edit.delete_count));
    previous.insert(previous.begin() + static_cast<std::ptrdiff_t>(edit.start),
                    edit.data.begin(),
                    edit.data.end());
    return previous;
}

} // namespace

TEST_CASE("SemanticTokenService encodes AST-backed tokens deterministically", "[server][semantic-tokens]") {
    analysis::SemanticTokenResult result;
    result.tokens = {token(1, 5, 8, "variable"), token(0, 2, 5, "class"), token(1, 5, 8, "variable")};

    CHECK(SemanticTokenService::encode(result) == std::vector<int>{0, 2, 3, 2, 0, 1, 5, 3, 6, 0});
}

TEST_CASE("SemanticTokenService delta is record-aligned and reconstructs full data",
          "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 0, 3, 6, 0, 0, 5, 3, 6, 0, 1, 0, 4, 5, 0};
    const std::vector<int> current{0, 0, 3, 6, 0, 0, 5, 4, 6, 0, 1, 0, 4, 5, 0, 0, 6, 2, 7, 0};
    const auto edit = SemanticTokenService::singleDelta(previous, current);

    CHECK(edit.start % 5 == 0);
    CHECK(edit.delete_count % 5 == 0);
    CHECK(edit.data.size() % 5 == 0);
    CHECK(applyDelta(previous, edit) == current);
}

TEST_CASE("SemanticTokenService no-op delta has no edits", "[server][semantic-tokens]") {
    const std::vector<int> data{0, 0, 3, 6, 0};
    const auto response = SemanticTokenService::deltaResponse("token-id", SemanticTokenService::singleDelta(data, data));
    CHECK(response.at("resultId") == "token-id");
    CHECK(response.at("edits").empty());
}

TEST_CASE("SemanticTokenService range and full responses preserve standard shapes", "[server][semantic-tokens]") {
    const auto range = SemanticTokenService::rangeResponse({0, 1, 2, 6, 0});
    CHECK(range.contains("data"));
    CHECK_FALSE(range.contains("resultId"));
    const auto full = SemanticTokenService::fullResponse("token-id", {0, 1, 2, 6, 0});
    CHECK(full.at("resultId") == "token-id");
    CHECK(full.at("data").size() == 5);
}

TEST_CASE("SemanticTokenService ignores unsupported token kinds", "[server][semantic-tokens]") {
    analysis::SemanticTokenResult result;
    result.tokens = {token(0, 0, 3, "unsupported")};
    CHECK(SemanticTokenService::encode(result).empty());
}

TEST_CASE("SemanticTokenService ignores multi-line semantic token ranges", "[server][semantic-tokens]") {
    analysis::SemanticTokenResult result;
    auto multi_line = token(0, 0, 3, "variable");
    multi_line.location.range.end_line = 1;
    result.tokens = {std::move(multi_line)};
    CHECK(SemanticTokenService::encode(result).empty());
}

TEST_CASE("SemanticTokenService delta reconstructs an insertion at the beginning", "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 3, 2, 6, 0};
    const std::vector<int> current{0, 0, 2, 2, 0, 0, 3, 2, 6, 0};
    CHECK(applyDelta(previous, SemanticTokenService::singleDelta(previous, current)) == current);
}

TEST_CASE("SemanticTokenService delta reconstructs an insertion at the end", "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 0, 2, 6, 0};
    const std::vector<int> current{0, 0, 2, 6, 0, 1, 0, 2, 2, 0};
    CHECK(applyDelta(previous, SemanticTokenService::singleDelta(previous, current)) == current);
}

TEST_CASE("SemanticTokenService delta reconstructs a complete deletion", "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 0, 2, 6, 0, 0, 3, 2, 2, 0};
    const std::vector<int> current{};
    CHECK(applyDelta(previous, SemanticTokenService::singleDelta(previous, current)) == current);
}

TEST_CASE("SemanticTokenService delta reconstructs a complete replacement", "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 0, 2, 6, 0};
    const std::vector<int> current{1, 2, 4, 5, 0};
    CHECK(applyDelta(previous, SemanticTokenService::singleDelta(previous, current)) == current);
}

TEST_CASE("SemanticTokenService delta edit omits empty replacement data", "[server][semantic-tokens]") {
    const std::vector<int> previous{0, 0, 2, 6, 0};
    const auto response = SemanticTokenService::deltaResponse("token-id", SemanticTokenService::singleDelta(previous, {}));
    REQUIRE(response.at("edits").size() == 1);
    CHECK_FALSE(response.at("edits").at(0).contains("data"));
}

TEST_CASE("SemanticTokenService preserves declaration modifier neutrality in encoding", "[server][semantic-tokens]") {
    analysis::SemanticTokenResult result;
    auto declaration = token(0, 0, 3, "variable");
    declaration.token_modifier = "declaration";
    result.tokens = {std::move(declaration)};
    CHECK(SemanticTokenService::encode(result) == std::vector<int>{0, 0, 3, 6, 0});
}

} // namespace pristine::server
