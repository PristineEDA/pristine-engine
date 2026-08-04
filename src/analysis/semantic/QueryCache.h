#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

class QueryCache {
public:
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t stores = 0;
        std::uint64_t evictions = 0;
        std::uint64_t signature_scanned_invocations = 0;
        std::uint64_t inlay_scanned_invocations = 0;
        std::uint64_t macro_scanned_visible_definitions = 0;
        std::uint64_t completion_resolve_scanned_facts = 0;
        std::uint64_t completion_resolve_identity_hits = 0;
        std::uint64_t completion_resolve_identity_misses = 0;
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

    void clear();
    void resetStats();
    [[nodiscard]] Stats snapshotAndResetStats();
    void setMaxEntriesPerQuery(size_t max_entries);
    [[nodiscard]] Stats stats() const;
    void recordReferenceLookup(size_t scanned_occurrences);
    void recordCallHierarchyScan(size_t scanned_edges, size_t scanned_modules);
    void recordNavigationScan(size_t scanned_occurrences,
                              size_t scanned_targets,
                              size_t scanned_implementation_edges,
                              size_t scanned_tokens,
                              size_t scanned_selection_candidates);
    void recordDesignGraphScan(size_t scanned_binding_facts,
                               size_t scanned_adjacency_edges,
                               size_t scanned_global_symbols,
                               size_t scanned_global_edges);

    [[nodiscard]] std::optional<std::vector<SemanticEngineDiagnostic>> diagnostics(
        std::uint64_t generation,
        std::string_view uri) const;
    void storeDiagnostics(std::uint64_t generation,
                          std::string_view uri,
                          std::vector<SemanticEngineDiagnostic> diagnostics);

    [[nodiscard]] std::optional<SemanticWorkspaceSymbolResult> workspaceSymbols(
        std::uint64_t generation,
        std::string_view query,
        size_t limit) const;
    void storeWorkspaceSymbols(std::uint64_t generation,
                               std::string_view query,
                               size_t limit,
                               SemanticWorkspaceSymbolResult result);

    [[nodiscard]] std::optional<SemanticReferenceResult> references(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        bool include_declaration,
        std::uint64_t plan_fingerprint = 0,
        std::string_view target_stable_id = {},
        std::string_view scope_kind = {}) const;
    void storeReferences(std::uint64_t generation,
                         std::string_view uri,
                         int line,
                         int character,
                         bool include_declaration,
                         SemanticReferenceResult result,
                         std::uint64_t plan_fingerprint = 0,
                         std::string_view target_stable_id = {},
                         std::string_view scope_kind = {});

    [[nodiscard]] std::optional<SemanticHoverResult> hover(std::uint64_t generation,
                                                            std::string_view uri,
                                                            int line,
                                                            int character) const;
    void storeHover(std::uint64_t generation,
                    std::string_view uri,
                    int line,
                    int character,
                    SemanticHoverResult result);

    [[nodiscard]] std::optional<SemanticReferenceResult> definitions(std::uint64_t generation,
                                                                       std::string_view uri,
                                                                       int line,
                                                                       int character) const;
    void storeDefinitions(std::uint64_t generation,
                          std::string_view uri,
                          int line,
                          int character,
                          SemanticReferenceResult result);

    [[nodiscard]] std::optional<SemanticReferenceResult> typeDefinitions(std::uint64_t generation,
                                                                           std::string_view uri,
                                                                           int line,
                                                                           int character) const;
    void storeTypeDefinitions(std::uint64_t generation,
                              std::string_view uri,
                              int line,
                              int character,
                              SemanticReferenceResult result);

    [[nodiscard]] std::optional<SemanticReferenceResult> implementations(std::uint64_t generation,
                                                                          std::string_view uri,
                                                                          int line,
                                                                          int character) const;
    void storeImplementations(std::uint64_t generation,
                              std::string_view uri,
                              int line,
                              int character,
                              SemanticReferenceResult result);

