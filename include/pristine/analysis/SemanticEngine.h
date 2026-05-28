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
    std::vector<SemanticEngineDiagnostic> diagnostics;
    bool has_ast = false;
};

class SemanticEngine {
public:
    void clear();
    void setWorkspaceRoot(std::string_view root_uri);
    void updateDocument(std::string_view uri,
                        std::string_view text,
                        SemanticEngineDocumentState state = {});
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticEngineDocument* document(std::string_view uri) const;
    [[nodiscard]] size_t documentCount() const { return documents_.size(); }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] bool snapshotDirty() const { return snapshot_dirty_; }

    [[nodiscard]] const SemanticEngineSnapshot& snapshot() const;
    [[nodiscard]] std::vector<SemanticEngineDiagnostic> diagnosticsFor(std::string_view uri) const;

private:
    void rebuildSnapshot() const;

    std::string workspace_root_uri_;
    std::unordered_map<std::string, SemanticEngineDocument> documents_;
    mutable std::optional<SemanticEngineSnapshot> snapshot_;
    mutable bool snapshot_dirty_ = true;
    std::uint64_t generation_ = 0;
};

} // namespace pristine::analysis
