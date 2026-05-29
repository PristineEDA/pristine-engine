#include "QueryCache.h"

namespace pristine::analysis::semantic {

void QueryCache::clear() {
    diagnostics_by_uri_.clear();
    workspace_symbols_by_key_.clear();
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

std::string QueryCache::workspaceSymbolsKey(std::string_view query, size_t limit) {
    std::string key(query);
    key.push_back('\x1f');
    key += std::to_string(limit);
    return key;
}

} // namespace pristine::analysis::semantic
