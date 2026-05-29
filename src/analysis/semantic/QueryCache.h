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
    void clear();

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

private:
    struct DiagnosticsEntry {
        std::uint64_t generation = 0;
        std::vector<SemanticEngineDiagnostic> diagnostics;
    };

    struct WorkspaceSymbolsEntry {
        std::uint64_t generation = 0;
        SemanticWorkspaceSymbolResult result;
    };

    [[nodiscard]] static std::string workspaceSymbolsKey(std::string_view query, size_t limit);

    std::unordered_map<std::string, DiagnosticsEntry> diagnostics_by_uri_;
    std::unordered_map<std::string, WorkspaceSymbolsEntry> workspace_symbols_by_key_;
};

} // namespace pristine::analysis::semantic
