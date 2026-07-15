#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis {

struct Location {
    std::string uri;
    ParseRange range;
};

struct SemanticDocumentState {
    int version = -1;
    bool is_open = false;
    bool dirty = false;
    bool invalidate_dependents = false;
};

struct SemanticDocument {
    std::string uri;
    int version = -1;
    bool is_open = false;
    bool dirty = false;
    bool stale = false;
    std::vector<IncludeDirective> includes;
    std::vector<std::string> included_uris;
};

class SemanticWorkspace {
public:
    void clear();
    void setWorkspaceRoot(std::string_view root_uri);
    void configureSemanticEngine(SemanticEngineConfig config);
    void updateDocument(std::string_view uri,
                        std::string_view text,
                        SemanticDocumentState state = {});
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticDocument* document(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> staleDocumentUris() const;
    [[nodiscard]] SemanticWorkspaceDiscoverySnapshot engineWorkspaceDiscovery() const;
    [[nodiscard]] SemanticLookupResult lookupAt(std::string_view uri, int line, int character) const;
    [[nodiscard]] SemanticReferenceResult engineDefinitionsAt(std::string_view uri,
                                                              int line,
                                                              int character) const;
    [[nodiscard]] SemanticReferenceResult engineTypeDefinitionsAt(std::string_view uri,
                                                                  int line,
                                                                  int character) const;
    [[nodiscard]] SemanticReferenceResult engineReferencesAt(std::string_view uri,
                                                            int line,
                                                            int character,
                                                            bool include_declaration) const;
    [[nodiscard]] SemanticReferenceResult engineDocumentHighlightsAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const;
    [[nodiscard]] SemanticReferenceResult engineImplementationsAt(std::string_view uri,
                                                                  int line,
                                                                  int character) const;
    [[nodiscard]] SemanticPrepareRenameResult enginePrepareRenameAt(std::string_view uri,
                                                                    int line,
                                                                    int character) const;
    [[nodiscard]] SemanticRenameResult engineRenameAt(std::string_view uri,
                                                      int line,
                                                      int character,
                                                      std::string_view new_name) const;
    [[nodiscard]] SemanticCompletionResult engineCompletionsAt(std::string_view uri,
                                                               int line,
                                                               int character,
                                                               std::string_view prefix = {}) const;
    [[nodiscard]] SemanticCompletionItem engineResolveCompletion(std::string_view stable_id,
                                                                 std::string_view label) const;
    [[nodiscard]] SemanticSignatureHelpResult engineSignatureHelpAt(std::string_view uri,
                                                                    int line,
                                                                    int character) const;
    [[nodiscard]] SemanticInlayHintResult engineInlayHints(std::string_view uri,
                                                          ParseRange range) const;
    [[nodiscard]] SemanticTokenResult engineSemanticTokens(std::string_view uri) const;
    [[nodiscard]] SemanticSelectionRangeResult engineSelectionRangesAt(std::string_view uri,
                                                                       int line,
                                                                       int character) const;
    [[nodiscard]] SemanticHoverResult engineHoverAt(std::string_view uri,
                                                    int line,
                                                    int character) const;
    [[nodiscard]] std::vector<SemanticEngineDiagnostic> engineDiagnosticsFor(std::string_view uri) const;
    [[nodiscard]] SemanticInactiveRegionResult engineInactiveRegions(std::string_view uri) const;
    [[nodiscard]] SemanticConeTrace engineBackwardConeAt(std::string_view uri,
                                                         int line,
                                                         int character) const;
    [[nodiscard]] SemanticModuleHierarchyResult engineModuleHierarchy(std::optional<std::string_view> module_name,
                                                                      int max_depth) const;
    [[nodiscard]] SemanticSchematicResult engineSchematic(std::optional<std::string_view> module_name,
                                                          int max_depth) const;
    [[nodiscard]] SemanticCallHierarchyPrepareResult enginePrepareCallHierarchy(std::string_view uri,
                                                                                int line,
                                                                                int character) const;
    [[nodiscard]] SemanticCallHierarchyCallsResult engineIncomingCalls(const SemanticCallHierarchyItem& item) const;
    [[nodiscard]] SemanticCallHierarchyCallsResult engineOutgoingCalls(const SemanticCallHierarchyItem& item) const;
    [[nodiscard]] SemanticCodeActionResult engineCodeActionsAt(std::string_view uri, ParseRange range) const;
    [[nodiscard]] SemanticWorkspaceSymbolResult engineWorkspaceSymbols(std::string_view query,
                                                                       size_t limit = 1000) const;
    [[nodiscard]] SemanticQueryCacheStats engineQueryCacheStats() const;
    [[nodiscard]] std::uint64_t engineGeneration() const { return semantic_engine_.generation(); }
    [[nodiscard]] bool engineHasFreshSnapshot() const { return semantic_engine_.hasFreshSnapshot(); }
    [[nodiscard]] size_t documentCount() const { return documents_.size(); }

private:
    [[nodiscard]] std::vector<std::string> resolveIncludeUris(std::string_view including_uri,
                                                              std::string_view target) const;
    void rebuildReverseIncludes();
    void markDependentsStale(std::string_view uri);

    CompilationService compilation_service_;
    SemanticEngine semantic_engine_;
    std::string workspace_root_uri_;
    std::unordered_map<std::string, SemanticDocument> documents_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
};

} // namespace pristine::analysis
