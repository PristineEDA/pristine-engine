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

    [[nodiscard]] std::optional<SemanticReferenceResult> references(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        bool include_declaration) const;
    void storeReferences(std::uint64_t generation,
                         std::string_view uri,
                         int line,
                         int character,
                         bool include_declaration,
                         SemanticReferenceResult result);

    [[nodiscard]] std::optional<SemanticRenameResult> rename(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        std::string_view new_name) const;
    void storeRename(std::uint64_t generation,
                     std::string_view uri,
                     int line,
                     int character,
                     std::string_view new_name,
                     SemanticRenameResult result);

    [[nodiscard]] std::optional<SemanticCompletionResult> completions(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character,
        std::string_view prefix) const;
    void storeCompletions(std::uint64_t generation,
                          std::string_view uri,
                          int line,
                          int character,
                          std::string_view prefix,
                          SemanticCompletionResult result);

    [[nodiscard]] std::optional<SemanticSignatureHelpResult> signatureHelp(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storeSignatureHelp(std::uint64_t generation,
                            std::string_view uri,
                            int line,
                            int character,
                            SemanticSignatureHelpResult result);

    [[nodiscard]] std::optional<SemanticInlayHintResult> inlayHints(
        std::uint64_t generation,
        std::string_view uri,
        ParseRange range) const;
    void storeInlayHints(std::uint64_t generation,
                         std::string_view uri,
                         ParseRange range,
                         SemanticInlayHintResult result);

    [[nodiscard]] std::optional<SemanticModuleHierarchyResult> moduleHierarchy(
        std::uint64_t generation,
        std::optional<std::string_view> module_name,
        int max_depth) const;
    void storeModuleHierarchy(std::uint64_t generation,
                              std::optional<std::string_view> module_name,
                              int max_depth,
                              SemanticModuleHierarchyResult result);

    [[nodiscard]] std::optional<SemanticSchematicResult> schematic(
        std::uint64_t generation,
        std::optional<std::string_view> module_name,
        int max_depth) const;
    void storeSchematic(std::uint64_t generation,
                        std::optional<std::string_view> module_name,
                        int max_depth,
                        SemanticSchematicResult result);

    [[nodiscard]] std::optional<SemanticConeTrace> backwardCone(
        std::uint64_t generation,
        std::string_view uri,
        int line,
        int character) const;
    void storeBackwardCone(std::uint64_t generation,
                           std::string_view uri,
                           int line,
                           int character,
                           SemanticConeTrace result);

    [[nodiscard]] std::optional<SemanticCodeActionResult> codeActions(
        std::uint64_t generation,
        std::string_view uri,
        ParseRange range) const;
    void storeCodeActions(std::uint64_t generation,
                          std::string_view uri,
                          ParseRange range,
                          SemanticCodeActionResult result);

private:
    struct DiagnosticsEntry {
        std::uint64_t generation = 0;
        std::vector<SemanticEngineDiagnostic> diagnostics;
    };

    struct WorkspaceSymbolsEntry {
        std::uint64_t generation = 0;
        SemanticWorkspaceSymbolResult result;
    };

    struct ReferencesEntry {
        std::uint64_t generation = 0;
        SemanticReferenceResult result;
    };

    struct RenameEntry {
        std::uint64_t generation = 0;
        SemanticRenameResult result;
    };

    struct CompletionEntry {
        std::uint64_t generation = 0;
        SemanticCompletionResult result;
    };

    struct SignatureHelpEntry {
        std::uint64_t generation = 0;
        SemanticSignatureHelpResult result;
    };

    struct InlayHintsEntry {
        std::uint64_t generation = 0;
        SemanticInlayHintResult result;
    };

    struct ModuleHierarchyEntry {
        std::uint64_t generation = 0;
        SemanticModuleHierarchyResult result;
    };

    struct SchematicEntry {
        std::uint64_t generation = 0;
        SemanticSchematicResult result;
    };

    struct BackwardConeEntry {
        std::uint64_t generation = 0;
        SemanticConeTrace result;
    };

    struct CodeActionsEntry {
        std::uint64_t generation = 0;
        SemanticCodeActionResult result;
    };

    [[nodiscard]] static std::string workspaceSymbolsKey(std::string_view query, size_t limit);
    [[nodiscard]] static std::string positionKey(std::string_view uri, int line, int character);
    [[nodiscard]] static std::string referencesKey(std::string_view uri,
                                                   int line,
                                                   int character,
                                                   bool include_declaration);
    [[nodiscard]] static std::string renameKey(std::string_view uri,
                                               int line,
                                               int character,
                                               std::string_view new_name);
    [[nodiscard]] static std::string completionKey(std::string_view uri,
                                                   int line,
                                                   int character,
                                                   std::string_view prefix);
    [[nodiscard]] static std::string moduleQueryKey(std::optional<std::string_view> module_name,
                                                    int max_depth);
    [[nodiscard]] static std::string rangeKey(std::string_view uri, ParseRange range);

    std::unordered_map<std::string, DiagnosticsEntry> diagnostics_by_uri_;
    std::unordered_map<std::string, WorkspaceSymbolsEntry> workspace_symbols_by_key_;
    std::unordered_map<std::string, ReferencesEntry> references_by_key_;
    std::unordered_map<std::string, RenameEntry> rename_by_key_;
    std::unordered_map<std::string, CompletionEntry> completions_by_key_;
    std::unordered_map<std::string, SignatureHelpEntry> signature_help_by_key_;
    std::unordered_map<std::string, InlayHintsEntry> inlay_hints_by_key_;
    std::unordered_map<std::string, ModuleHierarchyEntry> module_hierarchy_by_key_;
    std::unordered_map<std::string, SchematicEntry> schematic_by_key_;
    std::unordered_map<std::string, BackwardConeEntry> backward_cone_by_key_;
    std::unordered_map<std::string, CodeActionsEntry> code_actions_by_key_;
};

} // namespace pristine::analysis::semantic
