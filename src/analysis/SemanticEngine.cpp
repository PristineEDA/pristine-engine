#include "pristine/analysis/SemanticEngine.h"

#include "semantic/AstIndex.h"
#include "semantic/CodeActionProvider.h"
#include "semantic/CompletionProvider.h"
#include "semantic/DesignGraphProvider.h"
#include "semantic/DiagnosticProvider.h"
#include "semantic/NavigationProvider.h"
#include "semantic/QueryCache.h"
#include "semantic/SignatureInlayProvider.h"
#include "semantic/SnapshotBuilder.h"
#include "semantic/WorkspaceDiscoveryIndex.h"
#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/Symbol.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace pristine::analysis {
namespace {

bool containsPosition(const ParseRange& range, int line, int character) {
    if (line < range.start_line || line > range.end_line) {
        return false;
    }
    if (line == range.start_line && character < range.start_character) {
        return false;
    }
    if (line == range.end_line && character >= range.end_character) {
        return false;
    }
    return true;
}

bool locationLess(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    if (lhs.uri != rhs.uri) {
        return lhs.uri < rhs.uri;
    }
    if (lhs.range.start_line != rhs.range.start_line) {
        return lhs.range.start_line < rhs.range.start_line;
    }
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.range.end_line != rhs.range.end_line) {
        return lhs.range.end_line < rhs.range.end_line;
    }
    return lhs.range.end_character < rhs.range.end_character;
}

bool sameLocation(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    return lhs.uri == rhs.uri && lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line && lhs.range.end_character == rhs.range.end_character;
}

constexpr size_t kMaxSemanticLocations = 2000;

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
    context.assignment_edges_by_uri = ast_index.assignment_edges_by_uri;
    context.symbols_by_id = ast_index.design_graph_symbols_by_id;
    context.symbol_ranges_by_uri = ast_index.design_graph_symbol_ranges_by_uri;
    return context;
}

template<typename SnapshotData>
semantic::NavigationContext navigationContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 const semantic::AstIndexView& ast_index,
                                                 const std::string* document_text = nullptr) {
    semantic::NavigationContext context;
    context.generation = snapshot.generation;
    context.snapshot_available = data != nullptr;
    context.document_uri = std::move(document_uri);
    context.document_text = document_text;
    if (data == nullptr) {
        return context;
    }
    context.symbols_by_id = ast_index.navigation_symbols_by_id;
    context.references = ast_index.navigation_references;
    if (const auto ranges_it = data->selection_ranges_by_uri.find(context.document_uri);
        ranges_it != data->selection_ranges_by_uri.end()) {
        context.selection_ranges = ranges_it->second;
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
        context.symbols_by_id = ast_index.diagnostic_symbols_by_id;
        context.package_imports_by_uri = ast_index.package_imports_by_uri;
    }
    return context;
}

} // namespace

namespace {

template<typename SnapshotData>
void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const SnapshotData& data,
                                 const ModuleDefinition& module,
                                 std::string_view module_uri,
                                 std::string_view prefix,
                                 const std::set<std::string>& excluded_ports,
                                 bool& truncated) {
    const auto module_id = semantic::findSymbolIdByNameAndKind(data, module.name, "Definition")
                               .value_or(std::string("module|") + module.name);
    semantic::appendModulePortCompletions(items,
                                          emitted,
                                          module_id,
                                          module,
                                          module_uri,
                                          prefix,
                                          excluded_ports,
                                          truncated);
}

template<typename Map>
std::optional<std::string> firstUninstantiatedModuleName(const Map& modules_by_name) {
    std::set<std::string> instantiated;
    for (const auto& [_, module] : modules_by_name) {
        for (const auto& instance : module.instances) {
            instantiated.insert(instance.module_name);
        }
    }
    for (const auto& [name, _] : modules_by_name) {
        if (!instantiated.contains(name)) {
            return name;
        }
    }
    if (!modules_by_name.empty()) {
        return modules_by_name.begin()->first;
    }
    return std::nullopt;
}

