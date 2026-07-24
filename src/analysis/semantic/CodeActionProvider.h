#pragma once

#include "DiagnosticProvider.h"
#include "SnapshotBuilder.h"
#include "pristine/analysis/SemanticEngine.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct CodeActionContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::string workspace_root_uri;
    SemanticEngineDocument document;
    ParseRange range;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::vector<ModuleSchematic> document_schematics;
    std::vector<IncludeDirective> include_directives;
    std::vector<SnapshotModuleInstance> module_instances;
    const SnapshotDesignGraphBindingIndex* design_graph_bindings = nullptr;
    std::unordered_map<std::string, SnapshotPackageVisibility> packages_by_name;
    std::vector<MacroInvocationFact> macro_invocations;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::vector<SemanticEngineDiagnostic> diagnostics;
};

[[nodiscard]] constexpr std::string_view codeActionProviderName() {
    return "CodeActionProvider";
}

[[nodiscard]] SemanticCodeActionResult codeActionsAt(const CodeActionContext& context);

} // namespace pristine::analysis::semantic
