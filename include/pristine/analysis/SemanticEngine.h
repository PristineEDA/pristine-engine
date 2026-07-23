#pragma once

#include "pristine/analysis/CompilationService.h"

#include <cstddef>
#include <cstdint>
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
}

namespace slang::syntax {
class SyntaxTree;
}

namespace pristine::analysis {

namespace semantic {
class AffectedDependencyGraph;
class QueryCache;
struct SnapshotData;
}

enum class SemanticEngineMode {
    Shallow,
    Design
};

struct SemanticEngineConfig {
    struct IndexConfig {
        std::vector<std::string> dirs;
        std::vector<std::string> exclude_dirs;
    };

    std::optional<std::string> build;
    std::optional<std::string> build_pattern;
    bool build_relative_paths = false;
    std::optional<std::string> flags;
    std::optional<std::string> workspace_root_uri;
    std::vector<std::string> top_modules;
    std::vector<IndexConfig> index;
};

struct SemanticLocation {
    std::string uri;
    ParseRange range;
};

enum class SemanticReferenceRole {
    Declaration,
    Read,
    Write,
    Type,
    Instance,
};

struct SemanticReferenceOccurrence {
    SemanticLocation location;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
};

struct SemanticInactiveRegionResult {
    std::uint64_t generation = 0;
    std::vector<ParseRange> regions;
    std::vector<std::string> messages;
    size_t indexed_region_count = 0;
    std::int64_t build_micros = 0;
    bool unresolved = false;
};

struct SemanticSymbolIdentity {
    std::string stable_id;
    std::string name;
    std::string kind;
    SemanticLocation location;
};

struct SemanticLookupResult {
    SemanticEngineMode mode = SemanticEngineMode::Shallow;
    std::uint64_t generation = 0;
    std::optional<SemanticSymbolIdentity> symbol;
    SemanticLocation query_location;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    size_t scanned_target_count = 0;
    bool unresolved = false;
};

struct SemanticReferenceResult {
    std::uint64_t generation = 0;
    std::vector<SemanticLocation> locations;
    std::vector<SemanticReferenceOccurrence> occurrences;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    size_t scanned_implementation_edge_count = 0;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticHoverResult {
    std::uint64_t generation = 0;
    std::string contents;
    ParseRange range;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    size_t scanned_target_count = 0;
    bool unresolved = false;
};

struct SemanticPrepareRenameResult {
    std::uint64_t generation = 0;
    std::string placeholder;
    ParseRange range;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    size_t scanned_target_count = 0;
    bool unresolved = false;
};

struct SemanticTextEdit {
    SemanticLocation location;
    std::string new_text;
};

struct SemanticRenameResult {
    std::uint64_t generation = 0;
    std::vector<SemanticTextEdit> edits;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    size_t scanned_implementation_edge_count = 0;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticCompletionItem {
    std::string stable_id;
    std::string label;
    std::string detail;
    std::string documentation;
    std::string insert_text;
    int kind = 0;
    bool unresolved = false;
};

struct SemanticCompletionResult {
    std::uint64_t generation = 0;
    std::vector<SemanticCompletionItem> items;
    std::vector<std::string> messages;
    size_t scanned_candidate_count = 0;
    size_t scanned_scope_candidate_count = 0;
    size_t scanned_workspace_candidate_count = 0;
    size_t scanned_global_symbol_count = 0;
    size_t scope_visibility_count = 0;
    size_t package_visibility_count = 0;
    size_t member_visibility_count = 0;
    size_t callable_visibility_count = 0;
    std::int64_t scope_visibility_build_micros = 0;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticInlayHint {
    SemanticLocation location;
    std::string label;
    std::string kind;
    std::string tooltip;
};

struct SemanticInlayHintResult {
    std::uint64_t generation = 0;
    std::vector<SemanticInlayHint> hints;
    std::vector<std::string> messages;
    size_t scanned_invocation_count = 0;
    size_t scanned_macro_definition_count = 0;
    size_t scanned_global_symbol_count = 0;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticSignatureHelpResult {
    std::uint64_t generation = 0;
    std::string label;
    std::vector<std::string> parameters;
    int active_parameter = 0;
    std::vector<std::string> messages;
    size_t scanned_invocation_count = 0;
    size_t scanned_macro_definition_count = 0;
    size_t scanned_global_symbol_count = 0;
    bool unresolved = false;
};

struct SemanticToken {
    SemanticLocation location;
    std::string token_type;
    std::string token_modifier;
};

struct SemanticTokenResult {
    std::uint64_t generation = 0;
    std::vector<SemanticToken> tokens;
    std::vector<std::string> messages;
    size_t scanned_occurrence_count = 0;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticSelectionRange {
    ParseRange range;
    std::optional<size_t> parent;
};

struct SemanticSelectionRangeResult {
    std::uint64_t generation = 0;
    std::vector<SemanticSelectionRange> ranges;
    std::vector<std::string> messages;
    size_t scanned_candidate_count = 0;
    bool unresolved = false;
};

struct SemanticHierarchyNode {
    std::string module_name;
    std::string kind;
    std::string instance_name;
    SemanticLocation location;
    ParseRange selection_range;
    std::optional<ParseRange> instance_range;
    std::optional<ParseRange> instance_selection_range;
    std::optional<ParseRange> module_selection_range;
    bool unresolved = false;
    bool cycle = false;
    bool truncated = false;
    std::vector<SemanticHierarchyNode> children;
};

struct SemanticModuleHierarchyResult {
    std::uint64_t generation = 0;
    std::vector<SemanticHierarchyNode> roots;
    std::vector<std::string> messages;
    std::string discovery_closure_root_name;
    size_t discovery_closure_candidate_document_count = 0;
    size_t discovery_closure_document_count = 0;
    size_t discovery_closure_missing_candidate_count = 0;
    size_t discovery_closure_deduped_document_count = 0;
    std::int64_t discovery_closure_build_micros = 0;
    std::int64_t discovery_closure_query_micros = 0;
    bool discovery_closure_used = false;
    bool discovery_closure_cache_hit = false;
    bool unresolved = false;
    bool partial = false;
    bool truncated = false;
};

struct SemanticSchematicModule {
    std::string id;
    std::string name;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    std::vector<SchematicPort> ports;
    std::vector<SchematicCell> cells;
};

struct SemanticSchematicEndpoint {
    std::string node_id;
    std::string port_name;
};

struct SemanticSchematicNet {
    std::string name;
    std::vector<SemanticSchematicEndpoint> drivers;
    std::vector<SemanticSchematicEndpoint> loads;
};

struct SemanticSchematicModuleView {
    SemanticSchematicModule module;
    std::vector<SemanticSchematicNet> nets;
};

struct SemanticSchematicResult {
    std::uint64_t generation = 0;
    std::optional<std::string> root_module_id;
    std::vector<SemanticSchematicModuleView> modules;
    std::vector<std::string> messages;
    std::string discovery_closure_root_name;
    size_t discovery_closure_candidate_document_count = 0;
    size_t discovery_closure_document_count = 0;
    size_t discovery_closure_missing_candidate_count = 0;
    size_t discovery_closure_deduped_document_count = 0;
    std::int64_t discovery_closure_build_micros = 0;
    std::int64_t discovery_closure_query_micros = 0;
    bool discovery_closure_used = false;
    bool discovery_closure_cache_hit = false;
    size_t graph_binding_lookup_scanned_facts = 0;
    size_t schematic_connection_fact_lookup_count = 0;
    size_t schematic_source_part_scan_count = 0;
    size_t schematic_partial_connection_fact_count = 0;
    size_t schematic_cell_pin_fact_lookup_count = 0;
    size_t schematic_cell_pin_scan_count = 0;
    size_t schematic_partial_cell_pin_fact_count = 0;
    size_t graph_scanned_global_symbols = 0;
    bool unresolved = false;
    bool partial = false;
    bool truncated = false;
};

struct SemanticCallHierarchyItem {
    std::string name;
    int kind = 2;
    std::string detail;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    std::string opaque_id;
    std::uint64_t generation = 0;
};

struct SemanticCallHierarchyPrepareResult {
    std::uint64_t generation = 0;
    std::vector<SemanticCallHierarchyItem> items;
    std::vector<std::string> messages;
    size_t scanned_edge_count = 0;
    size_t scanned_module_count = 0;
    bool unresolved = false;
};

struct SemanticCallHierarchyCall {
    SemanticCallHierarchyItem item;
    std::vector<ParseRange> from_ranges;
};

struct SemanticCallHierarchyCallsResult {
    std::uint64_t generation = 0;
    std::vector<SemanticCallHierarchyCall> calls;
    std::vector<std::string> messages;
    size_t scanned_edge_count = 0;
    size_t scanned_module_count = 0;
    bool unresolved = false;
};

struct SemanticConeNode {
    std::string id;
    std::string name;
    SemanticLocation location;
    std::optional<std::int64_t> bit_width;
};

struct SemanticConeSlice {
    std::string precision = "whole";
    std::optional<std::int64_t> msb;
    std::optional<std::int64_t> lsb;
};

struct SemanticConeEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    std::string expression;
    std::string kind = "assignment";
    std::string source_role = "data";
    std::string slice_kind = "whole";
    std::string control_origin;
    std::optional<ParseRange> source_range;
    std::optional<SemanticConeSlice> source_slice;
    std::optional<SemanticConeSlice> sink_slice;
};

struct SemanticConeTrace {
    std::uint64_t generation = 0;
    std::optional<std::string> root_symbol_id;
    std::vector<SemanticConeNode> nodes;
    std::vector<SemanticConeEdge> edges;
    std::vector<std::string> messages;
    size_t cone_adjacency_scanned_edges = 0;
    size_t cone_scanned_global_edges = 0;
    size_t cone_control_edge_count = 0;
    size_t cone_ternary_control_edge_count = 0;
    size_t cone_slice_fact_count = 0;
    size_t cone_exact_slice_edge_count = 0;
    size_t cone_dynamic_slice_fact_count = 0;
    size_t cone_static_slice_match_count = 0;
    size_t cone_unresolved_source_fact_count = 0;
    size_t cone_connection_slice_adjacency_scanned_edges = 0;
    size_t cone_exact_connection_edge_count = 0;
    size_t cone_parameter_override_exact_mapping_count = 0;
    size_t cone_dynamic_connection_fact_count = 0;
    size_t cone_unresolved_connection_fact_count = 0;
    size_t graph_build_scoped_symbol_candidates = 0;
    size_t graph_build_connection_reference_candidates = 0;
    bool unresolved = false;
    bool partial = false;
    bool truncated = false;
};

struct SemanticDiagnosticData {
    std::string code;
    std::string message;
    ParseRange range;
    int severity = 1;
};

struct SemanticCodeActionEdit {
    std::string uri;
    ParseRange range;
    std::string new_text;
};

struct SemanticCodeActionCreateFile {
    std::string uri;
    bool ignore_if_exists = true;
};

struct SemanticCodeAction {
    std::string title;
    std::string kind = "quickfix";
    bool is_preferred = false;
    std::vector<SemanticDiagnosticData> diagnostics;
    std::vector<SemanticCodeActionEdit> edits;
    std::vector<SemanticCodeActionCreateFile> create_files;
};

struct SemanticCodeActionResult {
    std::uint64_t generation = 0;
    std::vector<SemanticCodeAction> actions;
    std::vector<std::string> messages;
    bool unresolved = false;
    bool partial = false;
    bool truncated = false;
};

struct SemanticWorkspaceSymbol {
    std::string name;
    int kind = 0;
    SemanticLocation location;
    ParseRange selection_range;
    std::string stable_id;
};

struct SemanticWorkspaceSymbolResult {
    std::uint64_t generation = 0;
    std::vector<SemanticWorkspaceSymbol> symbols;
    std::vector<std::string> messages;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticEngineDocumentState {
    int version = -1;
    bool is_open = false;
    bool dirty = false;
};

struct SemanticEngineDocument {
    std::string uri;
    std::string text;
    int version = -1;
    bool is_open = false;
    bool dirty = false;
};

struct SemanticEngineDiagnostic {
    std::string uri;
    std::string code;
    std::string message;
    ParseRange range;
    int severity = 1;
};

struct SemanticEngineSnapshot {
    std::uint64_t generation = 0;
    std::vector<std::string> document_uris;
    std::vector<std::string> dirty_document_uris;
    std::vector<std::string> top_modules;
    std::vector<SemanticEngineDiagnostic> diagnostics;
    SemanticEngineMode mode = SemanticEngineMode::Shallow;
    bool has_shallow_ast = false;
    bool has_design_ast = false;
};

struct SemanticDiscoverySymbol {
    std::string name;
    std::string kind;
    SemanticLocation location;
};

struct SemanticDiscoveryClosureMetric {
    std::string root_name;
    size_t candidate_document_count = 0;
    size_t selected_document_count = 0;
    size_t missing_candidate_count = 0;
    size_t deduped_document_count = 0;
    std::vector<std::string> selected_document_uris;
    std::vector<std::string> missing_candidate_uris;
};

struct SemanticWorkspaceDiscoverySnapshot {
    std::uint64_t generation = 0;
    std::uint64_t cache_key = 0;
    size_t file_count = 0;
    size_t byte_count = 0;
    size_t declaration_count = 0;
    size_t macro_count = 0;
    size_t reference_count = 0;
    std::int64_t build_micros = 0;
    bool cache_hit = false;
    std::vector<SemanticDiscoverySymbol> declarations;
    std::vector<std::string> top_candidates;
    std::unordered_map<std::string, std::vector<std::string>> closure_uris_by_name;
    std::vector<SemanticDiscoveryClosureMetric> closure_metrics;
    std::vector<std::string> messages;
};

struct SemanticQueryCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stores = 0;
    std::uint64_t evictions = 0;
    std::uint64_t signature_scanned_invocations = 0;
    std::uint64_t inlay_scanned_invocations = 0;
    std::uint64_t macro_scanned_visible_definitions = 0;
    std::uint64_t completion_resolve_scanned_facts = 0;
    std::uint64_t diagnostic_lookup_scanned_facts = 0;
    std::uint64_t reference_lookup_scanned_occurrences = 0;
    std::uint64_t call_hierarchy_scanned_edges = 0;
    std::uint64_t call_hierarchy_scanned_modules = 0;
    std::uint64_t navigation_occurrence_scanned = 0;
    std::uint64_t navigation_target_lookup_scanned = 0;
    std::uint64_t implementation_edge_scanned = 0;
    std::uint64_t semantic_token_scanned_occurrences = 0;
    std::uint64_t selection_range_scanned_candidates = 0;
    std::uint64_t graph_binding_lookup_scanned_facts = 0;
    std::uint64_t cone_adjacency_scanned_edges = 0;
    std::uint64_t graph_scanned_global_symbols = 0;
    std::uint64_t cone_scanned_global_edges = 0;
    std::uint64_t scanned_global_symbols = 0;
    size_t diagnostics_entries = 0;
    size_t workspace_symbols_entries = 0;
    size_t references_entries = 0;
    size_t rename_entries = 0;
    size_t hover_entries = 0;
    size_t definition_entries = 0;
    size_t type_definition_entries = 0;
    size_t implementation_entries = 0;
    size_t prepare_rename_entries = 0;
    size_t document_highlight_entries = 0;
    size_t completions_entries = 0;
    size_t signature_help_entries = 0;
    size_t inlay_hints_entries = 0;
    size_t module_hierarchy_entries = 0;
    size_t schematic_entries = 0;
    size_t backward_cone_entries = 0;
    size_t code_actions_entries = 0;
    size_t total_entries = 0;
};

class SemanticEngine {
public:
    SemanticEngine();
    ~SemanticEngine();