std::optional<std::string_view> inferredDiscoveryTop(
    std::optional<std::string_view> requested_module_name,
    const SemanticWorkspaceDiscoverySnapshot& discovery) {
    if (requested_module_name.has_value() && !requested_module_name->empty()) {
        return requested_module_name;
    }
    if (discovery.top_candidates.size() == 1) {
        return std::string_view(discovery.top_candidates.front());
    }
    return std::nullopt;
}

const SemanticDiscoveryClosureMetric* discoveryClosureMetricFor(
    std::optional<std::string_view> requested_module_name,
    const SemanticWorkspaceDiscoverySnapshot& discovery) {
    const auto selected_top = inferredDiscoveryTop(requested_module_name, discovery);
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
    if (const auto selected_top = inferredDiscoveryTop(module_name, discovery);
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

SemanticEngine::SemanticEngine() : query_cache_(std::make_unique<semantic::QueryCache>()) {}

SemanticEngine::~SemanticEngine() = default;

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    includes_.clear();
    reverse_includes_.clear();
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
        includes_[document_uri] = {};
        for (auto& [_, including_uris] : reverse_includes_) {
            including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                                 including_uris.end());
        }
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
    includes_.erase(document_uri);
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                             including_uris.end());
    }
    reverse_includes_.erase(document_uri);
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

std::vector<std::string> SemanticEngine::includedUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = includes_.find(document_uri);
    if (include_it == includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> SemanticEngine::includingUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = reverse_includes_.find(document_uri);
    if (include_it == reverse_includes_.end()) {
        return {};
    }
    return include_it->second;
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
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::vector<std::string> pending{document_uri};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        result.push_back(current);
        const auto reverse_it = reverse_includes_.find(current);
        if (reverse_it == reverse_includes_.end()) {
            continue;
        }
        pending.insert(pending.end(), reverse_it->second.begin(), reverse_it->second.end());
    }
    std::sort(result.begin(), result.end());
    return result;
}

const SemanticEngineSnapshot& SemanticEngine::snapshot() const {
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
    query_cache_->storeDiagnostics(current_snapshot.generation, document_uri, result);
    return result;
}

const semantic::SnapshotData* SemanticEngine::snapshotData() const {
    (void)snapshot();
    return snapshot_data_.get();
}

