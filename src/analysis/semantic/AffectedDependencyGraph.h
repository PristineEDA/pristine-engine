#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

class AffectedDependencyGraph {
public:
    void clear();
    void setIncludedUris(std::string uri, std::vector<std::string> included_uris);
    void addSemanticDependency(std::string dependency_uri, std::string dependent_uri);
    void removeDocument(std::string_view uri);

    [[nodiscard]] std::vector<std::string> includedUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> includingUris(std::string_view uri) const;
    [[nodiscard]] std::vector<std::string> affectedDocumentUris(std::string_view uri) const;

    [[nodiscard]] const std::unordered_map<std::string, std::vector<std::string>>& includes() const {
        return includes_;
    }

    [[nodiscard]] const std::unordered_map<std::string, std::vector<std::string>>& reverseIncludes() const {
        return reverse_includes_;
    }

    [[nodiscard]] const std::unordered_map<std::string, std::vector<std::string>>& reverseSemanticDependencies() const {
        return reverse_semantic_dependencies_;
    }

private:
    std::unordered_map<std::string, std::vector<std::string>> includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes_;
    std::unordered_map<std::string, std::vector<std::string>> reverse_semantic_dependencies_;
};

} // namespace pristine::analysis::semantic
