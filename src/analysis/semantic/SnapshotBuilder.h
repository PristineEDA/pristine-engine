#pragma once

#include "AffectedDependencyGraph.h"
#include "SignatureInlayProvider.h"
#include "pristine/analysis/SemanticEngine.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang {
class SourceManager;
}

namespace slang::ast {
class Compilation;
class Symbol;
}

namespace slang::syntax {
class SyntaxTree;
}

namespace pristine::analysis::semantic {

struct SnapshotIndexedSymbol {
    SemanticSymbolIdentity identity;
    const slang::ast::Symbol* symbol = nullptr;
    std::string type_display;
    std::string value_display;
};

struct SnapshotIndexedReference {
    std::string stable_id;
    std::string name;
    SemanticLocation location;
    bool is_declaration = false;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
};

// URI-local occurrence indexes keep cursor lookup independent from the total
// number of references in the snapshot. The ranges are stored as indexes into
// SnapshotData::references so the canonical occurrence facts have one owner.
struct SnapshotReferenceOccurrenceIndex {
    std::vector<size_t> reference_indexes;
    std::vector<ParseRange> prefix_max_end_ranges;
};

// Navigation queries use their own value-type occurrence view so providers do
// not need to reach back into SnapshotData::references or AST-owned symbols.
struct SnapshotNavigationOccurrence {
    std::string stable_id;
    SemanticLocation location;
    bool is_declaration = false;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
    bool has_type_display = false;
};

struct SnapshotNavigationOccurrenceIndex {
    std::vector<SnapshotNavigationOccurrence> occurrences;
    std::vector<ParseRange> prefix_max_end_ranges;
};

struct SnapshotNavigationTargetFact {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    std::vector<SemanticLocation> type_definition_locations;
    bool rename_eligible = false;
};

struct SnapshotImplementationEdge {
    std::string target_stable_id;
    std::string implementation_stable_id;
    SemanticLocation location;
    std::string kind;
};

struct SnapshotImplementationEdgeIndex {
    std::vector<SnapshotImplementationEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_target_stable_id;
};

struct SnapshotSelectionRangeIndex {
    std::vector<ParseRange> ranges;
    std::vector<ParseRange> prefix_max_end_ranges;
};

struct SnapshotMemberCompletion {
    std::string qualifier;
    SemanticSymbolIdentity identity;
    std::string type_display;
};

enum class SnapshotVisibilityOrigin {
    Local,
    ExplicitImport,
    WildcardImport,
    PackageExport,
    Workspace,
};

struct SnapshotVisibilityCandidate {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    SnapshotVisibilityOrigin origin = SnapshotVisibilityOrigin::Local;
};

struct SnapshotScopeVisibility {
    std::string stable_id;
    std::string parent_stable_id;
    std::string uri;
    ParseRange range;
    std::string context_kind;
    int lexical_depth = 0;
    std::vector<SnapshotVisibilityCandidate> candidates;
};

struct SnapshotPackageVisibility {
    std::string package_name;
    std::string uri;
    std::vector<SnapshotVisibilityCandidate> candidates;
    std::vector<std::string> exported_packages;
};

struct SnapshotVisibleMacro {
    MacroDefinition definition;
    std::string source_uri;
    ParseRange available_after;
    std::optional<ParseRange> unavailable_after;
};

enum class SnapshotCompletionResolveKind {
    Symbol,
    Member,
    Module,
    Port,
    Macro,
};

// A completion item's data is an opaque key into this build-time view. Providers
// must not reconstruct a target by parsing the key or scanning unrelated facts.
struct SnapshotCompletionResolveFact {
    SnapshotCompletionResolveKind kind = SnapshotCompletionResolveKind::Symbol;
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    std::string module_uri;
    std::optional<ModuleDefinition> module;
    std::optional<SchematicPort> port;
    std::optional<MacroDefinition> macro;
};

struct SnapshotDiagnosticLookupIndex {
    std::unordered_map<std::string, std::vector<std::string>> package_definition_ids_by_name;
    std::unordered_map<std::string, std::vector<SemanticLocation>> type_definition_locations_by_name;
    std::unordered_map<std::string, std::vector<std::string>> package_names_by_member;
    std::unordered_map<std::string, size_t> package_member_definition_counts;
    std::unordered_map<std::string, std::vector<SemanticSymbolIdentity>> duplicate_symbols_by_uri;
};

struct SnapshotInactiveRegion {
    SemanticLocation location;
    std::string directive_id;
};

struct SnapshotMacroUndef {
    std::string name;
    std::string source_uri;
    ParseRange range;
};

struct SnapshotModuleInstance {
    std::string module_name;
    std::string instance_name;
    std::string instance_stable_id;
    std::string type_display;
    std::string target_stable_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
    std::vector<SchematicConnection> port_connections;
    std::vector<SchematicConnection> parameter_connections;
};

struct SnapshotModuleEntry {
    std::string uri;
    ModuleDefinition definition;
};

struct SnapshotModuleCallHierarchyItem {
    std::string id;
    std::string name;
    std::string kind;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
};

struct SnapshotModuleCallEdge {
    std::string caller_item_id;
    std::string callee_item_id;
    std::string instance_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
};

struct SnapshotModuleCallHierarchyRange {
    ParseRange range;
    std::string item_id;
};

struct SnapshotModuleCallEdgeIndex {
    std::unordered_map<std::string, SnapshotModuleCallHierarchyItem> items_by_id;
    std::vector<SnapshotModuleCallEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_caller_item_id;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_callee_item_id;
    std::unordered_map<std::string, std::vector<SnapshotModuleCallHierarchyRange>> items_by_uri;
};

struct SnapshotAssignmentEdgeSeed {
    std::string uri;
    ParseRange scope_range;
    ParseRange assignment_range;
    ParseRange left_range;
    ParseRange right_range;
    std::string left_expression;
    std::string right_expression;
};

struct SnapshotAssignmentEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    SemanticLocation expression_location;
    std::string expression;
};

