#include "AffectedDependencyGraph.h"

#include <algorithm>
#include <set>

namespace pristine::analysis::semantic {
namespace {

void addUnique(std::vector<std::string>& values, std::string value) {
    values.push_back(std::move(value));
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void eraseValue(std::vector<std::string>& values, std::string_view value) {
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

void eraseFromReverseEdges(std::unordered_map<std::string, std::vector<std::string>>& edges,
                           std::string_view dependent_uri) {
    for (auto& [_, dependent_uris] : edges) {
        eraseValue(dependent_uris, dependent_uri);
    }
}

} // namespace

void AffectedDependencyGraph::clear() {
    includes_.clear();
    reverse_includes_.clear();
    reverse_semantic_dependencies_.clear();
}

void AffectedDependencyGraph::setIncludedUris(std::string uri, std::vector<std::string> included_uris) {
    eraseFromReverseEdges(reverse_includes_, uri);
    std::sort(included_uris.begin(), included_uris.end());
    included_uris.erase(std::unique(included_uris.begin(), included_uris.end()), included_uris.end());
    includes_[uri] = included_uris;
    for (const auto& included_uri : included_uris) {
        addUnique(reverse_includes_[included_uri], uri);
    }
}

void AffectedDependencyGraph::addSemanticDependency(std::string dependency_uri, std::string dependent_uri) {
    if (dependency_uri == dependent_uri) {
        return;
    }
    addUnique(reverse_semantic_dependencies_[std::move(dependency_uri)], std::move(dependent_uri));
}

void AffectedDependencyGraph::removeDocument(std::string_view uri) {
    includes_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_includes_, uri);
    reverse_includes_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_semantic_dependencies_, uri);
    reverse_semantic_dependencies_.erase(std::string(uri));
}

std::vector<std::string> AffectedDependencyGraph::includedUris(std::string_view uri) const {
    const auto include_it = includes_.find(std::string(uri));
    if (include_it == includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> AffectedDependencyGraph::includingUris(std::string_view uri) const {
    const auto include_it = reverse_includes_.find(std::string(uri));
    if (include_it == reverse_includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> AffectedDependencyGraph::affectedDocumentUris(std::string_view uri) const {
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::vector<std::string> pending{std::string(uri)};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        result.push_back(current);
        if (const auto reverse_it = reverse_includes_.find(current); reverse_it != reverse_includes_.end()) {
            pending.insert(pending.end(), reverse_it->second.begin(), reverse_it->second.end());
        }
        if (const auto semantic_reverse_it = reverse_semantic_dependencies_.find(current);
            semantic_reverse_it != reverse_semantic_dependencies_.end()) {
            pending.insert(pending.end(),
                           semantic_reverse_it->second.begin(),
                           semantic_reverse_it->second.end());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace pristine::analysis::semantic
