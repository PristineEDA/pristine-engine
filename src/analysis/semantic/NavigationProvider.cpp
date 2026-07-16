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

bool containsPosition(const ParseRange& range, int line, int character) {
    if (line < range.start_line || line > range.end_line) {
        return false;
    }
    if (line == range.start_line && character < range.start_character) {
        return false;
    }
    return line != range.end_line || character < range.end_character;
}

bool positionLess(int left_line, int left_character, int right_line, int right_character) {
    return left_line < right_line ||
           (left_line == right_line && left_character < right_character);
}

bool rangeEndBeforePosition(const ParseRange& range, int line, int character) {
    return positionLess(range.end_line, range.end_character, line, character) ||
           (range.end_line == line && range.end_character == character);
}

std::optional<SnapshotNavigationOccurrence> occurrenceAt(const SnapshotNavigationOccurrenceIndex& index,
                                                          int line,
                                                          int character,
                                                          size_t& scanned_occurrences) {
    scanned_occurrences = 0;
    const auto upper = std::upper_bound(index.occurrences.begin(),
                                        index.occurrences.end(),
                                        std::pair{line, character},
                                        [&](const auto& position,
                                            const SnapshotNavigationOccurrence& occurrence) {
                                            return positionLess(position.first,
                                                                position.second,
                                                                occurrence.location.range.start_line,
                                                                occurrence.location.range.start_character);
                                        });
    if (upper == index.occurrences.begin()) {
        return std::nullopt;
    }
    const SnapshotNavigationOccurrence* best = nullptr;
    for (auto it = upper; it != index.occurrences.begin();) {
        --it;
        ++scanned_occurrences;
        if (containsPosition(it->location.range, line, character) &&
            (best == nullptr || it->location.range.start_line > best->location.range.start_line ||
             (it->location.range.start_line == best->location.range.start_line &&
              it->location.range.start_character > best->location.range.start_character) ||
             (it->location.range.start_line == best->location.range.start_line &&
              it->location.range.start_character == best->location.range.start_character &&
              it->is_declaration != best->is_declaration && it->is_declaration) ||
             (it->location.range.start_line == best->location.range.start_line &&
              it->location.range.start_character == best->location.range.start_character &&
              it->is_declaration == best->is_declaration &&
              it->has_type_display != best->has_type_display && it->has_type_display) ||
             (it->location.range.start_line == best->location.range.start_line &&
              it->location.range.start_character == best->location.range.start_character &&
              it->is_declaration == best->is_declaration &&
              it->has_type_display == best->has_type_display && it->stable_id < best->stable_id))) {
            best = &*it;
        }
        const auto prefix_index = static_cast<size_t>(std::distance(index.occurrences.begin(), it));
        if (prefix_index == 0 ||
            rangeEndBeforePosition(index.prefix_max_end_ranges[prefix_index - 1], line, character)) {
            break;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<SnapshotNavigationOccurrence>(*best);
}

bool sameLocation(const SemanticLocation& left, const SemanticLocation& right) {
    return left.uri == right.uri && sameRange(left.range, right.range);
}

std::optional<MacroInvocationFact> macroInvocationAt(const NavigationContext& context,
                                                     int line,
                                                     int character) {
    if (context.macro_invocations == nullptr) {
        return std::nullopt;
    }
    const MacroInvocationFact* best = nullptr;
    for (const auto& invocation : *context.macro_invocations) {
        if (!containsPosition(invocation.range, line, character) &&
            !containsPosition(invocation.selection_range, line, character)) {
            continue;
        }
        if (best == nullptr || invocation.range.start_line > best->range.start_line ||
            (invocation.range.start_line == best->range.start_line &&
             invocation.range.start_character > best->range.start_character)) {
            best = &invocation;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<MacroInvocationFact>(*best);
}

std::string macroSignature(const MacroDefinition& macro) {
    std::string result = macro.name;
    if (!macro.function_like) {
        return result;
    }
    result += "(";
    for (size_t index = 0; index < macro.parameters.size(); ++index) {
        if (index > 0) {
            result += ", ";
        }
        result += macro.parameters[index];
    }
    result += ")";
    return result;
}

void sortUniqueLocations(std::vector<SemanticLocation>& locations) {
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
}

void appendOccurrencesForSymbol(const NavigationContext& context,
                                std::string_view stable_id,
                                bool include_declaration,
                                size_t max_locations,
                                SemanticReferenceResult& result) {
    if (context.occurrences_by_symbol == nullptr) {
        return;
    }
    std::vector<std::string> ids{std::string(stable_id)};
    if (context.reference_aliases_by_id != nullptr) {
        if (const auto aliases = context.reference_aliases_by_id->find(std::string(stable_id));
            aliases != context.reference_aliases_by_id->end()) {
            ids = aliases->second;
        }
    }
    for (const auto& id : ids) {
        const auto occurrences = context.occurrences_by_symbol->find(id);
        if (occurrences == context.occurrences_by_symbol->end()) {
            continue;
        }
        for (const auto& occurrence : occurrences->second) {
            ++result.scanned_occurrence_count;
            if (!include_declaration && occurrence.is_declaration) {
                continue;
            }
            if (max_locations > 0 && result.locations.size() >= max_locations) {
                result.truncated = true;
                return;
            }
            result.locations.push_back(occurrence.location);
            result.occurrences.push_back(
                SemanticReferenceOccurrence{.location = occurrence.location, .role = occurrence.role});
        }
    }
    sortUniqueLocations(result.locations);
    std::sort(result.occurrences.begin(), result.occurrences.end(), [](const auto& left, const auto& right) {
        if (!sameLocation(left.location, right.location)) {
            return locationLess(left.location, right.location);
        }
        return static_cast<int>(left.role) < static_cast<int>(right.role);
    });
    result.occurrences.erase(
        std::unique(result.occurrences.begin(), result.occurrences.end(), [](const auto& left, const auto& right) {
            return sameLocation(left.location, right.location) && left.role == right.role;
        }),
        result.occurrences.end());
}

void appendMacroUnresolved(std::vector<std::string>& messages) {
    messages.push_back("macro definition is unresolved in the indexed preprocessor facts");
}

} // namespace

SemanticLookupResult lookupAt(const NavigationContext& context, int line, int character) {
    SemanticLookupResult result;
    result.mode = context.mode;
    result.generation = context.generation;
    result.query_location = SemanticLocation{.uri = context.document_uri,
                                             .range = ParseRange{.start_line = line,
                                                                 .start_character = character,
                                                                 .end_line = line,
                                                                 .end_character = character}};
    result.unresolved = true;
    if (!context.snapshot_available || context.occurrence_index == nullptr ||
        context.targets_by_id == nullptr) {
        result.messages.push_back("AST-backed navigation index is unavailable");
        return result;
    }

    size_t scanned = 0;
    const auto occurrence = occurrenceAt(*context.occurrence_index, line, character, scanned);
    result.scanned_occurrence_count = scanned;
    if (!occurrence.has_value()) {
        result.messages.push_back("no indexed navigation target at position");
        return result;
    }
    result.query_location = occurrence->location;
    result.scanned_target_count = 1;
    const auto target = context.targets_by_id->find(occurrence->stable_id);
    if (target == context.targets_by_id->end()) {
        result.messages.push_back("indexed navigation target is unavailable");
        return result;
    }
    result.symbol = target->second.identity;
    result.unresolved = false;
    return result;
}

SemanticReferenceResult definitionsAt(const NavigationContext& context, int line, int character) {
    SemanticReferenceResult result;
    result.generation = context.generation;
    if (const auto macro = macroInvocationAt(context, line, character)) {
        result.unresolved = !macro->resolved || macro->definition_uri.empty();
        if (result.unresolved) {
            appendMacroUnresolved(result.messages);
        }
        else {
            result.locations.push_back(
                SemanticLocation{.uri = macro->definition_uri, .range = macro->definition.selection_range});
        }
        return result;
    }
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    if (lookup.symbol.has_value()) {
        result.locations.push_back(lookup.symbol->location);
    }
    return result;
}

SemanticReferenceResult typeDefinitionsAt(const NavigationContext& context, int line, int character) {
    SemanticReferenceResult result;
    result.generation = context.generation;
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    if (!lookup.symbol.has_value() || context.targets_by_id == nullptr) {
        return result;
    }
    if (context.type_references != nullptr) {
        for (const auto& reference : *context.type_references) {
            ++result.scanned_occurrence_count;
            if (containsPosition(reference.reference.range, line, character)) {
                result.locations = reference.definitions;
                result.unresolved = result.locations.empty();
                if (result.unresolved) {
                    result.messages.push_back("type definition is unresolved in indexed type facts");
                }
                return result;
            }
        }
    }
    const auto target = context.targets_by_id->find(lookup.symbol->stable_id);
    if (target == context.targets_by_id->end() || target->second.type_definition_locations.empty()) {
        result.unresolved = true;
        result.messages.push_back("type definition is unavailable in indexed navigation facts");
        return result;
    }
    result.locations = target->second.type_definition_locations;
    result.unresolved = false;
    return result;
}

SemanticReferenceResult referencesAt(const NavigationContext& context,
                                     int line,
                                     int character,
                                     bool include_declaration,
                                     size_t max_locations) {
    SemanticReferenceResult result;
    result.generation = context.generation;
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    if (lookup.symbol.has_value()) {
        appendOccurrencesForSymbol(context,
                                   lookup.symbol->stable_id,
                                   include_declaration,
                                   max_locations,
                                   result);
    }
    return result;
}

SemanticReferenceResult documentHighlightsAt(const NavigationContext& context,
                                             int line,
                                             int character,
                                             size_t max_locations) {
    auto result = referencesAt(context, line, character, true, max_locations);
    result.locations.erase(std::remove_if(result.locations.begin(), result.locations.end(), [&](const auto& location) {
                             return location.uri != context.document_uri;
                         }),
                         result.locations.end());
    result.occurrences.erase(
        std::remove_if(result.occurrences.begin(), result.occurrences.end(), [&](const auto& occurrence) {
            return occurrence.location.uri != context.document_uri;
        }),
        result.occurrences.end());
    return result;
}

SemanticReferenceResult implementationsAt(const NavigationContext& context,
                                          int line,
                                          int character,
                                          size_t max_locations) {
    SemanticReferenceResult result;
    result.generation = context.generation;
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    if (!lookup.symbol.has_value() || context.implementation_edges == nullptr) {
        return result;
    }
    const auto edges = context.implementation_edges->edges_by_target_stable_id.find(lookup.symbol->stable_id);
    if (edges == context.implementation_edges->edges_by_target_stable_id.end()) {
        result.unresolved = true;
        result.messages.push_back("implementation is unavailable in indexed implementation facts");
        return result;
    }
    for (const auto edge_index : edges->second) {
        ++result.scanned_implementation_edge_count;
        if (edge_index >= context.implementation_edges->edges.size()) {
            continue;
        }
        if (max_locations > 0 && result.locations.size() >= max_locations) {
            result.truncated = true;
            break;
        }
        result.locations.push_back(context.implementation_edges->edges[edge_index].location);
    }
    sortUniqueLocations(result.locations);
    result.unresolved = result.locations.empty();
    return result;
}

SemanticHoverResult hoverAt(const NavigationContext& context, int line, int character) {
    SemanticHoverResult result;
    result.generation = context.generation;
    if (const auto macro = macroInvocationAt(context, line, character)) {
        result.range = macro->selection_range;
        result.unresolved = !macro->resolved;
        if (result.unresolved) {
            appendMacroUnresolved(result.messages);
            return result;
        }
        result.contents = "**macro** `" + macroSignature(macro->definition) + "`";
        if (!macro->expansion_text.empty()) {
            result.contents += "\n\nExpansion: `" + macro->expansion_text + "`";
        }
        return result;
    }
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    result.scanned_target_count = lookup.scanned_target_count;
    if (!lookup.symbol.has_value() || context.targets_by_id == nullptr) {
        return result;
    }
    const auto target = context.targets_by_id->find(lookup.symbol->stable_id);
    if (target == context.targets_by_id->end()) {
        result.unresolved = true;
        result.messages.push_back("indexed navigation target is unavailable");
        return result;
    }
    result.contents = "**" + target->second.identity.kind + "** `" + target->second.identity.name + "`";
    if (!target->second.type_display.empty()) {
        result.contents += "\n\nType: `" + target->second.type_display + "`";
    }
    result.range = lookup.query_location.range;
    return result;
}

SemanticPrepareRenameResult prepareRenameAt(const NavigationContext& context, int line, int character) {
    SemanticPrepareRenameResult result;
    result.generation = context.generation;
    if (macroInvocationAt(context, line, character).has_value()) {
        result.unresolved = true;
        result.messages.push_back("macro rename is unsupported by indexed navigation facts");
        return result;
    }
    const auto lookup = lookupAt(context, line, character);
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    result.scanned_occurrence_count = lookup.scanned_occurrence_count;
    result.scanned_target_count = lookup.scanned_target_count;
    if (!lookup.symbol.has_value() || context.targets_by_id == nullptr) {
        return result;
    }
    const auto target = context.targets_by_id->find(lookup.symbol->stable_id);
    if (target == context.targets_by_id->end() || !target->second.rename_eligible) {
        result.unresolved = true;
        result.messages.push_back("symbol is not eligible for indexed rename");
        return result;
    }
    result.placeholder = target->second.identity.name;
    result.range = lookup.query_location.range;
    return result;
}

SemanticRenameResult renameAt(const NavigationContext& context,
                              int line,
                              int character,
                              std::string_view new_name,
                              size_t max_locations) {
    SemanticRenameResult result;
    const auto prepared = prepareRenameAt(context, line, character);
    result.generation = prepared.generation;
    result.messages = prepared.messages;
    result.unresolved = prepared.unresolved;
    result.scanned_occurrence_count = prepared.scanned_occurrence_count;
    if (prepared.unresolved) {
        return result;
    }
    const auto references = referencesAt(context, line, character, true, max_locations);
    result.messages.insert(result.messages.end(), references.messages.begin(), references.messages.end());
    result.unresolved = references.unresolved;
    result.truncated = references.truncated;
    result.scanned_occurrence_count += references.scanned_occurrence_count;
    for (const auto& location : references.locations) {
        result.edits.push_back(SemanticTextEdit{.location = location, .new_text = std::string(new_name)});
    }
    return result;
}

SemanticTokenResult semanticTokens(const NavigationContext& context) {
    SemanticTokenResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    if (context.occurrence_index == nullptr || context.targets_by_id == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed navigation token index is unavailable");
        return result;
    }
    for (const auto& reference : context.occurrence_index->occurrences) {
        ++result.scanned_occurrence_count;
        auto token_type = std::string("variable");
        if (const auto target = context.targets_by_id->find(reference.stable_id);
            target != context.targets_by_id->end()) {
            token_type = tokenTypeForSymbolKind(target->second.identity.kind);
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
                                               int line,
                                               int character) {
    auto lookup = lookupAt(context, line, character);
    if (lookup.unresolved && context.callable_invocations != nullptr) {
        const CallableInvocationFact* best_call = nullptr;
        for (const auto& call : *context.callable_invocations) {
            if (!containsPosition(call.selection_range, line, character)) {
                continue;
            }
            if (best_call == nullptr ||
                rangeContainsRange(best_call->selection_range, call.selection_range)) {
                best_call = &call;
            }
        }
        if (best_call != nullptr) {
            lookup.query_location = SemanticLocation{.uri = context.document_uri,
                                                     .range = best_call->selection_range};
            lookup.messages.clear();
            lookup.unresolved = false;
        }
    }
    SemanticSelectionRangeResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (lookup.unresolved) {
        return result;
    }

    std::vector<ParseRange> ranges;
    ranges.push_back(lookup.query_location.range);
    if (context.snapshot_available && context.selection_range_index != nullptr) {
        const auto& index = *context.selection_range_index;
        const auto upper = std::upper_bound(index.ranges.begin(),
                                            index.ranges.end(),
                                            std::pair{line, character},
                                            [](const auto& position, const ParseRange& range) {
                                                return position.first < range.start_line ||
                                                       (position.first == range.start_line &&
                                                        position.second < range.start_character);
                                            });
        for (auto it = upper; it != index.ranges.begin();) {
            --it;
            ++result.scanned_candidate_count;
            if (containsPosition(*it, line, character) &&
                rangeContainsRange(*it, lookup.query_location.range)) {
                ranges.push_back(*it);
            }
            const auto prefix_index = static_cast<size_t>(std::distance(index.ranges.begin(), it));
            if (prefix_index == 0 ||
                (index.prefix_max_end_ranges[prefix_index - 1].end_line < line ||
                 (index.prefix_max_end_ranges[prefix_index - 1].end_line == line &&
                  index.prefix_max_end_ranges[prefix_index - 1].end_character <= character))) {
                break;
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
