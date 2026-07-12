#pragma once

#include "AffectedDependencyGraph.h"
#include "SignatureInlayProvider.h"
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
    std::string value_display;
};

struct SnapshotIndexedReference {
    std::string stable_id;
    std::string name;
    SemanticLocation location;
    bool is_declaration = false;
};

struct SnapshotMemberCompletion {
    std::string qualifier;
    SemanticSymbolIdentity identity;
    std::string type_display;
};

enum class SnapshotVisibilityOrigin {
    Local,
    ExplicitImport,
    WildcardImport,
    PackageExport,
    Workspace,
};

struct SnapshotVisibilityCandidate {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    SnapshotVisibilityOrigin origin = SnapshotVisibilityOrigin::Local;
};

struct SnapshotScopeVisibility {
    std::string stable_id;
    std::string uri;
    ParseRange range;
    int lexical_depth = 0;
    std::vector<SnapshotVisibilityCandidate> candidates;
};

struct SnapshotPackageVisibility {
    std::string package_name;
    std::string uri;
    std::vector<SnapshotVisibilityCandidate> candidates;
    std::vector<std::string> exported_packages;
};

struct SnapshotVisibleMacro {
    MacroDefinition definition;
    std::string source_uri;
    ParseRange available_after;
};

struct SnapshotModuleInstance {
    std::string module_name;
    std::string instance_name;
    std::string type_display;
    std::string target_stable_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
    std::vector<SchematicConnection> port_connections;
    std::vector<SchematicConnection> parameter_connections;
};

struct SnapshotModuleEntry {
    std::string uri;
    ModuleDefinition definition;
};

struct SnapshotAssignmentEdgeSeed {
    std::string uri;
    ParseRange scope_range;
    ParseRange assignment_range;
    ParseRange left_range;
    ParseRange right_range;
    std::string left_expression;
    std::string right_expression;
};

struct SnapshotAssignmentEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    SemanticLocation expression_location;
    std::string expression;
};

struct SnapshotTypeReference {
    SemanticLocation reference;
    std::string type_name;
    std::vector<SemanticLocation> definitions;
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
    std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>> member_completions_by_uri;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>>>
        member_completions_by_qualifier_by_uri;
    std::unordered_map<std::string, SnapshotMemberCompletion> member_completions_by_stable_id;
    std::unordered_map<std::string, std::vector<SnapshotScopeVisibility>> scope_visibility_by_uri;
    std::unordered_map<std::string, SnapshotPackageVisibility> package_visibility_by_name;
    std::vector<SnapshotVisibilityCandidate> workspace_visibility;
    std::unordered_map<std::string, std::vector<SnapshotVisibleMacro>> visible_macros_by_uri;
    size_t scope_visibility_count = 0;
    size_t package_visibility_count = 0;
    size_t member_visibility_count = 0;
    size_t callable_visibility_count = 0;
    std::int64_t scope_visibility_build_micros = 0;
    std::vector<SnapshotModuleEntry> module_entries;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> ast_module_signatures_by_name;
    std::vector<SnapshotAssignmentEdgeSeed> assignment_edge_seeds;
    std::unordered_map<std::string, std::vector<SnapshotAssignmentEdge>> assignment_edges_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotTypeReference>> type_references_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlayCall>> signature_calls_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
};

struct SnapshotBuildInput {
    std::uint64_t generation = 0;
    SemanticEngineConfig config;
    std::vector<std::string> dirty_document_uris;
    std::unordered_map<std::string, SemanticEngineDocument> documents;
};

struct SnapshotBuildInputSummary {
    size_t document_count = 0;
    size_t open_document_count = 0;
    size_t dirty_document_count = 0;
    size_t top_module_count = 0;
    size_t index_config_count = 0;
};

struct SnapshotBuildOutput {
    SemanticEngineSnapshot snapshot;
    std::unique_ptr<SnapshotData> data;
    AffectedDependencyGraph affected_dependencies;
};

[[nodiscard]] constexpr std::string_view snapshotBuilderProviderName() {
    return "SnapshotBuilder";
}

[[nodiscard]] SnapshotBuildInput normalizeSnapshotBuildInput(SnapshotBuildInput input);
[[nodiscard]] SnapshotBuildInputSummary snapshotBuildInputSummary(const SnapshotBuildInput& input);

class SnapshotBuilder {
public:
    [[nodiscard]] SnapshotBuildOutput build(SnapshotBuildInput input) const;
};

} // namespace pristine::analysis::semantic
