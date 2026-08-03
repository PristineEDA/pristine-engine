#include "pristine/server/VersionedDocumentResultStore.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace pristine::server {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashBytes(std::uint64_t& hash, std::string_view value) {
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kFnvPrime;
    }
    hash ^= 0xffU;
    hash *= kFnvPrime;
}

std::string opaqueResultId(std::string_view kind,
                           std::string_view uri,
                           std::string_view payload) {
    auto hash = kFnvOffsetBasis;
    hashBytes(hash, kind);
    hashBytes(hash, uri);
    hashBytes(hash, payload);
    std::ostringstream stream;
    stream << kind << "-v1-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

} // namespace

std::optional<VersionedDocumentResultStore::DiagnosticResult>
VersionedDocumentResultStore::currentDiagnostics(std::string_view uri,
                                                 const DocumentResultMetadata& metadata) const {
    std::lock_guard lock(mutex_);
    const auto it = buckets_.find(uri);
    if (it == buckets_.end() || !it->second.diagnostics.has_value() ||
        !metadataMatches(it->second.diagnostics->metadata, metadata)) {
        ++stats_.miss_count;
        return std::nullopt;
    }
    it->second.last_access = ++access_clock_;
    ++stats_.hit_count;
    return it->second.diagnostics;
}

VersionedDocumentResultStore::DiagnosticResult VersionedDocumentResultStore::storeDiagnostics(
    std::string uri,
    nlohmann::json items,
    DocumentResultMetadata metadata) {
    std::lock_guard lock(mutex_);
    auto& bucket = touchBucketLocked(uri);
    DiagnosticResult result{.result_id = diagnosticResultId(uri, items),
                            .items = std::move(items),
                            .metadata = std::move(metadata)};
    bucket.diagnostics = result;
    ++stats_.store_count;
    evictLocked();
    refreshStatsLocked();
    return result;
}

std::optional<VersionedDocumentResultStore::SemanticTokenResult>
VersionedDocumentResultStore::currentSemanticTokens(std::string_view uri,
                                                    const DocumentResultMetadata& metadata,
                                                    std::uint64_t legend_version) const {
    std::lock_guard lock(mutex_);
    const auto it = buckets_.find(uri);
    if (it == buckets_.end()) {
        ++stats_.miss_count;
        return std::nullopt;
    }
    const auto token = std::find_if(it->second.semantic_tokens.rbegin(),
                                    it->second.semantic_tokens.rend(),
                                    [&](const SemanticTokenResult& candidate) {
                                        return candidate.legend_version == legend_version &&
                                               metadataMatches(candidate.metadata, metadata);
                                    });
    if (token == it->second.semantic_tokens.rend()) {
        ++stats_.miss_count;
        return std::nullopt;
    }
    it->second.last_access = ++access_clock_;
    ++stats_.hit_count;
    return *token;
}

std::optional<VersionedDocumentResultStore::SemanticTokenResult>
VersionedDocumentResultStore::semanticTokensForResultId(std::string_view uri,
                                                        std::string_view result_id,
                                                        std::uint64_t legend_version) const {
    std::lock_guard lock(mutex_);
    const auto it = buckets_.find(uri);
    if (it == buckets_.end()) {
        ++stats_.miss_count;
        return std::nullopt;
    }
    const auto token = std::find_if(it->second.semantic_tokens.begin(),
                                    it->second.semantic_tokens.end(),
                                    [&](const SemanticTokenResult& candidate) {
                                        return candidate.legend_version == legend_version &&
                                               candidate.result_id == result_id;
                                    });
    if (token == it->second.semantic_tokens.end()) {
        ++stats_.miss_count;
        return std::nullopt;
    }
    it->second.last_access = ++access_clock_;
    ++stats_.hit_count;
    return *token;
}

