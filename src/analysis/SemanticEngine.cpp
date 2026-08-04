#include "pristine/analysis/SemanticEngine.h"

#include "semantic/AstIndex.h"
#include "semantic/AffectedDependencyGraph.h"
#include "semantic/CodeActionProvider.h"
#include "semantic/CompletionProvider.h"
#include "semantic/DebugTrace.h"
#include "semantic/DesignGraphProvider.h"
#include "semantic/DiagnosticProvider.h"
#include "semantic/DocumentClosurePlanner.h"
#include "semantic/NavigationProvider.h"
#include "semantic/QueryCache.h"
#include "semantic/SemanticSnapshotCache.h"
#include "semantic/SignatureInlayProvider.h"
#include "semantic/SnapshotBuilder.h"
#include "semantic/WorkspaceDiscoveryIndex.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cassert>
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

std::string snapshotIdentity(std::string_view scope,
                             std::uint64_t generation,
                             std::uint64_t fingerprint) {
    return std::string(scope) + ":" + std::to_string(generation) + ":" +
           std::to_string(fingerprint);
}

SemanticSnapshotBuildStats snapshotBuildStats(
    const semantic::SnapshotBuildOutput& output,
    std::string scope_kind,
    std::string root_uri,
    std::string identity,
    std::string closure_confidence,
    std::string closure_reason,
    size_t selected_document_count,
    std::uint64_t cancelled_build_count,
    bool cache_hit = false) {
    return SemanticSnapshotBuildStats{
        .scope_kind = std::move(scope_kind),
        .root_uri = std::move(root_uri),
        .snapshot_identity = std::move(identity),
        .closure_confidence = std::move(closure_confidence),
        .closure_reason = std::move(closure_reason),
        .cancellation_checkpoint = output.metrics.cancellation_checkpoint,
        .input_document_count = output.metrics.input_document_count,
        .selected_document_count = selected_document_count,
        .normalize_micros = output.metrics.normalize_micros,
        .buffer_assignment_micros = output.metrics.assign_buffers_micros,
        .syntax_preprocessor_micros = output.metrics.syntax_facts_micros,
        .compilation_micros = output.metrics.compilation_micros,
        .ast_index_micros = output.metrics.ast_index_micros,
        .dependency_edges_micros = output.metrics.dependency_edges_micros,
        .semantic_diagnostics_micros = output.metrics.semantic_diagnostics_micros,
        .finalize_micros = output.metrics.finalize_micros,
        .total_micros = output.metrics.total_micros,
        .cancelled_build_count = cancelled_build_count,
        .cache_hit = cache_hit};
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

void traceSnapshotBuildStats(std::string_view phase, const SemanticSnapshotBuildStats& stats) {
    std::ostringstream out;
    out << "scope=" << stats.scope_kind << " root=" << stats.root_uri
        << " identity=" << stats.snapshot_identity
        << " confidence=" << stats.closure_confidence
        << " inputDocuments=" << stats.input_document_count
        << " selectedDocuments=" << stats.selected_document_count
        << " cacheHit=" << (stats.cache_hit ? "true" : "false")
        << " normalizeMicros=" << stats.normalize_micros
        << " bufferMicros=" << stats.buffer_assignment_micros
        << " syntaxMicros=" << stats.syntax_preprocessor_micros
        << " compilationMicros=" << stats.compilation_micros
        << " astIndexMicros=" << stats.ast_index_micros
        << " dependencyMicros=" << stats.dependency_edges_micros
        << " diagnosticsMicros=" << stats.semantic_diagnostics_micros
        << " finalizeMicros=" << stats.finalize_micros
        << " totalMicros=" << stats.total_micros
        << " cancelledBuilds=" << stats.cancelled_build_count;
    semantic::debugTraceInstant(phase, out.str());
}
#else
void traceQueryCacheStats(std::string_view, const SemanticQueryCacheStats&) {}
void traceSnapshotBuildStats(std::string_view, const SemanticSnapshotBuildStats&) {}
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
        context.design_graph_bindings = &ast_index.design_graph_binding_index;
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
      query_cache_(std::make_unique<semantic::QueryCache>()),
      semantic_snapshot_cache_(std::make_unique<semantic::SemanticSnapshotCache>()) {}

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
                                   .completion_resolve_identity_hits =
                                       stats.completion_resolve_identity_hits,
                                   .completion_resolve_identity_misses =
                                       stats.completion_resolve_identity_misses,
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

SemanticSnapshotBuildStats SemanticEngine::lastSnapshotBuildStats() const {
    return last_snapshot_build_stats_;
}

SemanticCompletionResolveTelemetry SemanticEngine::lastCompletionResolveTelemetry() const {
    return last_completion_resolve_telemetry_;
}

void SemanticEngine::clearDocumentSnapshotCache() {
    semantic_snapshot_cache_->clear();
}

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    affected_dependencies_->clear();
    snapshot_.reset();
    snapshot_data_.reset();
    full_snapshot_identity_.clear();
    discovery_snapshot_cache_.reset();
    discovery_cache_key_ = 0;
    query_cache_->clear();
    clearDocumentSnapshotCache();
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
    clearDocumentSnapshotCache();
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
    clearDocumentSnapshotCache();
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
    clearDocumentSnapshotCache();
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
    clearDocumentSnapshotCache();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setRequestControl(SemanticRequestControl control) {
    request_control_ = std::move(control);
}

void SemanticEngine::clearRequestControl() {
    request_control_ = {};
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
    result.reference_candidate_uris_by_name = discovery_index.reference_candidate_uris_by_name;
    result.macro_invocation_uris_by_name = discovery_index.macro_invocation_uris_by_name;
    result.reference_candidate_incomplete_reasons =
        discovery_index.reference_candidate_incomplete_reasons;
    result.macro_definitions.reserve(discovery_index.macro_definitions.size());
    for (const auto& definition : discovery_index.macro_definitions) {
        result.macro_definitions.push_back(SemanticWorkspaceDiscoverySnapshot::MacroDefinition{
            .name = definition.name,
            .uri = definition.uri,
            .body_identifiers = definition.body_identifiers,
            .complete = definition.complete,
            .reasons = definition.reasons});
    }
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
    for (const auto& [uri, document] : documents_) {
        if (!documentMatchesDiscoveryConfig(uri, workspace_root_uri_, config_)) {
            continue;
        }
        const auto closure = semantic::discoveryDocumentClosure(discovery_index, uri);
        SemanticWorkspaceDiscoverySnapshot::DocumentClosure projected;
        projected.uris = closure.uris;
        projected.reasons = closure.reasons;
        projected.fingerprint = closure.fingerprint;
        projected.complete = closure.confidence == semantic::DiscoveryClosureConfidence::Complete;
        result.document_closures_by_uri.emplace(uri, std::move(projected));
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

const SemanticWorkspaceDiscoverySnapshot& SemanticEngine::workspaceDiscoveryView() const {
    const auto cache_key =
        discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    if (!discovery_snapshot_cache_.has_value() || discovery_cache_key_ != cache_key) {
        (void)workspaceDiscovery();
    }
    return *discovery_snapshot_cache_;
}

std::vector<SemanticEngineDiagnostic> SemanticEngine::diagnosticsFor(std::string_view uri) const {
    PRISTINE_DEBUG_TRACE_SCOPE("semantic.diagnosticsFor", std::string(uri));
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->diagnostics(current_snapshot.generation, document_uri)) {
        return *cached;
    }

    const auto* data = selection.data;
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
    const auto selection = snapshotForDocument(uri);
    const auto& current_snapshot = *selection.snapshot;
    SemanticInactiveRegionResult result;
    result.generation = current_snapshot.generation;
    const auto* data = selection.data;
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

SemanticEngine::SnapshotSelection SemanticEngine::snapshotForDocument(
    std::string_view uri,
    std::optional<std::string_view> workspace_candidate_prefix) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto full_selection = [&](std::string_view closure_reason = {}) -> SnapshotSelection {
        const auto& full_snapshot = snapshot();
        if (!closure_reason.empty()) {
            last_snapshot_build_stats_.root_uri = document_uri;
            last_snapshot_build_stats_.closure_confidence = "incomplete";
            last_snapshot_build_stats_.closure_reason = std::string(closure_reason);
        }
        return SnapshotSelection{.snapshot = &full_snapshot,
                                 .data = snapshot_data_.get(),
                                 .snapshot_identity = full_snapshot_identity_,
                                 .selected_document_count = documents_.size(),
                                 .closure = false};
    };

    request_control_.cancellation.throwIfCancellationRequested();
    const auto& discovery = workspaceDiscoveryView();
    auto closure_roots = request_control_.closure_root_uris;
    if (closure_roots.empty()) {
        closure_roots.push_back(document_uri);
    }
    for (auto& root : closure_roots) {
        root = withoutTrailingSlash(normalizeFileUri(root));
    }
    std::sort(closure_roots.begin(), closure_roots.end());
    closure_roots.erase(std::unique(closure_roots.begin(), closure_roots.end()),
                        closure_roots.end());

    const auto base_plan = semantic::planDocumentClosure(discovery, closure_roots, config_.top_modules);
    std::optional<semantic::WorkspaceCompletionPlan> workspace_plan;
    if (workspace_candidate_prefix.has_value()) {
        workspace_plan = semantic::planWorkspaceCompletionClosure(
            discovery, closure_roots, config_.top_modules, *workspace_candidate_prefix);
    }
    const auto& closure_plan = workspace_plan.has_value() ? workspace_plan->closure : base_plan;
    const auto closure_uris = closure_plan.uris;
    if (!closure_plan.complete || closure_uris.empty()) {
        std::string reason;
        for (const auto& value : closure_plan.reasons) {
            if (!reason.empty()) {
                reason.append("; ");
            }
            reason.append(value);
        }
        return full_selection(reason.empty() ? "document-closure-incomplete" : reason);
    }
    if (!workspace_candidate_prefix.has_value() && closure_uris.size() >= documents_.size()) {
        return full_selection("document-closure-covers-workspace");
    }

    std::uint64_t cache_key = discovery.cache_key;
    hashCombine(cache_key, closure_plan.fingerprint);
    hashCombine(cache_key, hashString(closure_plan.root_key));
    if (workspace_candidate_prefix.has_value()) {
        hashCombine(cache_key, hashString(*workspace_candidate_prefix));
        hashCombine(cache_key, hashString(semantic::kWorkspaceCompletionPolicyVersion));
    }
    const auto scope_kind = workspace_candidate_prefix.has_value() ? "workspaceCompletionClosure"
                                                                    : "documentClosure";
    const auto closure_confidence = workspace_plan.has_value() && workspace_plan->incomplete
                                        ? "complete-with-capped-candidates"
                                        : "complete";
    if (auto* cached = semantic_snapshot_cache_->find(cache_key)) {
        last_snapshot_build_stats_.scope_kind = scope_kind;
        last_snapshot_build_stats_.root_uri = closure_plan.root_key;
        last_snapshot_build_stats_.snapshot_identity = cached->identity;
        last_snapshot_build_stats_.closure_confidence = closure_confidence;
        last_snapshot_build_stats_.closure_reason = closure_plan.reasons.empty()
                                                        ? std::string{}
                                                        : closure_plan.reasons.front();
        last_snapshot_build_stats_.input_document_count = documents_.size();
        last_snapshot_build_stats_.selected_document_count = closure_uris.size();
        last_snapshot_build_stats_.cache_hit = true;
        traceSnapshotBuildStats("semantic.documentClosureSnapshot.cache", last_snapshot_build_stats_);
        return SnapshotSelection{.snapshot = &cached->snapshot,
                                 .data = cached->data.get(),
                                 .snapshot_identity = cached->identity,
                                 .completion_plan_fingerprint = closure_plan.fingerprint,
                                 .planned_workspace_candidate_count = workspace_plan.has_value()
                                                                          ? workspace_plan->planned_candidate_count
                                                                          : 0,
                                 .selected_document_count = closure_uris.size(),
                                 .workspace_completion_incomplete = workspace_plan.has_value() &&
                                                                    workspace_plan->incomplete,
                                 .closure = true};
    }

    semantic::SnapshotBuildInput input;
    input.generation = generation_;
    input.config = config_;
    input.control.cancellation = request_control_.cancellation;
    input.control.report_progress = request_control_.report_progress;
    const std::set<std::string> closure_set(closure_uris.begin(), closure_uris.end());
    for (const auto& dirty_uri : dirtyDocumentUris()) {
        if (closure_set.contains(dirty_uri)) {
            input.dirty_document_uris.push_back(dirty_uri);
        }
    }
    for (const auto& closure_uri : closure_uris) {
        if (const auto found = documents_.find(closure_uri); found != documents_.end()) {
            input.documents.emplace(closure_uri, found->second);
        }
        else {
            return full_selection("closure-document-missing:" + closure_uri);
        }
    }

    const auto expected_generation = generation_;
    const auto expected_fingerprint =
        discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    auto output = semantic::SnapshotBuilder{}.build(std::move(input));
    const auto identity = snapshotIdentity(workspace_candidate_prefix.has_value() ? "workspaceCompletion"
                                                                                   : "closure",
                                           generation_,
                                           closure_plan.fingerprint);
    const auto closure_reason = closure_plan.reasons.empty() ? std::string{}
                                                              : closure_plan.reasons.front();
    if (output.status == semantic::SnapshotBuildStatus::Cancelled) {
        ++cancelled_snapshot_build_count_;
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        scope_kind,
                                                        closure_plan.root_key,
                                                        identity,
                                                        closure_confidence,
                                                        closure_reason,
                                                        closure_uris.size(),
                                                        cancelled_snapshot_build_count_);
        last_snapshot_build_stats_.input_document_count = documents_.size();
        throw pristine::OperationCancelled{};
    }
    if (output.status != semantic::SnapshotBuildStatus::Completed) {
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        scope_kind,
                                                        closure_plan.root_key,
                                                        identity,
                                                        closure_confidence,
                                                        closure_reason,
                                                        closure_uris.size(),
                                                        cancelled_snapshot_build_count_);
        last_snapshot_build_stats_.input_document_count = documents_.size();
        throw std::runtime_error(output.error.empty() ? "Document closure snapshot build failed"
                                                       : output.error);
    }
    const auto current_fingerprint =
        discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    if (generation_ != expected_generation || current_fingerprint != expected_fingerprint) {
        ++cancelled_snapshot_build_count_;
        throw pristine::OperationCancelled{};
    }

    last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                    scope_kind,
                                                    closure_plan.root_key,
                                                    identity,
                                                    closure_confidence,
                                                    closure_reason,
                                                    closure_uris.size(),
                                                    cancelled_snapshot_build_count_);
    last_snapshot_build_stats_.input_document_count = documents_.size();
    traceSnapshotBuildStats("semantic.documentClosureSnapshot.build", last_snapshot_build_stats_);
    if (closure_uris.size() == documents_.size()) {
        *affected_dependencies_ = std::move(output.affected_dependencies);
    }
    semantic::SemanticSnapshotCache::Entry entry;
    entry.key = cache_key;
    entry.generation = generation_;
    entry.config_discovery_fingerprint = discovery.cache_key;
    entry.closure_fingerprint = closure_plan.fingerprint;
    entry.scope_kind = scope_kind;
    entry.root_uri = closure_plan.root_key;
    entry.identity = identity;
    entry.snapshot = std::move(output.snapshot);
    entry.data = std::move(output.data);
    auto& stored = semantic_snapshot_cache_->insert(std::move(entry));
    ++completed_snapshot_build_count_;
    return SnapshotSelection{.snapshot = &stored.snapshot,
                             .data = stored.data.get(),
                             .snapshot_identity = stored.identity,
                             .completion_plan_fingerprint = closure_plan.fingerprint,
                             .planned_workspace_candidate_count = workspace_plan.has_value()
                                                                      ? workspace_plan->planned_candidate_count
                                                                      : 0,
                             .selected_document_count = closure_uris.size(),
                             .workspace_completion_incomplete = workspace_plan.has_value() &&
                                                                workspace_plan->incomplete,
                             .closure = true};
}

