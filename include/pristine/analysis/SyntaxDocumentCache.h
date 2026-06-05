#pragma once

#include "pristine/analysis/CompilationService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis {

struct SyntaxDocumentCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stores = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t entries = 0;
};

class SyntaxDocumentCache {
public:
    [[nodiscard]] const std::vector<DocumentSymbol>& documentSymbols(const CompilationService& service,
                                                                     std::string_view uri,
                                                                     int version,
                                                                     std::string_view text);
    [[nodiscard]] SyntaxDocumentCacheStats stats() const;
    void invalidate(std::string_view uri);
    void clear();

private:
    struct Entry {
        int version = 0;
        std::uint64_t text_hash = 0;
        std::vector<DocumentSymbol> symbols;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t stores_ = 0;
    std::uint64_t invalidations_ = 0;
};

} // namespace pristine::analysis
