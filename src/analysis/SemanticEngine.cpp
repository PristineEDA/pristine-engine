#include "pristine/analysis/SemanticEngine.h"

#include "semantic/AstIndex.h"
#include "semantic/AffectedDependencyGraph.h"
#include "semantic/CodeActionProvider.h"
#include "semantic/CompletionProvider.h"
#include "semantic/DebugTrace.h"
#include "semantic/DesignGraphProvider.h"
#include "semantic/DiagnosticProvider.h"
#include "semantic/NavigationProvider.h"
#include "semantic/QueryCache.h"
#include "semantic/SignatureInlayProvider.h"
#include "semantic/SnapshotBuilder.h"
#include "semantic/WorkspaceDiscoveryIndex.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace pristine::analysis {
namespace {

constexpr size_t kMaxSemanticLocations = 2000;

bool rangeContainsPosition(const ParseRange& range, int line, int character) {
    const auto starts_before_or_at = range.start_line < line ||
                                     (range.start_line == line && range.start_character <= character);
    const auto ends_after_or_at = range.end_line > line ||
                                 (range.end_line == line && range.end_character >= character);
    return starts_before_or_at && ends_after_or_at;
}

void hashCombine(std::uint64_t& seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

std::uint64_t hashString(std::string_view value) {
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(value));
}

void hashOptionalString(std::uint64_t& seed, const std::optional<std::string>& value) {
    hashCombine(seed, value.has_value() ? 1U : 0U);
    if (value.has_value()) {
        hashCombine(seed, hashString(*value));
    }
}

std::uint64_t discoveryCacheKeyFor(
    std::uint64_t generation,
    std::string_view workspace_root_uri,
    const SemanticEngineConfig& config,
    const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    std::uint64_t seed = 1469598103934665603ULL;
    hashCombine(seed, generation);
    hashCombine(seed, hashString(workspace_root_uri));
    hashOptionalString(seed, config.build);
    hashOptionalString(seed, config.build_pattern);
    hashCombine(seed, config.build_relative_paths ? 1U : 0U);
    hashOptionalString(seed, config.flags);
    hashOptionalString(seed, config.workspace_root_uri);
    for (const auto& top_module : config.top_modules) {
        hashCombine(seed, hashString(top_module));
    }
    for (const auto& index_config : config.index) {
        for (const auto& dir : index_config.dirs) {
            hashCombine(seed, hashString(dir));
        }
        hashCombine(seed, 0xfeedfaceULL);
        for (const auto& exclude_dir : index_config.exclude_dirs) {
            hashCombine(seed, hashString(exclude_dir));
        }
        hashCombine(seed, 0xdecafbadULL);
    }

    std::vector<std::string> document_uris;
    document_uris.reserve(documents.size());
    for (const auto& [uri, _] : documents) {
        document_uris.push_back(uri);
    }
    std::sort(document_uris.begin(), document_uris.end());
    for (const auto& uri : document_uris) {
        const auto& document = documents.at(uri);
        hashCombine(seed, hashString(uri));
        hashCombine(seed, static_cast<std::uint64_t>(document.version));
        hashCombine(seed, document.is_open ? 1U : 0U);
        hashCombine(seed, document.dirty ? 1U : 0U);
        hashCombine(seed, static_cast<std::uint64_t>(document.text.size()));
        hashCombine(seed, hashString(document.text));
    }
    return seed;
}

bool isDiscoveryDesignDeclaration(std::string_view kind) {
    return kind == "module" || kind == "interface";
}

std::string normalizeConfigDirectoryUri(std::string_view workspace_root_uri, std::string_view directory) {
    if (directory.empty()) {
        return {};
    }
    if (isFileUri(directory) || isWindowsAbsolutePath(directory)) {
        return withoutTrailingSlash(normalizeFileUri(directory));
    }
    const auto root = workspace_root_uri.empty() ? std::string("file:///") : std::string(workspace_root_uri);
    return joinFileUri(root, directory);
}

bool uriIsWithinDirectory(std::string_view uri, std::string_view directory_uri) {
    if (directory_uri.empty()) {
        return false;
    }
    const auto normalized_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto normalized_directory = withoutTrailingSlash(normalizeFileUri(directory_uri));
    return normalized_uri == normalized_directory || normalized_uri.starts_with(normalized_directory + "/");
}

bool documentMatchesDiscoveryConfig(std::string_view uri,
                                    std::string_view workspace_root_uri,
                                    const SemanticEngineConfig& config) {
    if (config.index.empty()) {
        return true;
    }

    for (const auto& index_config : config.index) {
        const bool in_index_dir = std::any_of(index_config.dirs.begin(),
                                             index_config.dirs.end(),
                                             [&](const std::string& dir) {
                                                 return uriIsWithinDirectory(
                                                     uri,
                                                     normalizeConfigDirectoryUri(workspace_root_uri, dir));
                                             });
        if (!in_index_dir) {
            continue;
        }
        const bool excluded = std::any_of(index_config.exclude_dirs.begin(),
                                          index_config.exclude_dirs.end(),
                                          [&](const std::string& exclude_dir) {
                                              return uriIsWithinDirectory(
                                                  uri,
                                                  normalizeConfigDirectoryUri(workspace_root_uri, exclude_dir));
                                          });
        if (!excluded) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace {

#ifndef NDEBUG
std::string queryCacheStatsDetail(const SemanticQueryCacheStats& stats) {
    std::ostringstream out;
    out << "hits=" << stats.hits << " misses=" << stats.misses << " stores=" << stats.stores
        << " evictions=" << stats.evictions << " entries=" << stats.total_entries
        << " workspaceSymbols=" << stats.workspace_symbols_entries
        << " hierarchy=" << stats.module_hierarchy_entries
        << " schematic=" << stats.schematic_entries
        << " backwardCone=" << stats.backward_cone_entries
        << " signatureScannedInvocations=" << stats.signature_scanned_invocations
        << " inlayScannedInvocations=" << stats.inlay_scanned_invocations
        << " macroScannedVisibleDefinitions=" << stats.macro_scanned_visible_definitions
        << " completionResolveScannedFacts=" << stats.completion_resolve_scanned_facts
        << " diagnosticLookupScannedFacts=" << stats.diagnostic_lookup_scanned_facts
        << " referenceLookupScannedOccurrences=" << stats.reference_lookup_scanned_occurrences
        << " callHierarchyScannedEdges=" << stats.call_hierarchy_scanned_edges
        << " callHierarchyScannedModules=" << stats.call_hierarchy_scanned_modules
        << " navigationOccurrenceScanned=" << stats.navigation_occurrence_scanned
        << " navigationTargetLookupScanned=" << stats.navigation_target_lookup_scanned
        << " implementationEdgeScanned=" << stats.implementation_edge_scanned
        << " semanticTokenScannedOccurrences=" << stats.semantic_token_scanned_occurrences
        << " selectionRangeScannedCandidates=" << stats.selection_range_scanned_candidates
        << " graphBindingLookupScannedFacts=" << stats.graph_binding_lookup_scanned_facts
        << " coneAdjacencyScannedEdges=" << stats.cone_adjacency_scanned_edges
        << " graphScannedGlobalSymbols=" << stats.graph_scanned_global_symbols
        << " coneScannedGlobalEdges=" << stats.cone_scanned_global_edges
        << " scannedGlobalSymbols=" << stats.scanned_global_symbols;
    return out.str();
}

void traceQueryCacheStats(std::string_view phase, const SemanticQueryCacheStats& stats) {
    semantic::debugTraceInstant(phase, queryCacheStatsDetail(stats));
}
#else
void traceQueryCacheStats(std::string_view, const SemanticQueryCacheStats&) {}
#endif

template<typename SnapshotData>
semantic::DesignGraphContext designGraphContextFor(const SnapshotData* data,
                                                   const SemanticEngineSnapshot& snapshot,
                                                   const SemanticEngineConfig& config,
                                                   const semantic::AstIndexView& ast_index) {
    semantic::DesignGraphContext context;
    context.generation = snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.top_modules = config.top_modules;
    if (data == nullptr) {
        return context;
    }
    context.modules_by_name = ast_index.modules_by_name;
    context.module_uris_by_name = ast_index.module_uris_by_name;
    context.module_signatures_by_name = ast_index.module_signatures_by_name;
    context.module_entries = ast_index.design_graph_module_entries;
    context.module_call_edge_index = ast_index.module_call_edge_index;
    context.symbols_by_id = ast_index.design_graph_symbols_by_id;
    context.binding_index = ast_index.design_graph_binding_index;
    context.cone_adjacency_index = ast_index.cone_adjacency_index;
    return context;
}

template<typename SnapshotData>
semantic::NavigationContext navigationContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 const std::string* document_text = nullptr) {
    semantic::NavigationContext context;
    context.mode = snapshot.mode;
    context.generation = snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.document_uri = std::move(document_uri);
    context.document_text = document_text;
    if (data == nullptr) {
        return context;
    }
    if (const auto occurrences = data->navigation_occurrences_by_uri.find(context.document_uri);
        occurrences != data->navigation_occurrences_by_uri.end()) {
        context.occurrence_index = &occurrences->second;
    }
    context.occurrences_by_symbol = &data->navigation_occurrences_by_symbol;
    context.reference_aliases_by_id = &data->reference_aliases_by_id;
    context.targets_by_id = &data->navigation_targets_by_id;
    context.implementation_edges = &data->implementation_edge_index;
    if (const auto types = data->type_references_by_uri.find(context.document_uri);
        types != data->type_references_by_uri.end()) {
        context.type_references = &types->second;
    }
    if (const auto macros = data->macro_invocations_by_uri.find(context.document_uri);
        macros != data->macro_invocations_by_uri.end()) {
        context.macro_invocations = &macros->second;
    }
    if (const auto calls = data->callable_invocations_by_uri.find(context.document_uri);
        calls != data->callable_invocations_by_uri.end()) {
        context.callable_invocations = &calls->second;
    }
    if (const auto ranges = data->selection_range_indexes_by_uri.find(context.document_uri);
        ranges != data->selection_range_indexes_by_uri.end()) {
        context.selection_range_index = &ranges->second;
    }
    return context;
}

template<typename SnapshotData>
semantic::DiagnosticContext diagnosticContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                                 std::string workspace_root_uri,
                                                 const semantic::AstIndexView& ast_index) {
    semantic::DiagnosticContext context;
    context.generation = snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.workspace_root_uri = std::move(workspace_root_uri);
    context.snapshot_diagnostics = snapshot.diagnostics;
    if (const auto document_it = documents.find(document_uri); document_it != documents.end()) {
        context.document = document_it->second;
    }
    else {
        context.document.uri = std::move(document_uri);
    }
    if (data == nullptr) {
        return context;
    }
    context.symbols_by_id = ast_index.diagnostic_symbols_by_id;
    context.lookup_index = ast_index.diagnostic_lookup_index;
    context.references = ast_index.diagnostic_references;
    context.assignment_edges_by_uri = ast_index.assignment_edges_by_uri;
    context.type_references_by_uri = ast_index.type_references_by_uri;
    context.include_directives_by_uri = ast_index.include_directives_by_uri;
    context.package_imports_by_uri = ast_index.package_imports_by_uri;
    context.modules_by_name = ast_index.modules_by_name;
    context.module_instances_by_uri = ast_index.module_instances_by_uri;
    return context;
}

semantic::CodeActionContext codeActionContextFor(const semantic::SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 ParseRange range,
                                                 const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                                 std::string workspace_root_uri,
                                                 const semantic::AstIndexView& ast_index,
                                                 std::vector<SemanticEngineDiagnostic> diagnostics) {
    semantic::CodeActionContext context;
    context.generation = snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.workspace_root_uri = std::move(workspace_root_uri);
    context.range = range;
    context.diagnostics = std::move(diagnostics);
    if (const auto document_it = documents.find(document_uri); document_it != documents.end()) {
        context.document = document_it->second;
    }
    else {
        context.document.uri = std::move(document_uri);
    }
    if (data != nullptr) {
        context.modules_by_name = ast_index.modules_by_name;
        for (const auto& signature_entry : ast_index.module_signatures_by_name) {
            const auto& signature = signature_entry.second;
            if (signature.uri == context.document.uri ||
                std::any_of(signature.schematic.cells.begin(),
                            signature.schematic.cells.end(),
                            [&](const SchematicCell& cell) {
                                return parseRangeContainsPosition(cell.range,
                                                                  range.start_line,
                                                                  range.start_character) ||
                                       parseRangeContainsPosition(cell.range,
                                                                  range.end_line,
                                                                  range.end_character);
                            })) {
                context.document_schematics.push_back(signature.schematic);
            }
        }
        if (const auto includes_it = ast_index.include_directives_by_uri.find(context.document.uri);
            includes_it != ast_index.include_directives_by_uri.end()) {
            context.include_directives = includes_it->second;
        }
        if (const auto instances_it = ast_index.module_instances_by_uri.find(context.document.uri);
            instances_it != ast_index.module_instances_by_uri.end()) {
            context.module_instances = instances_it->second;
        }
        context.packages_by_name = ast_index.package_visibility_by_name;
        if (const auto macros_it = ast_index.macro_invocations_by_uri.find(context.document.uri);
            macros_it != ast_index.macro_invocations_by_uri.end()) {
            context.macro_invocations = macros_it->second;
        }
        context.package_imports_by_uri = ast_index.package_imports_by_uri;
    }
    return context;
}

} // namespace

namespace {

std::optional<std::string_view> inferredDiscoveryTop(
    std::optional<std::string_view> requested_module_name,
    const SemanticEngineConfig& config,
    const SemanticWorkspaceDiscoverySnapshot& discovery) {
    if (requested_module_name.has_value() && !requested_module_name->empty()) {
        return requested_module_name;
    }
    if (config.top_modules.size() == 1) {
        return std::string_view(config.top_modules.front());
    }
    if (discovery.top_candidates.size() == 1) {
        return std::string_view(discovery.top_candidates.front());
    }
    return std::nullopt;
}

const SemanticDiscoveryClosureMetric* discoveryClosureMetricFor(
    std::optional<std::string_view> requested_module_name,
    const SemanticEngineConfig& config,
    const SemanticWorkspaceDiscoverySnapshot& discovery) {
    const auto selected_top = inferredDiscoveryTop(requested_module_name, config, discovery);
    if (!selected_top.has_value() || selected_top->empty()) {
        return nullptr;
    }

    const auto metric_it = std::find_if(discovery.closure_metrics.begin(),
                                        discovery.closure_metrics.end(),
                                        [&](const SemanticDiscoveryClosureMetric& metric) {
                                            return metric.root_name == *selected_top;
                                        });
    if (metric_it == discovery.closure_metrics.end()) {
        return nullptr;
    }
    return &*metric_it;
}

void applyDiscoveryClosureMetric(SemanticModuleHierarchyResult& result,
                                 const SemanticDiscoveryClosureMetric* metric) {
    if (metric == nullptr) {
        return;
    }
    result.discovery_closure_root_name = metric->root_name;
    result.discovery_closure_candidate_document_count = metric->candidate_document_count;
    result.discovery_closure_document_count = metric->selected_document_count;
    result.discovery_closure_missing_candidate_count = metric->missing_candidate_count;
    result.discovery_closure_deduped_document_count = metric->deduped_document_count;
}

void applyDiscoveryClosureMetric(SemanticSchematicResult& result,
                                 const SemanticDiscoveryClosureMetric* metric) {
    if (metric == nullptr) {
        return;
    }
    result.discovery_closure_root_name = metric->root_name;
    result.discovery_closure_candidate_document_count = metric->candidate_document_count;
    result.discovery_closure_document_count = metric->selected_document_count;
    result.discovery_closure_missing_candidate_count = metric->missing_candidate_count;
    result.discovery_closure_deduped_document_count = metric->deduped_document_count;
}

struct ClosureDesignGraphSnapshot {
    SemanticEngineSnapshot snapshot;
    std::unique_ptr<semantic::SnapshotData> data;
    SemanticEngineConfig context_config;
    size_t document_count = 0;
    std::int64_t build_micros = 0;
    bool used = false;
};

ClosureDesignGraphSnapshot buildClosureDesignGraphSnapshot(
    std::uint64_t generation,
    const SemanticEngineConfig& config,
    const std::unordered_map<std::string, SemanticEngineDocument>& documents,
    const std::vector<std::string>& dirty_document_uris,
    std::optional<std::string_view> module_name,
    const SemanticWorkspaceDiscoverySnapshot& discovery,
    const std::vector<std::string>& closure_uris) {
    ClosureDesignGraphSnapshot result;
    result.context_config = config;
    if (closure_uris.empty() || closure_uris.size() >= documents.size()) {
        return result;
    }

    const auto build_start = std::chrono::steady_clock::now();
    semantic::SnapshotBuildInput input;
    input.generation = generation;
    input.config = config;
    input.config.top_modules.clear();
    if (const auto selected_top = inferredDiscoveryTop(module_name, config, discovery);
        selected_top.has_value() && !selected_top->empty()) {
        input.config.top_modules.push_back(std::string(*selected_top));
    }
    result.context_config = input.config;

    std::set<std::string> closure_set(closure_uris.begin(), closure_uris.end());
    for (const auto& dirty_uri : dirty_document_uris) {
        if (closure_set.contains(dirty_uri)) {
            input.dirty_document_uris.push_back(dirty_uri);
        }
    }
    for (const auto& uri : closure_uris) {
        if (const auto document_it = documents.find(uri); document_it != documents.end()) {
            input.documents.emplace(uri, document_it->second);
        }
    }
    if (input.documents.empty()) {
        return result;
    }

    auto output = semantic::SnapshotBuilder{}.build(std::move(input));
    result.snapshot = std::move(output.snapshot);
    result.data = std::move(output.data);
    result.document_count = closure_uris.size();
    result.build_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - build_start)
                              .count();
    result.used = true;
    return result;
}

} // namespace