enum class SnapshotGraphEndpointKind { Port, Parameter, Instance, InterfacePort, ModportPort };
enum class SnapshotGraphPortDirection { Input, Output, Inout, Ref, Unknown };
enum class SnapshotConeEdgeKind { Assignment, InstancePort, ParameterOverride };

// A graph endpoint retains the AST-resolved identity and direction used by
// hierarchy, schematic, and cone queries. It is intentionally value-only so
// provider queries never need to recover endpoint meaning from a port name.
struct SnapshotGraphEndpointFact {
    std::string stable_id;
    std::string module_name;
    std::string name;
    SnapshotGraphEndpointKind kind = SnapshotGraphEndpointKind::Port;
    SnapshotGraphPortDirection direction = SnapshotGraphPortDirection::Unknown;
    SemanticLocation location;
    std::string generated_instance_id;
};

struct SnapshotGraphConnectionBindingFact {
    std::string instance_stable_id;
    std::string endpoint_stable_id;
    SemanticLocation location;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::InstancePort;
    std::vector<std::string> source_symbol_ids;
};

struct SnapshotConeAdjacencyEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    SemanticLocation expression_location;
    std::string expression;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::Assignment;
    std::string generated_instance_id;
};

// Design graph queries consume these precomputed maps rather than rebuilding
// name, range, or assignment relationships while serving a request.
struct SnapshotDesignGraphBindingIndex {
    std::unordered_map<std::string, std::string> symbol_ids_by_uri_range;
    std::unordered_map<std::string, std::string> symbol_ids_by_module_scope_name;
    std::unordered_map<std::string, std::string> port_symbol_ids_by_module_port;
    std::unordered_map<std::string, std::string> parameter_symbol_ids_by_module_parameter;
    std::unordered_map<std::string, std::string> instance_ids_by_uri_range;
    std::unordered_map<std::string, SnapshotGraphEndpointFact> endpoints_by_module_member;
    std::unordered_map<std::string, SnapshotGraphEndpointFact> endpoints_by_stable_id;
    std::vector<SnapshotGraphConnectionBindingFact> connection_bindings;
    std::unordered_map<std::string, std::vector<size_t>> connection_bindings_by_uri_range;
};

