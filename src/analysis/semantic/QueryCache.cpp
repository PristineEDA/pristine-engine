#include "QueryCache.h"

#include <algorithm>

namespace pristine::analysis::semantic {
namespace {

class QueryCacheKeyBuilder {
public:
    explicit QueryCacheKeyBuilder(std::string_view query_kind) {
        field("kind", query_kind);
    }

    QueryCacheKeyBuilder& field(std::string_view name, std::string_view value) {
        appendName(name);
        value_.append(std::to_string(value.size()));
        value_.push_back(':');
        value_.append(value);
        value_.push_back(';');
        return *this;
    }

    QueryCacheKeyBuilder& number(std::string_view name, std::uint64_t value) {
        return field(name, std::to_string(value));
    }

    QueryCacheKeyBuilder& integer(std::string_view name, int value) {
        return field(name, std::to_string(value));
    }

    QueryCacheKeyBuilder& boolean(std::string_view name, bool value) {
        return field(name, value ? "true" : "false");
    }

    QueryCacheKeyBuilder& optionalField(std::string_view name,
                                        std::optional<std::string_view> value) {
        if (!value.has_value()) {
            return field(name, "<none>");
        }
        return field(name, *value);
    }

    [[nodiscard]] std::string str() const {
        return value_;
    }

private:
    void appendName(std::string_view name) {
        value_.append(std::to_string(name.size()));
        value_.push_back('#');
        value_.append(name);
        value_.push_back('=');
    }

    std::string value_;
};

} // namespace

template <typename Map>
void QueryCache::evictOldestEntries(Map& map) {
    while (map.size() > max_entries_per_query_) {
        const auto oldest = std::min_element(map.begin(),
                                             map.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.sequence < right.second.sequence;
                                             });
        if (oldest == map.end()) {
            return;
        }
        map.erase(oldest);
        recordEviction();
    }
}

void QueryCache::clear() {
    diagnostics_by_uri_.clear();
    workspace_symbols_by_key_.clear();
    references_by_key_.clear();
    rename_by_key_.clear();
    hover_by_key_.clear();
    definitions_by_key_.clear();
    type_definitions_by_key_.clear();
    implementations_by_key_.clear();
    prepare_rename_by_key_.clear();
    document_highlights_by_key_.clear();
    completions_by_key_.clear();
    signature_help_by_key_.clear();
    inlay_hints_by_key_.clear();
    module_hierarchy_by_key_.clear();
    schematic_by_key_.clear();
    backward_cone_by_key_.clear();
    code_actions_by_key_.clear();
}

void QueryCache::resetStats() {
    hits_ = 0;
    misses_ = 0;
    stores_ = 0;
    evictions_ = 0;
    signature_scanned_invocations_ = 0;
    inlay_scanned_invocations_ = 0;
    macro_scanned_visible_definitions_ = 0;
    completion_resolve_scanned_facts_ = 0;
    completion_resolve_identity_hits_ = 0;
    completion_resolve_identity_misses_ = 0;
    diagnostic_lookup_scanned_facts_ = 0;
    reference_lookup_scanned_occurrences_ = 0;
    call_hierarchy_scanned_edges_ = 0;
    call_hierarchy_scanned_modules_ = 0;
    navigation_occurrence_scanned_ = 0;
    navigation_target_lookup_scanned_ = 0;
    implementation_edge_scanned_ = 0;
    semantic_token_scanned_occurrences_ = 0;
    selection_range_scanned_candidates_ = 0;
    graph_binding_lookup_scanned_facts_ = 0;
    cone_adjacency_scanned_edges_ = 0;
    graph_scanned_global_symbols_ = 0;
    cone_scanned_global_edges_ = 0;
    scanned_global_symbols_ = 0;
}

QueryCache::Stats QueryCache::snapshotAndResetStats() {
    auto result = stats();
    resetStats();
    return result;
}

void QueryCache::setMaxEntriesPerQuery(size_t max_entries) {
    max_entries_per_query_ = max_entries;
    evictOldestEntries(workspace_symbols_by_key_);
    evictOldestEntries(references_by_key_);
    evictOldestEntries(rename_by_key_);
    evictOldestEntries(hover_by_key_);
    evictOldestEntries(definitions_by_key_);
    evictOldestEntries(type_definitions_by_key_);
    evictOldestEntries(implementations_by_key_);
    evictOldestEntries(prepare_rename_by_key_);
    evictOldestEntries(document_highlights_by_key_);
    evictOldestEntries(completions_by_key_);
    evictOldestEntries(signature_help_by_key_);
    evictOldestEntries(inlay_hints_by_key_);
    evictOldestEntries(module_hierarchy_by_key_);
    evictOldestEntries(schematic_by_key_);
    evictOldestEntries(backward_cone_by_key_);
    evictOldestEntries(code_actions_by_key_);
}

