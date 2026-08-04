#include "DocumentClosurePlanner.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace pristine::analysis::semantic {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hashCombine(std::uint64_t& hash, std::string_view value) {
    for (const auto ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= kFnvPrime;
    }
}

void hashCombine(std::uint64_t& hash, std::uint64_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<unsigned char>((value >> (index * 8)) & 0xffu);
        hash *= kFnvPrime;
    }
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool caseInsensitivePrefix(std::string_view value, std::string_view prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return lowercase(value.substr(0, prefix.size())) == lowercase(prefix);
}

void sortAndDedupe(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void appendReason(std::vector<std::string>& reasons, std::string reason) {
    reasons.push_back(std::move(reason));
}

bool containsAny(const std::vector<std::string>& values, const std::set<std::string>& needles) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return needles.contains(value);
    });
}

void finalize(DocumentClosurePlan& plan) {
    sortAndDedupe(plan.uris);
    sortAndDedupe(plan.reasons);
    plan.fingerprint = kFnvOffsetBasis;
    hashCombine(plan.fingerprint, plan.root_key);
    hashCombine(plan.fingerprint, plan.complete ? 1ull : 0ull);
    for (const auto& uri : plan.uris) {
        hashCombine(plan.fingerprint, uri);
    }
    for (const auto& reason : plan.reasons) {
        hashCombine(plan.fingerprint, reason);
    }
}

struct RankedCandidate {
    const SemanticDiscoverySymbol* symbol = nullptr;
    int match_rank = 0;
    std::string normalized_name;
};

bool candidateLess(const RankedCandidate& lhs, const RankedCandidate& rhs) {
    const auto& left = *lhs.symbol;
    const auto& right = *rhs.symbol;
    return std::tie(lhs.match_rank,
                    left.kind,
                    lhs.normalized_name,
                    left.name,
                    left.location.uri,
                    left.location.range.start_line,
                    left.location.range.start_character,
                    left.location.range.end_line,
                    left.location.range.end_character) <
           std::tie(rhs.match_rank,
                    right.kind,
                    rhs.normalized_name,
                    right.name,
                    right.location.uri,
                    right.location.range.start_line,
                    right.location.range.start_character,
                    right.location.range.end_line,
                    right.location.range.end_character);
}

std::string candidateKey(const SemanticDiscoverySymbol& symbol) {
    return symbol.name + "\x1f" + symbol.kind + "\x1f" + symbol.location.uri + "\x1f" +
           std::to_string(symbol.location.range.start_line) + ":" +
           std::to_string(symbol.location.range.start_character) + ":" +
           std::to_string(symbol.location.range.end_line) + ":" +
           std::to_string(symbol.location.range.end_character);
}

} // namespace

DocumentClosurePlan planDocumentClosure(const SemanticWorkspaceDiscoverySnapshot& discovery,
                                        std::vector<std::string> roots,
                                        const std::vector<std::string>& configured_top_modules) {
    DocumentClosurePlan plan;
    sortAndDedupe(roots);
    for (const auto& root : roots) {
        if (!plan.root_key.empty()) {
            plan.root_key.push_back('\n');
        }
        plan.root_key.append(root);

        const auto closure = discovery.document_closures_by_uri.find(root);
        if (closure == discovery.document_closures_by_uri.end()) {
            appendReason(plan.reasons, "closure-not-discovered:" + root);
            continue;
        }
        if (!closure->second.complete || closure->second.uris.empty()) {
            plan.reasons.insert(plan.reasons.end(),
                                closure->second.reasons.begin(),
                                closure->second.reasons.end());
            if (closure->second.uris.empty()) {
                appendReason(plan.reasons, "closure-empty:" + root);
            }
            continue;
        }
        plan.uris.insert(plan.uris.end(), closure->second.uris.begin(), closure->second.uris.end());
        hashCombine(plan.fingerprint, closure->second.fingerprint);
    }

    for (const auto& top_module : configured_top_modules) {
        const auto closure = discovery.closure_uris_by_name.find(top_module);
        if (closure == discovery.closure_uris_by_name.end() || closure->second.empty()) {
            appendReason(plan.reasons, "configured-top-not-discovered:" + top_module);
            continue;
        }
        plan.uris.insert(plan.uris.end(), closure->second.begin(), closure->second.end());
    }

    plan.complete = plan.reasons.empty() && !plan.uris.empty();
    finalize(plan);
    return plan;
}

