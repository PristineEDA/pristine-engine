#include "QueryCache.h"

namespace pristine::analysis::semantic {

void QueryCache::clear() {
    diagnostics_by_uri_.clear();
    workspace_symbols_by_key_.clear();
    references_by_key_.clear();
    rename_by_key_.clear();
    completions_by_key_.clear();
    module_hierarchy_by_key_.clear();
    schematic_by_key_.clear();
    backward_cone_by_key_.clear();
    code_actions_by_key_.clear();
}

std::optional<std::vector<SemanticEngineDiagnostic>> QueryCache::diagnostics(
    std::uint64_t generation,
    std::string_view uri) const {
    const auto found = diagnostics_by_uri_.find(std::string(uri));
    if (found == diagnostics_by_uri_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.diagnostics;
}

void QueryCache::storeDiagnostics(std::uint64_t generation,
                                  std::string_view uri,
                                  std::vector<SemanticEngineDiagnostic> diagnostics) {
    diagnostics_by_uri_.insert_or_assign(std::string(uri),
                                         DiagnosticsEntry{.generation = generation,
                                                          .diagnostics = std::move(diagnostics)});
}

std::optional<SemanticWorkspaceSymbolResult> QueryCache::workspaceSymbols(
    std::uint64_t generation,
    std::string_view query,
    size_t limit) const {
    const auto found = workspace_symbols_by_key_.find(workspaceSymbolsKey(query, limit));
    if (found == workspace_symbols_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeWorkspaceSymbols(std::uint64_t generation,
                                       std::string_view query,
                                       size_t limit,
                                       SemanticWorkspaceSymbolResult result) {
    workspace_symbols_by_key_.insert_or_assign(workspaceSymbolsKey(query, limit),
                                               WorkspaceSymbolsEntry{.generation = generation,
                                                                     .result = std::move(result)});
}

std::optional<SemanticReferenceResult> QueryCache::references(std::uint64_t generation,
                                                              std::string_view uri,
                                                              int line,
                                                              int character,
                                                              bool include_declaration) const {
    const auto found = references_by_key_.find(referencesKey(uri, line, character, include_declaration));
    if (found == references_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeReferences(std::uint64_t generation,
                                 std::string_view uri,
                                 int line,
                                 int character,
                                 bool include_declaration,
                                 SemanticReferenceResult result) {
    references_by_key_.insert_or_assign(referencesKey(uri, line, character, include_declaration),
                                        ReferencesEntry{.generation = generation,
                                                        .result = std::move(result)});
}

std::optional<SemanticRenameResult> QueryCache::rename(std::uint64_t generation,
                                                       std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view new_name) const {
    const auto found = rename_by_key_.find(renameKey(uri, line, character, new_name));
    if (found == rename_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeRename(std::uint64_t generation,
                             std::string_view uri,
                             int line,
                             int character,
                             std::string_view new_name,
                             SemanticRenameResult result) {
    rename_by_key_.insert_or_assign(renameKey(uri, line, character, new_name),
                                    RenameEntry{.generation = generation,
                                                .result = std::move(result)});
}

std::optional<SemanticCompletionResult> QueryCache::completions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix) const {
    const auto found = completions_by_key_.find(completionKey(uri, line, character, prefix));
    if (found == completions_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeCompletions(std::uint64_t generation,
                                  std::string_view uri,
                                  int line,
                                  int character,
                                  std::string_view prefix,
                                  SemanticCompletionResult result) {
    completions_by_key_.insert_or_assign(completionKey(uri, line, character, prefix),
                                         CompletionEntry{.generation = generation,
                                                         .result = std::move(result)});
}

std::optional<SemanticModuleHierarchyResult> QueryCache::moduleHierarchy(
    std::uint64_t generation,
    std::optional<std::string_view> module_name,
    int max_depth) const {
    const auto found = module_hierarchy_by_key_.find(moduleQueryKey(module_name, max_depth));
    if (found == module_hierarchy_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeModuleHierarchy(std::uint64_t generation,
                                      std::optional<std::string_view> module_name,
                                      int max_depth,
                                      SemanticModuleHierarchyResult result) {
    module_hierarchy_by_key_.insert_or_assign(moduleQueryKey(module_name, max_depth),
                                              ModuleHierarchyEntry{.generation = generation,
                                                                   .result = std::move(result)});
}

std::optional<SemanticSchematicResult> QueryCache::schematic(std::uint64_t generation,
                                                             std::optional<std::string_view> module_name,
                                                             int max_depth) const {
    const auto found = schematic_by_key_.find(moduleQueryKey(module_name, max_depth));
    if (found == schematic_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeSchematic(std::uint64_t generation,
                                std::optional<std::string_view> module_name,
                                int max_depth,
                                SemanticSchematicResult result) {
    schematic_by_key_.insert_or_assign(moduleQueryKey(module_name, max_depth),
                                       SchematicEntry{.generation = generation,
                                                      .result = std::move(result)});
}

std::optional<SemanticConeTrace> QueryCache::backwardCone(std::uint64_t generation,
                                                          std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto found = backward_cone_by_key_.find(positionKey(uri, line, character));
    if (found == backward_cone_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeBackwardCone(std::uint64_t generation,
                                   std::string_view uri,
                                   int line,
                                   int character,
                                   SemanticConeTrace result) {
    backward_cone_by_key_.insert_or_assign(positionKey(uri, line, character),
                                           BackwardConeEntry{.generation = generation,
                                                             .result = std::move(result)});
}

std::optional<SemanticCodeActionResult> QueryCache::codeActions(std::uint64_t generation,
                                                                std::string_view uri,
                                                                ParseRange range) const {
    const auto found = code_actions_by_key_.find(rangeKey(uri, range));
    if (found == code_actions_by_key_.end() || found->second.generation != generation) {
        return std::nullopt;
    }
    return found->second.result;
}

void QueryCache::storeCodeActions(std::uint64_t generation,
                                  std::string_view uri,
                                  ParseRange range,
                                  SemanticCodeActionResult result) {
    code_actions_by_key_.insert_or_assign(rangeKey(uri, range),
                                          CodeActionsEntry{.generation = generation,
                                                           .result = std::move(result)});
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
