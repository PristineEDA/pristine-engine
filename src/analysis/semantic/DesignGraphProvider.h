#pragma once

#include "pristine/analysis/SemanticEngine.h"
#include "SnapshotBuilder.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct DesignGraphModuleEntry {
    std::string uri;
    ModuleDefinition definition;
};

struct DesignGraphSymbol {
    SemanticSymbolIdentity identity;
};

struct DesignGraphRangeSymbol {
    ParseRange range;
    std::string stable_id;
};

struct DesignGraphContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::vector<std::string> top_modules;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> module_signatures_by_name;
    std::vector<DesignGraphModuleEntry> module_entries;
    SnapshotModuleCallEdgeIndex module_call_edge_index;
    std::unordered_map<std::string, DesignGraphSymbol> symbols_by_id;
    SnapshotDesignGraphBindingIndex binding_index;
    SnapshotConeAdjacencyIndex cone_adjacency_index;
};

[[nodiscard]] constexpr std::string_view designGraphProviderName() {
    return "DesignGraphProvider";
}

[[nodiscard]] SemanticModuleHierarchyResult moduleHierarchy(const DesignGraphContext& context,
                                                            std::optional<std::string_view> module_name,
                                                            int max_depth);

[[nodiscard]] SemanticSchematicResult schematic(const DesignGraphContext& context,
                                                std::optional<std::string_view> module_name,
                                                int max_depth);

[[nodiscard]] SemanticCallHierarchyPrepareResult prepareCallHierarchy(const DesignGraphContext& context,
                                                                      std::string_view document_uri,
                                                                      int line,
                                                                      int character);

[[nodiscard]] SemanticCallHierarchyCallsResult incomingCalls(const DesignGraphContext& context,
                                                            const SemanticCallHierarchyItem& item);

[[nodiscard]] SemanticCallHierarchyCallsResult outgoingCalls(const DesignGraphContext& context,
                                                            const SemanticCallHierarchyItem& item);

[[nodiscard]] SemanticConeTrace backwardCone(const DesignGraphContext& context,
                                             std::string_view document_uri,
                                             const SemanticLookupResult& lookup,
                                             size_t max_results,
                                             std::optional<SnapshotConeSliceFact> root_slice = std::nullopt);

} // namespace pristine::analysis::semantic
