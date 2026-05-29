#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::analysis::semantic {

struct AstIndexSymbol {
    std::string stable_id;
    SemanticSymbolIdentity identity;
};

struct AstIndexContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::vector<AstIndexSymbol> symbols;
};

[[nodiscard]] constexpr std::string_view astIndexProviderName() {
    return "AstIndex";
}

[[nodiscard]] SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                                             std::string_view query,
                                                             size_t limit);

} // namespace pristine::analysis::semantic
