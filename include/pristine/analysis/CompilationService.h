#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

namespace pristine::analysis {

struct ParseRange {
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

struct ParseDiagnostic {
    std::string code;
    std::string message;
    ParseRange range;
    int severity = 1;
    bool is_error = false;
};

struct OutlineMetadata {
    std::string detail;
    std::string declaration;
    std::string type;
    std::string direction;
    std::string value;
    std::string module_name;
};

struct DocumentSymbol {
    std::string name;
    int kind = 0;
    ParseRange range;
    ParseRange selection_range;
    OutlineMetadata metadata;
    std::vector<DocumentSymbol> children;
};

struct OutlineOptions {
    int max_depth = 8;
    size_t limit = 2000;
    bool include_children = true;
    bool include_flat = true;
};

struct OutlineItem {
    std::string id;
    std::optional<std::string> parent_id;
    std::string name;
    std::string kind;
    int symbol_kind = 0;
    ParseRange range;
    ParseRange selection_range;
    int depth = 0;
    OutlineMetadata metadata;
    std::vector<OutlineItem> children;
};

struct OutlineResult {
    std::string uri;
    int version = 0;
    std::uint64_t generation = 0;
    std::vector<OutlineItem> roots;
    std::vector<OutlineItem> items;
    bool partial = false;
    bool truncated = false;
    std::vector<std::string> messages;
};

struct ModuleInstantiation {
    std::string module_name;
    std::string instance_name;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
};

struct SchematicPort {
    std::string name;
    std::string direction;
    std::string width_text;
    ParseRange range;
    ParseRange selection_range;
};

struct ModuleDefinition {
    std::string name;
    std::string kind = "module";
    ParseRange range;
    ParseRange selection_range;
    std::vector<std::string> ports;
    std::vector<SchematicPort> port_details;
    std::vector<SchematicPort> parameter_details;
    std::vector<ModuleInstantiation> instances;
};

struct SchematicConnection {
    std::string port_name;
    int port_index = -1;
    std::string signal;
    ParseRange range;
};

struct SchematicCell {
    std::string id;
    std::string name;
    std::string type;
    std::string kind;
    ParseRange range;
    ParseRange selection_range;
    std::vector<SchematicConnection> connections;
};

struct ModuleSchematic {
    std::string name;
    ParseRange range;
    ParseRange selection_range;
    std::vector<SchematicPort> ports;
    std::vector<SchematicCell> cells;
};

struct ContinuousAssignment {
    std::string left_expression;
    std::string right_expression;
    ParseRange range;
    ParseRange left_range;
    ParseRange right_range;
};

struct HoverResult {
    std::string contents;
    ParseRange range;
};

struct IncludeDirective {
    std::string target;
    ParseRange range;
};

struct MacroDefinition {
    std::string name;
    std::vector<std::string> parameters;
    std::string body;
    ParseRange range;
    ParseRange selection_range;
    bool function_like = false;
};

struct PackageImport {
    std::string package_name;
    std::optional<std::string> item_name;
    ParseRange package_range;
    ParseRange range;
};

struct PackageExport {
    std::string package_name;
    std::optional<std::string> item_name;
    ParseRange package_range;
    ParseRange range;
};

struct LexicalIdentifierScan {
    std::vector<std::string> names;
    bool complete = true;
    std::vector<std::string> reasons;
};

struct ParseResult {
    std::shared_ptr<slang::syntax::SyntaxTree> syntax_tree;
    std::vector<ParseDiagnostic> diagnostics;
    bool has_errors = false;
};

class CompilationService {
public:
    [[nodiscard]] ParseResult parse(std::string_view text, std::string_view uri) const;
    [[nodiscard]] std::vector<DocumentSymbol> documentSymbols(std::string_view text,
                                                              std::string_view uri) const;
    [[nodiscard]] OutlineResult outline(std::string_view text,
                                        std::string_view uri,
                                        int version,
                                        std::uint64_t generation,
                                        OutlineOptions options = {}) const;
    [[nodiscard]] std::vector<ModuleDefinition> moduleDefinitions(std::string_view text,
                                                                  std::string_view uri) const;
    [[nodiscard]] std::optional<HoverResult> hover(std::string_view text,
                                                   std::string_view uri,
                                                   int line,
                                                   int character) const;
    [[nodiscard]] std::string completionPrefix(std::string_view text,
                                               int line,
                                               int character) const;
    [[nodiscard]] std::vector<IncludeDirective> includeDirectives(std::string_view text) const;
    [[nodiscard]] std::vector<MacroDefinition> macroDefinitions(std::string_view text) const;
    [[nodiscard]] std::vector<PackageImport> packageImports(std::string_view text) const;
    [[nodiscard]] std::vector<PackageExport> packageExports(std::string_view text) const;
    [[nodiscard]] LexicalIdentifierScan lexicalIdentifiers(std::string_view text) const;
};

} // namespace pristine::analysis
