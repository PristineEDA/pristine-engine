#pragma once

#include "SnapshotBuilder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::analysis::semantic {

// Stores completed closure snapshots that LSP result identities can safely reference.
class SemanticSnapshotCache {
public:
    struct Entry {
        std::uint64_t key = 0;
        std::uint64_t last_used = 0;
        std::uint64_t generation = 0;
        std::uint64_t config_discovery_fingerprint = 0;
        std::uint64_t closure_fingerprint = 0;
        std::string scope_kind;
        std::string root_uri;
        std::string identity;
        SemanticEngineSnapshot snapshot;
        std::unique_ptr<SnapshotData> data;
    };

    [[nodiscard]] Entry* find(std::uint64_t key);
    [[nodiscard]] Entry* findIdentity(std::string_view identity);
    Entry& insert(Entry entry);
    void clear();
    [[nodiscard]] size_t size() const;

private:
    std::vector<Entry> entries_;
    std::uint64_t clock_ = 0;
};

} // namespace pristine::analysis::semantic
