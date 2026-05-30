#pragma once

#include "SnapshotBuilder.h"
#include "pristine/analysis/SemanticEngine.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct DiagnosticSymbol {
    SemanticSymbolIdentity identity;
    std::string type_display;
};

struct DiagnosticReference {
    std::string stable_id;
    SemanticLocation location;
};

struct DiagnosticContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::string workspace_root_uri;
    SemanticEngineDocument document;
    std::vector<SemanticEngineDiagnostic> snapshot_diagnostics;
    std::unordered_map<std::string, DiagnosticSymbol> symbols_by_id;
    std::vector<DiagnosticReference> references;
    std::unordered_map<std::string, std::vector<ContinuousAssignment>> assignments_by_uri;
    std::unordered_map<std::string, std::vector<Identifier>> identifiers_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SemanticSymbolMetadata>> metadata_by_uri;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
};

[[nodiscard]] std::vector<SemanticEngineDiagnostic> diagnosticsFor(const DiagnosticContext& context);

void sortAndDedupeDiagnostics(std::vector<SemanticEngineDiagnostic>& diagnostics);

} // namespace pristine::analysis::semantic
