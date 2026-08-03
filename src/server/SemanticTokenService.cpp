#include "pristine/server/SemanticTokenService.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace pristine::server {
namespace {

struct EncodedToken {
    int line = 0;
    int character = 0;
    int length = 0;
    int type = 0;
};

std::optional<int> tokenType(std::string_view value) {
    static constexpr std::string_view kTypes[] = {"namespace", "type", "class", "enum", "interface",
                                                   "function", "variable", "parameter", "enumMember"};
    const auto it = std::find(std::begin(kTypes), std::end(kTypes), value);
    if (it == std::end(kTypes)) {
        return std::nullopt;
    }
    return static_cast<int>(std::distance(std::begin(kTypes), it));
}

} // namespace

std::vector<int> SemanticTokenService::encode(const analysis::SemanticTokenResult& result) {
    std::vector<EncodedToken> tokens;
    tokens.reserve(result.tokens.size());
    for (const auto& token : result.tokens) {
        const auto type = tokenType(token.token_type);
        const auto& range = token.location.range;
        if (!type.has_value() || range.start_line != range.end_line ||
            range.end_character <= range.start_character) {
            continue;
        }
        tokens.push_back(EncodedToken{.line = range.start_line,
                                      .character = range.start_character,
                                      .length = range.end_character - range.start_character,
                                      .type = *type});
    }
    std::sort(tokens.begin(), tokens.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.line != rhs.line) {
            return lhs.line < rhs.line;
        }
        if (lhs.character != rhs.character) {
            return lhs.character < rhs.character;
        }
        if (lhs.length != rhs.length) {
            return lhs.length < rhs.length;
        }
        return lhs.type < rhs.type;
    });
    tokens.erase(std::unique(tokens.begin(), tokens.end(), [](const auto& lhs, const auto& rhs) {
                     return lhs.line == rhs.line && lhs.character == rhs.character &&
                            lhs.length == rhs.length && lhs.type == rhs.type;
                 }),
                 tokens.end());

    std::vector<int> data;
    data.reserve(tokens.size() * 5U);
    int previous_line = 0;
    int previous_character = 0;
    bool first = true;
    for (const auto& token : tokens) {
        const auto delta_line = first ? token.line : token.line - previous_line;
        const auto delta_character = first || delta_line != 0 ? token.character
                                                               : token.character - previous_character;
        data.insert(data.end(), {delta_line, delta_character, token.length, token.type, 0});
        previous_line = token.line;
        previous_character = token.character;
        first = false;
    }
    return data;
}

SemanticTokenService::DeltaEdit SemanticTokenService::singleDelta(const std::vector<int>& previous,
                                                                   const std::vector<int>& current) {
    constexpr size_t kTokenWidth = 5;
    const auto previous_count = previous.size() / kTokenWidth;
    const auto current_count = current.size() / kTokenWidth;
    const auto recordsEqual = [](const std::vector<int>& lhs,
                                 size_t lhs_index,
                                 const std::vector<int>& rhs,
                                 size_t rhs_index) {
        const auto lhs_start = lhs.begin() + static_cast<std::ptrdiff_t>(lhs_index * kTokenWidth);
        const auto rhs_start = rhs.begin() + static_cast<std::ptrdiff_t>(rhs_index * kTokenWidth);
        return std::equal(lhs_start, lhs_start + static_cast<std::ptrdiff_t>(kTokenWidth), rhs_start);
    };

    size_t prefix_records = 0;
    while (prefix_records < previous_count && prefix_records < current_count &&
           recordsEqual(previous, prefix_records, current, prefix_records)) {
        ++prefix_records;
    }

    size_t suffix_records = 0;
    while (suffix_records < previous_count - prefix_records &&
           suffix_records < current_count - prefix_records &&
           recordsEqual(previous,
                        previous_count - suffix_records - 1U,
                        current,
                        current_count - suffix_records - 1U)) {
        ++suffix_records;
    }

    DeltaEdit result;
    result.start = prefix_records * kTokenWidth;
    result.delete_count = (previous_count - prefix_records - suffix_records) * kTokenWidth;
    const auto current_end = (current_count - suffix_records) * kTokenWidth;
    result.data.assign(current.begin() + static_cast<std::ptrdiff_t>(result.start),
                       current.begin() + static_cast<std::ptrdiff_t>(current_end));
    return result;
}

nlohmann::json SemanticTokenService::fullResponse(std::string result_id,
                                                  const std::vector<int>& data) {
    return nlohmann::json{{"resultId", std::move(result_id)}, {"data", data}};
}

nlohmann::json SemanticTokenService::rangeResponse(const std::vector<int>& data) {
    return nlohmann::json{{"data", data}};
}

nlohmann::json SemanticTokenService::deltaResponse(std::string result_id, const DeltaEdit& edit) {
    if (edit.delete_count == 0 && edit.data.empty()) {
        return nlohmann::json{{"resultId", std::move(result_id)},
                              {"edits", nlohmann::json::array()}};
    }
    nlohmann::json edit_json{{"start", edit.start}, {"deleteCount", edit.delete_count}};
    if (!edit.data.empty()) {
        edit_json["data"] = edit.data;
    }
    return nlohmann::json{{"resultId", std::move(result_id)},
                          {"edits", nlohmann::json::array({std::move(edit_json)})}};
}

} // namespace pristine::server
