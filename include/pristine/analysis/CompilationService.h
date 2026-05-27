#pragma once

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

struct DocumentSymbol {
    std::string name;
    int kind = 0;
    ParseRange range;
    ParseRange selection_range;
    std::vector<DocumentSymbol> children;
};

struct ModuleInstantiation {
    std::string module_name;
    std::string instance_name;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
};

struct ModuleDefinition {
    std::string name;
    ParseRange range;
    ParseRange selection_range;
    std::vector<std::string> ports;
    std::vector<ModuleInstantiation> instances;
};

struct HoverResult {
    std::string contents;
    ParseRange range;
};

struct Identifier {
    std::string name;
    ParseRange range;
};

struct IncludeDirective {
    std::string target;
    ParseRange range;
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
    [[nodiscard]] std::vector<ModuleDefinition> moduleDefinitions(std::string_view text,
                                                                  std::string_view uri) const;
    [[nodiscard]] std::optional<HoverResult> hover(std::string_view text,
                                                   std::string_view uri,
                                                   int line,
                                                   int character) const;
    [[nodiscard]] std::vector<Identifier> identifiers(std::string_view text) const;
    [[nodiscard]] std::optional<Identifier> identifierAt(std::string_view text,
                                                         int line,
                                                         int character) const;
    [[nodiscard]] std::string completionPrefix(std::string_view text,
                                               int line,
                                               int character) const;
    [[nodiscard]] std::vector<IncludeDirective> includeDirectives(std::string_view text) const;
};

} // namespace pristine::analysis