std::vector<std::string> SemanticEngine::closureDocumentUrisFor(
    std::optional<std::string_view> module_name,
    const SemanticWorkspaceDiscoverySnapshot& discovery) const {
    const auto selected_top = inferredDiscoveryTop(module_name, discovery);
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
    SemanticLookupResult result;
    result.mode = current_snapshot.mode;
    result.generation = current_snapshot.generation;
    result.query_location = SemanticLocation{.uri = document_uri,
                                             .range = ParseRange{.start_line = line,
                                                                 .start_character = character,
                                                                 .end_line = line,
                                                                 .end_character = character}};
    result.unresolved = true;
    const auto* data = snapshotData();
    if (data == nullptr || !data->compilation) {
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    if (const auto instance = semantic::moduleInstanceAt(*data, document_uri, line, character)) {
        const auto target_id = !instance->target_stable_id.empty()
                                   ? std::optional<std::string>{instance->target_stable_id}
                                   : semantic::findDefinitionSymbolId(*data, instance->module_name);
        if (target_id.has_value()) {
            const auto symbol_it = data->symbols_by_id.find(*target_id);
            if (symbol_it != data->symbols_by_id.end()) {
                result.query_location = SemanticLocation{.uri = instance->uri,
                                                         .range = instance->module_selection_range};
                result.symbol = symbol_it->second.identity;
                result.unresolved = false;
                return result;
            }
        }
        result.messages.push_back("module instance target is not indexed");
        return result;
    }

    const auto id = semantic::symbolIdAtLocation(*data, document_uri, line, character);
    if (!id.has_value()) {
        result.messages.push_back("no AST symbol at position");
        return result;
    }

    const auto symbol_it = data->symbols_by_id.find(*id);
    if (symbol_it == data->symbols_by_id.end()) {
        result.messages.push_back("AST symbol identity is not indexed");
        return result;
    }

    const auto reference_it = std::find_if(data->references.begin(),
                                           data->references.end(),
                                           [&](const semantic::SnapshotIndexedReference& reference) {
                                               return reference.stable_id == *id &&
                                                      reference.location.uri == document_uri &&
                                                      containsPosition(reference.location.range, line, character);
                                           });
    if (reference_it != data->references.end()) {
        result.query_location = reference_it->location;
    }
    else {
        result.query_location = symbol_it->second.identity.location;
    }
    result.symbol = symbol_it->second.identity;
    result.unresolved = false;
    return result;
}

SemanticReferenceResult SemanticEngine::definitionsAt(std::string_view uri,
                                                      int line,
                                                      int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (lookup.symbol.has_value()) {
        result.locations.push_back(lookup.symbol->location);
    }
    return result;
}

SemanticReferenceResult SemanticEngine::typeDefinitionsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;

    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
        const auto ast_index = semantic::buildAstIndexView(data, lookup.generation);
        auto locations = semantic::typeDefinitionLocationsAt(ast_index, document_uri, line, character);
        if (!locations.empty()) {
            result.locations = std::move(locations);
            result.unresolved = false;
            return result;
        }
    }

    if (!lookup.symbol.has_value()) {
        return result;
    }

    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && symbol_it->second.symbol != nullptr) {
            const auto* declared_type = symbol_it->second.symbol->getDeclaredType();
            if (declared_type != nullptr) {
                const auto& type = declared_type->getType();
                if (const auto type_location = semantic::declarationLocationForSymbol(*data->source_manager, type)) {
                    result.locations.push_back(*type_location);
                    return result;
                }
            }
        }
    }

    result.locations.push_back(lookup.symbol->location);
    result.messages.push_back("type definition resolved to declaration because the type has no source location");
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

    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    const auto finish = [&](SemanticReferenceResult value) {
        query_cache_->storeReferences(current_snapshot.generation,
                                      document_uri,
                                      line,
                                      character,
                                      include_declaration,
                                      value);
        return value;
    };
    if (!lookup.symbol.has_value()) {
        return finish(std::move(result));
    }

    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return finish(std::move(result));
    }

    if (const auto instance = semantic::moduleInstanceAt(*data, document_uri, line, character)) {
        result.locations = semantic::moduleImplementationLocations(*data,
                                                                   instance->module_name,
                                                                   kMaxSemanticLocations,
                                                                   result.truncated);
        if (include_declaration) {
            const auto target_id = !instance->target_stable_id.empty()
                                       ? std::optional<std::string>{instance->target_stable_id}
                                       : semantic::findDefinitionSymbolId(*data, instance->module_name);
            if (target_id.has_value()) {
                const auto target_it = data->symbols_by_id.find(*target_id);
                if (target_it != data->symbols_by_id.end()) {
                    result.locations.push_back(target_it->second.identity.location);
                }
            }
        }
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
        result.unresolved = false;
        return finish(std::move(result));
    }

    result.locations = semantic::locationsForSymbol(*data,
                                                    lookup.symbol->stable_id,
                                                    include_declaration,
                                                    kMaxSemanticLocations,
                                                    result.truncated);
    if (lookup.symbol->kind == "Definition") {
        bool implementation_truncated = false;
        auto implementations = semantic::moduleImplementationLocations(*data,
                                                                       lookup.symbol->name,
                                                                       kMaxSemanticLocations,
                                                                       implementation_truncated);
        for (auto& location : implementations) {
            if (!include_declaration && sameLocation(location, lookup.query_location)) {
                continue;
            }
            if (result.locations.size() >= kMaxSemanticLocations) {
                result.truncated = true;
                break;
            }
            result.locations.push_back(std::move(location));
        }
        result.truncated = result.truncated || implementation_truncated;
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
    }
    return finish(std::move(result));
}

