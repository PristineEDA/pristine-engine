#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct NavigationReference {
    std::string stable_id;
    SemanticLocation location;
    bool is_declaration = false;
};

struct NavigationContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::string document_uri;
    const std::string* document_text = nullptr;
    std::unordered_map<std::string, SemanticSymbolIdentity> symbols_by_id;
    std::vector<NavigationReference> references;
    std::vector<ParseRange> selection_ranges;
};

[[nodiscard]] constexpr std::string_view navigationProviderName() {
    return "NavigationProvider";
}

[[nodiscard]] SemanticTokenResult semanticTokens(const NavigationContext& context);

[[nodiscard]] SemanticSelectionRangeResult selectionRangesAt(const NavigationContext& context,
                                                             const SemanticLookupResult& lookup,
                                                             int line,
                                                             int character);

} // namespace pristine::analysis::semantic
