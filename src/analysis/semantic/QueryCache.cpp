#include "QueryCache.h"

#include <algorithm>

namespace pristine::analysis::semantic {

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
}

void QueryCache::setMaxEntriesPerQuery(size_t max_entries) {
    max_entries_per_query_ = max_entries;
    evictOldestEntries(workspace_symbols_by_key_);
    evictOldestEntries(references_by_key_);
    evictOldestEntries(rename_by_key_);
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
    result.diagnostics_entries = diagnostics_by_uri_.size();
    result.workspace_symbols_entries = workspace_symbols_by_key_.size();
    result.references_entries = references_by_key_.size();
    result.rename_entries = rename_by_key_.size();
    result.completions_entries = completions_by_key_.size();
    result.signature_help_entries = signature_help_by_key_.size();
    result.inlay_hints_entries = inlay_hints_by_key_.size();
    result.module_hierarchy_entries = module_hierarchy_by_key_.size();
    result.schematic_entries = schematic_by_key_.size();
    result.backward_cone_entries = backward_cone_by_key_.size();
    result.code_actions_entries = code_actions_by_key_.size();
    result.total_entries = result.diagnostics_entries + result.workspace_symbols_entries +
                           result.references_entries + result.rename_entries +
                           result.completions_entries + result.signature_help_entries +
                           result.inlay_hints_entries + result.module_hierarchy_entries +
                           result.schematic_entries + result.backward_cone_entries +
                           result.code_actions_entries;
    return result;
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
                                                              bool include_declaration) const {
    const auto found = references_by_key_.find(referencesKey(uri, line, character, include_declaration));
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
                                 SemanticReferenceResult result) {
    ReferencesEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    references_by_key_.insert_or_assign(referencesKey(uri, line, character, include_declaration),
                                        std::move(entry));
    recordStore();
    evictOldestEntries(references_by_key_);
}

std::optional<SemanticRenameResult> QueryCache::rename(std::uint64_t generation,
                                                       std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view new_name) const {
    const auto found = rename_by_key_.find(renameKey(uri, line, character, new_name));
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
                             SemanticRenameResult result) {
    RenameEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    rename_by_key_.insert_or_assign(renameKey(uri, line, character, new_name), std::move(entry));
    recordStore();
    evictOldestEntries(rename_by_key_);
}

std::optional<SemanticCompletionResult> QueryCache::completions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix) const {
    const auto found = completions_by_key_.find(completionKey(uri, line, character, prefix));
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
                                  SemanticCompletionResult result) {
    CompletionEntry entry;
    entry.generation = generation;
    entry.sequence = nextSequence();
    entry.result = std::move(result);
    completions_by_key_.insert_or_assign(completionKey(uri, line, character, prefix),
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
    std::string key(query);
    key.push_back('\x1f');
    key += std::to_string(limit);
    return key;
}

std::string QueryCache::positionKey(std::string_view uri, int line, int character) {
    std::string key(uri);
    key.push_back('\x1f');
    key += std::to_string(line);
    key.push_back(':');
    key += std::to_string(character);
    return key;
}

std::string QueryCache::referencesKey(std::string_view uri,
                                      int line,
                                      int character,
                                      bool include_declaration) {
    auto key = positionKey(uri, line, character);
    key.push_back('\x1f');
    key += include_declaration ? "1" : "0";
    return key;
}

std::string QueryCache::renameKey(std::string_view uri,
                                  int line,
                                  int character,
                                  std::string_view new_name) {
    auto key = positionKey(uri, line, character);
    key.push_back('\x1f');
    key += new_name;
    return key;
}

std::string QueryCache::completionKey(std::string_view uri,
                                      int line,
                                      int character,
                                      std::string_view prefix) {
    auto key = positionKey(uri, line, character);
    key.push_back('\x1f');
    key += prefix;
    return key;
}

std::string QueryCache::moduleQueryKey(std::optional<std::string_view> module_name, int max_depth) {
    std::string key = module_name.has_value() ? std::string(*module_name) : std::string("<inferred>");
    key.push_back('\x1f');
    key += std::to_string(max_depth);
    return key;
}

std::string QueryCache::rangeKey(std::string_view uri, ParseRange range) {
    auto key = positionKey(uri, range.start_line, range.start_character);
    key.push_back('\x1f');
    key += std::to_string(range.end_line);
    key.push_back(':');
    key += std::to_string(range.end_character);
    return key;
}

} // namespace pristine::analysis::semantic