SemanticReferenceResult SemanticEngine::documentHighlightsAt(std::string_view uri,
                                                              int line,
                                                              int character) const {
    auto result = referencesAt(uri, line, character, true);
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    result.locations.erase(std::remove_if(result.locations.begin(),
                                          result.locations.end(),
                                          [&](const SemanticLocation& location) {
                                              return location.uri != document_uri;
                                          }),
                           result.locations.end());
    return result;
}

SemanticReferenceResult SemanticEngine::implementationsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (!lookup.symbol.has_value()) {
        return result;
    }
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    result.locations = semantic::moduleImplementationLocations(*data,
                                                               lookup.symbol->name,
                                                               kMaxSemanticLocations,
                                                               result.truncated);
    result.unresolved = false;
    return result;
}

SemanticHoverResult SemanticEngine::hoverAt(std::string_view uri, int line, int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticHoverResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (!lookup.symbol.has_value()) {
        return result;
    }

    result.contents = "**" + lookup.symbol->kind + "** `" + lookup.symbol->name + "`";
    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && !symbol_it->second.type_display.empty()) {
            result.contents += "\n\nType: `" + symbol_it->second.type_display + "`";
        }
    }
    result.range = lookup.query_location.range;
    return result;
}

SemanticPrepareRenameResult SemanticEngine::prepareRenameAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticPrepareRenameResult result;
    result.generation = lookup.generation;
    result.messages = lookup.messages;
    result.unresolved = lookup.unresolved;
    if (!lookup.symbol.has_value()) {
        return result;
    }
    result.placeholder = lookup.symbol->name;
    result.range = lookup.query_location.range;
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

    const auto references = referencesAt(uri, line, character, true);
    SemanticRenameResult result;
    result.generation = references.generation;
    result.messages = references.messages;
    result.unresolved = references.unresolved;
    result.truncated = references.truncated;
    for (const auto& location : references.locations) {
        result.edits.push_back(SemanticTextEdit{.location = location,
                                                .new_text = std::string(new_name)});
    }
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

    SemanticCompletionResult result;
    result.generation = current_snapshot.generation;
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
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return finish(std::move(result));
    }

    std::set<std::string> emitted;
    const auto document_it = documents_.find(document_uri);
    const auto* document = document_it == documents_.end() ? nullptr : &document_it->second;
    const auto completion_context = document == nullptr
                                        ? semantic::CompletionContext{}
                                        : semantic::detectCompletionContext(document->text,
                                                                            line,
                                                                            character,
                                                                            prefix);

    const auto append_items = [&](const std::vector<SemanticCompletionItem>& items) {
        for (const auto& item : items) {
            semantic::appendCompletionItem(result.items, emitted, item, prefix, result.truncated);
            if (result.truncated) {
                return;
            }
        }
    };

    if (document != nullptr && completion_context.macro_invocation) {
        const auto append_macros = [&](const std::vector<MacroDefinition>& macros) {
            for (const auto& macro : macros) {
                semantic::appendCompletionItem(
                    result.items,
                    emitted,
                    SemanticCompletionItem{.stable_id = document_uri + "|macro|" + macro.name,
                                            .label = macro.name,
                                            .detail = macro.function_like ? "Macro function" : "Macro",
                                            .documentation = semantic::macroDocumentation(macro),
                                            .insert_text = semantic::macroInsertText(macro),
                                            .kind = macro.function_like ? 3 : 21,
                                            .unresolved = false},
                    prefix,
                    result.truncated);
                if (result.truncated) {
                    return;
                }
            }
        };
        if (const auto macros_it = data->macros_by_uri.find(document_uri); macros_it != data->macros_by_uri.end()) {
            append_macros(macros_it->second);
        }
        for (const auto& [macro_uri, macros] : data->macros_by_uri) {
            if (macro_uri != document_uri) {
                append_macros(macros);
            }
            if (result.truncated) {
                break;
            }
        }
        return finish(std::move(result));
    }

    if (document != nullptr) {
        if (const auto package_name = completion_context.package_qualifier) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                const auto& symbol = indexed_symbol.identity;
                if (symbol.name == *package_name || symbol.location.uri.empty()) {
                    continue;
                }
                if (symbol.stable_id.find("|" + *package_name + "::") == std::string::npos &&
                    symbol.stable_id.find("." + *package_name + ".") == std::string::npos &&
                    symbol.stable_id.find(*package_name) == std::string::npos) {
                    continue;
                }
                semantic::appendSymbolCompletion(result.items,
                                                 emitted,
                                                 indexed_symbol.identity,
                                                 prefix,
                                                 result.truncated);
                if (result.truncated) {
                    return finish(std::move(result));
                }
            }
            if (result.items.empty()) {
                result.messages.push_back("package completion had no indexed AST members");
            }
            return finish(std::move(result));
        }

        if (completion_context.member_access) {
            const auto instances_it = data->module_instances_by_uri.find(document_uri);
            if (instances_it != data->module_instances_by_uri.end()) {
                for (const auto& instance : instances_it->second) {
                    if (!parseRangeContainsPosition(instance.range, line, character)) {
                        continue;
                    }
                    const auto module_it = data->modules_by_name.find(instance.module_name);
                    if (module_it != data->modules_by_name.end()) {
                        std::set<std::string> connected_ports;
                        connected_ports = semantic::connectedNamedPortsForInstance(
                            document->text,
                            line,
                            character,
                            instance.selection_range,
                            instance.range);
                        const auto module_uri_it = data->module_uris_by_name.find(instance.module_name);
                        appendModulePortCompletions(result.items,
                                                    emitted,
                                                    *data,
                                                    module_it->second,
                                                    module_uri_it == data->module_uris_by_name.end()
                                                        ? std::string_view{}
                                                        : std::string_view(module_uri_it->second),
                                                    prefix,
                                                    connected_ports,
                                                    result.truncated);
                        return finish(std::move(result));
                    }
                }
            }
        }

        if (const auto member_name = completion_context.member_qualifier) {
            semantic::CompletionMemberContext member_context;
            member_context.qualifier = *member_name;
            const auto qualifier_base = semantic::baseMemberQualifier(*member_name);
            bool used_typed_member_view = false;
            const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
            if (const auto completions_it = ast_index.member_completions_by_uri.find(document_uri);
                completions_it != ast_index.member_completions_by_uri.end()) {
                for (const auto& member_completion : completions_it->second) {
                    if (member_completion.qualifier != qualifier_base) {
                        continue;
                    }
                    member_context.candidates.push_back(
                        semantic::CompletionMemberCandidate{.identity = member_completion.identity});
                }
            }
            if (!member_context.candidates.empty()) {
                used_typed_member_view = true;
            }
            else {
                member_context.candidates.reserve(data->symbols_by_id.size());
                for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                    member_context.candidates.push_back(
                        semantic::CompletionMemberCandidate{.identity = indexed_symbol.identity});
                }
            }
            semantic::appendMemberCompletions(result.items,
                                              emitted,
                                              member_context,
                                              prefix,
                                              result.truncated);
            result.messages.push_back(used_typed_member_view
                                          ? "member completion used typed AstIndex member view"
                                          : "member completion used AST-backed member context provider");
            return finish(std::move(result));
        }

        if (completion_context.module_instantiation_position) {
            std::vector<std::string> module_names;
            module_names.reserve(data->modules_by_name.size());
            for (const auto& [module_name, _] : data->modules_by_name) {
                module_names.push_back(module_name);
            }
            std::sort(module_names.begin(), module_names.end());
            for (const auto& module_name : module_names) {
                const auto& module = data->modules_by_name.at(module_name);
                const auto module_id = semantic::findSymbolIdByNameAndKind(*data, module.name, "Definition")
                                           .value_or(std::string("module|") + module.name);
                const auto module_uri_it = data->module_uris_by_name.find(module.name);
                semantic::appendCompletionItem(
                    result.items,
                    emitted,
                    SemanticCompletionItem{.stable_id = module_id,
                                            .label = module.name,
                                            .detail = semantic::moduleSignatureLabel(module),
                                            .documentation = semantic::moduleDocumentation(
                                                module,
                                                module_uri_it == data->module_uris_by_name.end()
                                                    ? std::string_view{}
                                                    : std::string_view(module_uri_it->second)),
                                            .insert_text = module.name,
                                            .kind = 9,
                                            .unresolved = false},
                    prefix,
                    result.truncated);
                if (result.truncated) {
                    return finish(std::move(result));
                }
            }
        }
    }

    const auto completion_it = data->completions_by_uri.find(document_uri);
    if (completion_it != data->completions_by_uri.end()) {
        append_items(completion_it->second);
    }
    for (const auto& [completion_uri, items] : data->completions_by_uri) {
        if (completion_uri == document_uri) {
            continue;
        }
        append_items(items);
        if (result.truncated) {
            break;
        }
    }
    return finish(std::move(result));
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
    context.modules_by_name = &ast_index.modules_by_name;
    context.module_uris_by_name = &ast_index.module_uris_by_name;
    context.macros_by_uri = &ast_index.macros_by_uri;

    const auto symbol_it = data->symbols_by_id.find(std::string(stable_id));
    if (symbol_it != data->symbols_by_id.end()) {
        context.symbol = semantic::CompletionResolveSymbol{.identity = symbol_it->second.identity,
                                                           .type_display = symbol_it->second.type_display};
    }
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
    const auto document_it = documents_.find(document_uri);

    semantic::SignatureInlayContext context;
    context.generation = current_snapshot.generation;
    context.document_uri = document_uri;
    context.document_text = document_it == documents_.end() ? nullptr : &document_it->second.text;
    context.modules_by_name = data == nullptr ? nullptr : &ast_index.modules_by_name;
    context.snapshot_available = data != nullptr;
    if (data != nullptr) {
        const auto instances_it = ast_index.signature_module_instances_by_uri.find(document_uri);
        if (instances_it != ast_index.signature_module_instances_by_uri.end()) {
            context.module_instances = instances_it->second;
        }
        const auto macros_it = ast_index.macros_by_uri.find(document_uri);
        if (macros_it != ast_index.macros_by_uri.end()) {
            context.macros = macros_it->second;
        }
        const auto calls_it = ast_index.signature_calls_by_uri.find(document_uri);
        if (calls_it != ast_index.signature_calls_by_uri.end()) {
            context.calls = calls_it->second;
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
    const auto document_it = documents_.find(document_uri);
    context.document_text = document_it == documents_.end() ? nullptr : &document_it->second.text;
    context.modules_by_name = data == nullptr ? nullptr : &ast_index.modules_by_name;
    context.snapshot_available = data != nullptr;
    if (data != nullptr) {
        context.symbols.reserve(data->symbols_by_id.size());
        for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
            context.symbols.push_back(semantic::SignatureInlaySymbol{
                .identity = indexed_symbol.identity,
                .type_display = indexed_symbol.type_display});
        }
    }
    if (data != nullptr) {
        const auto instances_it = ast_index.signature_module_instances_by_uri.find(document_uri);
        if (instances_it != ast_index.signature_module_instances_by_uri.end()) {
            context.module_instances = instances_it->second;
        }
        const auto calls_it = ast_index.signature_calls_by_uri.find(document_uri);
        if (calls_it != ast_index.signature_calls_by_uri.end()) {
            context.calls = calls_it->second;
        }
    }
    return finish(semantic::inlayHints(context, range));
}

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = navigationContextFor(data, current_snapshot, document_uri, ast_index);
    return semantic::semanticTokens(context);
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto lookup = lookupAt(uri, line, character);
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, lookup.generation);
    const auto document_it = documents_.find(document_uri);
    auto context = navigationContextFor(data,
                                        snapshot(),
                                        document_uri,
                                        ast_index,
                                        document_it == documents_.end() ? nullptr : &document_it->second.text);
    return semantic::selectionRangesAt(context, lookup, line, character);
}