SemanticEngine::SemanticEngine()
    : affected_dependencies_(std::make_unique<semantic::AffectedDependencyGraph>()),
      query_cache_(std::make_unique<semantic::QueryCache>()) {}

SemanticEngine::~SemanticEngine() = default;

SemanticQueryCacheStats SemanticEngine::queryCacheStats() const {
    const auto stats = query_cache_->stats();
    return SemanticQueryCacheStats{.hits = stats.hits,
                                   .misses = stats.misses,
                                   .stores = stats.stores,
                                   .evictions = stats.evictions,
                                   .signature_scanned_invocations = stats.signature_scanned_invocations,
                                   .inlay_scanned_invocations = stats.inlay_scanned_invocations,
                                   .macro_scanned_visible_definitions =
                                       stats.macro_scanned_visible_definitions,
                                   .completion_resolve_scanned_facts =
                                       stats.completion_resolve_scanned_facts,
                                   .diagnostic_lookup_scanned_facts =
                                       stats.diagnostic_lookup_scanned_facts,
                                   .reference_lookup_scanned_occurrences =
                                       stats.reference_lookup_scanned_occurrences,
                                   .call_hierarchy_scanned_edges = stats.call_hierarchy_scanned_edges,
                                   .call_hierarchy_scanned_modules = stats.call_hierarchy_scanned_modules,
                                   .navigation_occurrence_scanned = stats.navigation_occurrence_scanned,
                                   .navigation_target_lookup_scanned =
                                       stats.navigation_target_lookup_scanned,
                                   .implementation_edge_scanned = stats.implementation_edge_scanned,
                                   .semantic_token_scanned_occurrences =
                                       stats.semantic_token_scanned_occurrences,
                                   .selection_range_scanned_candidates =
                                       stats.selection_range_scanned_candidates,
                                   .graph_binding_lookup_scanned_facts =
                                       stats.graph_binding_lookup_scanned_facts,
                                   .cone_adjacency_scanned_edges = stats.cone_adjacency_scanned_edges,
                                   .graph_scanned_global_symbols = stats.graph_scanned_global_symbols,
                                   .cone_scanned_global_edges = stats.cone_scanned_global_edges,
                                   .scanned_global_symbols = stats.scanned_global_symbols,
                                   .diagnostics_entries = stats.diagnostics_entries,
                                   .workspace_symbols_entries = stats.workspace_symbols_entries,
                                   .references_entries = stats.references_entries,
                                   .rename_entries = stats.rename_entries,
                                   .hover_entries = stats.hover_entries,
                                   .definition_entries = stats.definition_entries,
                                   .type_definition_entries = stats.type_definition_entries,
                                   .implementation_entries = stats.implementation_entries,
                                   .prepare_rename_entries = stats.prepare_rename_entries,
                                   .document_highlight_entries = stats.document_highlight_entries,
                                   .completions_entries = stats.completions_entries,
                                   .signature_help_entries = stats.signature_help_entries,
                                   .inlay_hints_entries = stats.inlay_hints_entries,
                                   .module_hierarchy_entries = stats.module_hierarchy_entries,
                                   .schematic_entries = stats.schematic_entries,
                                   .backward_cone_entries = stats.backward_cone_entries,
                                   .code_actions_entries = stats.code_actions_entries,
                                   .total_entries = stats.total_entries};
}

