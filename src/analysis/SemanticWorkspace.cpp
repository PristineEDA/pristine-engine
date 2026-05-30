#include "pristine/analysis/SemanticWorkspace.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <set>
#include <string>

namespace pristine::analysis {

void SemanticWorkspace::clear() {
    semantic_engine_.clear();
    documents_.clear();
    reverse_includes_.clear();
}

void SemanticWorkspace::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    semantic_engine_.setWorkspaceRoot(workspace_root_uri_);
}

void SemanticWorkspace::configureSemanticEngine(SemanticEngineConfig config) {
    semantic_engine_.configure(std::move(config));
}

void SemanticWorkspace::updateDocument(std::string_view uri,
                                       std::string_view text,
                                       SemanticDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    semantic_engine_.updateDocument(document_uri,
                                    text,
                                    SemanticEngineDocumentState{.version = state.version,
                                                                .is_open = state.is_open,
                                                                .dirty = state.dirty});
    if (state.invalidate_dependents) {
        markDependentsStale(document_uri);
    }

    std::vector<IncludeDirective> includes;
    try {
        includes = compilation_service_.includeDirectives(text);
    }
    catch (...) {
        includes.clear();
    }

    std::set<std::string> included_uris;
    for (const auto& include : includes) {
        for (const auto& included_uri : resolveIncludeUris(document_uri, include.target)) {
            included_uris.insert(included_uri);
        }
    }

    documents_.insert_or_assign(
        document_uri,
        SemanticDocument{.uri = document_uri,
                         .version = state.version,
                         .is_open = state.is_open,
                         .dirty = state.dirty,
                         .stale = false,
                         .includes = std::move(includes),
                         .included_uris = std::vector<std::string>(included_uris.begin(),
                                                                   included_uris.end())});
    rebuildReverseIncludes();
}

void SemanticWorkspace::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    semantic_engine_.removeDocument(document_uri);
    markDependentsStale(document_uri);
    documents_.erase(document_uri);
    rebuildReverseIncludes();
}

const SemanticDocument* SemanticWorkspace::document(std::string_view uri) const {
    const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (document_it == documents_.end()) {
        return nullptr;
    }
    return &document_it->second;
}

std::vector<std::string> SemanticWorkspace::includedUris(std::string_view uri) const {
    const auto* source = document(uri);
    if (!source) {
        return {};
    }
    return source->included_uris;
}

std::vector<std::string> SemanticWorkspace::includingUris(std::string_view uri) const {
    const auto graph_it = reverse_includes_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (graph_it == reverse_includes_.end()) {
        return {};
    }
    return graph_it->second;
}

