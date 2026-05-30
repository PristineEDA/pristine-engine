#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang {
class SourceManager;
}

namespace slang::ast {
class Compilation;
class Symbol;
}

namespace slang::syntax {
class SyntaxTree;
}

namespace pristine::analysis::semantic {

struct SnapshotIndexedSymbol {
    SemanticSymbolIdentity identity;
    const slang::ast::Symbol* symbol = nullptr;
    std::string type_display;
};

struct SnapshotIndexedReference {
    std::string stable_id;
    std::string name;
    SemanticLocation location;
    bool is_declaration = false;
};

struct SnapshotModuleInstance {
    std::string module_name;
    std::string instance_name;
    std::string target_stable_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
};

struct SnapshotModuleEntry {
    std::string uri;
    ModuleDefinition definition;
};

struct SemanticModuleSignature {
    ModuleDefinition definition;
    ModuleSchematic schematic;
    std::string uri;
};

struct SnapshotData {
    SnapshotData();
    ~SnapshotData();
    SnapshotData(SnapshotData&&) noexcept;
    SnapshotData& operator=(SnapshotData&&) noexcept;
    SnapshotData(const SnapshotData&) = delete;
    SnapshotData& operator=(const SnapshotData&) = delete;

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, SnapshotIndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<SnapshotIndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
    std::vector<SnapshotModuleEntry> module_entries;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, ModuleSchematic> schematics_by_name;
    std::unordered_map<std::string, std::string> schematic_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> ast_module_signatures_by_name;
    std::unordered_map<std::string, std::vector<ContinuousAssignment>> assignments_by_uri;
    std::unordered_map<std::string, std::vector<Identifier>> identifiers_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SemanticSymbolMetadata>> metadata_by_uri;
};

struct SnapshotBuildInput {
    std::uint64_t generation = 0;
    SemanticEngineConfig config;
    std::vector<std::string> dirty_document_uris;
    std::unordered_map<std::string, SemanticEngineDocument> documents;
};

struct SnapshotBuildOutput {
    SemanticEngineSnapshot snapshot;
    std::unique_ptr<SnapshotData> data;
    std::unordered_map<std::string, std::vector<std::string>> includes;
    std::unordered_map<std::string, std::vector<std::string>> reverse_includes;
};

[[nodiscard]] constexpr std::string_view snapshotBuilderProviderName() {
    return "SnapshotBuilder";
}

class SnapshotBuilder {
public:
    [[nodiscard]] SnapshotBuildOutput build(SnapshotBuildInput input) const;
};

} // namespace pristine::analysis::semantic