void SemanticEngine::resetQueryCacheStats() {
    query_cache_->resetStats();
}

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    affected_dependencies_->clear();
    snapshot_.reset();
    snapshot_data_.reset();
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    config_.workspace_root_uri = workspace_root_uri_.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>{workspace_root_uri_};
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::configure(SemanticEngineConfig config) {
    config_ = std::move(config);
    if (config_.workspace_root_uri.has_value()) {
        workspace_root_uri_ = withoutTrailingSlash(normalizeFileUri(*config_.workspace_root_uri));
        config_.workspace_root_uri = workspace_root_uri_;
    }
    std::sort(config_.top_modules.begin(), config_.top_modules.end());
    config_.top_modules.erase(std::unique(config_.top_modules.begin(), config_.top_modules.end()),
                              config_.top_modules.end());
    for (auto& index_config : config_.index) {
        for (auto& dir : index_config.dirs) {
            dir = normalizeConfigDirectoryUri(workspace_root_uri_, dir);
        }
        std::sort(index_config.dirs.begin(), index_config.dirs.end());
        index_config.dirs.erase(std::unique(index_config.dirs.begin(), index_config.dirs.end()),
                                index_config.dirs.end());
        for (auto& exclude_dir : index_config.exclude_dirs) {
            exclude_dir = normalizeConfigDirectoryUri(workspace_root_uri_, exclude_dir);
        }
        std::sort(index_config.exclude_dirs.begin(), index_config.exclude_dirs.end());
        index_config.exclude_dirs.erase(std::unique(index_config.exclude_dirs.begin(),
                                                    index_config.exclude_dirs.end()),
                                        index_config.exclude_dirs.end());
    }
    config_.index.erase(std::remove_if(config_.index.begin(),
                                       config_.index.end(),
                                       [](const SemanticEngineConfig::IndexConfig& index_config) {
                                           return index_config.dirs.empty();
                                       }),
                        config_.index.end());
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::updateDocument(std::string_view uri,
                                    std::string_view text,
                                    SemanticEngineDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.insert_or_assign(document_uri,
                                SemanticEngineDocument{.uri = document_uri,
                                                       .text = std::string(text),
                                                       .version = state.version,
                                                       .is_open = state.is_open,
                                                       .dirty = state.dirty});
    try {
        rebuildDependenciesFor(document_uri, text);
    }
    catch (...) {
        affected_dependencies_->setIncludedUris(document_uri, {});
    }
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.erase(document_uri);
    affected_dependencies_->removeDocument(document_uri);
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

const SemanticEngineDocument* SemanticEngine::document(std::string_view uri) const {
    const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (document_it == documents_.end()) {
        return nullptr;
    }
    return &document_it->second;
}

size_t SemanticEngine::documentCount() const {
    return documents_.size();
}

std::uint64_t SemanticEngine::generation() const {
    return generation_;
}

bool SemanticEngine::snapshotDirty() const {
    return snapshot_dirty_;
}

bool SemanticEngine::hasFreshSnapshot() const {
    return snapshot_.has_value() && !snapshot_dirty_;
}

std::vector<std::string> SemanticEngine::includedUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    return affected_dependencies_->includedUris(document_uri);
}

std::vector<std::string> SemanticEngine::includingUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    return affected_dependencies_->includingUris(document_uri);
}

