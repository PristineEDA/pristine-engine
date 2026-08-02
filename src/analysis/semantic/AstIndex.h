#pragma once

#include "DesignGraphProvider.h"
#include "DiagnosticProvider.h"
#include "NavigationProvider.h"
#include "CompletionProvider.h"
#include "SignatureInlayProvider.h"
#include "SnapshotBuilder.h"
#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <optional>
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
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> module_signatures_by_name;
    std::vector<DesignGraphModuleEntry> design_graph_module_entries;
    SnapshotDesignGraphBindingIndex design_graph_binding_index;
    SnapshotConeAdjacencyIndex cone_adjacency_index;
    SnapshotInterfaceModportBindingIndex interface_modport_binding_index;
    std::unordered_map<std::string, std::vector<SnapshotAssignmentEdge>> assignment_edges_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotTypeReference>> type_references_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>> member_completions_by_uri;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>>>
        member_completions_by_qualifier_by_uri;
    std::unordered_map<std::string, SnapshotMemberCompletion> member_completions_by_stable_id;
    std::unordered_map<std::string, std::vector<SnapshotScopeVisibility>> scope_visibility_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotVisibilityCandidate>> document_visibility_by_uri;
    std::unordered_map<std::string, SnapshotPackageVisibility> package_visibility_by_name;
    std::vector<SnapshotVisibilityCandidate> workspace_visibility;
    std::unordered_map<std::string, std::string> module_definition_ids_by_name;
    std::unordered_map<std::string, std::vector<SnapshotVisibleMacro>> visible_macros_by_uri;
    std::unordered_map<std::string, SnapshotCompletionResolveFact> completion_resolve_by_id;
    SnapshotDiagnosticLookupIndex diagnostic_lookup_index;
    std::unordered_map<std::string, std::vector<SnapshotInactiveRegion>> inactive_regions_by_uri;
    size_t scope_visibility_count = 0;
    size_t package_visibility_count = 0;
    size_t member_visibility_count = 0;
    size_t callable_visibility_count = 0;
    std::int64_t scope_visibility_build_micros = 0;
    size_t inactive_region_count = 0;
    std::int64_t inactive_region_build_micros = 0;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlayModuleInstance>> signature_module_instances_by_uri;
    std::unordered_map<std::string, std::vector<CallableInvocationFact>> callable_invocations_by_uri;
    std::unordered_map<std::string, std::vector<MacroInvocationFact>> macro_invocations_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlaySymbol>> inlay_symbols_by_uri;
    std::unordered_map<std::string, SnapshotNavigationOccurrenceIndex> navigation_occurrences_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>>
        navigation_occurrences_by_symbol;
    std::unordered_map<std::string, SnapshotNavigationTargetFact> navigation_targets_by_id;
    SnapshotImplementationEdgeIndex implementation_edge_index;
    std::unordered_map<std::string, SnapshotSelectionRangeIndex> selection_range_indexes_by_uri;
    std::unordered_map<std::string, DiagnosticSymbol> diagnostic_symbols_by_id;
    std::vector<DiagnosticReference> diagnostic_references;
    std::unordered_map<std::string, DesignGraphSymbol> design_graph_symbols_by_id;
    SnapshotModuleCallEdgeIndex module_call_edge_index;
};

struct ReferenceOccurrenceLookup {
    std::string stable_id;
    SemanticLocation location;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
    size_t scanned_occurrence_count = 0;
};

[[nodiscard]] constexpr std::string_view astIndexProviderName() {
    return "AstIndex";
}

void buildAstIndexes(SnapshotData& data,
                     const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                     const SnapshotBuildInput::Control& control = {});

[[nodiscard]] AstIndexView buildAstIndexView(const SnapshotData* data,
                                             std::uint64_t generation);

[[nodiscard]] std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
                                                                std::string_view name);

[[nodiscard]] std::optional<std::string> findSymbolIdByNameAndKind(const SnapshotData& data,
                                                                   std::string_view name,
                                                                   std::string_view kind);

[[nodiscard]] std::vector<SemanticLocation> typeDefinitionLocationsByName(const SnapshotData& data,
                                                                          std::string_view name);

[[nodiscard]] std::vector<SemanticLocation> typeDefinitionLocationsAt(const AstIndexView& view,
                                                                      std::string_view uri,
                                                                      int line,
                                                                      int character);

[[nodiscard]] std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                                            std::string_view uri,
                                                            int line,
                                                            int character);

[[nodiscard]] std::optional<ReferenceOccurrenceLookup> referenceOccurrenceAtLocation(
    const SnapshotData& data,
    std::string_view uri,
    int line,
    int character);

[[nodiscard]] std::optional<SnapshotNavigationOccurrence> navigationOccurrenceAtLocation(
    const SnapshotNavigationOccurrenceIndex& index,
    int line,
    int character,
    size_t& scanned_occurrences);

[[nodiscard]] std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                               std::string_view stable_id,
                                                               bool include_declaration,
                                                               size_t max_locations,
                                                               bool& truncated);
[[nodiscard]] SemanticReferenceRole referenceRoleAtLocation(const SnapshotData& data,
                                                            std::string_view stable_id,
                                                            const SemanticLocation& location);

[[nodiscard]] std::optional<MacroInvocationFact> macroInvocationAt(const AstIndexView& view,
                                                                   std::string_view uri,
                                                                   int line,
                                                                   int character);

[[nodiscard]] std::vector<ParseRange> inactiveRegionsForUri(const AstIndexView& view,
                                                             std::string_view uri);

[[nodiscard]] std::optional<SemanticLocation> declarationLocationForSymbol(
    const slang::SourceManager& source_manager,
    const slang::ast::Symbol& symbol);

[[nodiscard]] AstIndexContext workspaceSymbolContext(const AstIndexView& view);

[[nodiscard]] SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                                             std::string_view query,
                                                             size_t limit);

} // namespace pristine::analysis::semantic