SemanticEngine::SnapshotSelection SemanticEngine::findSnapshotForIdentity(
    std::string_view identity) const {
    if (identity.empty()) {
        return {};
    }
    if (auto* cached = semantic_snapshot_cache_->findIdentity(identity)) {
        return SnapshotSelection{.snapshot = &cached->snapshot,
                                 .data = cached->data.get(),
                                 .snapshot_identity = cached->identity,
                                 .closure = true};
    }
    if (snapshot_.has_value() && !snapshot_dirty_ && snapshot_->generation == generation_ &&
        full_snapshot_identity_ == identity) {
        return SnapshotSelection{.snapshot = &*snapshot_,
                                 .data = snapshot_data_.get(),
                                 .snapshot_identity = std::string(identity),
                                 .closure = false};
    }
    return {};
}

SemanticEngine::ReferenceSnapshotSelection SemanticEngine::snapshotForReferenceQuery(
    std::string_view uri,
    int line,
    int character) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    ReferenceSnapshotSelection result;
    const auto base = snapshotForDocument(document_uri);
    result.snapshot = base;
    result.scope_kind = base.closure ? "documentClosure" : "full";
    result.selected_document_count = base.selected_document_count;
    result.confidence = base.closure ? "complete-base-closure" : "full-base-closure";
    if (base.snapshot == nullptr || base.data == nullptr) {
        result.confidence = "snapshot-unavailable";
        return result;
    }

    const auto base_context = navigationContextFor(base.data, *base.snapshot, document_uri);
    const auto lookup = semantic::lookupAt(base_context, line, character);
    if (!lookup.symbol.has_value()) {
        result.confidence = "target-unresolved";
        return result;
    }

    semantic::ReferenceSearchSeed seed;
    seed.stable_id = lookup.symbol->stable_id;
    seed.declaration_uri = lookup.symbol->location.uri;
    seed.spellings.push_back(lookup.symbol->name);
    if (const auto aliases = base.data->reference_aliases_by_id.find(seed.stable_id);
        aliases != base.data->reference_aliases_by_id.end()) {
        seed.alias_stable_ids = aliases->second;
        for (const auto& alias_id : aliases->second) {
            if (const auto target = base.data->navigation_targets_by_id.find(alias_id);
                target != base.data->navigation_targets_by_id.end()) {
                seed.spellings.push_back(target->second.identity.name);
            }
        }
    }
    std::sort(seed.spellings.begin(), seed.spellings.end());
    seed.spellings.erase(std::unique(seed.spellings.begin(), seed.spellings.end()), seed.spellings.end());
    if (seed.declaration_uri.empty() || seed.spellings.empty()) {
        seed.complete = false;
        seed.reasons.push_back("reference-search-seed-missing-source-location-or-spelling");
    }

    const auto& discovery = workspaceDiscoveryView();
    std::vector<std::string> roots{document_uri};
    if (!request_control_.closure_root_uris.empty()) {
        roots = request_control_.closure_root_uris;
        for (auto& root : roots) {
            root = withoutTrailingSlash(normalizeFileUri(root));
        }
    }
    auto plan = semantic::planReferenceCandidateClosure(discovery, roots, config_.top_modules, seed);
    result.target_stable_id = seed.stable_id;
    result.candidate_document_count = plan.candidate_document_count;
    result.selected_document_count = plan.selected_document_count;
    result.confidence = plan.confidence;
    result.plan_fingerprint = plan.closure.fingerprint;

    const auto full_selection = [&](std::string_view reason) {
        const auto& full_snapshot = snapshot();
        last_snapshot_build_stats_.root_uri = document_uri;
        last_snapshot_build_stats_.closure_confidence = "full";
        last_snapshot_build_stats_.closure_reason = std::string(reason);
        result.snapshot = SnapshotSelection{.snapshot = &full_snapshot,
                                            .data = snapshot_data_.get(),
                                            .snapshot_identity = full_snapshot_identity_,
                                            .selected_document_count = documents_.size(),
                                            .closure = false};
        result.scope_kind = "full";
        result.selected_document_count = documents_.size();
    };
    if (plan.requires_full_snapshot || !plan.closure.complete || plan.closure.uris.empty()) {
        full_selection(plan.confidence);
        return result;
    }

    std::uint64_t cache_key = discovery.cache_key;
    hashCombine(cache_key, plan.closure.fingerprint);
    hashCombine(cache_key, hashString(semantic::kReferenceCandidatePolicyVersion));
    hashCombine(cache_key, hashString(seed.stable_id));
    if (auto* cached = semantic_snapshot_cache_->find(cache_key)) {
        result.snapshot = SnapshotSelection{.snapshot = &cached->snapshot,
                                            .data = cached->data.get(),
                                            .snapshot_identity = cached->identity,
                                            .selected_document_count = plan.selected_document_count,
                                            .closure = true};
        result.scope_kind = "referenceClosure";
        last_snapshot_build_stats_.scope_kind = result.scope_kind;
        last_snapshot_build_stats_.root_uri = plan.closure.root_key;
        last_snapshot_build_stats_.snapshot_identity = cached->identity;
        last_snapshot_build_stats_.closure_confidence = plan.confidence;
        last_snapshot_build_stats_.input_document_count = documents_.size();
        last_snapshot_build_stats_.selected_document_count = plan.selected_document_count;
        last_snapshot_build_stats_.cache_hit = true;
        return result;
    }

    semantic::SnapshotBuildInput input;
    input.generation = generation_;
    input.config = config_;
    input.control.cancellation = request_control_.cancellation;
    input.control.report_progress = request_control_.report_progress;
    const std::set<std::string> selected(plan.closure.uris.begin(), plan.closure.uris.end());
    for (const auto& dirty_uri : dirtyDocumentUris()) {
        if (selected.contains(dirty_uri)) {
            input.dirty_document_uris.push_back(dirty_uri);
        }
    }
    for (const auto& selected_uri : plan.closure.uris) {
        const auto document = documents_.find(selected_uri);
        if (document == documents_.end()) {
            full_selection("reference-candidate-document-missing:" + selected_uri);
            return result;
        }
        input.documents.emplace(selected_uri, document->second);
    }

    const auto expected_generation = generation_;
    const auto expected_fingerprint = discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    auto output = semantic::SnapshotBuilder{}.build(std::move(input));
    const auto identity = snapshotIdentity("referenceClosure", generation_, plan.closure.fingerprint);
    const auto closure_reason = plan.closure.reasons.empty() ? std::string{} : plan.closure.reasons.front();
    if (output.status == semantic::SnapshotBuildStatus::Cancelled) {
        ++cancelled_snapshot_build_count_;
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        "referenceClosure",
                                                        plan.closure.root_key,
                                                        identity,
                                                        plan.confidence,
                                                        closure_reason,
                                                        plan.selected_document_count,
                                                        cancelled_snapshot_build_count_);
        last_snapshot_build_stats_.input_document_count = documents_.size();
        throw pristine::OperationCancelled{};
    }
    if (output.status != semantic::SnapshotBuildStatus::Completed) {
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        "referenceClosure",
                                                        plan.closure.root_key,
                                                        identity,
                                                        plan.confidence,
                                                        closure_reason,
                                                        plan.selected_document_count,
                                                        cancelled_snapshot_build_count_);
        last_snapshot_build_stats_.input_document_count = documents_.size();
        throw std::runtime_error(output.error.empty() ? "Reference candidate snapshot build failed"
                                                       : output.error);
    }
    const auto current_fingerprint = discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    if (generation_ != expected_generation || current_fingerprint != expected_fingerprint) {
        ++cancelled_snapshot_build_count_;
        throw pristine::OperationCancelled{};
    }

    last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                    "referenceClosure",
                                                    plan.closure.root_key,
                                                    identity,
                                                    plan.confidence,
                                                    closure_reason,
                                                    plan.selected_document_count,
                                                    cancelled_snapshot_build_count_);
    last_snapshot_build_stats_.input_document_count = documents_.size();
    semantic::SemanticSnapshotCache::Entry entry;
    entry.key = cache_key;
    entry.generation = generation_;
    entry.config_discovery_fingerprint = discovery.cache_key;
    entry.closure_fingerprint = plan.closure.fingerprint;
    entry.scope_kind = "referenceClosure";
    entry.root_uri = plan.closure.root_key;
    entry.identity = identity;
    entry.snapshot = std::move(output.snapshot);
    entry.data = std::move(output.data);
    auto& stored = semantic_snapshot_cache_->insert(std::move(entry));
    ++completed_snapshot_build_count_;
    result.snapshot = SnapshotSelection{.snapshot = &stored.snapshot,
                                        .data = stored.data.get(),
                                        .snapshot_identity = stored.identity,
                                        .selected_document_count = plan.selected_document_count,
                                        .closure = true};
    result.scope_kind = "referenceClosure";

    const auto verification_context = navigationContextFor(stored.data.get(), stored.snapshot, document_uri);
    const auto verification = semantic::lookupAt(verification_context, line, character);
    if (!verification.symbol.has_value() || verification.symbol->stable_id != seed.stable_id) {
#ifndef NDEBUG
        assert(false && "reference candidate snapshot must preserve the source-backed target identity");
#endif
        result.confidence = "candidate-target-consistency-failure";
        full_selection(result.confidence);
    }
    return result;
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->definitions(current_snapshot.generation, document_uri, line, character)) {
        return *cached;
    }
    const auto context = navigationContextFor(selection.data,
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->typeDefinitions(current_snapshot.generation,
                                                           document_uri,
                                                           line,
                                                           character)) {
        return *cached;
    }
    const auto context = navigationContextFor(selection.data,
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto reference_selection = snapshotForReferenceQuery(document_uri, line, character);
    if (reference_selection.snapshot.snapshot == nullptr || reference_selection.snapshot.data == nullptr) {
        return SemanticReferenceResult{.generation = generation_,
                                       .locations = {},
                                       .occurrences = {},
                                       .messages = {"reference snapshot is unavailable"},
                                       .scanned_occurrence_count = 0,
                                       .scanned_implementation_edge_count = 0,
                                       .candidate_document_count = reference_selection.candidate_document_count,
                                       .selected_document_count = reference_selection.selected_document_count,
                                       .snapshot_scope = reference_selection.scope_kind,
                                       .plan_confidence = reference_selection.confidence,
                                       .plan_fingerprint = reference_selection.plan_fingerprint,
                                       .unresolved = true,
                                       .truncated = false};
    }
    const auto& current_snapshot = *reference_selection.snapshot.snapshot;
    if (const auto cached = query_cache_->references(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     include_declaration,
                                                     reference_selection.plan_fingerprint,
                                                     reference_selection.target_stable_id,
                                                     reference_selection.scope_kind)) {
        return *cached;
    }

    const auto finish = [&](SemanticReferenceResult value) {
        value.candidate_document_count = reference_selection.candidate_document_count;
        value.selected_document_count = reference_selection.selected_document_count;
        value.snapshot_scope = reference_selection.scope_kind;
        value.plan_confidence = reference_selection.confidence;
        value.plan_fingerprint = reference_selection.plan_fingerprint;
        query_cache_->storeReferences(current_snapshot.generation,
                                      document_uri,
                                      line,
                                      character,
                                      include_declaration,
                                      value,
                                      reference_selection.plan_fingerprint,
                                      reference_selection.target_stable_id,
                                      reference_selection.scope_kind);
        return value;
    };
    const auto context = navigationContextFor(reference_selection.snapshot.data, current_snapshot, document_uri);
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->documentHighlights(current_snapshot.generation,
                                                              document_uri,
                                                              line,
                                                              character)) {
        return *cached;
    }
    const auto context = navigationContextFor(selection.data,
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->hover(current_snapshot.generation, document_uri, line, character)) {
        return *cached;
    }
    const auto context = navigationContextFor(selection.data,
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->prepareRename(current_snapshot.generation,
                                                         document_uri,
                                                         line,
                                                         character)) {
        return *cached;
    }
    const auto context = navigationContextFor(selection.data,
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto reference_selection = snapshotForReferenceQuery(document_uri, line, character);
    if (reference_selection.snapshot.snapshot == nullptr || reference_selection.snapshot.data == nullptr) {
        return SemanticRenameResult{.generation = generation_,
                                    .edits = {},
                                    .messages = {"rename snapshot is unavailable"},
                                    .scanned_occurrence_count = 0,
                                    .scanned_implementation_edge_count = 0,
                                    .candidate_document_count = reference_selection.candidate_document_count,
                                    .selected_document_count = reference_selection.selected_document_count,
                                    .snapshot_scope = reference_selection.scope_kind,
                                    .plan_confidence = reference_selection.confidence,
                                    .plan_fingerprint = reference_selection.plan_fingerprint,
                                    .unresolved = true,
                                    .truncated = false};
    }
    const auto& current_snapshot = *reference_selection.snapshot.snapshot;
    if (const auto cached = query_cache_->rename(current_snapshot.generation,
                                                document_uri,
                                                line,
                                                character,
                                                new_name,
                                                reference_selection.plan_fingerprint,
                                                reference_selection.target_stable_id,
                                                reference_selection.scope_kind)) {
        return *cached;
    }

    const auto context = navigationContextFor(reference_selection.snapshot.data,
                                              current_snapshot,
                                              document_uri);
    auto result = semantic::renameAt(context, line, character, new_name, kMaxSemanticLocations);
    result.candidate_document_count = reference_selection.candidate_document_count;
    result.selected_document_count = reference_selection.selected_document_count;
    result.snapshot_scope = reference_selection.scope_kind;
    result.plan_confidence = reference_selection.confidence;
    result.plan_fingerprint = reference_selection.plan_fingerprint;
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
                              result,
                              reference_selection.plan_fingerprint,
                              reference_selection.target_stable_id,
                              reference_selection.scope_kind);
    return result;
}