std::vector<std::string> SemanticEngine::dirtyDocumentUris() const {
    std::vector<std::string> result;
    for (const auto& [uri, document] : documents_) {
        if (document.dirty) {
            result.push_back(uri);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> SemanticEngine::affectedDocumentUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    return affected_dependencies_->affectedDocumentUris(document_uri);
}

const SemanticEngineSnapshot& SemanticEngine::snapshot() const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.snapshot",
                               std::to_string(documents_.size()) + " documents generation=" +
                                   std::to_string(generation_));
    if (!snapshot_.has_value() || snapshot_dirty_) {
        rebuildSnapshot();
    }
    return *snapshot_;
}

SemanticWorkspaceDiscoverySnapshot SemanticEngine::workspaceDiscovery() const {
    const auto cache_key = discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    if (discovery_snapshot_cache_.has_value() && discovery_cache_key_ == cache_key) {
        auto cached = *discovery_snapshot_cache_;
        cached.cache_hit = true;
        cached.build_micros = 0;
        return cached;
    }

    const auto build_start = std::chrono::steady_clock::now();
    std::vector<semantic::DiscoveryDocumentInput> documents;
    documents.reserve(documents_.size());
    for (const auto& [uri, document] : documents_) {
        if (!documentMatchesDiscoveryConfig(uri, workspace_root_uri_, config_)) {
            continue;
        }
        documents.push_back(semantic::DiscoveryDocumentInput{.uri = uri, .text = document.text});
    }
    const auto discovery_index = semantic::buildWorkspaceDiscoveryIndex(generation_, std::move(documents));

    SemanticWorkspaceDiscoverySnapshot result;
    result.generation = discovery_index.generation;
    result.cache_key = cache_key;
    result.file_count = discovery_index.file_count;
    result.byte_count = discovery_index.byte_count;
    result.declaration_count = discovery_index.declaration_count;
    result.macro_count = discovery_index.macro_count;
    result.reference_count = discovery_index.reference_count;
    result.messages = discovery_index.messages;
    result.declarations.reserve(discovery_index.declarations.size());
    for (const auto& declaration : discovery_index.declarations) {
        result.declarations.push_back(SemanticDiscoverySymbol{.name = declaration.name,
                                                              .kind = declaration.kind,
                                                              .location = SemanticLocation{
                                                                  .uri = declaration.location.uri,
                                                                  .range = declaration.location.range}});
    }
    std::set<std::string> referenced_design_names;
    for (const auto& [name, _] : discovery_index.referenced_files_by_name) {
        referenced_design_names.insert(name);
    }
    std::set<std::string> all_design_names;
    for (const auto& declaration : discovery_index.declarations) {
        if (!isDiscoveryDesignDeclaration(declaration.kind)) {
            continue;
        }
        all_design_names.insert(declaration.name);
        if (!referenced_design_names.contains(declaration.name)) {
            result.top_candidates.push_back(declaration.name);
        }
    }
    if (result.top_candidates.empty()) {
        result.top_candidates.assign(all_design_names.begin(), all_design_names.end());
    }
    std::sort(result.top_candidates.begin(), result.top_candidates.end());
    result.top_candidates.erase(std::unique(result.top_candidates.begin(), result.top_candidates.end()),
                                result.top_candidates.end());
    for (const auto& design_name : all_design_names) {
        auto closure = semantic::discoveryDependencyClosure(discovery_index, std::string_view(design_name));
        if (!closure.empty()) {
            SemanticDiscoveryClosureMetric metric;
            metric.root_name = design_name;
            metric.candidate_document_count = closure.size();
            for (const auto& uri : closure) {
                if (documents_.contains(uri)) {
                    metric.selected_document_uris.push_back(uri);
                }
                else {
                    metric.missing_candidate_uris.push_back(uri);
                }
            }
            std::sort(metric.selected_document_uris.begin(), metric.selected_document_uris.end());
            metric.selected_document_uris.erase(std::unique(metric.selected_document_uris.begin(),
                                                            metric.selected_document_uris.end()),
                                                metric.selected_document_uris.end());
            std::sort(metric.missing_candidate_uris.begin(), metric.missing_candidate_uris.end());
            metric.missing_candidate_uris.erase(std::unique(metric.missing_candidate_uris.begin(),
                                                           metric.missing_candidate_uris.end()),
                                               metric.missing_candidate_uris.end());
            metric.selected_document_count = metric.selected_document_uris.size();
            metric.missing_candidate_count = metric.missing_candidate_uris.size();
            metric.deduped_document_count =
                metric.candidate_document_count > metric.selected_document_count + metric.missing_candidate_count
                    ? metric.candidate_document_count - metric.selected_document_count -
                          metric.missing_candidate_count
                    : 0;
            result.closure_metrics.push_back(std::move(metric));
            result.closure_uris_by_name.emplace(design_name, std::move(closure));
        }
    }
    std::sort(result.closure_metrics.begin(),
              result.closure_metrics.end(),
              [](const SemanticDiscoveryClosureMetric& lhs, const SemanticDiscoveryClosureMetric& rhs) {
                  return lhs.root_name < rhs.root_name;
              });
    result.build_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - build_start)
                              .count();
    result.cache_hit = false;
    discovery_snapshot_cache_ = result;
    discovery_cache_key_ = cache_key;
    return result;
}