    [[nodiscard]] std::optional<SemanticPrepareRenameResult> prepareRename(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storePrepareRename(std::uint64_t generation,
                            std::string_view uri,
                            int line,
                            int character,
                            SemanticPrepareRenameResult result);

    [[nodiscard]] std::optional<SemanticReferenceResult> documentHighlights(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storeDocumentHighlights(std::uint64_t generation,
                                 std::string_view uri,
                                 int line,
                                 int character,
                                 SemanticReferenceResult result);

    [[nodiscard]] std::optional<SemanticRenameResult> rename(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        std::string_view new_name,
        std::uint64_t plan_fingerprint = 0,
        std::string_view target_stable_id = {},
        std::string_view scope_kind = {}) const;
    void storeRename(std::uint64_t generation,
                     std::string_view uri,
                     int line,
                     int character,
                     std::string_view new_name,
                     SemanticRenameResult result,
                     std::uint64_t plan_fingerprint = 0,
                     std::string_view target_stable_id = {},
                     std::string_view scope_kind = {});

    [[nodiscard]] std::optional<SemanticCompletionResult> completions(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        std::string_view prefix,
        std::uint64_t plan_fingerprint = 0) const;
    void storeCompletions(std::uint64_t generation,
                          std::string_view uri,
                          int line,
                          int character,
                          std::string_view prefix,
                          SemanticCompletionResult result,
                          std::uint64_t plan_fingerprint = 0);

    [[nodiscard]] std::optional<SemanticSignatureHelpResult> signatureHelp(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storeSignatureHelp(std::uint64_t generation,
                            std::string_view uri,
                            int line,
                            int character,
                            SemanticSignatureHelpResult result);

    [[nodiscard]] std::optional<SemanticInlayHintResult> inlayHints(
        std::uint64_t generation,
        std::string_view uri,
        ParseRange range) const;
    void storeInlayHints(std::uint64_t generation,
                         std::string_view uri,
                         ParseRange range,
                         SemanticInlayHintResult result);

    [[nodiscard]] std::optional<SemanticModuleHierarchyResult> moduleHierarchy(
        std::uint64_t generation,
        std::optional<std::string_view> module_name,
        int max_depth) const;
    void storeModuleHierarchy(std::uint64_t generation,
                              std::optional<std::string_view> module_name,
                              int max_depth,
                              SemanticModuleHierarchyResult result);

    [[nodiscard]] std::optional<SemanticSchematicResult> schematic(
        std::uint64_t generation,
        std::optional<std::string_view> module_name,
        int max_depth) const;
    void storeSchematic(std::uint64_t generation,
                        std::optional<std::string_view> module_name,
                        int max_depth,
                        SemanticSchematicResult result);

    [[nodiscard]] std::optional<SemanticConeTrace> backwardCone(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storeBackwardCone(std::uint64_t generation,
                           std::string_view uri,
                           int line,
                           int character,
                           SemanticConeTrace result);

    [[nodiscard]] std::optional<SemanticCodeActionResult> codeActions(
        std::uint64_t generation,
        std::string_view uri,
        ParseRange range) const;
    void storeCodeActions(std::uint64_t generation,
                          std::string_view uri,
                          ParseRange range,
                          SemanticCodeActionResult result);
    void recordCompletionResolveFactLookup(size_t count);
    void recordCompletionResolveIdentityLookup(bool hit);
    void recordDiagnosticLookupFacts(size_t count);

private:
    struct DiagnosticsEntry {
        std::uint64_t generation = 0;
        std::vector<SemanticEngineDiagnostic> diagnostics;
    };

    struct WorkspaceSymbolsEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticWorkspaceSymbolResult result;
    };

    struct ReferencesEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticReferenceResult result;
    };

    struct RenameEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticRenameResult result;
    };

    struct HoverEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticHoverResult result;
    };

