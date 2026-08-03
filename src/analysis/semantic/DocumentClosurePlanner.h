#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::analysis::semantic {

inline constexpr std::string_view kWorkspaceCompletionPolicyVersion =
    "workspace-completion-closure-v1";
inline constexpr size_t kShortWorkspaceCompletionCandidateLimit = 64;
inline constexpr size_t kLongWorkspaceCompletionCandidateLimit = 128;
inline constexpr size_t kWorkspaceCompletionAdditionalDocumentLimit = 256;

struct DocumentClosurePlan {
    std::vector<std::string> uris;
    std::vector<std::string> reasons;
    std::string root_key;
    std::uint64_t fingerprint = 0;
    bool complete = false;
};

struct WorkspaceCompletionPlan {
    DocumentClosurePlan closure;
    size_t matched_candidate_count = 0;
    size_t planned_candidate_count = 0;
    size_t skipped_candidate_count = 0;
    size_t added_document_count = 0;
    bool incomplete = false;
};

[[nodiscard]] DocumentClosurePlan planDocumentClosure(
    const SemanticWorkspaceDiscoverySnapshot& discovery,
    std::vector<std::string> roots,
    const std::vector<std::string>& configured_top_modules);

[[nodiscard]] WorkspaceCompletionPlan planWorkspaceCompletionClosure(
    const SemanticWorkspaceDiscoverySnapshot& discovery,
    std::vector<std::string> roots,
    const std::vector<std::string>& configured_top_modules,
    std::string_view prefix);

} // namespace pristine::analysis::semantic