std::vector<SemanticEngineDiagnostic> SemanticEngine::diagnosticsFor(std::string_view uri) const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.diagnosticsFor", std::string(uri));
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->diagnostics(current_snapshot.generation, document_uri)) {
        return *cached;
    }

    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = diagnosticContextFor(data,
                                        current_snapshot,
                                        document_uri,
                                        documents_,
                                        workspace_root_uri_,
                                        ast_index);
    auto result = semantic::diagnosticsFor(context);
    query_cache_->recordDiagnosticLookupFacts(context.lookup_scanned_fact_count);
    query_cache_->storeDiagnostics(current_snapshot.generation, document_uri, result);
    return result;
}

SemanticInactiveRegionResult SemanticEngine::inactiveRegions(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    SemanticInactiveRegionResult result;
    result.generation = current_snapshot.generation;
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    result.regions = semantic::inactiveRegionsForUri(ast_index, withoutTrailingSlash(normalizeFileUri(uri)));
    result.indexed_region_count = ast_index.inactive_region_count;
    result.build_micros = ast_index.inactive_region_build_micros;
    return result;
}

const semantic::SnapshotData* SemanticEngine::snapshotData() const {
    (void)snapshot();
    return snapshot_data_.get();
}

std::vector<std::string> SemanticEngine::closureDocumentUrisFor(
    std::optional<std::string_view> module_name,
    const SemanticWorkspaceDiscoverySnapshot& discovery) const {
    const auto selected_top = inferredDiscoveryTop(module_name, config_, discovery);
    if (!selected_top.has_value() || selected_top->empty()) {
        return {};
    }

    const auto closure_it = discovery.closure_uris_by_name.find(std::string(*selected_top));
    if (closure_it == discovery.closure_uris_by_name.end()) {
        return {};
    }

    std::vector<std::string> result;
    result.reserve(closure_it->second.size());
    for (const auto& uri : closure_it->second) {
        if (documents_.contains(uri)) {
            result.push_back(uri);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

SemanticLookupResult SemanticEngine::lookupAt(std::string_view uri, int line, int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto context = navigationContextFor(data, current_snapshot, document_uri);
    auto result = semantic::lookupAt(context, line, character);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, result.scanned_target_count, 0, 0, 0);
    return result;
}

SemanticReferenceResult SemanticEngine::definitionsAt(std::string_view uri,
                                                      int line,
                                                      int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->definitions(current_snapshot.generation, document_uri, line, character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::definitionsAt(context, line, character);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, 0, 0, 0, 0);
    query_cache_->storeDefinitions(current_snapshot.generation, document_uri, line, character, result);
    return result;
}

SemanticReferenceResult SemanticEngine::typeDefinitionsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->typeDefinitions(current_snapshot.generation,
                                                           document_uri,
                                                           line,
                                                           character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::typeDefinitionsAt(context, line, character);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, 0, 0, 0, 0);
    query_cache_->storeTypeDefinitions(current_snapshot.generation,
                                       document_uri,
                                       line,
                                       character,
                                       result);
    return result;
}

SemanticReferenceResult SemanticEngine::referencesAt(std::string_view uri,
                                                     int line,
                                                     int character,
                                                     bool include_declaration) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->references(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     include_declaration)) {
        return *cached;
    }

    const auto finish = [&](SemanticReferenceResult value) {
        query_cache_->storeReferences(current_snapshot.generation,
                                      document_uri,
                                      line,
                                      character,
                                      include_declaration,
                                      value);
        return value;
    };
    const auto context = navigationContextFor(snapshotData(), current_snapshot, document_uri);
    auto result = semantic::referencesAt(context,
                                         line,
                                         character,
                                         include_declaration,
                                         kMaxSemanticLocations);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, 0, 0, 0, 0);
    return finish(std::move(result));
}

