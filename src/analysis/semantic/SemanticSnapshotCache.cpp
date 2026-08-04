#include "SemanticSnapshotCache.h"

#include <algorithm>
#include <utility>

namespace pristine::analysis::semantic {

SemanticSnapshotCache::Entry* SemanticSnapshotCache::find(std::uint64_t key) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.key == key;
    });
    if (found == entries_.end()) {
        return nullptr;
    }
    found->last_used = ++clock_;
    return &*found;
}

SemanticSnapshotCache::Entry* SemanticSnapshotCache::findIdentity(std::string_view identity) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.identity == identity;
    });
    if (found == entries_.end()) {
        return nullptr;
    }
    found->last_used = ++clock_;
    return &*found;
}

SemanticSnapshotCache::Entry& SemanticSnapshotCache::insert(Entry entry) {
    entry.last_used = ++clock_;
    if (entries_.size() >= 4) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const Entry& lhs,
                                                                                   const Entry& rhs) {
            return lhs.last_used < rhs.last_used;
        });
        entries_.erase(oldest);
    }
    entries_.push_back(std::move(entry));
    return entries_.back();
}

void SemanticSnapshotCache::clear() {
    entries_.clear();
    clock_ = 0;
}

size_t SemanticSnapshotCache::size() const {
    return entries_.size();
}

} // namespace pristine::analysis::semantic
