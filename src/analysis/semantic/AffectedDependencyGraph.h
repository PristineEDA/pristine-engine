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
    SemanticExport,
    CallableType,
    InterfaceModport,
    MacroInclude,
    ModuleInstance,
    Config,
};

class AffectedDependencyGraph {
public:
    struct Stats {
        size_t documents_with_includes = 0;
        size_t include_edges = 0;
        size_t semantic_import_edges = 0;
        size_t semantic_export_edges = 0;
        size_t callable_type_edges = 0;
        size_t interface_modport_edges = 0;
        size_t macro_include_edges = 0;
        size_t module_instance_edges = 0;
        size_t config_edges = 0;
        size_t total_edges = 0;
    };

    struct Edge {
        AffectedDependencyEdgeKind kind = AffectedDependencyEdgeKind::Include;
        std::string dependency_uri;
        std::string dependent_uri;
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
    [[nodiscard]] std::vector<Edge> edges() const;
    [[nodiscard]] Stats stats() const;

private:
    std::unordered_map<std::string, std::vector<std::string>> includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_semantic_import_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_semantic_export_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_callable_type_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_interface_modport_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_macro_include_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_module_instance_dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_config_dependencies_;
};

} // namespace pristine::analysis::semantic
