#pragma once

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SemanticEngine.h"
#include "SnapshotBuilder.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct CompletionResolveSymbol {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
};

struct CompletionMemberCandidate {
    SemanticSymbolIdentity identity;
};

struct CompletionMemberContext {
    std::string qualifier;
    std::vector<CompletionMemberCandidate> candidates;
};

struct CompletionResolveContext {
    const std::unordered_map<std::string, ModuleDefinition>* modules_by_name = nullptr;
    const std::unordered_map<std::string, std::string>* module_uris_by_name = nullptr;
    const std::unordered_map<std::string, std::vector<MacroDefinition>>* macros_by_uri = nullptr;
    std::optional<CompletionResolveSymbol> symbol;
    std::optional<CompletionResolveSymbol> member;
};

struct CompletionQueryContext {
    std::uint64_t generation = 0;
    bool snapshot_available = false;
    std::string document_uri;
    const std::string* document_text = nullptr;
    const std::vector<SnapshotScopeVisibility>* scopes = nullptr;
    const std::vector<SnapshotVisibilityCandidate>* document_candidates = nullptr;
    const std::unordered_map<std::string, SnapshotPackageVisibility>* packages = nullptr;
    const std::vector<SnapshotVisibilityCandidate>* workspace_candidates_by_name = nullptr;
    const std::unordered_map<std::string, std::string>* module_definition_ids_by_name = nullptr;
    const std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>>*
        member_candidates_by_qualifier = nullptr;
    const std::vector<SnapshotVisibleMacro>* macros = nullptr;
    const std::vector<SnapshotModuleInstance>* module_instances = nullptr;
    const std::unordered_map<std::string, ModuleDefinition>* modules_by_name = nullptr;
    const std::unordered_map<std::string, std::string>* module_uris_by_name = nullptr;
    size_t scope_visibility_count = 0;
    size_t package_visibility_count = 0;
    size_t member_visibility_count = 0;
    size_t callable_visibility_count = 0;
    std::int64_t scope_visibility_build_micros = 0;
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

[[nodiscard]] SemanticCompletionResult completeAt(const CompletionQueryContext& context,
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
[[nodiscard]] std::string baseMemberQualifier(std::string_view qualifier);
[[nodiscard]] SemanticCompletionItem resolveCompletionItem(std::string_view stable_id,
                                                           std::string_view label,
                                                           const CompletionResolveContext& context);

[[nodiscard]] std::set<std::string> connectedNamedPortsForInstance(std::string_view text,
                                                                    int line,
                                                                    int character,
                                                                    ParseRange instance_selection_range,
                                                                    ParseRange instance_range);

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

void appendMemberCompletions(std::vector<SemanticCompletionItem>& items,
                             std::set<std::string>& emitted,
                             const CompletionMemberContext& context,
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