SemanticReferenceResult SemanticEngine::documentHighlightsAt(std::string_view uri,
                                                              int line,
                                                              int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->documentHighlights(current_snapshot.generation,
                                                              document_uri,
                                                              line,
                                                              character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::documentHighlightsAt(context, line, character, kMaxSemanticLocations);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, 0, 0, 0, 0);
    query_cache_->storeDocumentHighlights(current_snapshot.generation,
                                          document_uri,
                                          line,
                                          character,
                                          result);
    return result;
}

SemanticReferenceResult SemanticEngine::implementationsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->implementations(current_snapshot.generation,
                                                           document_uri,
                                                           line,
                                                           character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::implementationsAt(context, line, character, kMaxSemanticLocations);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count,
                                       0,
                                       result.scanned_implementation_edge_count,
                                       0,
                                       0);
    query_cache_->storeImplementations(current_snapshot.generation,
                                       document_uri,
                                       line,
                                       character,
                                       result);
    return result;
}

SemanticHoverResult SemanticEngine::hoverAt(std::string_view uri, int line, int character) const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.hoverAt",
                               std::string(uri) + ":" + std::to_string(line) + ":" +
                                   std::to_string(character));
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->hover(current_snapshot.generation, document_uri, line, character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::hoverAt(context, line, character);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, result.scanned_target_count, 0, 0, 0);
    query_cache_->storeHover(current_snapshot.generation, document_uri, line, character, result);
    return result;
}

SemanticPrepareRenameResult SemanticEngine::prepareRenameAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->prepareRename(current_snapshot.generation,
                                                         document_uri,
                                                         line,
                                                         character)) {
        return *cached;
    }
    const auto context = navigationContextFor(snapshotData(),
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::prepareRenameAt(context, line, character);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count, result.scanned_target_count, 0, 0, 0);
    query_cache_->storePrepareRename(current_snapshot.generation,
                                     document_uri,
                                     line,
                                     character,
                                     result);
    return result;
}

SemanticRenameResult SemanticEngine::renameAt(std::string_view uri,
                                              int line,
                                              int character,
                                              std::string_view new_name) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->rename(current_snapshot.generation,
                                                document_uri,
                                                line,
                                                character,
                                                new_name)) {
        return *cached;
    }

    const auto context = navigationContextFor(snapshotData(), current_snapshot, document_uri);
    auto result = semantic::renameAt(context, line, character, new_name, kMaxSemanticLocations);
    query_cache_->recordNavigationScan(result.scanned_occurrence_count,
                                       0,
                                       result.scanned_implementation_edge_count,
                                       0,
                                       0);
    query_cache_->storeRename(current_snapshot.generation,
                              document_uri,
                              line,
                              character,
                              new_name,
                              result);
    return result;
}

SemanticCompletionResult SemanticEngine::completionsAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view prefix) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->completions(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     prefix)) {
        return *cached;
    }

    const auto finish = [&](SemanticCompletionResult value) {
        query_cache_->storeCompletions(current_snapshot.generation,
                                       document_uri,
                                       line,
                                       character,
                                       prefix,
                                       value);
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    const auto document_it = documents_.find(document_uri);
    const auto* document = document_it == documents_.end() ? nullptr : &document_it->second;
    semantic::CompletionQueryContext context;
    context.generation = current_snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.document_uri = document_uri;
    context.document_text = document == nullptr ? nullptr : &document->text;
    context.packages = &ast_index.package_visibility_by_name;
    context.workspace_candidates_by_name = &ast_index.workspace_visibility;
    context.module_definition_ids_by_name = &ast_index.module_definition_ids_by_name;
    context.modules_by_name = &ast_index.modules_by_name;
    context.module_uris_by_name = &ast_index.module_uris_by_name;
    context.scope_visibility_count = ast_index.scope_visibility_count;
    context.package_visibility_count = ast_index.package_visibility_count;
    context.member_visibility_count = ast_index.member_visibility_count;
    context.callable_visibility_count = ast_index.callable_visibility_count;
    context.scope_visibility_build_micros = ast_index.scope_visibility_build_micros;
    if (const auto scopes_it = ast_index.scope_visibility_by_uri.find(document_uri);
        scopes_it != ast_index.scope_visibility_by_uri.end()) {
        context.scopes = &scopes_it->second;
    }
    if (const auto candidates_it = ast_index.document_visibility_by_uri.find(document_uri);
        candidates_it != ast_index.document_visibility_by_uri.end()) {
        context.document_candidates = &candidates_it->second;
    }
    if (const auto members_it = ast_index.member_completions_by_qualifier_by_uri.find(document_uri);
        members_it != ast_index.member_completions_by_qualifier_by_uri.end()) {
        context.member_candidates_by_qualifier = &members_it->second;
    }
    if (const auto macros_it = ast_index.visible_macros_by_uri.find(document_uri);
        macros_it != ast_index.visible_macros_by_uri.end()) {
        context.macros = &macros_it->second;
    }
    if (const auto instances_it = ast_index.module_instances_by_uri.find(document_uri);
        instances_it != ast_index.module_instances_by_uri.end()) {
        context.module_instances = &instances_it->second;
    }
    return finish(semantic::completeAt(context, line, character, prefix));
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    if (data == nullptr) {
        return semantic::resolveCompletionItem(stable_id, label, semantic::CompletionResolveContext{});
    }

    semantic::CompletionResolveContext context;
    context.facts_by_id = &ast_index.completion_resolve_by_id;
    query_cache_->recordCompletionResolveFactLookup(1);
    return semantic::resolveCompletionItem(stable_id, label, context);
}

SemanticSignatureHelpResult SemanticEngine::signatureHelpAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->signatureHelp(current_snapshot.generation,
                                                        document_uri,
                                                        line,
                                                        character)) {
        return *cached;
    }
    const auto finish = [&](SemanticSignatureHelpResult value) {
        query_cache_->storeSignatureHelp(current_snapshot.generation,
                                         document_uri,
                                         line,
                                         character,
                                         value);
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);

    semantic::SignatureInlayContext context;
    context.generation = current_snapshot.generation;
    context.document_uri = document_uri;
    context.modules_by_name = data == nullptr ? nullptr : &ast_index.modules_by_name;
    context.snapshot_available = data != nullptr;
    if (data != nullptr) {
        const auto instances_it = ast_index.signature_module_instances_by_uri.find(document_uri);
        if (instances_it != ast_index.signature_module_instances_by_uri.end()) {
            context.module_instances = instances_it->second;
        }
        const auto macros_it = ast_index.macro_invocations_by_uri.find(document_uri);
        if (macros_it != ast_index.macro_invocations_by_uri.end()) {
            context.macro_invocations = macros_it->second;
        }
        const auto calls_it = ast_index.callable_invocations_by_uri.find(document_uri);
        if (calls_it != ast_index.callable_invocations_by_uri.end()) {
            context.callable_invocations = calls_it->second;
        }
    }
    return finish(semantic::signatureHelpAt(context, line, character));
}