struct SnapshotConeAdjacencyIndex {
    std::vector<SnapshotConeAdjacencyEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_from_symbol_id;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_to_symbol_id;
};

struct SnapshotTypeReference {
    SemanticLocation reference;
    std::string type_name;
    std::vector<SemanticLocation> definitions;
};

struct SemanticModuleSignature {
    ModuleDefinition definition;
    ModuleSchematic schematic;
    std::string uri;
};

struct SnapshotData {
    SnapshotData();
    ~SnapshotData();
    SnapshotData(SnapshotData&&) noexcept;
    SnapshotData& operator=(SnapshotData&&) noexcept;
    SnapshotData(const SnapshotData&) = delete;
    SnapshotData& operator=(const SnapshotData&) = delete;

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, SnapshotIndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<SnapshotIndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<std::string>> reference_aliases_by_id;
    std::unordered_map<std::string, SnapshotReferenceOccurrenceIndex> reference_occurrences_by_uri;
    std::unordered_map<std::string, SnapshotNavigationOccurrenceIndex> navigation_occurrences_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>>
        navigation_occurrences_by_symbol;
    std::unordered_map<std::string, SnapshotNavigationTargetFact> navigation_targets_by_id;
    SnapshotImplementationEdgeIndex implementation_edge_index;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
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
    std::vector<SnapshotModuleEntry> module_entries;
    SnapshotModuleCallEdgeIndex module_call_edge_index;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> ast_module_signatures_by_name;
    std::vector<SnapshotAssignmentEdgeSeed> assignment_edge_seeds;
    std::unordered_map<std::string, std::vector<SnapshotAssignmentEdge>> assignment_edges_by_uri;
    SnapshotDesignGraphBindingIndex design_graph_binding_index;
    SnapshotConeAdjacencyIndex cone_adjacency_index;
    std::unordered_map<std::string, std::vector<SnapshotTypeReference>> type_references_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<CallableInvocationFact>> callable_invocations_by_uri;
    std::unordered_map<std::string, std::vector<MacroInvocationFact>> macro_invocations_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlaySymbol>> inlay_symbols_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, SnapshotSelectionRangeIndex> selection_range_indexes_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotMacroUndef>> macro_undefs_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
};

struct SnapshotBuildInput {
    std::uint64_t generation = 0;
    SemanticEngineConfig config;
    std::vector<std::string> dirty_document_uris;
    std::unordered_map<std::string, SemanticEngineDocument> documents;
};

struct SnapshotBuildInputSummary {
    size_t document_count = 0;
    size_t open_document_count = 0;
    size_t dirty_document_count = 0;
    size_t top_module_count = 0;
    size_t index_config_count = 0;
};

struct SnapshotBuildOutput {
    SemanticEngineSnapshot snapshot;
    std::unique_ptr<SnapshotData> data;
    AffectedDependencyGraph affected_dependencies;
};

[[nodiscard]] constexpr std::string_view snapshotBuilderProviderName() {
    return "SnapshotBuilder";
}

[[nodiscard]] SnapshotBuildInput normalizeSnapshotBuildInput(SnapshotBuildInput input);
[[nodiscard]] SnapshotBuildInputSummary snapshotBuildInputSummary(const SnapshotBuildInput& input);

class SnapshotBuilder {
public:
    [[nodiscard]] SnapshotBuildOutput build(SnapshotBuildInput input) const;
};

} // namespace pristine::analysis::semantic
