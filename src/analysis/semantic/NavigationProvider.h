#pragma once

#include "SnapshotBuilder.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::analysis::semantic {

struct NavigationContext {
    SemanticEngineMode mode = SemanticEngineMode::Shallow;
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::string document_uri;
    const std::string* document_text = nullptr;
    const SnapshotNavigationOccurrenceIndex* occurrence_index = nullptr;
    const std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>>*
        occurrences_by_symbol = nullptr;
    const std::unordered_map<std::string, std::vector<std::string>>* reference_aliases_by_id = nullptr;
    const std::unordered_map<std::string, SnapshotNavigationTargetFact>* targets_by_id = nullptr;
    const SnapshotImplementationEdgeIndex* implementation_edges = nullptr;
    const std::vector<SnapshotTypeReference>* type_references = nullptr;
    const std::vector<MacroInvocationFact>* macro_invocations = nullptr;
    const std::vector<CallableInvocationFact>* callable_invocations = nullptr;
    const SnapshotSelectionRangeIndex* selection_range_index = nullptr;
};

[[nodiscard]] constexpr std::string_view navigationProviderName() {
    return "NavigationProvider";
}

[[nodiscard]] SemanticLookupResult lookupAt(const NavigationContext& context,
                                            int line,
                                            int character);
[[nodiscard]] SemanticReferenceResult definitionsAt(const NavigationContext& context,
                                                     int line,
                                                     int character);
[[nodiscard]] SemanticReferenceResult typeDefinitionsAt(const NavigationContext& context,
                                                         int line,
                                                         int character);
[[nodiscard]] SemanticReferenceResult referencesAt(const NavigationContext& context,
                                                    int line,
                                                    int character,
                                                    bool include_declaration,
                                                    size_t max_locations);
[[nodiscard]] SemanticReferenceResult documentHighlightsAt(const NavigationContext& context,
                                                            int line,
                                                            int character,
                                                            size_t max_locations);
[[nodiscard]] SemanticReferenceResult implementationsAt(const NavigationContext& context,
                                                         int line,
                                                         int character,
                                                         size_t max_locations);
[[nodiscard]] SemanticHoverResult hoverAt(const NavigationContext& context, int line, int character);
[[nodiscard]] SemanticPrepareRenameResult prepareRenameAt(const NavigationContext& context,
                                                           int line,
                                                           int character);
[[nodiscard]] SemanticRenameResult renameAt(const NavigationContext& context,
                                             int line,
                                             int character,
                                             std::string_view new_name,
                                             size_t max_locations);

[[nodiscard]] SemanticTokenResult semanticTokens(const NavigationContext& context);

[[nodiscard]] SemanticSelectionRangeResult selectionRangesAt(const NavigationContext& context,
                                                             int line,
                                                             int character);

} // namespace pristine::analysis::semantic