QueryCache::Stats QueryCache::stats() const {
    Stats result;
    result.hits = hits_;
    result.misses = misses_;
    result.stores = stores_;
    result.evictions = evictions_;
    result.signature_scanned_invocations = signature_scanned_invocations_;
    result.inlay_scanned_invocations = inlay_scanned_invocations_;
    result.macro_scanned_visible_definitions = macro_scanned_visible_definitions_;
    result.completion_resolve_scanned_facts = completion_resolve_scanned_facts_;
    result.completion_resolve_identity_hits = completion_resolve_identity_hits_;
    result.completion_resolve_identity_misses = completion_resolve_identity_misses_;
    result.diagnostic_lookup_scanned_facts = diagnostic_lookup_scanned_facts_;
    result.reference_lookup_scanned_occurrences = reference_lookup_scanned_occurrences_;
    result.call_hierarchy_scanned_edges = call_hierarchy_scanned_edges_;
    result.call_hierarchy_scanned_modules = call_hierarchy_scanned_modules_;
    result.navigation_occurrence_scanned = navigation_occurrence_scanned_;
    result.navigation_target_lookup_scanned = navigation_target_lookup_scanned_;
    result.implementation_edge_scanned = implementation_edge_scanned_;
    result.semantic_token_scanned_occurrences = semantic_token_scanned_occurrences_;
    result.selection_range_scanned_candidates = selection_range_scanned_candidates_;
    result.graph_binding_lookup_scanned_facts = graph_binding_lookup_scanned_facts_;
    result.cone_adjacency_scanned_edges = cone_adjacency_scanned_edges_;
    result.graph_scanned_global_symbols = graph_scanned_global_symbols_;
    result.cone_scanned_global_edges = cone_scanned_global_edges_;
    result.scanned_global_symbols = scanned_global_symbols_;
    result.diagnostics_entries = diagnostics_by_uri_.size();
    result.workspace_symbols_entries = workspace_symbols_by_key_.size();
    result.references_entries = references_by_key_.size();
    result.rename_entries = rename_by_key_.size();
    result.hover_entries = hover_by_key_.size();
    result.definition_entries = definitions_by_key_.size();
    result.type_definition_entries = type_definitions_by_key_.size();
    result.implementation_entries = implementations_by_key_.size();
    result.prepare_rename_entries = prepare_rename_by_key_.size();
    result.document_highlight_entries = document_highlights_by_key_.size();
    result.completions_entries = completions_by_key_.size();
    result.signature_help_entries = signature_help_by_key_.size();
    result.inlay_hints_entries = inlay_hints_by_key_.size();
    result.module_hierarchy_entries = module_hierarchy_by_key_.size();
    result.schematic_entries = schematic_by_key_.size();
    result.backward_cone_entries = backward_cone_by_key_.size();
    result.code_actions_entries = code_actions_by_key_.size();
    result.total_entries = result.diagnostics_entries + result.workspace_symbols_entries +
                           result.references_entries + result.rename_entries + result.hover_entries +
                           result.definition_entries + result.type_definition_entries +
                           result.implementation_entries + result.prepare_rename_entries +
                           result.document_highlight_entries +
                           result.completions_entries + result.signature_help_entries +
                           result.inlay_hints_entries + result.module_hierarchy_entries +
                           result.schematic_entries + result.backward_cone_entries +
                           result.code_actions_entries;
    return result;
}

void QueryCache::recordReferenceLookup(size_t scanned_occurrences) {
    reference_lookup_scanned_occurrences_ += scanned_occurrences;
}

void QueryCache::recordCallHierarchyScan(size_t scanned_edges, size_t scanned_modules) {
    call_hierarchy_scanned_edges_ += scanned_edges;
    call_hierarchy_scanned_modules_ += scanned_modules;
}

