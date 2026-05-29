#pragma once

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
    std::vector<SemanticEngineDiagnostic> diagnostics;
};

[[nodiscard]] constexpr std::string_view codeActionProviderName() {
    return "CodeActionProvider";
}

[[nodiscard]] SemanticCodeActionResult codeActionsAt(const CodeActionContext& context);

} // namespace pristine::analysis::semantic
