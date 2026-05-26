#pragma once

#include "pristine/analysis/CompilationService.h"

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

struct SymbolEntry {
    std::string name;
    int kind = 0;
    Location location;
    ParseRange selection_range;
};

struct ReferenceEntry {
    std::string name;
    Location location;
};

struct CompletionEntry {
    std::string label;
    int kind = 0;
    std::string detail;
};

class SymbolIndex {
public:
    void clear();
    void updateDocument(std::string_view uri, std::string_view text);
    void removeDocument(std::string_view uri);

    [[nodiscard]] std::vector<SymbolEntry> definitions(std::string_view name,
                                                       std::string_view preferred_uri) const;
    [[nodiscard]] std::vector<ReferenceEntry> references(std::string_view name,
                                                         bool include_declaration) const;
    [[nodiscard]] std::vector<SymbolEntry> workspaceSymbols(std::string_view query) const;
    [[nodiscard]] std::vector<CompletionEntry> completions(std::string_view prefix,
                                                           std::string_view preferred_uri) const;
    [[nodiscard]] size_t documentCount() const { return documents_.size(); }

private:
    struct IndexedDocument {
        std::vector<SymbolEntry> symbols;
        std::vector<ReferenceEntry> references;
    };

    CompilationService compilation_service_;
    std::unordered_map<std::string, IndexedDocument> documents_;
};

} // namespace pristine::analysis