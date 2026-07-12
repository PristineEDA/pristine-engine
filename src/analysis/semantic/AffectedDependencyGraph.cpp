#include "AffectedDependencyGraph.h"

#include <algorithm>
#include <array>
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

size_t edgeCount(const std::unordered_map<std::string, std::vector<std::string>>& edges) {
    size_t result = 0;
    for (const auto& [_, dependent_uris] : edges) {
        result += dependent_uris.size();
    }
    return result;
}

} // namespace

void AffectedDependencyGraph::clear() {
    includes_.clear();
    reverse_includes_.clear();
    reverse_semantic_import_dependencies_.clear();
    reverse_semantic_export_dependencies_.clear();
    reverse_module_instance_dependencies_.clear();
    reverse_config_dependencies_.clear();
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
    addSemanticDependency(AffectedDependencyEdgeKind::SemanticImport,
                          std::move(dependency_uri),
                          std::move(dependent_uri));
}

void AffectedDependencyGraph::addSemanticDependency(AffectedDependencyEdgeKind kind,
                                                   std::string dependency_uri,
                                                   std::string dependent_uri) {
    if (dependency_uri == dependent_uri) {
        return;
    }
    switch (kind) {
    case AffectedDependencyEdgeKind::SemanticImport:
        addUnique(reverse_semantic_import_dependencies_[std::move(dependency_uri)],
                  std::move(dependent_uri));
        return;
    case AffectedDependencyEdgeKind::SemanticExport:
        addUnique(reverse_semantic_export_dependencies_[std::move(dependency_uri)],
                  std::move(dependent_uri));
        return;
    case AffectedDependencyEdgeKind::ModuleInstance:
        addUnique(reverse_module_instance_dependencies_[std::move(dependency_uri)],
                  std::move(dependent_uri));
        return;
    case AffectedDependencyEdgeKind::Config:
        addUnique(reverse_config_dependencies_[std::move(dependency_uri)], std::move(dependent_uri));
        return;
    case AffectedDependencyEdgeKind::Include:
        addUnique(reverse_includes_[std::move(dependency_uri)], std::move(dependent_uri));
        return;
    }
}

void AffectedDependencyGraph::removeDocument(std::string_view uri) {
    includes_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_includes_, uri);
    reverse_includes_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_semantic_import_dependencies_, uri);
    reverse_semantic_import_dependencies_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_semantic_export_dependencies_, uri);
    reverse_semantic_export_dependencies_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_module_instance_dependencies_, uri);
    reverse_module_instance_dependencies_.erase(std::string(uri));
    eraseFromReverseEdges(reverse_config_dependencies_, uri);
    reverse_config_dependencies_.erase(std::string(uri));
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

std::vector<std::string> AffectedDependencyGraph::dependentUris(
    std::string_view uri,
    AffectedDependencyEdgeKind kind) const {
    const auto* edges = &reverse_semantic_import_dependencies_;
    switch (kind) {
    case AffectedDependencyEdgeKind::Include:
        edges = &reverse_includes_;
        break;
    case AffectedDependencyEdgeKind::SemanticImport:
        edges = &reverse_semantic_import_dependencies_;
        break;
    case AffectedDependencyEdgeKind::SemanticExport:
        edges = &reverse_semantic_export_dependencies_;
        break;
    case AffectedDependencyEdgeKind::ModuleInstance:
        edges = &reverse_module_instance_dependencies_;
        break;
    case AffectedDependencyEdgeKind::Config:
        edges = &reverse_config_dependencies_;
        break;
    }
    const auto edge_it = edges->find(std::string(uri));
    if (edge_it == edges->end()) {
        return {};
    }
    return edge_it->second;
}

std::vector<std::string> AffectedDependencyGraph::affectedDocumentUris(std::string_view uri) const {
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::vector<std::string> pending{std::string(uri)};
    const std::array<const std::unordered_map<std::string, std::vector<std::string>>*, 5> reverse_edges{
        &reverse_includes_,
        &reverse_semantic_import_dependencies_,
        &reverse_semantic_export_dependencies_,
        &reverse_module_instance_dependencies_,
        &reverse_config_dependencies_,
    };
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        result.push_back(current);
        for (const auto* edges : reverse_edges) {
            if (const auto reverse_it = edges->find(current); reverse_it != edges->end()) {
                pending.insert(pending.end(), reverse_it->second.begin(), reverse_it->second.end());
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<AffectedDependencyGraph::Edge> AffectedDependencyGraph::edges() const {
    std::vector<Edge> result;
    const auto append_edges = [&](AffectedDependencyEdgeKind kind,
                                  const std::unordered_map<std::string, std::vector<std::string>>& edges) {
        for (const auto& [dependency_uri, dependent_uris] : edges) {
            for (const auto& dependent_uri : dependent_uris) {
                result.push_back(Edge{.kind = kind,
                                      .dependency_uri = dependency_uri,
                                      .dependent_uri = dependent_uri});
            }
        }
    };

    append_edges(AffectedDependencyEdgeKind::Include, reverse_includes_);
    append_edges(AffectedDependencyEdgeKind::SemanticImport, reverse_semantic_import_dependencies_);
    append_edges(AffectedDependencyEdgeKind::SemanticExport, reverse_semantic_export_dependencies_);
    append_edges(AffectedDependencyEdgeKind::ModuleInstance, reverse_module_instance_dependencies_);
    append_edges(AffectedDependencyEdgeKind::Config, reverse_config_dependencies_);
    std::sort(result.begin(),
              result.end(),
              [](const Edge& left, const Edge& right) {
                  const auto left_kind = static_cast<int>(left.kind);
                  const auto right_kind = static_cast<int>(right.kind);
                  if (left_kind != right_kind) {
                      return left_kind < right_kind;
                  }
                  if (left.dependency_uri != right.dependency_uri) {
                      return left.dependency_uri < right.dependency_uri;
                  }
                  return left.dependent_uri < right.dependent_uri;
              });
    return result;
}

AffectedDependencyGraph::Stats AffectedDependencyGraph::stats() const {
    Stats result;
    result.documents_with_includes = includes_.size();
    result.include_edges = edgeCount(reverse_includes_);
    result.semantic_import_edges = edgeCount(reverse_semantic_import_dependencies_);
    result.semantic_export_edges = edgeCount(reverse_semantic_export_dependencies_);
    result.module_instance_edges = edgeCount(reverse_module_instance_dependencies_);
    result.config_edges = edgeCount(reverse_config_dependencies_);
    result.total_edges = result.include_edges + result.semantic_import_edges +
                         result.semantic_export_edges +
                         result.module_instance_edges + result.config_edges;
    return result;
}

} // namespace pristine::analysis::semantic
