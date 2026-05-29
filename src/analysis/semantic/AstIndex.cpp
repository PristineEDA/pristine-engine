#include "AstIndex.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace pristine::analysis::semantic {
namespace {

bool locationLess(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    if (lhs.uri != rhs.uri) {
        return lhs.uri < rhs.uri;
    }
    if (lhs.range.start_line != rhs.range.start_line) {
        return lhs.range.start_line < rhs.range.start_line;
    }
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.range.end_line != rhs.range.end_line) {
        return lhs.range.end_line < rhs.range.end_line;
    }
    return lhs.range.end_character < rhs.range.end_character;
}

bool fuzzyMatch(std::string_view query, std::string_view candidate) {
    if (query.empty()) {
        return true;
    }

    auto query_it = query.begin();
    for (auto candidate_it = candidate.begin();
         query_it != query.end() && candidate_it != candidate.end(); ++candidate_it) {
        const auto query_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*query_it)));
        const auto candidate_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*candidate_it)));
        if (query_char == candidate_char) {
            ++query_it;
        }
    }

    return query_it == query.end();
}

int lspSymbolKindForSemanticKind(std::string_view kind) {
    if (kind == "Package" || kind == "Namespace") {
        return 4;
    }
    if (kind == "ClassType") {
        return 5;
    }
    if (kind == "EnumType") {
        return 10;
    }
    if (kind == "Interface" || kind == "Modport") {
        return 11;
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return 12;
    }
    if (kind == "Definition") {
        return 2;
    }
    if (kind == "TypeAlias" || kind == "Type") {
        return 26;
    }
    if (kind == "Parameter") {
        return 14;
    }
    if (kind == "EnumValue") {
        return 22;
    }
    if (kind == "Net" || kind == "Variable" || kind == "Field" || kind == "Member") {
        return 13;
    }
    return 0;
}

} // namespace

SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                               std::string_view query,
                                               size_t limit) {
    SemanticWorkspaceSymbolResult result{.generation = context.generation};
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::set<std::string> emitted_ids;
    for (const auto& symbol : context.symbols) {
        const auto& identity = symbol.identity;
        if (identity.name.empty() || identity.location.uri.empty() ||
            !fuzzyMatch(query, identity.name) || !emitted_ids.insert(symbol.stable_id).second) {
            continue;
        }
        result.symbols.push_back(SemanticWorkspaceSymbol{.name = identity.name,
                                                         .kind = lspSymbolKindForSemanticKind(identity.kind),
                                                         .location = identity.location,
                                                         .selection_range = identity.location.range,
                                                         .stable_id = symbol.stable_id});
    }

    std::sort(result.symbols.begin(), result.symbols.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return locationLess(lhs.location, rhs.location);
    });

    if (limit > 0 && result.symbols.size() > limit) {
        result.symbols.resize(limit);
        result.truncated = true;
        result.messages.push_back("workspace/symbol results were truncated at " + std::to_string(limit) +
                                  " entries");
    }
    return result;
}

} // namespace pristine::analysis::semantic