SemanticInlayHintResult SemanticEngine::inlayHints(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->inlayHints(current_snapshot.generation,
                                                    document_uri,
                                                    range)) {
        return *cached;
    }
    const auto finish = [&](SemanticInlayHintResult value) {
        query_cache_->storeInlayHints(current_snapshot.generation,
                                      document_uri,
                                      range,
                                      value);
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);

    semantic::SignatureInlayContext context;
    context.generation = current_snapshot.generation;
    context.document_uri = document_uri;
    context.modules_by_name = data == nullptr ? nullptr : &ast_index.modules_by_name;
    context.snapshot_available = data != nullptr;
    if (const auto symbols_it = ast_index.inlay_symbols_by_uri.find(document_uri);
        symbols_it != ast_index.inlay_symbols_by_uri.end()) {
        context.symbols = symbols_it->second;
    }
    if (data != nullptr) {
        const auto instances_it = ast_index.signature_module_instances_by_uri.find(document_uri);
        if (instances_it != ast_index.signature_module_instances_by_uri.end()) {
            context.module_instances = instances_it->second;
        }
        const auto calls_it = ast_index.callable_invocations_by_uri.find(document_uri);
        if (calls_it != ast_index.callable_invocations_by_uri.end()) {
            context.callable_invocations = calls_it->second;
        }
        const auto macros_it = ast_index.macro_invocations_by_uri.find(document_uri);
        if (macros_it != ast_index.macro_invocations_by_uri.end()) {
            context.macro_invocations = macros_it->second;
        }
    }
    return finish(semantic::inlayHints(context, range));
}

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto context = navigationContextFor(data, current_snapshot, document_uri);
    auto result = semantic::semanticTokens(context);
    query_cache_->recordNavigationScan(0, 0, 0, result.scanned_occurrence_count, 0);
    return result;
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto document_it = documents_.find(document_uri);
    auto context = navigationContextFor(data,
                                        current_snapshot,
                                        document_uri,
                                        document_it == documents_.end() ? nullptr : &document_it->second.text);
    auto result = semantic::selectionRangesAt(context, line, character);
    query_cache_->recordNavigationScan(0, 0, 0, 0, result.scanned_candidate_count);
    return result;
}

SemanticModuleHierarchyResult SemanticEngine::moduleHierarchy(std::optional<std::string_view> module_name,
                                                              int max_depth) const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.moduleHierarchy",
                               module_name.has_value() ? std::string(*module_name) : std::string("<auto>"));
    const auto discovery = workspaceDiscovery();
    if (const auto cached = query_cache_->moduleHierarchy(generation_,
                                                          module_name,
                                                          max_depth)) {
        auto result = *cached;
        if (result.discovery_closure_used) {
            result.discovery_closure_cache_hit = true;
        }
        traceQueryCacheStats("semantic.moduleHierarchy.queryCache", queryCacheStats());
        return result;
    }
    const auto closure_uris = closureDocumentUrisFor(module_name, discovery);
    const auto* closure_metric = discoveryClosureMetricFor(module_name, config_, discovery);
    auto closure_snapshot = buildClosureDesignGraphSnapshot(generation_,
                                                            config_,
                                                            documents_,
                                                            dirtyDocumentUris(),
                                                            module_name,
                                                            discovery,
                                                            closure_uris);
    if (closure_snapshot.used) {
        const auto ast_index = semantic::buildAstIndexView(closure_snapshot.data.get(),
                                                           closure_snapshot.snapshot.generation);
        auto context = designGraphContextFor(closure_snapshot.data.get(),
                                             closure_snapshot.snapshot,
                                             closure_snapshot.context_config,
                                             ast_index);
        const auto query_start = std::chrono::steady_clock::now();
        auto result = semantic::moduleHierarchy(context, module_name, max_depth);
        result.discovery_closure_query_micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  query_start)
                .count();
        result.discovery_closure_used = true;
        result.discovery_closure_document_count = closure_snapshot.document_count;
        result.discovery_closure_build_micros = closure_snapshot.build_micros;
        applyDiscoveryClosureMetric(result, closure_metric);
        auto cached_result = result;
        cached_result.discovery_closure_build_micros = 0;
        cached_result.discovery_closure_query_micros = 0;
        query_cache_->storeModuleHierarchy(generation_, module_name, max_depth, std::move(cached_result));
        traceQueryCacheStats("semantic.moduleHierarchy.queryCache", queryCacheStats());
        return result;
    }

    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->moduleHierarchy(current_snapshot.generation,
                                                          module_name,
                                                          max_depth)) {
        traceQueryCacheStats("semantic.moduleHierarchy.queryCache", queryCacheStats());
        return *cached;
    }

    SemanticModuleHierarchyResult result;
    result.generation = current_snapshot.generation;
    const auto finish = [&](SemanticModuleHierarchyResult value) {
        query_cache_->storeModuleHierarchy(current_snapshot.generation,
                                           module_name,
                                           max_depth,
                                           value);
        traceQueryCacheStats("semantic.moduleHierarchy.queryCache", queryCacheStats());
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    result = semantic::moduleHierarchy(context, module_name, max_depth);
    return finish(std::move(result));
}