    struct ReferenceResultEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticReferenceResult result;
    };

    struct PrepareRenameEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticPrepareRenameResult result;
    };

    struct CompletionEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticCompletionResult result;
    };

    struct SignatureHelpEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticSignatureHelpResult result;
    };

    struct InlayHintsEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticInlayHintResult result;
    };

    struct ModuleHierarchyEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticModuleHierarchyResult result;
    };

    struct SchematicEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticSchematicResult result;
    };

    struct BackwardConeEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticConeTrace result;
    };

    struct CodeActionsEntry {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        SemanticCodeActionResult result;
    };

    [[nodiscard]] static std::string workspaceSymbolsKey(std::string_view query, size_t limit);
    [[nodiscard]] static std::string positionKey(std::string_view uri, int line, int character);
    [[nodiscard]] static std::string referencesKey(std::string_view uri,
                                                   int line,
                                                   int character,
                                                   bool include_declaration,
                                                   std::uint64_t plan_fingerprint,
                                                   std::string_view target_stable_id,
                                                   std::string_view scope_kind);
    [[nodiscard]] static std::string renameKey(std::string_view uri,
                                               int line,
                                               int character,
                                               std::string_view new_name,
                                               std::uint64_t plan_fingerprint,
                                               std::string_view target_stable_id,
                                               std::string_view scope_kind);
    [[nodiscard]] static std::string completionKey(std::string_view uri,
                                                   int line,
                                                   int character,
                                                   std::string_view prefix,
                                                   std::uint64_t plan_fingerprint);
    [[nodiscard]] static std::string moduleQueryKey(std::optional<std::string_view> module_name,
                                                    int max_depth);
    [[nodiscard]] static std::string rangeKey(std::string_view uri, ParseRange range);

    void recordHit() const;
    void recordMiss() const;
    void recordStore();
    void recordEviction();
    [[nodiscard]] std::uint64_t nextSequence();

    template <typename Map>
    void evictOldestEntries(Map& map);

    std::unordered_map<std::string, DiagnosticsEntry> diagnostics_by_uri_;
    std::unordered_map<std::string, WorkspaceSymbolsEntry> workspace_symbols_by_key_;
    std::unordered_map<std::string, ReferencesEntry> references_by_key_;
    std::unordered_map<std::string, RenameEntry> rename_by_key_;
    std::unordered_map<std::string, HoverEntry> hover_by_key_;
    std::unordered_map<std::string, ReferenceResultEntry> definitions_by_key_;
    std::unordered_map<std::string, ReferenceResultEntry> type_definitions_by_key_;
    std::unordered_map<std::string, ReferenceResultEntry> implementations_by_key_;
    std::unordered_map<std::string, PrepareRenameEntry> prepare_rename_by_key_;
    std::unordered_map<std::string, ReferenceResultEntry> document_highlights_by_key_;
    std::unordered_map<std::string, CompletionEntry> completions_by_key_;
    std::unordered_map<std::string, SignatureHelpEntry> signature_help_by_key_;
    std::unordered_map<std::string, InlayHintsEntry> inlay_hints_by_key_;
    std::unordered_map<std::string, ModuleHierarchyEntry> module_hierarchy_by_key_;
    std::unordered_map<std::string, SchematicEntry> schematic_by_key_;
    std::unordered_map<std::string, BackwardConeEntry> backward_cone_by_key_;
    std::unordered_map<std::string, CodeActionsEntry> code_actions_by_key_;

    size_t max_entries_per_query_ = 128;
    std::uint64_t sequence_ = 0;
    mutable std::uint64_t hits_ = 0;
    mutable std::uint64_t misses_ = 0;
    std::uint64_t stores_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t signature_scanned_invocations_ = 0;
    std::uint64_t inlay_scanned_invocations_ = 0;
    std::uint64_t macro_scanned_visible_definitions_ = 0;
    std::uint64_t completion_resolve_scanned_facts_ = 0;
    std::uint64_t completion_resolve_identity_hits_ = 0;
    std::uint64_t completion_resolve_identity_misses_ = 0;
    std::uint64_t diagnostic_lookup_scanned_facts_ = 0;
    std::uint64_t reference_lookup_scanned_occurrences_ = 0;
    std::uint64_t call_hierarchy_scanned_edges_ = 0;
    std::uint64_t call_hierarchy_scanned_modules_ = 0;
    std::uint64_t navigation_occurrence_scanned_ = 0;
    std::uint64_t navigation_target_lookup_scanned_ = 0;
    std::uint64_t implementation_edge_scanned_ = 0;
    std::uint64_t semantic_token_scanned_occurrences_ = 0;
    std::uint64_t selection_range_scanned_candidates_ = 0;
    std::uint64_t graph_binding_lookup_scanned_facts_ = 0;
    std::uint64_t cone_adjacency_scanned_edges_ = 0;
    std::uint64_t graph_scanned_global_symbols_ = 0;
    std::uint64_t cone_scanned_global_edges_ = 0;
    std::uint64_t scanned_global_symbols_ = 0;
};

} // namespace pristine::analysis::semantic
