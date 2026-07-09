#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

enum class AffectedDependencyEdgeKind {
    Include,
    SemanticImport,
    ModuleInstance,
    Config,
};

class AffectedDependencyGraph {
public:
    struct Stats {
        size_t documents_with_includes = 0;
        size_t include_edges = 0;
        size_t semantic_import_edges = 0;
        size_t module_instance_edges = 0;
        size_t config_edges = 0;
        size_t total_edges = 0;
    };

    void clear();
    void setIncludedUris(std::string uri, std::vector<std::string> included_uris);
    void addSemanticDependency(std::string dependency_uri, std::string dependent_uri);
    void addSemanticDependency(AffectedDependencyEdgeKind kind,
                               std::string dependency_uri,
                               std::string dependent_uri);
    void removeDocument(std::string_view uri);

    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> dependentUris(std::string_view uri,
                                                         AffectedDependencyEdgeKind kind) const;
    [[nodiscard]] std::vector<std::string> affectedDocumentUris(std::string_view uri) const;
    [[nodiscard]] Stats stats() const;

private:
    std::unordered_map<std::string, std::vector<std::string>> includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_semantic_import_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_module_instance_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_config_dependencies_;
};

} // namespace pristine::analysis::semantic