SemanticModuleHierarchyResult SemanticEngine::moduleHierarchy(std::optional<std::string_view> module_name,
                                                              int max_depth) const {
    const auto discovery = workspaceDiscovery();
    if (const auto cached = query_cache_->moduleHierarchy(generation_,
                                                          module_name,
                                                          max_depth)) {
        auto result = *cached;
        if (result.discovery_closure_used) {
            result.discovery_closure_cache_hit = true;
        }
        return result;
    }
    const auto closure_uris = closureDocumentUrisFor(module_name, discovery);
    const auto* closure_metric = discoveryClosureMetricFor(module_name, discovery);
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
        return result;
    }

    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->moduleHierarchy(current_snapshot.generation,
                                                          module_name,
                                                          max_depth)) {
        return *cached;
    }

    SemanticModuleHierarchyResult result;
    result.generation = current_snapshot.generation;
    const auto finish = [&](SemanticModuleHierarchyResult value) {
        query_cache_->storeModuleHierarchy(current_snapshot.generation,
                                           module_name,
                                           max_depth,
                                           value);
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
    const auto discovery = workspaceDiscovery();
    if (const auto cached = query_cache_->schematic(generation_, module_name, max_depth)) {
        auto result = *cached;
        if (result.discovery_closure_used) {
            result.discovery_closure_cache_hit = true;
        }
        return result;
    }
    const auto closure_uris = closureDocumentUrisFor(module_name, discovery);
    const auto* closure_metric = discoveryClosureMetricFor(module_name, discovery);
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
        return result;
    }

    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->schematic(current_snapshot.generation, module_name, max_depth)) {
        return *cached;
    }

    SemanticSchematicResult result;
    result.generation = current_snapshot.generation;
    const auto finish = [&](SemanticSchematicResult value) {
        query_cache_->storeSchematic(current_snapshot.generation, module_name, max_depth, value);
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
    return semantic::prepareCallHierarchy(context, document_uri, line, character);
}

