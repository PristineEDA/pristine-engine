#include "pristine/analysis/SyntaxDocumentCache.h"

#include <functional>
#include <string>

namespace pristine::analysis {
namespace {

std::uint64_t textHash(std::string_view text) {
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(text));
}

} // namespace

const std::vector<DocumentSymbol>& SyntaxDocumentCache::documentSymbols(
    const CompilationService& service,
    std::string_view uri,
    int version,
    std::string_view text) {
    const auto hash = textHash(text);
    auto entry_it = entries_.find(std::string(uri));
    if (entry_it != entries_.end() && entry_it->second.version == version &&
        entry_it->second.text_hash == hash) {
        ++hits_;
        return entry_it->second.symbols;
    }

    ++misses_;
    Entry entry{.version = version,
                .text_hash = hash,
                .symbols = service.documentSymbols(text, uri)};
    auto [stored_it, inserted] = entries_.insert_or_assign(std::string(uri), std::move(entry));
    (void)inserted;
    ++stores_;
    return stored_it->second.symbols;
}

SyntaxDocumentCacheStats SyntaxDocumentCache::stats() const {
    return SyntaxDocumentCacheStats{.hits = hits_,
                                    .misses = misses_,
                                    .stores = stores_,
                                    .invalidations = invalidations_,
                                    .entries = entries_.size()};
}

void SyntaxDocumentCache::invalidate(std::string_view uri) {
    invalidations_ += entries_.erase(std::string(uri));
}

void SyntaxDocumentCache::clear() {
    invalidations_ += entries_.size();
    entries_.clear();
}

} // namespace pristine::analysis
