#pragma once

#include "pristine/analysis/SymbolIndex.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis {

enum class SemanticTypeKind {
    Unknown,
    Builtin,
    Alias,
    Enum,
    Module,
    Interface,
    Class
};

struct SemanticType {
    SemanticTypeKind kind = SemanticTypeKind::Unknown;
    std::string name;
    std::string display_name;
    std::optional<Location> declaration;
};

struct SemanticDocumentState {
    int version = -1;
    bool is_open = false;
    bool dirty = false;
    bool invalidate_dependents = false;
};

struct SemanticScope {
    std::string path;
    std::string parent_path;
    ParseRange range;
};

struct SemanticSymbol {
    std::string id;
    std::string name;
    int kind = 0;
    std::string scope_path;
    Location location;
    ParseRange selection_range;
};

struct SemanticReference {
    std::string name;
    std::string scope_path;
    Location location;
    std::optional<std::string> target_symbol_id;
    bool is_declaration = false;
};

struct SemanticImport {
    std::string package_name;
    std::optional<std::string> item_name;
    std::string scope_path;
    ParseRange range;
};

struct SemanticDocument {
    std::string uri;
    int version = -1;
    bool is_open = false;
    bool dirty = false;
    bool stale = false;
    std::vector<IncludeDirective> includes;
    std::vector<std::string> included_uris;
    std::vector<SemanticImport> imports;
    std::vector<SemanticScope> scopes;
    std::vector<SemanticSymbol> symbols;
    std::vector<SemanticReference> references;
};

class SemanticWorkspace {
public:
    void clear();
    void setWorkspaceRoot(std::string_view root_uri);
    void updateDocument(std::string_view uri,
                        std::string_view text,
                        SemanticDocumentState state = {});
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticDocument* document(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> staleDocumentUris() const;
    [[nodiscard]] std::optional<SemanticSymbol> findResolvedSymbolAt(std::string_view uri,
                                                                      int line,
                                                                      int character) const;
    [[nodiscard]] std::vector<SemanticSymbol> findDefinitionsAt(std::string_view uri,
                                                                int line,
                                                                int character) const;
    [[nodiscard]] std::vector<SemanticSymbol> findTypeDefinitionsAt(std::string_view uri,
                                                                    int line,
                                                                    int character) const;
    [[nodiscard]] std::vector<SemanticReference> findReferencesAt(std::string_view uri,
                                                                  int line,
                                                                  int character,
                                                                  bool include_declaration) const;
    [[nodiscard]] std::vector<SemanticReference> findDocumentReferencesAt(std::string_view uri,
                                                                          int line,
                                                                          int character,
                                                                          bool include_declaration) const;
    [[nodiscard]] std::optional<SemanticSymbol> resolvedSymbolAt(std::string_view uri,
                                                                  int line,
                                                                  int character) const;
    [[nodiscard]] std::vector<SemanticSymbol> definitionsAt(std::string_view uri,
                                                            int line,
                                                            int character) const;
    [[nodiscard]] std::vector<SemanticReference> referencesAt(std::string_view uri,
                                                              int line,
                                                              int character,
                                                              bool include_declaration) const;
    [[nodiscard]] std::vector<SemanticReference> documentReferencesAt(std::string_view uri,
                                                                      int line,
                                                                      int character,
                                                                      bool include_declaration) const;
    [[nodiscard]] std::vector<SemanticSymbol> visibleSymbolsAt(std::string_view uri,
                                                               int line,
                                                               int character,
                                                               std::string_view prefix) const;
    [[nodiscard]] size_t documentCount() const { return documents_.size(); }

private:
    [[nodiscard]] std::optional<SemanticSymbol> symbolById(std::string_view symbol_id) const;
    [[nodiscard]] std::optional<SemanticSymbol> symbolAt(std::string_view uri,
                                                         int line,
                                                         int character) const;
    [[nodiscard]] std::optional<SemanticReference> referenceAt(std::string_view uri,
                                                              int line,
                                                              int character) const;
    [[nodiscard]] std::optional<SemanticSymbol> resolveReference(const SemanticReference& reference) const;
    [[nodiscard]] std::vector<SemanticSymbol> resolveName(std::string_view name,
                                                          std::string_view scope_path,
                                                          std::string_view preferred_uri) const;
    [[nodiscard]] std::vector<std::string> resolveIncludeUris(std::string_view including_uri,
                                                              std::string_view target) const;
    void rebuildReferenceBindings();
    void rebuildReverseIncludes();
    void markDependentsStale(std::string_view uri);

    CompilationService compilation_service_;
    std::string workspace_root_uri_;
    std::unordered_map<std::string, SemanticDocument> documents_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
};

} // namespace pristine::analysis