VersionedDocumentResultStore::SemanticTokenResult
VersionedDocumentResultStore::storeSemanticTokens(std::string uri,
                                                  std::vector<int> data,
                                                  DocumentResultMetadata metadata,
                                                  std::uint64_t legend_version) {
    std::lock_guard lock(mutex_);
    auto& bucket = touchBucketLocked(uri);
    SemanticTokenResult result{.result_id = semanticTokenResultId(uri, data, legend_version),
                                .data = std::move(data),
                                .legend_version = legend_version,
                                .metadata = std::move(metadata)};
    const auto existing = std::find_if(bucket.semantic_tokens.begin(),
                                       bucket.semantic_tokens.end(),
                                       [&](const SemanticTokenResult& candidate) {
                                           return candidate.result_id == result.result_id &&
                                                  candidate.legend_version == result.legend_version;
                                       });
    if (existing != bucket.semantic_tokens.end()) {
        *existing = result;
    }
    else {
        bucket.semantic_tokens.push_back(result);
        if (bucket.semantic_tokens.size() > kMaxSemanticTokenHistory) {
            bucket.semantic_tokens.erase(bucket.semantic_tokens.begin());
            ++stats_.eviction_count;
        }
    }
    ++stats_.store_count;
    evictLocked();
    refreshStatsLocked();
    return result;
}

void VersionedDocumentResultStore::clearUri(std::string_view uri) {
    std::lock_guard lock(mutex_);
    buckets_.erase(std::string(uri));
    refreshStatsLocked();
}

void VersionedDocumentResultStore::clear() {
    std::lock_guard lock(mutex_);
    buckets_.clear();
    access_clock_ = 0;
    refreshStatsLocked();
}

VersionedDocumentResultStore::Stats VersionedDocumentResultStore::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    size_t diagnostic_entries = 0;
    size_t token_entries = 0;
    for (const auto& [_, bucket] : buckets_) {
        diagnostic_entries += bucket.diagnostics.has_value() ? 1U : 0U;
        token_entries += bucket.semantic_tokens.size();
    }
    result.bucket_count = buckets_.size();
    result.diagnostic_entry_count = diagnostic_entries;
    result.semantic_token_entry_count = token_entries;
    return result;
}

void VersionedDocumentResultStore::resetStats() {
    std::lock_guard lock(mutex_);
    stats_ = {};
    refreshStatsLocked();
}

std::string VersionedDocumentResultStore::diagnosticResultId(std::string_view uri,
                                                              const nlohmann::json& items) {
    return opaqueResultId("pristine-diagnostics", uri, items.dump());
}

std::string VersionedDocumentResultStore::semanticTokenResultId(std::string_view uri,
                                                                 const std::vector<int>& data,
                                                                 std::uint64_t legend_version) {
    std::string payload = std::to_string(legend_version);
    payload.push_back('|');
    for (const auto value : data) {
        payload.append(std::to_string(value));
        payload.push_back(',');
    }
    return opaqueResultId("pristine-semantic-tokens", uri, payload);
}

bool VersionedDocumentResultStore::metadataMatches(const DocumentResultMetadata& lhs,
                                                   const DocumentResultMetadata& rhs) {
    return lhs.document_version == rhs.document_version &&
           lhs.semantic_generation == rhs.semantic_generation &&
           lhs.snapshot_identity == rhs.snapshot_identity;
}

VersionedDocumentResultStore::Bucket& VersionedDocumentResultStore::touchBucketLocked(
    std::string_view uri) {
    auto& bucket = buckets_[std::string(uri)];
    bucket.last_access = ++access_clock_;
    return bucket;
}

void VersionedDocumentResultStore::evictLocked() {
    while (buckets_.size() > kMaxDocumentBuckets) {
        auto victim = buckets_.end();
        auto oldest_access = std::numeric_limits<std::uint64_t>::max();
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
            if (it->second.last_access < oldest_access) {
                oldest_access = it->second.last_access;
                victim = it;
            }
        }
        if (victim == buckets_.end()) {
            break;
        }
        buckets_.erase(victim);
        ++stats_.eviction_count;
    }
}

void VersionedDocumentResultStore::refreshStatsLocked() const {
    stats_.bucket_count = buckets_.size();
    stats_.diagnostic_entry_count = 0;
    stats_.semantic_token_entry_count = 0;
    for (const auto& [_, bucket] : buckets_) {
        stats_.diagnostic_entry_count += bucket.diagnostics.has_value() ? 1U : 0U;
        stats_.semantic_token_entry_count += bucket.semantic_tokens.size();
    }
}

} // namespace pristine::server