std::vector<std::string> SemanticWorkspace::staleDocumentUris() const {
    std::vector<std::string> result;
    for (const auto& document_entry : documents_) {
        if (document_entry.second.stale) {
            result.push_back(document_entry.first);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

SemanticLookupResult SemanticWorkspace::lookupAt(std::string_view uri, int line, int character) const {
    return semantic_engine_.lookupAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineDefinitionsAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    return semantic_engine_.definitionsAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineTypeDefinitionsAt(std::string_view uri,
                                                                   int line,
                                                                   int character) const {
    return semantic_engine_.typeDefinitionsAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineReferencesAt(std::string_view uri,
                                                              int line,
                                                              int character,
                                                              bool include_declaration) const {
    return semantic_engine_.referencesAt(uri, line, character, include_declaration);
}

SemanticReferenceResult SemanticWorkspace::engineDocumentHighlightsAt(std::string_view uri,
                                                                      int line,
                                                                      int character) const {
    return semantic_engine_.documentHighlightsAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineImplementationsAt(std::string_view uri,
                                                                   int line,
                                                                   int character) const {
    return semantic_engine_.implementationsAt(uri, line, character);
}

SemanticPrepareRenameResult SemanticWorkspace::enginePrepareRenameAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    return semantic_engine_.prepareRenameAt(uri, line, character);
}

SemanticRenameResult SemanticWorkspace::engineRenameAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view new_name) const {
    return semantic_engine_.renameAt(uri, line, character, new_name);
}

SemanticCompletionResult SemanticWorkspace::engineCompletionsAt(std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix) const {
    return semantic_engine_.completionsAt(uri, line, character, prefix);
}

SemanticCompletionItem SemanticWorkspace::engineResolveCompletion(std::string_view stable_id,
                                                                  std::string_view label) const {
    return semantic_engine_.resolveCompletion(stable_id, label);
}

SemanticSignatureHelpResult SemanticWorkspace::engineSignatureHelpAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    return semantic_engine_.signatureHelpAt(uri, line, character);
}

SemanticInlayHintResult SemanticWorkspace::engineInlayHints(std::string_view uri,
                                                           ParseRange range) const {
    return semantic_engine_.inlayHints(uri, range);
}

SemanticTokenResult SemanticWorkspace::engineSemanticTokens(std::string_view uri) const {
    return semantic_engine_.semanticTokens(uri);
}

SemanticSelectionRangeResult SemanticWorkspace::engineSelectionRangesAt(std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    return semantic_engine_.selectionRangesAt(uri, line, character);
}

SemanticHoverResult SemanticWorkspace::engineHoverAt(std::string_view uri,
                                                     int line,
                                                     int character) const {
    return semantic_engine_.hoverAt(uri, line, character);
}

SemanticModuleHierarchyResult SemanticWorkspace::engineModuleHierarchy(std::optional<std::string_view> module_name,
                                                                       int max_depth) const {
    return semantic_engine_.moduleHierarchy(module_name, max_depth);
}

SemanticSchematicResult SemanticWorkspace::engineSchematic(std::optional<std::string_view> module_name,
                                                           int max_depth) const {
    return semantic_engine_.schematic(module_name, max_depth);
}

SemanticCallHierarchyPrepareResult SemanticWorkspace::enginePrepareCallHierarchy(std::string_view uri,
                                                                                 int line,
                                                                                 int character) const {
    return semantic_engine_.prepareCallHierarchy(uri, line, character);
}

SemanticCallHierarchyCallsResult SemanticWorkspace::engineIncomingCalls(const SemanticCallHierarchyItem& item) const {
    return semantic_engine_.incomingCalls(item);
}

SemanticCallHierarchyCallsResult SemanticWorkspace::engineOutgoingCalls(const SemanticCallHierarchyItem& item) const {
    return semantic_engine_.outgoingCalls(item);
}

SemanticCodeActionResult SemanticWorkspace::engineCodeActionsAt(std::string_view uri, ParseRange range) const {
    return semantic_engine_.codeActionsAt(uri, range);
}

SemanticWorkspaceSymbolResult SemanticWorkspace::engineWorkspaceSymbols(std::string_view query,
                                                                        size_t limit) const {
    return semantic_engine_.workspaceSymbols(query, limit);
}

std::vector<SemanticEngineDiagnostic> SemanticWorkspace::engineDiagnosticsFor(std::string_view uri) const {
    return semantic_engine_.diagnosticsFor(uri);
}

SemanticConeTrace SemanticWorkspace::engineBackwardConeAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    return semantic_engine_.backwardConeAt(uri, line, character);
}

std::vector<std::string> SemanticWorkspace::resolveIncludeUris(std::string_view including_uri,
                                                               std::string_view target) const {
    std::set<std::string> result;
    const auto target_text = toForwardSlashes(target);
    if (target_text.empty()) {
        return {};
    }

    if (isFileUri(target_text) || (!target_text.empty() && target_text.front() == '/') ||
        isWindowsAbsolutePath(target_text)) {
        result.insert(joinFileUri({}, target_text));
    }
    else {
        result.insert(joinFileUri(uriDirectory(including_uri), target_text));
        if (!workspace_root_uri_.empty()) {
            result.insert(joinFileUri(workspace_root_uri_, target_text));
        }
    }

    return std::vector<std::string>(result.begin(), result.end());
}

void SemanticWorkspace::rebuildReverseIncludes() {
    reverse_includes_.clear();
    for (const auto& document_entry : documents_) {
        for (const auto& included_uri : document_entry.second.included_uris) {
            reverse_includes_[included_uri].push_back(document_entry.first);
        }
    }

    for (auto& graph_entry : reverse_includes_) {
        auto& including_uris = graph_entry.second;
        std::sort(including_uris.begin(), including_uris.end());
        including_uris.erase(std::unique(including_uris.begin(), including_uris.end()), including_uris.end());
    }
}

void SemanticWorkspace::markDependentsStale(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::set<std::string> visited;
    std::vector<std::string> pending = includingUris(document_uri);
    while (!pending.empty()) {
        auto current_uri = std::move(pending.back());
        pending.pop_back();
        if (current_uri == document_uri || !visited.insert(current_uri).second) {
            continue;
        }

        if (auto document_it = documents_.find(current_uri); document_it != documents_.end()) {
            document_it->second.stale = true;
        }

        for (const auto& parent_uri : includingUris(current_uri)) {
            pending.push_back(parent_uri);
        }
    }
}

} // namespace pristine::analysis
