#pragma once

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SemanticEngine.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::analysis::semantic {

struct CompletionContext {
    std::optional<size_t> prefix_start;
    std::optional<std::string> package_qualifier;
    std::optional<std::string> member_qualifier;
    bool macro_invocation = false;
    bool member_access = false;
    bool module_instantiation_position = false;
};

[[nodiscard]] constexpr std::string_view completionProviderName() {
    return "CompletionProvider";
}

[[nodiscard]] std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix);

[[nodiscard]] CompletionContext detectCompletionContext(std::string_view text,
                                                        int line,
                                                        int character,
                                                        std::string_view prefix);

[[nodiscard]] int completionKindForSemanticKind(std::string_view kind);
[[nodiscard]] std::string completionDetailForSemanticKind(std::string_view kind);
[[nodiscard]] int completionPriorityForDetail(std::string_view detail);

[[nodiscard]] std::string portSignatureLabel(const SchematicPort& port);
[[nodiscard]] std::string moduleSignatureLabel(const ModuleDefinition& module);
[[nodiscard]] std::string moduleInstantiationSnippet(const ModuleDefinition& module);
[[nodiscard]] std::string portConnectionSnippet(std::string_view port_name);
[[nodiscard]] std::string macroSignatureLabel(const MacroDefinition& macro);
[[nodiscard]] std::string macroInsertText(const MacroDefinition& macro);
[[nodiscard]] std::string macroDocumentation(const MacroDefinition& macro);
[[nodiscard]] std::string moduleDocumentation(const ModuleDefinition& module,
                                             std::string_view declaration_uri);
[[nodiscard]] std::string portDocumentation(const ModuleDefinition& module,
                                           const SchematicPort& port,
                                           std::string_view declaration_uri);

void appendCompletionItem(std::vector<SemanticCompletionItem>& items,
                          std::set<std::string>& emitted,
                          SemanticCompletionItem item,
                          std::string_view prefix,
                          bool& truncated);

void appendSymbolCompletion(std::vector<SemanticCompletionItem>& items,
                            std::set<std::string>& emitted,
                            const SemanticSymbolIdentity& symbol,
                            std::string_view prefix,
                            bool& truncated);

void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const std::string& module_stable_id,
                                 const ModuleDefinition& module,
                                 std::string_view module_uri,
                                 std::string_view prefix,
                                 const std::set<std::string>& excluded_ports,
                                 bool& truncated);

} // namespace pristine::analysis::semantic
