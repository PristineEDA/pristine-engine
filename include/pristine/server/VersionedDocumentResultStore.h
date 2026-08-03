#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pristine::server {

struct DocumentResultMetadata {
    int document_version = -1;
    std::uint64_t semantic_generation = 0;
    std::string snapshot_identity;
};

class VersionedDocumentResultStore {
public:
    static constexpr size_t kMaxDocumentBuckets = 256;
    static constexpr size_t kMaxSemanticTokenHistory = 4;
    static constexpr std::uint64_t kSemanticTokenLegendVersion = 1;

    struct DiagnosticResult {
        std::string result_id;
        nlohmann::json items;
        DocumentResultMetadata metadata;
    };

    struct SemanticTokenResult {
        std::string result_id;
        std::vector<int> data;
        std::uint64_t legend_version = kSemanticTokenLegendVersion;
        DocumentResultMetadata metadata;
    };

    struct Stats {
        size_t hit_count = 0;
        size_t miss_count = 0;
        size_t store_count = 0;
        size_t eviction_count = 0;
        size_t bucket_count = 0;
        size_t diagnostic_entry_count = 0;
        size_t semantic_token_entry_count = 0;
    };

    [[nodiscard]] std::optional<DiagnosticResult> currentDiagnostics(
        std::string_view uri,
        const DocumentResultMetadata& metadata) const;
    [[nodiscard]] DiagnosticResult storeDiagnostics(std::string uri,
                                                    nlohmann::json items,
                                                    DocumentResultMetadata metadata);

    [[nodiscard]] std::optional<SemanticTokenResult> currentSemanticTokens(
        std::string_view uri,
        const DocumentResultMetadata& metadata,
        std::uint64_t legend_version = kSemanticTokenLegendVersion) const;
    [[nodiscard]] std::optional<SemanticTokenResult> semanticTokensForResultId(
        std::string_view uri,
        std::string_view result_id,
        std::uint64_t legend_version = kSemanticTokenLegendVersion) const;
    [[nodiscard]] SemanticTokenResult storeSemanticTokens(
        std::string uri,
        std::vector<int> data,
        DocumentResultMetadata metadata,
        std::uint64_t legend_version = kSemanticTokenLegendVersion);

    void clearUri(std::string_view uri);
    void clear();
    [[nodiscard]] Stats stats() const;
    void resetStats();

private:
    struct Bucket {
        std::optional<DiagnosticResult> diagnostics;
        std::vector<SemanticTokenResult> semantic_tokens;
        std::uint64_t last_access = 0;
    };

    static std::string diagnosticResultId(std::string_view uri, const nlohmann::json& items);
    static std::string semanticTokenResultId(std::string_view uri,
                                             const std::vector<int>& data,
                                             std::uint64_t legend_version);
    static bool metadataMatches(const DocumentResultMetadata& lhs,
                                const DocumentResultMetadata& rhs);

    Bucket& touchBucketLocked(std::string_view uri);
    void evictLocked();
    void refreshStatsLocked() const;

    mutable std::mutex mutex_;
    mutable Stats stats_;
    mutable std::map<std::string, Bucket, std::less<>> buckets_;
    mutable std::uint64_t access_clock_ = 0;
};

} // namespace pristine::server
