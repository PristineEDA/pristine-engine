#pragma once

#include "pristine/analysis/CompilationService.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct DiscoveryDocumentInput {
    std::string uri;
    std::string text;
};

struct DiscoveryLocation {
    std::string uri;
    ParseRange range;
};

struct DiscoverySymbol {
    std::string name;
    std::string kind;
    DiscoveryLocation location;
};

struct DiscoveryFile {
    std::string uri;
    size_t byte_count = 0;
    std::vector<DiscoverySymbol> declarations;
    std::vector<std::string> referenced_top_level_names;
    std::vector<std::string> included_uris;
};

struct WorkspaceDiscoveryIndex {
    std::uint64_t generation = 0;
    size_t file_count = 0;
    size_t byte_count = 0;
    size_t declaration_count = 0;
    size_t macro_count = 0;
    size_t reference_count = 0;
    std::vector<DiscoveryFile> files;
    std::vector<DiscoverySymbol> declarations;
    std::vector<DiscoverySymbol> macros;
    std::unordered_map<std::string, std::vector<DiscoverySymbol>> declarations_by_name;
    std::unordered_map<std::string, std::vector<std::string>> files_by_declaration;
    std::unordered_map<std::string, std::vector<std::string>> referenced_files_by_name;
    std::vector<std::string> messages;
};

[[nodiscard]] WorkspaceDiscoveryIndex buildWorkspaceDiscoveryIndex(
    std::uint64_t generation,
    std::vector<DiscoveryDocumentInput> documents);

[[nodiscard]] std::vector<std::string> discoveryDependencyClosure(
    const WorkspaceDiscoveryIndex& index,
    std::optional<std::string_view> top_name = std::nullopt,
    size_t max_files = 0);

} // namespace pristine::analysis::semantic