SemanticCompletionResult SemanticEngine::completionsAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view prefix) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri, prefix);
    const auto& current_snapshot = *selection.snapshot;
    if (const auto cached = query_cache_->completions(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     prefix,
                                                     selection.completion_plan_fingerprint)) {
        return *cached;
    }

    const auto finish = [&](SemanticCompletionResult value) {
        value.planned_workspace_candidate_count = selection.planned_workspace_candidate_count;
        value.emitted_workspace_candidate_count = value.items.size();
        value.selected_document_count = selection.selected_document_count;
        value.is_incomplete = value.is_incomplete || value.truncated ||
                             selection.workspace_completion_incomplete;
        query_cache_->storeCompletions(current_snapshot.generation,
                                       document_uri,
                                       line,
                                       character,
                                       prefix,
                                       value,
                                       selection.completion_plan_fingerprint);
        return value;
    };
    const auto* data = selection.data;
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
    auto result = semantic::completeAt(context, line, character, prefix);
    for (auto& item : result.items) {
        item.snapshot_identity = selection.snapshot_identity;
    }
    return finish(std::move(result));
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label,
                                                         std::string_view snapshot_identity) const {
    const auto build_count_before = completed_snapshot_build_count_;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&](SemanticCompletionItem value, bool identity_hit, std::string_view scope_kind) {
        last_completion_resolve_telemetry_.scope_kind = std::string(scope_kind);
        last_completion_resolve_telemetry_.lookup_micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                .count();
        last_completion_resolve_telemetry_.identity_hits = identity_hit ? 1U : 0U;
        last_completion_resolve_telemetry_.identity_misses = identity_hit ? 0U : 1U;
        last_completion_resolve_telemetry_.snapshot_build_delta =
            completed_snapshot_build_count_ - build_count_before;
        query_cache_->recordCompletionResolveIdentityLookup(identity_hit);
        return value;
    };
    if (snapshot_identity.empty()) {
        SemanticCompletionItem unresolved;
        unresolved.stable_id = std::string(stable_id);
        unresolved.label = std::string(label);
        unresolved.unresolved = true;
        return finish(std::move(unresolved), false, "missing");
    }
    const auto selection = findSnapshotForIdentity(snapshot_identity);
    if (selection.snapshot == nullptr || selection.data == nullptr) {
        SemanticCompletionItem unresolved;
        unresolved.stable_id = std::string(stable_id);
        unresolved.snapshot_identity = std::string(snapshot_identity);
        unresolved.label = std::string(label);
        unresolved.unresolved = true;
        return finish(std::move(unresolved), false, "missing");
    }
    const auto& current_snapshot = *selection.snapshot;
    const auto* data = selection.data;
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    if (data == nullptr) {
        SemanticCompletionItem unresolved;
        unresolved.stable_id = std::string(stable_id);
        unresolved.label = std::string(label);
        unresolved.unresolved = true;
        unresolved.snapshot_identity = std::string(snapshot_identity);
        return finish(std::move(unresolved), false, selection.closure ? "closure" : "full");
    }

    semantic::CompletionResolveContext context;
    context.facts_by_id = &ast_index.completion_resolve_by_id;
    query_cache_->recordCompletionResolveFactLookup(1);
    auto result = semantic::resolveCompletionItem(stable_id, label, context);
    result.snapshot_identity = selection.snapshot_identity;
    return finish(std::move(result), true, selection.closure ? "closure" : "full");
}