    void clear();
    void setWorkspaceRoot(std::string_view root_uri);
    void configure(SemanticEngineConfig config);
    void updateDocument(std::string_view uri,
                        std::string_view text,
                        SemanticEngineDocumentState state = {});
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticEngineDocument* document(std::string_view uri) const;
    [[nodiscard]] size_t documentCount() const;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] bool snapshotDirty() const;
    [[nodiscard]] bool hasFreshSnapshot() const;
    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> dirtyDocumentUris() const;
    [[nodiscard]] std::vector<std::string> affectedDocumentUris(std::string_view uri) const;

    [[nodiscard]] const SemanticEngineSnapshot& snapshot() const;
    [[nodiscard]] SemanticWorkspaceDiscoverySnapshot workspaceDiscovery() const;
    [[nodiscard]] std::vector<SemanticEngineDiagnostic> diagnosticsFor(std::string_view uri) const;
    [[nodiscard]] SemanticInactiveRegionResult inactiveRegions(std::string_view uri) const;
    [[nodiscard]] SemanticLookupResult lookupAt(std::string_view uri, int line, int character) const;
    [[nodiscard]] SemanticReferenceResult definitionsAt(std::string_view uri,
                                                        int line,
                                                        int character) const;
    [[nodiscard]] SemanticReferenceResult typeDefinitionsAt(std::string_view uri,
                                                            int line,
                                                            int character) const;
    [[nodiscard]] SemanticReferenceResult referencesAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       bool include_declaration) const;
    [[nodiscard]] SemanticReferenceResult documentHighlightsAt(std::string_view uri,
                                                                int line,
                                                                int character) const;
    [[nodiscard]] SemanticReferenceResult implementationsAt(std::string_view uri,
                                                            int line,
                                                            int character) const;
    [[nodiscard]] SemanticHoverResult hoverAt(std::string_view uri, int line, int character) const;
    [[nodiscard]] SemanticPrepareRenameResult prepareRenameAt(std::string_view uri,
                                                              int line,
                                                              int character) const;
    [[nodiscard]] SemanticRenameResult renameAt(std::string_view uri,
                                                int line,
                                                int character,
                                                std::string_view new_name) const;
    [[nodiscard]] SemanticCompletionResult completionsAt(std::string_view uri,
                                                         int line,
                                                         int character,
                                                         std::string_view prefix = {}) const;
    [[nodiscard]] SemanticCompletionItem resolveCompletion(std::string_view stable_id,
                                                           std::string_view label) const;
    [[nodiscard]] SemanticSignatureHelpResult signatureHelpAt(std::string_view uri,
                                                              int line,
                                                              int character) const;
    [[nodiscard]] SemanticInlayHintResult inlayHints(std::string_view uri, ParseRange range) const;
    [[nodiscard]] SemanticTokenResult semanticTokens(std::string_view uri) const;
    [[nodiscard]] SemanticSelectionRangeResult selectionRangesAt(std::string_view uri,
                                                                 int line,
                                                                 int character) const;
    [[nodiscard]] SemanticModuleHierarchyResult moduleHierarchy(std::optional<std::string_view> module_name = std::nullopt,
                                                                int max_depth = 64) const;
    [[nodiscard]] SemanticSchematicResult schematic(std::optional<std::string_view> module_name = std::nullopt,
                                                    int max_depth = 64) const;
    [[nodiscard]] SemanticCallHierarchyPrepareResult prepareCallHierarchy(std::string_view uri,
                                                                          int line,
                                                                          int character) const;
    [[nodiscard]] SemanticCallHierarchyCallsResult incomingCalls(const SemanticCallHierarchyItem& item) const;
    [[nodiscard]] SemanticCallHierarchyCallsResult outgoingCalls(const SemanticCallHierarchyItem& item) const;
    [[nodiscard]] SemanticConeTrace backwardConeAt(std::string_view uri,
                                                   int line,
                                                   int character) const;
    [[nodiscard]] SemanticCodeActionResult codeActionsAt(std::string_view uri, ParseRange range) const;
    [[nodiscard]] SemanticWorkspaceSymbolResult workspaceSymbols(std::string_view query,
                                                                 size_t limit = 1000) const;
    [[nodiscard]] SemanticQueryCacheStats queryCacheStats() const;
    void resetQueryCacheStats();

private:
    void rebuildDependenciesFor(std::string_view document_uri, std::string_view text);
    void rebuildSnapshot() const;
    [[nodiscard]] std::vector<std::string> closureDocumentUrisFor(
        std::optional<std::string_view> module_name,
        const SemanticWorkspaceDiscoverySnapshot& discovery) const;
    [[nodiscard]] const semantic::SnapshotData* snapshotData() const;

    std::string workspace_root_uri_;
    SemanticEngineConfig config_;
    std::unordered_map<std::string, SemanticEngineDocument> documents_;
    mutable std::unique_ptr<semantic::AffectedDependencyGraph> affected_dependencies_;
    mutable std::optional<SemanticEngineSnapshot> snapshot_;
    mutable std::unique_ptr<semantic::SnapshotData> snapshot_data_;
    mutable std::unique_ptr<semantic::QueryCache> query_cache_;
    mutable std::optional<SemanticWorkspaceDiscoverySnapshot> discovery_snapshot_cache_;
    mutable std::uint64_t discovery_cache_key_ = 0;
    mutable bool snapshot_dirty_ = true;
    std::uint64_t generation_ = 0;
};

} // namespace pristine::analysis
