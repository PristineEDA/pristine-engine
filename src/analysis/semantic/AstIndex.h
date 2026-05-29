#pragma once

#include "DesignGraphProvider.h"
#include "DiagnosticProvider.h"
#include "NavigationProvider.h"
#include "SnapshotBuilder.h"
#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct AstIndexView {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::vector<AstIndexSymbol> symbols;
    std::vector<NavigationReference> navigation_references;
    std::unordered_map<std::string, SemanticSymbolIdentity> navigation_symbols_by_id;
    std::unordered_map<std::string, DiagnosticSymbol> diagnostic_symbols_by_id;
    std::vector<DiagnosticReference> diagnostic_references;
    std::unordered_map<std::string, DesignGraphSymbol> design_graph_symbols_by_id;
    std::unordered_map<std::string, std::vector<DesignGraphRangeSymbol>> design_graph_symbol_ranges_by_uri;
};

[[nodiscard]] constexpr std::string_view astIndexProviderName() {
    return "AstIndex";
}

[[nodiscard]] AstIndexView buildAstIndexView(const SnapshotData* data,
                                             std::uint64_t generation);

[[nodiscard]] AstIndexContext workspaceSymbolContext(const AstIndexView& view);

[[nodiscard]] SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                                             std::string_view query,
                                                             size_t limit);

} // namespace pristine::analysis::semantic