WorkspaceCompletionPlan planWorkspaceCompletionClosure(
    const SemanticWorkspaceDiscoverySnapshot& discovery,
    std::vector<std::string> roots,
    const std::vector<std::string>& configured_top_modules,
    std::string_view prefix) {
    WorkspaceCompletionPlan plan;
    plan.closure = planDocumentClosure(discovery, std::move(roots), configured_top_modules);
    if (!plan.closure.complete) {
        return plan;
    }

    std::vector<RankedCandidate> candidates;
    candidates.reserve(discovery.declarations.size());
    for (const auto& declaration : discovery.declarations) {
        const auto exact = declaration.name == prefix;
        const auto case_sensitive = declaration.name.starts_with(prefix);
        const auto insensitive = caseInsensitivePrefix(declaration.name, prefix);
        if (!insensitive) {
            continue;
        }
        candidates.push_back(RankedCandidate{.symbol = &declaration,
                                             .match_rank = exact ? 0 : (case_sensitive ? 1 : 2),
                                             .normalized_name = lowercase(declaration.name)});
    }
    std::sort(candidates.begin(), candidates.end(), candidateLess);
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
                         return candidateKey(*left.symbol) == candidateKey(*right.symbol);
                     }),
                     candidates.end());
    plan.matched_candidate_count = candidates.size();

    const auto candidate_limit = prefix.size() <= 1 ? kShortWorkspaceCompletionCandidateLimit
                                                     : kLongWorkspaceCompletionCandidateLimit;
    if (candidates.size() > candidate_limit) {
        plan.incomplete = true;
        plan.skipped_candidate_count += candidates.size() - candidate_limit;
        candidates.resize(candidate_limit);
    }
    plan.planned_candidate_count = candidates.size();

    std::set<std::string> selected(plan.closure.uris.begin(), plan.closure.uris.end());
    for (const auto& candidate : candidates) {
        const auto closure = discovery.document_closures_by_uri.find(candidate.symbol->location.uri);
        if (closure == discovery.document_closures_by_uri.end() || !closure->second.complete ||
            closure->second.uris.empty()) {
            plan.incomplete = true;
            ++plan.skipped_candidate_count;
            appendReason(plan.closure.reasons,
                         "workspace-candidate-closure-incomplete:" + candidate.symbol->location.uri);
            continue;
        }

        std::vector<std::string> additions;
        for (const auto& uri : closure->second.uris) {
            if (!selected.contains(uri)) {
                additions.push_back(uri);
            }
        }
        sortAndDedupe(additions);
        if (plan.added_document_count + additions.size() >
            kWorkspaceCompletionAdditionalDocumentLimit) {
            plan.incomplete = true;
            ++plan.skipped_candidate_count;
            appendReason(plan.closure.reasons,
                         "workspace-candidate-document-cap:" + candidate.symbol->location.uri);
            continue;
        }
        for (const auto& uri : additions) {
            selected.insert(uri);
        }
        plan.added_document_count += additions.size();
    }

    plan.closure.uris.assign(selected.begin(), selected.end());
    plan.closure.complete = true;
    finalize(plan.closure);
    hashCombine(plan.closure.fingerprint, kWorkspaceCompletionPolicyVersion);
    hashCombine(plan.closure.fingerprint, prefix);
    hashCombine(plan.closure.fingerprint, plan.matched_candidate_count);
    hashCombine(plan.closure.fingerprint, plan.planned_candidate_count);
    hashCombine(plan.closure.fingerprint, plan.skipped_candidate_count);
    hashCombine(plan.closure.fingerprint, plan.added_document_count);
    hashCombine(plan.closure.fingerprint, plan.incomplete ? 1ull : 0ull);
    return plan;
}