SemanticCallHierarchyCallsResult SemanticEngine::incomingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    return semantic::incomingCalls(context, item);
}

SemanticCallHierarchyCallsResult SemanticEngine::outgoingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    const auto ast_index = semantic::buildAstIndexView(data, current_snapshot.generation);
    auto context = designGraphContextFor(data, current_snapshot, config_, ast_index);
    return semantic::outgoingCalls(context, item);
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
        return *cached;
    }

    const auto lookup = lookupAt(uri, line, character);
    SemanticConeTrace trace;
    trace.generation = lookup.generation;
    trace.messages = lookup.messages;
    trace.unresolved = lookup.unresolved;
    const auto finish = [&](SemanticConeTrace value) {
        query_cache_->storeBackwardCone(current_snapshot.generation,
                                        document_uri,
                                        line,
                                        character,
                                        value);
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
    trace = semantic::backwardCone(context, document_uri, lookup, kMaxSemanticLocations);
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
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), normalized_uri),
                             including_uris.end());
    }

    CompilationService compilation_service;
    std::vector<std::string> included_uris;
    for (const auto& include : compilation_service.includeDirectives(text)) {
        included_uris.push_back(joinFileUri(uriDirectory(normalized_uri), include.target));
    }
    std::sort(included_uris.begin(), included_uris.end());
    included_uris.erase(std::unique(included_uris.begin(), included_uris.end()), included_uris.end());
    includes_[normalized_uri] = included_uris;

    for (const auto& included_uri : included_uris) {
        auto& including_uris = reverse_includes_[included_uri];
        including_uris.push_back(normalized_uri);
        std::sort(including_uris.begin(), including_uris.end());
        including_uris.erase(std::unique(including_uris.begin(), including_uris.end()), including_uris.end());
    }
}

void SemanticEngine::rebuildSnapshot() const {
    semantic::SnapshotBuildInput input;
    input.generation = generation_;
    input.config = config_;
    input.dirty_document_uris = dirtyDocumentUris();
    input.documents = documents_;
    auto output = semantic::SnapshotBuilder{}.build(std::move(input));

    includes_ = std::move(output.includes);
    reverse_includes_ = std::move(output.reverse_includes);
    snapshot_ = std::move(output.snapshot);
    snapshot_data_ = std::move(output.data);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