void QueryCache::recordNavigationScan(size_t scanned_occurrences,
                                      size_t scanned_targets,
                                      size_t scanned_implementation_edges,
                                      size_t scanned_tokens,
                                      size_t scanned_selection_candidates) {
    navigation_occurrence_scanned_ += scanned_occurrences;
    navigation_target_lookup_scanned_ += scanned_targets;
    implementation_edge_scanned_ += scanned_implementation_edges;
    semantic_token_scanned_occurrences_ += scanned_tokens;
    selection_range_scanned_candidates_ += scanned_selection_candidates;
}

void QueryCache::recordDesignGraphScan(size_t scanned_binding_facts,
                                       size_t scanned_adjacency_edges,
                                       size_t scanned_global_symbols,
                                       size_t scanned_global_edges) {
    graph_binding_lookup_scanned_facts_ += scanned_binding_facts;
    cone_adjacency_scanned_edges_ += scanned_adjacency_edges;
    graph_scanned_global_symbols_ += scanned_global_symbols;
    cone_scanned_global_edges_ += scanned_global_edges;
    scanned_global_symbols_ += scanned_global_symbols;
}

void QueryCache::recordCompletionResolveFactLookup(size_t count) {
    completion_resolve_scanned_facts_ += count;
}

void QueryCache::recordCompletionResolveIdentityLookup(bool hit) {
    if (hit) {
        ++completion_resolve_identity_hits_;
    }
    else {
        ++completion_resolve_identity_misses_;
    }
}

void QueryCache::recordDiagnosticLookupFacts(size_t count) {
    diagnostic_lookup_scanned_facts_ += count;
}