SemanticSignatureHelpResult SemanticEngine::signatureHelpAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
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
    const auto* data = selection.data;
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
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
    const auto* data = selection.data;
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

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri,
                                                   std::optional<ParseRange> range) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    const auto* data = selection.data;
    const auto context = navigationContextFor(data, current_snapshot, document_uri);
    auto result = semantic::semanticTokens(context, range);
    query_cache_->recordNavigationScan(0, 0, 0, result.scanned_occurrence_count, 0);
    return result;
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
    const auto* data = selection.data;
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
    const auto& discovery = workspaceDiscoveryView();
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
    const auto& discovery = workspaceDiscoveryView();
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto selection = snapshotForDocument(document_uri);
    const auto& current_snapshot = *selection.snapshot;
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
    const auto* data = selection.data;
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
    const auto expected_generation = generation_;
    const auto expected_fingerprint =
        discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    semantic::SnapshotBuildInput input;
    input.generation = generation_;
    input.config = config_;
    input.dirty_document_uris = dirtyDocumentUris();
    input.documents = documents_;
    input.control.cancellation = request_control_.cancellation;
    input.control.report_progress = request_control_.report_progress;
    auto output = semantic::SnapshotBuilder{}.build(std::move(input));
    const auto identity = snapshotIdentity("full", generation_, expected_fingerprint);

    if (output.status == semantic::SnapshotBuildStatus::Cancelled) {
        ++cancelled_snapshot_build_count_;
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        "full",
                                                        {},
                                                        identity,
                                                        "notApplicable",
                                                        {},
                                                        documents_.size(),
                                                        cancelled_snapshot_build_count_);
        throw pristine::OperationCancelled{};
    }
    if (output.status != semantic::SnapshotBuildStatus::Completed) {
        last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                        "full",
                                                        {},
                                                        identity,
                                                        "notApplicable",
                                                        {},
                                                        documents_.size(),
                                                        cancelled_snapshot_build_count_);
        throw std::runtime_error(output.error.empty() ? "Semantic snapshot build failed" : output.error);
    }
    const auto current_fingerprint =
        discoveryCacheKeyFor(generation_, workspace_root_uri_, config_, documents_);
    if (generation_ != expected_generation || current_fingerprint != expected_fingerprint) {
        ++cancelled_snapshot_build_count_;
        throw pristine::OperationCancelled{};
    }

    last_snapshot_build_stats_ = snapshotBuildStats(output,
                                                    "full",
                                                    {},
                                                    identity,
                                                    "notApplicable",
                                                    {},
                                                    documents_.size(),
                                                    cancelled_snapshot_build_count_);
    traceSnapshotBuildStats("semantic.fullSnapshot.build", last_snapshot_build_stats_);
    *affected_dependencies_ = std::move(output.affected_dependencies);
    snapshot_ = std::move(output.snapshot);
    snapshot_data_ = std::move(output.data);
    full_snapshot_identity_ = identity;
    snapshot_dirty_ = false;
    ++completed_snapshot_build_count_;
}

} // namespace pristine::analysis