ReferenceCandidatePlan planReferenceCandidateClosure(
    const SemanticWorkspaceDiscoverySnapshot& discovery,
    std::vector<std::string> roots,
    const std::vector<std::string>& configured_top_modules,
    const ReferenceSearchSeed& seed) {
    ReferenceCandidatePlan result;
    result.closure = planDocumentClosure(discovery, std::move(roots), configured_top_modules);
    result.confidence = "complete";
    if (!result.closure.complete) {
        result.requires_full_snapshot = true;
        result.confidence = "incomplete-base-closure";
        return result;
    }
    if (!discovery.reference_candidate_incomplete_reasons.empty()) {
        result.closure.reasons.insert(result.closure.reasons.end(),
                                      discovery.reference_candidate_incomplete_reasons.begin(),
                                      discovery.reference_candidate_incomplete_reasons.end());
        appendReason(result.closure.reasons, "reference-candidate-index-incomplete");
        finalize(result.closure);
        result.requires_full_snapshot = true;
        result.confidence = "incomplete-discovery";
        return result;
    }
    if (!seed.complete || seed.stable_id.empty() || seed.spellings.empty()) {
        result.closure.reasons.insert(result.closure.reasons.end(), seed.reasons.begin(), seed.reasons.end());
        appendReason(result.closure.reasons, "reference-search-seed-incomplete");
        finalize(result.closure);
        result.requires_full_snapshot = true;
        result.confidence = "incomplete-seed";
        return result;
    }

    std::set<std::string> selected(result.closure.uris.begin(), result.closure.uris.end());
    std::set<std::string> candidate_roots;
    std::set<std::string> spellings(seed.spellings.begin(), seed.spellings.end());
    candidate_roots.insert(seed.declaration_uri);
    for (const auto& spelling : spellings) {
        const auto postings = discovery.reference_candidate_uris_by_name.find(spelling);
        if (postings != discovery.reference_candidate_uris_by_name.end()) {
            candidate_roots.insert(postings->second.begin(), postings->second.end());
        }
    }

    // Expand macro propagation transitively. Incomplete macro bodies are a full-snapshot preselection,
    // never a provider-side name recovery.
    std::set<std::string> relevant_macro_names;
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& definition : discovery.macro_definitions) {
            if (!containsAny(definition.body_identifiers, spellings) &&
                !containsAny(definition.body_identifiers, relevant_macro_names)) {
                continue;
            }
            if (!definition.complete) {
                result.closure.reasons.insert(result.closure.reasons.end(),
                                               definition.reasons.begin(),
                                               definition.reasons.end());
                appendReason(result.closure.reasons, "reference-macro-body-incomplete:" + definition.name);
                finalize(result.closure);
                result.requires_full_snapshot = true;
                result.confidence = "incomplete-macro-propagation";
                return result;
            }
            candidate_roots.insert(definition.uri);
            if (relevant_macro_names.insert(definition.name).second) {
                expanded = true;
            }
        }
    }
    for (const auto& macro_name : relevant_macro_names) {
        const auto invocations = discovery.macro_invocation_uris_by_name.find(macro_name);
        if (invocations != discovery.macro_invocation_uris_by_name.end()) {
            candidate_roots.insert(invocations->second.begin(), invocations->second.end());
        }
    }

    result.candidate_document_count = candidate_roots.size();
    for (const auto& root : candidate_roots) {
        if (root.empty()) {
            continue;
        }
        const auto closure = discovery.document_closures_by_uri.find(root);
        if (closure == discovery.document_closures_by_uri.end() || !closure->second.complete ||
            closure->second.uris.empty()) {
            appendReason(result.closure.reasons, "reference-candidate-closure-incomplete:" + root);
            if (closure != discovery.document_closures_by_uri.end()) {
                result.closure.reasons.insert(result.closure.reasons.end(),
                                               closure->second.reasons.begin(),
                                               closure->second.reasons.end());
            }
            finalize(result.closure);
            result.requires_full_snapshot = true;
            result.confidence = "incomplete-candidate-closure";
            return result;
        }
        selected.insert(closure->second.uris.begin(), closure->second.uris.end());
    }

    result.closure.uris.assign(selected.begin(), selected.end());
    result.closure.complete = !result.closure.uris.empty();
    result.selected_document_count = result.closure.uris.size();
    if (discovery.file_count != 0 && result.selected_document_count >= discovery.file_count) {
        result.requires_full_snapshot = true;
        result.confidence = "candidate-closure-covers-workspace";
    }
    finalize(result.closure);
    hashCombine(result.closure.fingerprint, kReferenceCandidatePolicyVersion);
    hashCombine(result.closure.fingerprint, seed.stable_id);
    for (const auto& spelling : spellings) {
        hashCombine(result.closure.fingerprint, spelling);
    }
    hashCombine(result.closure.fingerprint, result.candidate_document_count);
    hashCombine(result.closure.fingerprint, result.requires_full_snapshot ? 1ull : 0ull);
    return result;
}

} // namespace pristine::analysis::semantic
