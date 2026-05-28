#pragma once

#include "pristine/analysis/CompilationService.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang {
class SourceManager;
}

namespace slang::ast {
class Compilation;
}

namespace slang::syntax {
class SyntaxTree;
}

namespace pristine::analysis {

enum class SemanticEngineMode {
    Shallow,
    Design
};

struct SemanticEngineConfig {
    std::optional<std::string> build;
    std::optional<std::string> build_pattern;
    bool build_relative_paths = false;
    std::optional<std::string> flags;
    std::vector<std::string> top_modules;
};

struct SemanticLocation {
    std::string uri;
    ParseRange range;
};

struct SemanticSymbolIdentity {
    std::string stable_id;
    std::string name;
    std::string kind;
    SemanticLocation location;
};

struct SemanticLookupResult {
    SemanticEngineMode mode = SemanticEngineMode::Shallow;
    std::uint64_t generation = 0;
    std::optional<SemanticSymbolIdentity> symbol;
    SemanticLocation query_location;
    std::vector<std::string> messages;
    bool unresolved = false;
};

struct SemanticReferenceResult {
    std::uint64_t generation = 0;
    std::vector<SemanticLocation> locations;
    std::vector<std::string> messages;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticHoverResult {
    std::uint64_t generation = 0;
    std::string contents;
    ParseRange range;
    std::vector<std::string> messages;
    bool unresolved = false;
};

struct SemanticPrepareRenameResult {
    std::uint64_t generation = 0;
    std::string placeholder;
    ParseRange range;
    std::vector<std::string> messages;
    bool unresolved = false;
};

struct SemanticTextEdit {
    SemanticLocation location;
    std::string new_text;
};

struct SemanticRenameResult {
    std::uint64_t generation = 0;
    std::vector<SemanticTextEdit> edits;
    std::vector<std::string> messages;
    bool unresolved = false;
    bool truncated = false;
};

struct SemanticCompletionItem {
    std::string label;
    std::string detail;
    std::string documentation;
    std::string insert_text;
    int kind = 0;
    bool unresolved = false;
};

struct SemanticInlayHint {
    SemanticLocation location;
    std::string label;
    std::string kind;
};

struct SemanticHierarchyNode {
    std::string id;
    std::string name;
    std::string kind;
    SemanticLocation location;
    std::vector<std::string> children;
};

struct SemanticEngineConeEdge {
    std::string from_id;
    std::string to_id;
    SemanticLocation location;
    std::string expression;
};

struct SemanticEngineDocumentState {
    int version = -1;
    bool is_open = false;
    bool dirty = false;
};

struct SemanticEngineDocument {
    std::string uri;
    std::string text;
    int version = -1;
    bool is_open = false;
    bool dirty = false;
};

struct SemanticEngineDiagnostic {
    std::string uri;
    std::string code;
    std::string message;
    ParseRange range;
    int severity = 1;
};

struct SemanticEngineSnapshot {
    std::uint64_t generation = 0;
    std::vector<std::string> document_uris;
    std::vector<std::string> dirty_document_uris;
    std::vector<std::string> top_modules;
    std::vector<SemanticEngineDiagnostic> diagnostics;
    SemanticEngineMode mode = SemanticEngineMode::Shallow;
    bool has_shallow_ast = false;
    bool has_design_ast = false;
};

class SemanticEngine {
public:
    void clear();
    void setWorkspaceRoot(std::string_view root_uri);
    void configure(SemanticEngineConfig config);
    void updateDocument(std::string_view uri,
                        std::string_view text,
                        SemanticEngineDocumentState state = {});
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticEngineDocument* document(std::string_view uri) const;
    [[nodiscard]] size_t documentCount() const { return documents_.size(); }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] bool snapshotDirty() const { return snapshot_dirty_; }
    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> dirtyDocumentUris() const;
    [[nodiscard]] std::vector<std::string> affectedDocumentUris(std::string_view uri) const;

    [[nodiscard]] const SemanticEngineSnapshot& snapshot() const;
    [[nodiscard]] std::vector<SemanticEngineDiagnostic> diagnosticsFor(std::string_view uri) const;
    [[nodiscard]] SemanticLookupResult lookupAt(std::string_view uri, int line, int character) const;
    [[nodiscard]] SemanticReferenceResult definitionsAt(std::string_view uri,
                                                        int line,
                                                        int character) const;
    [[nodiscard]] SemanticReferenceResult typeDefinitionsAt(std::string_view uri,
                                                            int line,
                                                            int character) const;
    [[nodiscard]] SemanticReferenceResult referencesAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       bool include_declaration) const;
    [[nodiscard]] SemanticReferenceResult documentHighlightsAt(std::string_view uri,
                                                               int line,
                                                               int character) const;
    [[nodiscard]] SemanticHoverResult hoverAt(std::string_view uri, int line, int character) const;
    [[nodiscard]] SemanticPrepareRenameResult prepareRenameAt(std::string_view uri,
                                                              int line,
                                                              int character) const;
    [[nodiscard]] SemanticRenameResult renameAt(std::string_view uri,
                                                int line,
                                                int character,
                                                std::string_view new_name) const;

private:
    void rebuildDependenciesFor(std::string_view document_uri, std::string_view text);
    void rebuildSnapshot() const;

    std::string workspace_root_uri_;
    SemanticEngineConfig config_;
    std::unordered_map<std::string, SemanticEngineDocument> documents_;
    std::unordered_map<std::string, std::vector<std::string>> includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
    mutable std::optional<SemanticEngineSnapshot> snapshot_;
    mutable bool snapshot_dirty_ = true;
    std::uint64_t generation_ = 0;
};

} // namespace pristine::analysis
