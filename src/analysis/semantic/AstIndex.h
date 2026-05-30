#pragma once

#include "DesignGraphProvider.h"
#include "DiagnosticProvider.h"
#include "NavigationProvider.h"
#include "CompletionProvider.h"
#include "SignatureInlayProvider.h"
#include "SnapshotBuilder.h"
#include "pristine/analysis/SemanticEngine.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct AstIndexSymbol {
    std::string stable_id;
    SemanticSymbolIdentity identity;
};

struct AstIndexContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::vector<AstIndexSymbol> symbols;
};

struct AstIndexView {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::vector<AstIndexSymbol> symbols;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> module_signatures_by_name;
    std::vector<DesignGraphModuleEntry> design_graph_module_entries;
    std::unordered_map<std::string, std::vector<SnapshotAssignmentEdge>> assignment_edges_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotTypeReference>> type_references_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlayModuleInstance>> signature_module_instances_by_uri;
    std::vector<NavigationReference> navigation_references;
    std::unordered_map<std::string, SemanticSymbolIdentity> navigation_symbols_by_id;
    std::unordered_map<std::string, DiagnosticSymbol> diagnostic_symbols_by_id;
    std::vector<DiagnosticReference> diagnostic_references;
    std::unordered_map<std::string, DesignGraphSymbol> design_graph_symbols_by_id;
    std::unordered_map<std::string, std::vector<DesignGraphRangeSymbol>> design_graph_symbol_ranges_by_uri;
};

[[nodiscard]] constexpr std::string_view astIndexProviderName() {
    return "AstIndex";
}

void buildAstIndexes(SnapshotData& data,
                     const std::unordered_map<std::string, SemanticEngineDocument>& documents);

[[nodiscard]] AstIndexView buildAstIndexView(const SnapshotData* data,
                                             std::uint64_t generation);

[[nodiscard]] std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
                                                                std::string_view name);

[[nodiscard]] std::optional<std::string> findSymbolIdByNameAndKind(const SnapshotData& data,
                                                                   std::string_view name,
                                                                   std::string_view kind);

[[nodiscard]] std::vector<SemanticLocation> typeDefinitionLocationsByName(const SnapshotData& data,
                                                                          std::string_view name);

[[nodiscard]] std::vector<SemanticLocation> typeDefinitionLocationsAt(const AstIndexView& view,
                                                                      std::string_view uri,
                                                                      int line,
                                                                      int character);

[[nodiscard]] std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                                            std::string_view uri,
                                                            int line,
                                                            int character);

[[nodiscard]] std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                               std::string_view stable_id,
                                                               bool include_declaration,
                                                               size_t max_locations,
                                                               bool& truncated);

[[nodiscard]] std::vector<SemanticLocation> moduleImplementationLocations(const SnapshotData& data,
                                                                          std::string_view module_name,
                                                                          size_t max_locations,
                                                                          bool& truncated);

[[nodiscard]] std::optional<SnapshotModuleInstance> moduleInstanceAt(const SnapshotData& data,
                                                                     std::string_view uri,
                                                                     int line,
                                                                     int character);

[[nodiscard]] std::optional<SemanticLocation> declarationLocationForSymbol(
    const slang::SourceManager& source_manager,
    const slang::ast::Symbol& symbol);

[[nodiscard]] AstIndexContext workspaceSymbolContext(const AstIndexView& view);

[[nodiscard]] SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                                             std::string_view query,
                                                             size_t limit);

} // namespace pristine::analysis::semantic