SemanticSchematicResult SemanticEngine::schematic(std::optional<std::string_view> module_name,
                                                  int max_depth) const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.schematic",
                               module_name.has_value() ? std::string(*module_name) : std::string("<auto>"));
    const auto discovery = workspaceDiscovery();
    if (const auto cached = query_cache_->schematic(generation_, module_name, max_depth)) {
        auto result = *cached;
        if (result.discovery_closure_used) {
            result.discovery_closure_cache_hit = true;
        }
        traceQueryCacheStats("semantic.schematic.queryCache", queryCacheStats());
        return result;
    }
    const auto closure_uris = closureDocumentUrisFor(module_name, discovery);
    const auto* closure_metric = discoveryClosureMetricFor(module_name, config_, discovery);
    auto closure_snapshot = buildClosureDesignGraphSnapshot(generation_,
                                                            config_,
                                                            documents_,
                                                            dirtyDocumentUris(),
                                                            module_name,
                                                            discovery,
                                                            closure_uris);
    if (closure_snapshot.used) {
        const auto ast_index = semantic::buildAstIndexView(closure_snapshot.data.get(),
                                                           closure_snapshot.snapshot.generation);
        auto context = designGraphContextFor(closure_snapshot.data.get(),
                                             closure_snapshot.snapshot,
                                             closure_snapshot.context_config,
                                             ast_index);
        const auto query_start = std::chrono::steady_clock::now();
        auto result = semantic::schematic(context, module_name, max_depth);
        result.discovery_closure_query_micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  query_start)
                .count();
        result.discovery_closure_used = true;
        result.discovery_closure_document_count = closure_snapshot.document_count;
        result.discovery_closure_build_micros = closure_snapshot.build_micros;
        applyDiscoveryClosureMetric(result, closure_metric);
        auto cached_result = result;
        cached_result.discovery_closure_build_micros = 0;
        cached_result.discovery_closure_query_micros = 0;
        query_cache_->storeSchematic(generation_, module_name, max_depth, std::move(cached_result));
        traceQueryCacheStats("semantic.schematic.queryCache", queryCacheStats());
        return result;
    }

    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->schematic(current_snapshot.generation, module_name, max_depth)) {
        traceQueryCacheStats("semantic.schematic.queryCache", queryCacheStats());
        return *cached;
    }

    SemanticSchematicResult result;
    result.generation = current_snapshot.generation;
    const auto finish = [&](SemanticSchematicResult value) {
        query_cache_->recordDesignGraphScan(value.graph_binding_lookup_scanned_facts,
                                            0,
                                            value.graph_scanned_global_symbols,
                                            0);
        query_cache_->storeSchematic(current_snapshot.generation, module_name, max_depth, value);
        traceQueryCacheStats("semantic.schematic.queryCache", queryCacheStats());
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    result = semantic::schematic(context, module_name, max_depth);
    return finish(std::move(result));
}

SemanticCallHierarchyPrepareResult SemanticEngine::prepareCallHierarchy(std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    auto result = semantic::prepareCallHierarchy(context, document_uri, line, character);
    query_cache_->recordCallHierarchyScan(result.scanned_edge_count, result.scanned_module_count);
    return result;
}

SemanticCallHierarchyCallsResult SemanticEngine::incomingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    auto result = semantic::incomingCalls(context, item);
    query_cache_->recordCallHierarchyScan(result.scanned_edge_count, result.scanned_module_count);
    return result;
}

SemanticCallHierarchyCallsResult SemanticEngine::outgoingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    auto result = semantic::outgoingCalls(context, item);
    query_cache_->recordCallHierarchyScan(result.scanned_edge_count, result.scanned_module_count);
    return result;
}

SemanticConeTrace SemanticEngine::backwardConeAt(std::string_view uri,
                                                 int line,
                                                 int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->backwardCone(current_snapshot.generation,
                                                       document_uri,
                                                       line,
                                                       character)) {
        traceQueryCacheStats("semantic.backwardCone.queryCache", queryCacheStats());
        return *cached;
    }

    const auto lookup = lookupAt(uri, line, character);
    SemanticConeTrace trace;
    trace.generation = lookup.generation;
    trace.messages = lookup.messages;
    trace.unresolved = lookup.unresolved;
    const auto finish = [&](SemanticConeTrace value) {
        query_cache_->recordDesignGraphScan(0,
                                            value.cone_adjacency_scanned_edges,
                                            0,
                                            value.cone_scanned_global_edges);
        query_cache_->storeBackwardCone(current_snapshot.generation,
                                        document_uri,
                                        line,
                                        character,
                                        value);
        traceQueryCacheStats("semantic.backwardCone.queryCache", queryCacheStats());
        return value;
    };
    if (!lookup.symbol.has_value()) {
        if (trace.messages.empty()) {
            trace.messages.push_back("No signal symbol was found at the requested position.");
        }
        return finish(std::move(trace));
    }

    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    std::optional<semantic::SnapshotConeSliceFact> root_slice;
    if (const auto roots = context.cone_adjacency_index.root_selections_by_uri.find(document_uri);
        roots != context.cone_adjacency_index.root_selections_by_uri.end()) {
        for (const auto& root : roots->second) {
            if (root.symbol_id == lookup.symbol->stable_id && rangeContainsPosition(root.range, line, character)) {
                root_slice = root.slice;
                break;
            }
        }
    }
    trace = semantic::backwardCone(context, document_uri, lookup, kMaxSemanticLocations, root_slice);
    return finish(std::move(trace));
}

SemanticCodeActionResult SemanticEngine::codeActionsAt(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->codeActions(current_snapshot.generation, document_uri, range)) {
        return *cached;
    }

    SemanticCodeActionResult result;
    result.generation = current_snapshot.generation;
    const auto finish = [&](SemanticCodeActionResult value) {
        query_cache_->storeCodeActions(current_snapshot.generation,
                                       document_uri,
                                       range,
                                       value);
        return value;
    };
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = codeActionContextFor(data,
                                        current_snapshot,
                                        document_uri,
                                        range,
                                        documents_,
                                        workspace_root_uri_,
                                        ast_index,
                                        diagnosticsFor(document_uri));
    result = semantic::codeActionsAt(context);

    return finish(std::move(result));
}

SemanticWorkspaceSymbolResult SemanticEngine::workspaceSymbols(std::string_view query,
                                                               size_t limit) const {
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->workspaceSymbols(current_snapshot.generation, query, limit)) {
        return *cached;
    }

    const auto* data = snapshotData();
    auto context = semantic::workspaceSymbolContext(semantic::buildAstIndexView(data,
                                                                                current_snapshot.generation));
    auto result = semantic::workspaceSymbols(context, query, limit);
    query_cache_->storeWorkspaceSymbols(current_snapshot.generation, query, limit, result);
    return result;
}

void SemanticEngine::rebuildDependenciesFor(std::string_view document_uri, std::string_view text) {
    const auto normalized_uri = withoutTrailingSlash(normalizeFileUri(document_uri));
    CompilationService compilation_service;
    std::vector<std::string> included_uris;
    for (const auto& include : compilation_service.includeDirectives(text)) {
        included_uris.push_back(joinFileUri(uriDirectory(normalized_uri), include.target));
    }
    std::sort(included_uris.begin(), included_uris.end());
    included_uris.erase(std::unique(included_uris.begin(), included_uris.end()), included_uris.end());
    affected_dependencies_->setIncludedUris(normalized_uri, std::move(included_uris));
}

void SemanticEngine::rebuildSnapshot() const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.rebuildSnapshot",
                               std::to_string(documents_.size()) + " documents generation=" +
                                   std::to_string(generation_));
    semantic::SnapshotBuildInput input;
    input.generation = generation_;
    input.config = config_;
    input.dirty_document_uris = dirtyDocumentUris();
    input.documents = documents_;
    auto output = semantic::SnapshotBuilder{}.build(std::move(input));

    *affected_dependencies_ = std::move(output.affected_dependencies);
    snapshot_ = std::move(output.snapshot);
    snapshot_data_ = std::move(output.data);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