std::optional<std::vector<SemanticEngineDiagnostic>> QueryCache::diagnostics(
    std::uint64_t generation,
    std::string_view uri) const {
    const auto found = diagnostics_by_uri_.find(std::string(uri));
    if (found == diagnostics_by_uri_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.diagnostics;
}

void QueryCache::storeDiagnostics(std::uint64_t generation,
                                  std::string_view uri,
                                  std::vector<SemanticEngineDiagnostic> diagnostics) {
    DiagnosticsEntry entry;
    entry.generation = generation;
    entry.diagnostics = std::move(diagnostics);
    diagnostics_by_uri_.insert_or_assign(std::string(uri), std::move(entry));
    recordStore();
}

std::optional<SemanticWorkspaceSymbolResult> QueryCache::workspaceSymbols(
    std::uint64_t generation,
    std::string_view query,
    size_t limit) const {
    const auto found = workspace_symbols_by_key_.find(workspaceSymbolsKey(query, limit));
    if (found == workspace_symbols_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeWorkspaceSymbols(std::uint64_t generation,
                                       std::string_view query,
                                       size_t limit,
                                       SemanticWorkspaceSymbolResult result) {
    WorkspaceSymbolsEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    workspace_symbols_by_key_.insert_or_assign(workspaceSymbolsKey(query, limit), std::move(entry));
    recordStore();
    evictOldestEntries(workspace_symbols_by_key_);
}

std::optional<SemanticReferenceResult> QueryCache::references(std::uint64_t generation,
                                                              std::string_view uri,
                                                              int line,
                                                              int character,
                                                              bool include_declaration,
                                                              std::uint64_t plan_fingerprint,
                                                              std::string_view target_stable_id,
                                                              std::string_view scope_kind) const {
    const auto found = references_by_key_.find(
        referencesKey(uri, line, character, include_declaration, plan_fingerprint, target_stable_id, scope_kind));
    if (found == references_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeReferences(std::uint64_t generation,
                                 std::string_view uri,
                                 int line,
                                 int character,
                                 bool include_declaration,
                                 SemanticReferenceResult result,
                                 std::uint64_t plan_fingerprint,
                                 std::string_view target_stable_id,
                                 std::string_view scope_kind) {
    ReferencesEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    references_by_key_.insert_or_assign(
        referencesKey(uri, line, character, include_declaration, plan_fingerprint, target_stable_id, scope_kind),
        std::move(entry));
    recordStore();
    evictOldestEntries(references_by_key_);
}

std::optional<SemanticHoverResult> QueryCache::hover(std::uint64_t generation,
                                                      std::string_view uri,
                                                      int line,
                                                      int character) const {
    const auto found = hover_by_key_.find(positionKey(uri, line, character));
    if (found == hover_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeHover(std::uint64_t generation,
                            std::string_view uri,
                            int line,
                            int character,
                            SemanticHoverResult result) {
    hover_by_key_.insert_or_assign(positionKey(uri, line, character),
                                   HoverEntry{.generation = generation,
                                              .sequence = nextSequence(),
                                              .result = std::move(result)});
    recordStore();
    evictOldestEntries(hover_by_key_);
}

std::optional<SemanticReferenceResult> QueryCache::definitions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                int line,
                                                                int character) const {
    const auto found = definitions_by_key_.find(positionKey(uri, line, character));
    if (found == definitions_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeDefinitions(std::uint64_t generation,
                                  std::string_view uri,
                                  int line,
                                  int character,
                                  SemanticReferenceResult result) {
    definitions_by_key_.insert_or_assign(positionKey(uri, line, character),
                                         ReferenceResultEntry{.generation = generation,
                                                              .sequence = nextSequence(),
                                                              .result = std::move(result)});
    recordStore();
    evictOldestEntries(definitions_by_key_);
}

std::optional<SemanticReferenceResult> QueryCache::typeDefinitions(std::uint64_t generation,
                                                                    std::string_view uri,
                                                                    int line,
                                                                    int character) const {
    const auto found = type_definitions_by_key_.find(positionKey(uri, line, character));
    if (found == type_definitions_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeTypeDefinitions(std::uint64_t generation,
                                      std::string_view uri,
                                      int line,
                                      int character,
                                      SemanticReferenceResult result) {
    type_definitions_by_key_.insert_or_assign(positionKey(uri, line, character),
                                              ReferenceResultEntry{.generation = generation,
                                                                   .sequence = nextSequence(),
                                                                   .result = std::move(result)});
    recordStore();
    evictOldestEntries(type_definitions_by_key_);
}

std::optional<SemanticReferenceResult> QueryCache::implementations(std::uint64_t generation,
                                                                     std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    const auto found = implementations_by_key_.find(positionKey(uri, line, character));
    if (found == implementations_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeImplementations(std::uint64_t generation,
                                      std::string_view uri,
                                      int line,
                                      int character,
                                      SemanticReferenceResult result) {
    implementations_by_key_.insert_or_assign(positionKey(uri, line, character),
                                             ReferenceResultEntry{.generation = generation,
                                                                  .sequence = nextSequence(),
                                                                  .result = std::move(result)});
    recordStore();
    evictOldestEntries(implementations_by_key_);
}

std::optional<SemanticPrepareRenameResult> QueryCache::prepareRename(std::uint64_t generation,
                                                                       std::string_view uri,
                                                                       int line,
                                                                       int character) const {
    const auto found = prepare_rename_by_key_.find(positionKey(uri, line, character));
    if (found == prepare_rename_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storePrepareRename(std::uint64_t generation,
                                    std::string_view uri,
                                    int line,
                                    int character,
                                    SemanticPrepareRenameResult result) {
    prepare_rename_by_key_.insert_or_assign(positionKey(uri, line, character),
                                            PrepareRenameEntry{.generation = generation,
                                                               .sequence = nextSequence(),
                                                               .result = std::move(result)});
    recordStore();
    evictOldestEntries(prepare_rename_by_key_);
}

std::optional<SemanticReferenceResult> QueryCache::documentHighlights(std::uint64_t generation,
                                                                        std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    const auto found = document_highlights_by_key_.find(positionKey(uri, line, character));
    if (found == document_highlights_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeDocumentHighlights(std::uint64_t generation,
                                         std::string_view uri,
                                         int line,
                                         int character,
                                         SemanticReferenceResult result) {
    document_highlights_by_key_.insert_or_assign(positionKey(uri, line, character),
                                                 ReferenceResultEntry{.generation = generation,
                                                                      .sequence = nextSequence(),
                                                                      .result = std::move(result)});
    recordStore();
    evictOldestEntries(document_highlights_by_key_);
}

std::optional<SemanticRenameResult> QueryCache::rename(std::uint64_t generation,
                                                       std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view new_name,
                                                       std::uint64_t plan_fingerprint,
                                                       std::string_view target_stable_id,
                                                       std::string_view scope_kind) const {
    const auto found = rename_by_key_.find(
        renameKey(uri, line, character, new_name, plan_fingerprint, target_stable_id, scope_kind));
    if (found == rename_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeRename(std::uint64_t generation,
                             std::string_view uri,
                             int line,
                             int character,
                             std::string_view new_name,
                             SemanticRenameResult result,
                             std::uint64_t plan_fingerprint,
                             std::string_view target_stable_id,
                             std::string_view scope_kind) {
    RenameEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    rename_by_key_.insert_or_assign(
        renameKey(uri, line, character, new_name, plan_fingerprint, target_stable_id, scope_kind),
        std::move(entry));
    recordStore();
    evictOldestEntries(rename_by_key_);
}

std::optional<SemanticCompletionResult> QueryCache::completions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix,
                                                                std::uint64_t plan_fingerprint) const {
    const auto found = completions_by_key_.find(
        completionKey(uri, line, character, prefix, plan_fingerprint));
    if (found == completions_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeCompletions(std::uint64_t generation,
                                  std::string_view uri,
                                  int line,
                                  int character,
                                  std::string_view prefix,
                                  SemanticCompletionResult result,
                                  std::uint64_t plan_fingerprint) {
    CompletionEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    completions_by_key_.insert_or_assign(completionKey(uri, line, character, prefix, plan_fingerprint),
                                         std::move(entry));
    recordStore();
    evictOldestEntries(completions_by_key_);
}

std::optional<SemanticSignatureHelpResult> QueryCache::signatureHelp(
    std::uint64_t generation,
    std::string_view uri,
    int line,
    int character) const {
    const auto found = signature_help_by_key_.find(positionKey(uri, line, character));
    if (found == signature_help_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeSignatureHelp(std::uint64_t generation,
                                    std::string_view uri,
                                    int line,
                                    int character,
                                    SemanticSignatureHelpResult result) {
    signature_scanned_invocations_ += result.scanned_invocation_count;
    macro_scanned_visible_definitions_ += result.scanned_macro_definition_count;
    scanned_global_symbols_ += result.scanned_global_symbol_count;
    SignatureHelpEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    signature_help_by_key_.insert_or_assign(positionKey(uri, line, character), std::move(entry));
    recordStore();
    evictOldestEntries(signature_help_by_key_);
}

std::optional<SemanticInlayHintResult> QueryCache::inlayHints(std::uint64_t generation,
                                                              std::string_view uri,
                                                              ParseRange range) const {
    const auto found = inlay_hints_by_key_.find(rangeKey(uri, range));
    if (found == inlay_hints_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeInlayHints(std::uint64_t generation,
                                 std::string_view uri,
                                 ParseRange range,
                                 SemanticInlayHintResult result) {
    inlay_scanned_invocations_ += result.scanned_invocation_count;
    macro_scanned_visible_definitions_ += result.scanned_macro_definition_count;
    scanned_global_symbols_ += result.scanned_global_symbol_count;
    InlayHintsEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    inlay_hints_by_key_.insert_or_assign(rangeKey(uri, range), std::move(entry));
    recordStore();
    evictOldestEntries(inlay_hints_by_key_);
}

std::optional<SemanticModuleHierarchyResult> QueryCache::moduleHierarchy(
    std::uint64_t generation,
    std::optional<std::string_view> module_name,
    int max_depth) const {
    const auto found = module_hierarchy_by_key_.find(moduleQueryKey(module_name, max_depth));
    if (found == module_hierarchy_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeModuleHierarchy(std::uint64_t generation,
                                      std::optional<std::string_view> module_name,
                                      int max_depth,
                                      SemanticModuleHierarchyResult result) {
    ModuleHierarchyEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    module_hierarchy_by_key_.insert_or_assign(moduleQueryKey(module_name, max_depth),
                                              std::move(entry));
    recordStore();
    evictOldestEntries(module_hierarchy_by_key_);
}

std::optional<SemanticSchematicResult> QueryCache::schematic(std::uint64_t generation,
                                                             std::optional<std::string_view> module_name,
                                                             int max_depth) const {
    const auto found = schematic_by_key_.find(moduleQueryKey(module_name, max_depth));
    if (found == schematic_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeSchematic(std::uint64_t generation,
                                std::optional<std::string_view> module_name,
                                int max_depth,
                                SemanticSchematicResult result) {
    SchematicEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    schematic_by_key_.insert_or_assign(moduleQueryKey(module_name, max_depth), std::move(entry));
    recordStore();
    evictOldestEntries(schematic_by_key_);
}

std::optional<SemanticConeTrace> QueryCache::backwardCone(std::uint64_t generation,
                                                          std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto found = backward_cone_by_key_.find(positionKey(uri, line, character));
    if (found == backward_cone_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeBackwardCone(std::uint64_t generation,
                                   std::string_view uri,
                                   int line,
                                   int character,
                                   SemanticConeTrace result) {
    BackwardConeEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    backward_cone_by_key_.insert_or_assign(positionKey(uri, line, character), std::move(entry));
    recordStore();
    evictOldestEntries(backward_cone_by_key_);
}

std::optional<SemanticCodeActionResult> QueryCache::codeActions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                ParseRange range) const {
    const auto found = code_actions_by_key_.find(rangeKey(uri, range));
    if (found == code_actions_by_key_.end() || found->second.generation != generation) {
        recordMiss();
        return std::nullopt;
    }
    recordHit();
    return found->second.result;
}

void QueryCache::storeCodeActions(std::uint64_t generation,
                                  std::string_view uri,
                                  ParseRange range,
                                  SemanticCodeActionResult result) {
    CodeActionsEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    code_actions_by_key_.insert_or_assign(rangeKey(uri, range), std::move(entry));
    recordStore();
    evictOldestEntries(code_actions_by_key_);
}

void QueryCache::recordHit() const {
    ++hits_;
}

void QueryCache::recordMiss() const {
    ++misses_;
}

void QueryCache::recordStore() {
    ++stores_;
}

void QueryCache::recordEviction() {
    ++evictions_;
}

std::uint64_t QueryCache::nextSequence() {
    ++sequence_;
    return sequence_;
}

std::string QueryCache::workspaceSymbolsKey(std::string_view query, size_t limit) {
    return QueryCacheKeyBuilder("workspaceSymbols")
        .field("query", query)
        .number("limit", static_cast<std::uint64_t>(limit))
        .str();
}

std::string QueryCache::positionKey(std::string_view uri, int line, int character) {
    return QueryCacheKeyBuilder("position")
        .field("uri", uri)
        .integer("line", line)
        .integer("character", character)
        .str();
}

std::string QueryCache::referencesKey(std::string_view uri,
                                      int line,
                                      int character,
                                      bool include_declaration,
                                      std::uint64_t plan_fingerprint,
                                      std::string_view target_stable_id,
                                      std::string_view scope_kind) {
    return QueryCacheKeyBuilder("references")
        .field("uri", uri)
        .integer("line", line)
        .integer("character", character)
        .boolean("includeDeclaration", include_declaration)
        .field("planFingerprint", std::to_string(plan_fingerprint))
        .field("targetStableId", target_stable_id)
        .field("scopeKind", scope_kind)
        .str();
}

std::string QueryCache::renameKey(std::string_view uri,
                                  int line,
                                  int character,
                                  std::string_view new_name,
                                  std::uint64_t plan_fingerprint,
                                  std::string_view target_stable_id,
                                  std::string_view scope_kind) {
    return QueryCacheKeyBuilder("rename")
        .field("uri", uri)
        .integer("line", line)
        .integer("character", character)
        .field("newName", new_name)
        .field("planFingerprint", std::to_string(plan_fingerprint))
        .field("targetStableId", target_stable_id)
        .field("scopeKind", scope_kind)
        .str();
}

std::string QueryCache::completionKey(std::string_view uri,
                                      int line,
                                      int character,
                                      std::string_view prefix,
                                      std::uint64_t plan_fingerprint) {
    return QueryCacheKeyBuilder("completion")
        .field("uri", uri)
        .integer("line", line)
        .integer("character", character)
        .field("prefix", prefix)
        .field("planFingerprint", std::to_string(plan_fingerprint))
        .str();
}

std::string QueryCache::moduleQueryKey(std::optional<std::string_view> module_name, int max_depth) {
    return QueryCacheKeyBuilder("moduleQuery")
        .boolean("hasModuleName", module_name.has_value())
        .optionalField("moduleName", module_name)
        .integer("maxDepth", max_depth)
        .str();
}

std::string QueryCache::rangeKey(std::string_view uri, ParseRange range) {
    return QueryCacheKeyBuilder("range")
        .field("uri", uri)
        .integer("startLine", range.start_line)
        .integer("startCharacter", range.start_character)
        .integer("endLine", range.end_line)
        .integer("endCharacter", range.end_character)
        .str();
}

} // namespace pristine::analysis::semantic
