#pragma once

#include "pristine/analysis/SymbolIndex.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis {

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
};

struct SemanticDocument {
    std::string uri;
    std::vector<IncludeDirective> includes;
    std::vector<SemanticScope> scopes;
    std::vector<SemanticSymbol> symbols;
    std::vector<SemanticReference> references;
};

class SemanticWorkspace {
public:
    void clear();
    void updateDocument(std::string_view uri, std::string_view text);
    void removeDocument(std::string_view uri);

    [[nodiscard]] const SemanticDocument* document(std::string_view uri) const;
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

    CompilationService compilation_service_;
    std::unordered_map<std::string, SemanticDocument> documents_;
};

} // namespace pristine::analysis