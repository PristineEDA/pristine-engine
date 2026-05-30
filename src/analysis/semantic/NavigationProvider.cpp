#include "NavigationProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>

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

bool rangeContainsRange(const ParseRange& outer, const ParseRange& inner) {
    if (inner.start_line < outer.start_line || inner.end_line > outer.end_line) {
        return false;
    }
    if (inner.start_line == outer.start_line && inner.start_character < outer.start_character) {
        return false;
    }
    if (inner.end_line == outer.end_line && inner.end_character > outer.end_character) {
        return false;
    }
    return true;
}

std::string tokenTypeForSymbolKind(std::string_view kind) {
    if (kind == "Package" || kind == "Namespace") {
        return "namespace";
    }
    if (kind == "Definition" || kind == "TypeAlias" || kind == "Type") {
        return "type";
    }
    if (kind == "ClassType") {
        return "class";
    }
    if (kind == "EnumType") {
        return "enum";
    }
    if (kind == "Interface" || kind == "Modport") {
        return "interface";
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return "function";
    }
    if (kind == "Parameter") {
        return "parameter";
    }
    if (kind == "EnumValue") {
        return "enumMember";
    }
    return "variable";
}

bool sameRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

} // namespace

SemanticTokenResult semanticTokens(const NavigationContext& context) {
    SemanticTokenResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& reference : context.references) {
        if (reference.location.uri != context.document_uri) {
            continue;
        }
        auto token_type = std::string("variable");
        if (const auto symbol_it = context.symbols_by_id.find(reference.stable_id);
            symbol_it != context.symbols_by_id.end()) {
            token_type = tokenTypeForSymbolKind(symbol_it->second.kind);
        }
        result.tokens.push_back(SemanticToken{.location = reference.location,
                                              .token_type = std::move(token_type),
                                              .token_modifier = reference.is_declaration
                                                                    ? std::string("declaration")
                                                                    : std::string{}});
    }
    std::sort(result.tokens.begin(), result.tokens.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

SemanticSelectionRangeResult selectionRangesAt(const NavigationContext& context,
                                               const SemanticLookupResult& lookup,
                                               int line,
                                               int character) {
    SemanticSelectionRangeResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (lookup.unresolved) {
        return result;
    }

    std::vector<ParseRange> ranges;
    ranges.push_back(lookup.query_location.range);
    if (context.snapshot_available) {
        for (const auto& candidate : context.selection_ranges) {
            if (parseRangeContainsPosition(candidate, line, character) &&
                rangeContainsRange(candidate, lookup.query_location.range)) {
                ranges.push_back(candidate);
            }
        }
    }
    if (context.document_text != nullptr) {
        if (const auto line_range = lineRangeAtPosition(*context.document_text, line, character)) {
            if (rangeContainsRange(*line_range, lookup.query_location.range)) {
                ranges.push_back(*line_range);
            }
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const ParseRange& lhs, const ParseRange& rhs) {
        if (lhs.start_line != rhs.start_line) {
            return lhs.start_line > rhs.start_line;
        }
        if (lhs.start_character != rhs.start_character) {
            return lhs.start_character > rhs.start_character;
        }
        if (lhs.end_line != rhs.end_line) {
            return lhs.end_line < rhs.end_line;
        }
        return lhs.end_character < rhs.end_character;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(), sameRange), ranges.end());

    std::vector<ParseRange> chain;
    for (const auto& candidate : ranges) {
        if (chain.empty() || rangeContainsRange(candidate, chain.back())) {
            chain.push_back(candidate);
        }
    }

    for (size_t index = 0; index < chain.size(); ++index) {
        const auto parent = index + 1 < chain.size()
                                ? std::optional<size_t>{index + 1}
                                : std::optional<size_t>{};
        result.ranges.push_back(SemanticSelectionRange{.range = chain[index], .parent = parent});
    }
    return result;
}

} // namespace pristine::analysis::semantic
