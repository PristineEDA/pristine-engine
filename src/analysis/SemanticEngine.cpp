#include "pristine/analysis/SemanticEngine.h"

#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace pristine::analysis {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUnknownIncludeDiagnosticCode = "unknownInclude";
constexpr std::string_view kUnresolvedModuleDiagnosticCode = "unresolvedModule";
constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";

std::optional<fs::path> resolveIncludeTarget(std::string_view workspace_root_uri,
                                             std::string_view document_uri,
                                             std::string_view target);
std::optional<fs::path> proposedIncludeTarget(std::string_view workspace_root_uri,
                                              std::string_view document_uri,
                                              std::string_view target);

int toLspSeverity(slang::DiagnosticSeverity severity) {
    switch (severity) {
        case slang::DiagnosticSeverity::Fatal:
        case slang::DiagnosticSeverity::Error:
            return 1;
        case slang::DiagnosticSeverity::Warning:
            return 2;
        case slang::DiagnosticSeverity::Note:
            return 3;
        case slang::DiagnosticSeverity::Ignored:
            return 4;
    }
    return 1;
}

std::string diagnosticUri(const slang::SourceManager& source_manager,
                          const slang::Diagnostic& diagnostic) {
    if (!diagnostic.location.valid()) {
        return {};
    }

    const auto location = source_manager.getFullyOriginalLoc(diagnostic.location);
    const auto path = source_manager.getFullPath(location.buffer());
    if (!path.empty()) {
        return pathToFileUri(path);
    }

    const auto file_name = source_manager.getFileName(location);
    if (file_name.empty()) {
        return {};
    }
    return withoutTrailingSlash(normalizeFileUri(file_name));
}

slang::Bag makeCompilationOptions() {
    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;
    compilation_options.flags |= slang::ast::CompilationFlags::IgnoreUnknownModules;
    compilation_options.flags |= slang::ast::CompilationFlags::AllowUseBeforeDeclare;
    compilation_options.errorLimit = 0;

    slang::Bag options;
    options.set(compilation_options);
    return options;
}

bool containsPosition(const ParseRange& range, int line, int character) {
    if (line < range.start_line || line > range.end_line) {
        return false;
    }
    if (line == range.start_line && character < range.start_character) {
        return false;
    }
    if (line == range.end_line && character >= range.end_character) {
        return false;
    }
    return true;
}

int comparePosition(int lhs_line, int lhs_character, int rhs_line, int rhs_character) {
    if (lhs_line != rhs_line) {
        return lhs_line < rhs_line ? -1 : 1;
    }
    if (lhs_character == rhs_character) {
        return 0;
    }
    return lhs_character < rhs_character ? -1 : 1;
}

bool rangesIntersect(const ParseRange& target, const ParseRange& range) {
    if (range.start_line == range.end_line && range.start_character == range.end_character) {
        return containsPosition(target, range.start_line, range.start_character);
    }
    return comparePosition(target.end_line,
                           target.end_character,
                           range.start_line,
                           range.start_character) > 0 &&
           comparePosition(range.end_line,
                           range.end_character,
                           target.start_line,
                           target.start_character) > 0;
}

ParseRange endOfTextRange(std::string_view text) {
    int line = 0;
    int character = 0;
    for (const char value : text) {
        if (value == '\n') {
            ++line;
            character = 0;
            continue;
        }
        if (value != '\r') {
            ++character;
        }
    }
    return ParseRange{.start_line = line,
                      .start_character = character,
                      .end_line = line,
                      .end_character = character};
}

bool locationLess(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    if (lhs.uri != rhs.uri) {
        return lhs.uri < rhs.uri;
    }
    if (lhs.range.start_line != rhs.range.start_line) {
        return lhs.range.start_line < rhs.range.start_line;
    }
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.range.end_line != rhs.range.end_line) {
        return lhs.range.end_line < rhs.range.end_line;
    }
    return lhs.range.end_character < rhs.range.end_character;
}

bool sameLocation(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    return lhs.uri == rhs.uri && lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line && lhs.range.end_character == rhs.range.end_character;
}

std::string symbolKindName(slang::ast::SymbolKind kind) {
    return std::string(slang::ast::toString(kind));
}

std::string symbolStableId(const slang::SourceManager& source_manager,
                           const slang::ast::Symbol& symbol,
                           const SemanticLocation& location) {
    std::string path = symbol.getLexicalPath();
    if (path.empty()) {
        path = symbol.getHierarchicalPath();
    }
    if (path.empty()) {
        path = std::string(symbol.name);
    }

    return location.uri + "|" + path + "|" + symbolKindName(symbol.kind) + "|" +
           std::to_string(location.range.start_line) + ":" +
           std::to_string(location.range.start_character) + ":" +
           std::to_string(source_manager.getFullyOriginalLoc(symbol.location).offset());
}

std::string locationUriForSourceLocation(const slang::SourceManager& source_manager,
                                         slang::SourceLocation location) {
    if (!location.valid()) {
        return {};
    }
    const auto original = source_manager.getFullyOriginalLoc(location);
    const auto path = source_manager.getFullPath(original.buffer());
    if (!path.empty()) {
        return pathToFileUri(path);
    }
    const auto file_name = source_manager.getFileName(original);
    return file_name.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(file_name));
}

std::optional<SemanticLocation> locationForSourceRange(const slang::SourceManager& source_manager,
                                                       slang::SourceRange range) {
    if (!range.start().valid()) {
        return std::nullopt;
    }
    const auto uri = locationUriForSourceLocation(source_manager, range.start());
    if (uri.empty()) {
        return std::nullopt;
    }
    return SemanticLocation{.uri = uri, .range = sourceRangeForSourceRange(source_manager, range)};
}

std::optional<SemanticLocation> declarationLocationForSymbol(const slang::SourceManager& source_manager,
                                                             const slang::ast::Symbol& symbol) {
    if (!symbol.location.valid() || symbol.name.empty()) {
        return std::nullopt;
    }
    const auto original = source_manager.getFullyOriginalLoc(symbol.location);
    const auto uri = locationUriForSourceLocation(source_manager, original);
    if (uri.empty()) {
        return std::nullopt;
    }
    const auto name_length = static_cast<int>(symbol.name.size());
    auto range = sourceRangeForSourceRange(source_manager,
                                           slang::SourceRange(original, original + name_length));
    return SemanticLocation{.uri = uri, .range = range};
}

bool rangesOverlapOrTouch(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.end_line < rhs.start_line || rhs.end_line < lhs.start_line) {
        return false;
    }
    if (lhs.end_line == rhs.start_line && lhs.end_character < rhs.start_character) {
        return false;
    }
    if (rhs.end_line == lhs.start_line && rhs.end_character < lhs.start_character) {
        return false;
    }
    return true;
}

bool rangeContainsRange(const ParseRange& outer, const ParseRange& inner) {
    if (inner.start_line < outer.start_line || inner.end_line > outer.end_line) {
        return false;
    }
    if (inner.start_line == outer.start_line && inner.start_character < outer.start_character) {
        return false;
    }
    if (inner.end_line == outer.end_line && inner.end_character > outer.end_character) {
        return false;
    }
    return true;
}

bool rangeLessWideFirst(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.start_line) {
        return lhs.start_line < rhs.start_line;
    }
    if (lhs.start_character != rhs.start_character) {
        return lhs.start_character < rhs.start_character;
    }
    if (lhs.end_line != rhs.end_line) {
        return lhs.end_line > rhs.end_line;
    }
    return lhs.end_character > rhs.end_character;
}

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

bool isValidIdentifier(std::string_view value) {
    if (value.empty() || !isIdentifierStart(value.front())) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), isIdentifierContinue);
}

std::string unknownIncludeMessage(std::string_view target) {
    return std::string("Include file '") + std::string(target) + "' could not be resolved.";
}

std::string unresolvedModuleMessage(std::string_view module_name) {
    return std::string("Module '") + std::string(module_name) + "' could not be resolved.";
}

std::string unresolvedTypeMessage(std::string_view name) {
    return std::string("Type '") + std::string(name) + "' could not be resolved.";
}

std::string duplicateSymbolMessage(std::string_view name) {
    return std::string("Duplicate symbol '") + std::string(name) + "' in the same scope.";
}

std::string ambiguousReferenceMessage(std::string_view name, size_t definition_count) {
    return std::string("Symbol '") + std::string(name) + "' has " +
           std::to_string(definition_count) + " possible definitions in scope.";
}

std::string unresolvedPackageMessage(std::string_view name) {
    return std::string("Package '") + std::string(name) + "' could not be resolved.";
}

std::string widthMismatchMessage(std::string_view left_name,
                                 std::int64_t left_width,
                                 std::string_view right_name,
                                 std::int64_t right_width) {
    return std::string("Width mismatch: assigning ") + std::to_string(right_width) + "-bit '" +
           std::string(right_name) + "' to " + std::to_string(left_width) + "-bit '" +
           std::string(left_name) + "'.";
}

bool isBuiltinTypeName(std::string_view name) {
    return name == "bit" || name == "logic" || name == "reg" || name == "wire" || name == "tri" ||
           name == "byte" || name == "shortint" || name == "int" || name == "integer" ||
           name == "longint" || name == "time" || name == "genvar";
}

bool isTypeDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "TypeAlias" || kind == "Type" || kind == "ClassType" ||
           kind == "EnumType" || kind == "Interface" || kind == "Modport";
}

bool isModuleDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "Instance" || kind == "InstanceBody";
}

std::string moduleStubInsertionText(std::string_view text, std::string_view module_name) {
    std::string insertion = text.empty() || text.back() == '\n' ? "\n" : "\n\n";
    insertion += "module ";
    insertion += module_name;
    insertion += ";\nendmodule\n";
    return insertion;
}

std::string typedefSkeletonInsertionText(std::string_view text, std::string_view type_name) {
    std::string insertion = text.empty() || text.back() == '\n' ? "\n" : "\n\n";
    insertion += "typedef logic ";
    insertion += type_name;
    insertion += ";\n";
    return insertion;
}

std::string missingPortConnectionText(const std::vector<SchematicPort>& ports,
                                      bool has_existing_connections) {
    std::string text = has_existing_connections ? ", " : "";
    for (size_t index = 0; index < ports.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        text += ".";
        text += ports[index].name;
        text += "(";
        text += ports[index].name;
        text += ")";
    }
    return text;
}

bool startsWithInsensitive(std::string_view prefix, std::string_view candidate) {
    if (prefix.size() > candidate.size()) {
        return false;
    }
    for (size_t index = 0; index < prefix.size(); ++index) {
        const auto lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
        const auto rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[index])));
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

bool fuzzyMatch(std::string_view query, std::string_view candidate) {
    if (query.empty()) {
        return true;
    }

    auto query_it = query.begin();
    for (auto candidate_it = candidate.begin();
         query_it != query.end() && candidate_it != candidate.end(); ++candidate_it) {
        const auto query_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*query_it)));
        const auto candidate_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*candidate_it)));
        if (query_char == candidate_char) {
            ++query_it;
        }
    }

    return query_it == query.end();
}

int completionKindForSemanticKind(std::string_view kind) {
    if (kind == "Definition") {
        return 9;
    }
    if (kind == "ClassType") {
        return 7;
    }
    if (kind == "EnumType") {
        return 13;
    }
    if (kind == "Interface" || kind == "Modport") {
        return 8;
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return 3;
    }
    if (kind == "Net" || kind == "Variable" || kind == "Field" || kind == "Member") {
        return 6;
    }
    if (kind == "Parameter") {
        return 21;
    }
    if (kind == "EnumValue") {
        return 20;
    }
    if (kind == "TypeAlias" || kind == "Type") {
        return 25;
    }
    return 18;
}

std::string completionDetailForSemanticKind(std::string_view kind) {
    if (kind == "Definition") {
        return "Module";
    }
    if (kind == "Package") {
        return "Package";
    }
    if (kind == "ClassType") {
        return "Class";
    }
    if (kind == "EnumType") {
        return "Enum";
    }
    if (kind == "Interface" || kind == "Modport") {
        return "Interface / Modport";
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return "Callable";
    }
    if (kind == "Net" || kind == "Variable" || kind == "Field" || kind == "Member") {
        return "Variable";
    }
    if (kind == "Parameter") {
        return "Parameter";
    }
    if (kind == "EnumValue") {
        return "Enum Member";
    }
    if (kind == "TypeAlias" || kind == "Type") {
        return "Typedef";
    }
    return std::string(kind);
}

int completionPriorityForDetail(std::string_view detail) {
    if (detail == "Variable" || detail == "Parameter" || detail == "Enum Member" ||
        detail == "Callable") {
        return 0;
    }
    if (detail == "Typedef" || detail == "Class" || detail == "Enum") {
        return 1;
    }
    if (detail == "Module" || detail == "Instance" || detail == "Interface / Modport") {
        return 2;
    }
    return 3;
}

int lspSymbolKindForSemanticKind(std::string_view kind) {
    if (kind == "Package" || kind == "Namespace") {
        return 4;
    }
    if (kind == "ClassType") {
        return 5;
    }
    if (kind == "EnumType") {
        return 10;
    }
    if (kind == "Interface" || kind == "Modport") {
        return 11;
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return 12;
    }
    if (kind == "Definition") {
        return 2;
    }
    if (kind == "TypeAlias" || kind == "Type") {
        return 26;
    }
    if (kind == "Parameter") {
        return 14;
    }
    if (kind == "EnumValue") {
        return 22;
    }
    if (kind == "Net" || kind == "Variable" || kind == "Field" || kind == "Member") {
        return 13;
    }
    return 0;
}

std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                  int line,
                                                  int character,
                                                  std::string_view prefix) {
    const auto offset = utf8OffsetAtUtf16Position(text, line, character);
    if (!offset.has_value() || *offset < prefix.size()) {
        return std::nullopt;
    }
    return *offset - prefix.size();
}

bool hasOnlyWhitespaceSinceLineStart(std::string_view text, size_t offset) {
    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    for (size_t index = line_start; index < offset; ++index) {
        if (std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::string portSignatureLabel(const SchematicPort& port) {
    std::string label;
    const auto append_part = [&](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!label.empty()) {
            label.push_back(' ');
        }
        label += part;
    };
    append_part(port.direction);
    append_part(port.width_text);
    append_part(port.name);
    return label.empty() ? port.name : label;
}

std::string moduleSignatureLabel(const ModuleDefinition& module) {
    std::string label = module.name + "(";
    const auto port_count = module.port_details.empty() ? module.ports.size() : module.port_details.size();
    for (size_t index = 0; index < port_count; ++index) {
        if (index != 0) {
            label += ", ";
        }
        label += module.port_details.empty() ? module.ports[index] : portSignatureLabel(module.port_details[index]);
    }
    label += ")";
    return label;
}

std::string declarationLocationLabel(const SemanticLocation& location) {
    return location.uri + ":" + std::to_string(location.range.start_line + 1) + ":" +
           std::to_string(location.range.start_character + 1);
}

std::string snippetEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '$' || character == '}') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

std::string moduleInstantiationSnippet(const ModuleDefinition& module) {
    std::string snippet = module.name + " ${1:" + snippetEscape(module.name) + "_i}(";
    const auto port_count = module.port_details.empty() ? module.ports.size() : module.port_details.size();
    for (size_t index = 0; index < port_count; ++index) {
        if (index != 0) {
            snippet += ", ";
        }
        const auto& port_name = module.port_details.empty() ? module.ports[index]
                                                            : module.port_details[index].name;
        snippet += ".";
        snippet += port_name;
        snippet += "(${";
        snippet += std::to_string(index + 2);
        snippet += ":";
        snippet += snippetEscape(port_name);
        snippet += "})";
    }
    snippet += ");";
    return snippet;
}

std::string portConnectionSnippet(std::string_view port_name) {
    return std::string(port_name) + "(${1:" + snippetEscape(port_name) + "})";
}

std::string macroSignatureLabel(const MacroDefinition& macro) {
    std::string label = macro.name;
    if (!macro.function_like) {
        return label;
    }

    label += "(";
    for (size_t index = 0; index < macro.parameters.size(); ++index) {
        if (index != 0) {
            label += ", ";
        }
        label += macro.parameters[index];
    }
    label += ")";
    return label;
}

std::string macroInsertText(const MacroDefinition& macro) {
    if (!macro.function_like) {
        return macro.name;
    }

    std::string text = macro.name + "(";
    for (size_t index = 0; index < macro.parameters.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        text += "${";
        text += std::to_string(index + 1);
        text += ":";
        text += snippetEscape(macro.parameters[index]);
        text += "}";
    }
    text += ")";
    return text;
}

std::string macroDocumentation(const MacroDefinition& macro) {
    std::string documentation = "**Macro** `";
    documentation += macroSignatureLabel(macro);
    documentation += "`";
    if (!macro.parameters.empty()) {
        documentation += "\n\nParameters: `";
        for (size_t index = 0; index < macro.parameters.size(); ++index) {
            if (index != 0) {
                documentation += ", ";
            }
            documentation += macro.parameters[index];
        }
        documentation += "`";
    }
    if (!macro.body.empty()) {
        documentation += "\n\nBody:\n```systemverilog\n";
        documentation += macro.body;
        documentation += "\n```";
    }
    return documentation;
}

std::string moduleDocumentation(const ModuleDefinition& module, std::string_view declaration_uri) {
    std::string documentation = "**";
    documentation += module.kind.empty() ? "Module" : module.kind;
    documentation += "** `";
    documentation += moduleSignatureLabel(module);
    documentation += "`";
    const auto port_count = module.port_details.empty() ? module.ports.size() : module.port_details.size();
    if (port_count > 0) {
        documentation += "\n\nPorts: `";
        for (size_t index = 0; index < port_count; ++index) {
            if (index != 0) {
                documentation += ", ";
            }
            documentation += module.port_details.empty() ? module.ports[index]
                                                          : portSignatureLabel(module.port_details[index]);
        }
        documentation += "`";
    }
    if (!declaration_uri.empty()) {
        documentation += "\n\nDeclared: `" + std::string(declaration_uri) + ":" +
                         std::to_string(module.selection_range.start_line + 1) + ":" +
                         std::to_string(module.selection_range.start_character + 1) + "`";
    }
    return documentation;
}

std::string portDocumentation(const ModuleDefinition& module,
                              const SchematicPort& port,
                              std::string_view declaration_uri) {
    std::string documentation = "**Port** `";
    documentation += portSignatureLabel(port);
    documentation += "`";
    documentation += "\n\nModule: `" + module.name + "`";
    if (!declaration_uri.empty()) {
        documentation += "\n\nDeclared: `" + std::string(declaration_uri) + ":" +
                         std::to_string(port.selection_range.start_line + 1) + ":" +
                         std::to_string(port.selection_range.start_character + 1) + "`";
    }
    return documentation;
}

int activeParameterAt(std::string_view text, size_t open_paren_offset, size_t position_offset) {
    int active_parameter = 0;
    int depth = 0;
    for (size_t offset = open_paren_offset + 1; offset < position_offset && offset < text.size(); ++offset) {
        const char value = text[offset];
        if (value == '(') {
            ++depth;
            continue;
        }
        if (value == ')') {
            if (depth == 0) {
                break;
            }
            --depth;
            continue;
        }
        if (value == ',' && depth == 0) {
            ++active_parameter;
        }
    }
    return active_parameter;
}

std::optional<std::string> packageQualifierBeforeCompletion(std::string_view text,
                                                            int line,
                                                            int character,
                                                            std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!prefix_start.has_value() || *prefix_start < 2 || text[*prefix_start - 1] != ':' ||
        text[*prefix_start - 2] != ':') {
        return std::nullopt;
    }

    size_t name_end = *prefix_start - 2;
    size_t name_start = name_end;
    while (name_start > 0 && isIdentifierContinue(text[name_start - 1])) {
        --name_start;
    }
    const auto qualifier = text.substr(name_start, name_end - name_start);
    if (qualifier.empty() || !isIdentifierStart(qualifier.front())) {
        return std::nullopt;
    }
    return std::string(qualifier);
}

std::optional<std::string> memberQualifierBeforeCompletion(std::string_view text,
                                                           int line,
                                                           int character,
                                                           std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!prefix_start.has_value() || *prefix_start == 0 || text[*prefix_start - 1] != '.') {
        return std::nullopt;
    }

    size_t name_end = *prefix_start - 1;
    size_t name_start = name_end;
    while (name_start > 0 && isIdentifierContinue(text[name_start - 1])) {
        --name_start;
    }
    const auto qualifier = text.substr(name_start, name_end - name_start);
    if (qualifier.empty() || !isIdentifierStart(qualifier.front())) {
        return std::nullopt;
    }
    return std::string(qualifier);
}

std::optional<size_t> openParenBeforePosition(std::string_view text,
                                              size_t search_start,
                                              size_t search_end) {
    if (search_start >= text.size() || search_start >= search_end) {
        return std::nullopt;
    }
    const auto bounded_end = std::min(search_end, text.size());
    for (size_t offset = search_start; offset < bounded_end; ++offset) {
        if (text[offset] == '(') {
            return offset;
        }
    }
    return std::nullopt;
}

std::set<std::string> connectedNamedPortsBeforePosition(std::string_view text,
                                                        size_t open_paren_offset,
                                                        size_t position_offset) {
    std::set<std::string> connected_ports;
    int depth = 0;
    for (size_t offset = open_paren_offset + 1; offset < position_offset && offset < text.size(); ++offset) {
        const char value = text[offset];
        if (value == '(') {
            ++depth;
            continue;
        }
        if (value == ')') {
            if (depth == 0) {
                break;
            }
            --depth;
            continue;
        }
        if (value != '.' || depth != 0) {
            continue;
        }

        const auto name_start = offset + 1;
        if (name_start >= position_offset || !isIdentifierStart(text[name_start])) {
            continue;
        }

        size_t name_end = name_start + 1;
        while (name_end < position_offset && isIdentifierContinue(text[name_end])) {
            ++name_end;
        }

        size_t cursor = name_end;
        while (cursor < position_offset && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < position_offset && text[cursor] == '(') {
            connected_ports.insert(std::string(text.substr(name_start, name_end - name_start)));
        }

        offset = name_end;
    }
    return connected_ports;
}

std::optional<SemanticLocation> identifierRangeWithin(const SemanticEngineDocument& document,
                                                      const SemanticLocation& broad_location,
                                                      std::string_view name) {
    CompilationService compilation_service;
    std::optional<SemanticLocation> best;
    for (const auto& identifier : compilation_service.identifiers(document.text)) {
        if (identifier.name != name || !rangesOverlapOrTouch(identifier.range, broad_location.range)) {
            continue;
        }
        const auto location = SemanticLocation{.uri = broad_location.uri, .range = identifier.range};
        if (!best.has_value() || locationLess(location, *best)) {
            best = location;
        }
    }
    return best;
}

std::optional<ParseRange> userTypeReferenceRange(std::string_view text,
                                                 const SemanticSymbolMetadata& metadata) {
    if (metadata.type_name.empty() || metadata.type_name == "enum" ||
        metadata.type_display_name.find("::") != std::string::npos ||
        isBuiltinTypeName(metadata.type_name)) {
        return std::nullopt;
    }

    CompilationService compilation_service;
    std::optional<ParseRange> best;
    for (const auto& identifier : compilation_service.identifiers(text)) {
        if (identifier.name != metadata.type_name ||
            identifier.range.start_line != metadata.selection_range.start_line ||
            identifier.range.end_line != metadata.selection_range.start_line ||
            identifier.range.end_character > metadata.selection_range.start_character) {
            continue;
        }
        if (!best.has_value() || identifier.range.start_character > best->start_character) {
            best = identifier.range;
        }
    }
    return best;
}

ParseRange pointRangeAtUtf8Offset(std::string_view text, size_t target_offset) {
    int line = 0;
    int character = 0;
    const auto clamped_offset = std::min(target_offset, text.size());
    for (size_t offset = 0; offset < clamped_offset; ++offset) {
        const char value = text[offset];
        if (value == '\r') {
            if (offset + 1 < clamped_offset && text[offset + 1] == '\n') {
                ++offset;
            }
            ++line;
            character = 0;
            continue;
        }
        if (value == '\n') {
            ++line;
            character = 0;
            continue;
        }
        ++character;
    }
    return ParseRange{.start_line = line,
                      .start_character = character,
                      .end_line = line,
                      .end_character = character};
}

std::optional<ParseRange> instancePortInsertionRange(std::string_view text,
                                                     const SchematicCell& cell) {
    const auto start_offset = utf8OffsetAtUtf16Position(text,
                                                        cell.range.start_line,
                                                        cell.range.start_character);
    const auto end_offset = utf8OffsetAtUtf16Position(text,
                                                      cell.range.end_line,
                                                      cell.range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value() || *start_offset >= *end_offset) {
        return std::nullopt;
    }

    for (size_t offset = *end_offset; offset > *start_offset; --offset) {
        if (text[offset - 1] == ')') {
            return pointRangeAtUtf8Offset(text, offset - 1);
        }
    }
    return std::nullopt;
}

constexpr size_t kMaxSemanticLocations = 2000;

} // namespace

struct SemanticEngine::SnapshotData {
    struct IndexedSymbol {
        SemanticSymbolIdentity identity;
        const slang::ast::Symbol* symbol = nullptr;
        std::string type_display;
    };

    struct IndexedReference {
        std::string stable_id;
        std::string name;
        SemanticLocation location;
        bool is_declaration = false;
    };

    struct ModuleInstance {
        std::string module_name;
        std::string instance_name;
        std::string target_stable_id;
        std::string uri;
        ParseRange range;
        ParseRange selection_range;
        ParseRange module_selection_range;
    };

    struct ModuleEntry {
        std::string uri;
        ModuleDefinition definition;
    };

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, IndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<IndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
    std::vector<ModuleEntry> module_entries;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, ModuleSchematic> schematics_by_name;
    std::unordered_map<std::string, std::string> schematic_uris_by_name;
    std::unordered_map<std::string, std::vector<ContinuousAssignment>> assignments_by_uri;
    std::unordered_map<std::string, std::vector<ModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SemanticSymbolMetadata>> metadata_by_uri;
};

namespace {

template<typename SnapshotData>
void insertSymbol(SnapshotData& data,
                  const slang::SourceManager& source_manager,
                  const slang::ast::Symbol& symbol) {
    if (symbol.name.empty()) {
        return;
    }

    const auto location = declarationLocationForSymbol(source_manager, symbol);
    if (!location.has_value()) {
        return;
    }

    const auto stable_id = symbolStableId(source_manager, symbol, *location);
    if (data.symbols_by_id.find(stable_id) == data.symbols_by_id.end()) {
        std::string type_display;
        if (const auto* declared_type = symbol.getDeclaredType()) {
            type_display = declared_type->getType().toString();
        }
        else if (symbol.isType()) {
            if (const auto* type = symbol.as_if<slang::ast::Type>()) {
                type_display = type->toString();
            }
        }

        data.symbols_by_id.emplace(
            stable_id,
            typename SnapshotData::IndexedSymbol{
                .identity = SemanticSymbolIdentity{.stable_id = stable_id,
                                                   .name = std::string(symbol.name),
                                                   .kind = symbolKindName(symbol.kind),
                                                   .location = *location},
                .symbol = &symbol,
                .type_display = std::move(type_display)});
    }
    data.ids_by_symbol.emplace(&symbol, stable_id);

    auto& completions = data.completions_by_uri[location->uri];
    const auto duplicate = std::any_of(completions.begin(),
                                       completions.end(),
                                       [&](const SemanticCompletionItem& item) {
                                           return item.label == symbol.name;
                                       });
    if (!duplicate) {
        const auto kind_name = symbolKindName(symbol.kind);
        completions.push_back(SemanticCompletionItem{.stable_id = stable_id,
                                                      .label = std::string(symbol.name),
                                                      .detail = completionDetailForSemanticKind(kind_name),
                                                      .documentation = {},
                                                      .insert_text = std::string(symbol.name),
                                                      .kind = completionKindForSemanticKind(kind_name),
                                                      .unresolved = false});
    }
}

template<typename SnapshotData>
void insertReference(SnapshotData& data,
                     std::string stable_id,
                     std::string name,
                     SemanticLocation location,
                     bool is_declaration) {
    const auto duplicate = std::any_of(data.references.begin(),
                                       data.references.end(),
                                       [&](const auto& reference) {
                                           return reference.stable_id == stable_id &&
                                                  sameLocation(reference.location, location);
                                       });
    if (duplicate) {
        return;
    }

    const auto index = data.references.size();
    data.references.push_back(typename SnapshotData::IndexedReference{
        .stable_id = std::move(stable_id),
        .name = std::move(name),
        .location = std::move(location),
        .is_declaration = is_declaration});
    data.references_by_symbol[data.references.back().stable_id].push_back(index);
}

template<typename SnapshotData>
void indexSymbolReferences(SnapshotData& data,
                           const slang::SourceManager& source_manager,
                           const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                           const slang::ast::Expression& expression) {
    expression.visitSymbolReferences([&](const slang::ast::Expression& reference_expression,
                                          const slang::ast::Symbol& symbol) {
        const auto id_it = data.ids_by_symbol.find(&symbol);
        if (id_it == data.ids_by_symbol.end()) {
            return;
        }

        auto location = locationForSourceRange(source_manager, reference_expression.sourceRange);
        if (!location.has_value()) {
            return;
        }

        const auto document_it = documents.find(location->uri);
        if (document_it != documents.end()) {
            if (const auto narrow_location = identifierRangeWithin(document_it->second,
                                                                   *location,
                                                                   symbol.name)) {
                location = narrow_location;
            }
        }

        insertReference(data, id_it->second, std::string(symbol.name), *location, false);
    });
}

template<typename SnapshotData>
void indexModuleInstanceBinding(SnapshotData& data,
                                const slang::SourceManager& source_manager,
                                const slang::ast::InstanceSymbol& instance) {
    const auto instance_location = declarationLocationForSymbol(source_manager, instance);
    if (!instance_location.has_value()) {
        return;
    }

    const auto& definition = instance.getDefinition();
    insertSymbol(data, source_manager, definition);
    const auto definition_location = declarationLocationForSymbol(source_manager, definition);
    if (!definition_location.has_value()) {
        return;
    }
    const auto definition_id = symbolStableId(source_manager, definition, *definition_location);

    const auto ranges_equal = [](const ParseRange& lhs, const ParseRange& rhs) {
        return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
               lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
    };

    const auto instances_it = data.module_instances_by_uri.find(instance_location->uri);
    if (instances_it == data.module_instances_by_uri.end()) {
        return;
    }
    for (auto& module_instance : instances_it->second) {
        if (module_instance.instance_name == instance.name &&
            ranges_equal(module_instance.selection_range, instance_location->range)) {
            module_instance.target_stable_id = definition_id;
            return;
        }
    }
}

template<typename SnapshotData>
struct SemanticIndexVisitor
    : slang::ast::ASTVisitor<SemanticIndexVisitor<SnapshotData>,
                             slang::ast::VisitFlags::AllGood> {
    SnapshotData& data;
    const slang::SourceManager& source_manager;
    const std::unordered_map<std::string, SemanticEngineDocument>& documents;

    SemanticIndexVisitor(SnapshotData& data,
                         const slang::SourceManager& source_manager,
                         const std::unordered_map<std::string, SemanticEngineDocument>& documents) :
        data(data),
        source_manager(source_manager),
        documents(documents) {}

    template<typename T>
    void handle(const T& symbol)
        requires std::is_base_of_v<slang::ast::Symbol, T>
    {
        insertSymbol(data, source_manager, symbol);
        if constexpr (std::is_same_v<T, slang::ast::InstanceSymbol>) {
            indexModuleInstanceBinding(data, source_manager, symbol);
        }
        this->visitDefault(symbol);
    }

    template<typename T>
    void handle(const T& expression)
        requires std::is_base_of_v<slang::ast::Expression, T>
    {
        indexSymbolReferences(data, source_manager, documents, expression);
        this->visitDefault(expression);
    }
};

template<typename SnapshotData>
void addDeclarationReferences(SnapshotData& data) {
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        insertReference(data,
                        stable_id,
                        indexed_symbol.identity.name,
                        indexed_symbol.identity.location,
                        true);
    }
}

template<typename SnapshotData>
std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
                                                  std::string_view name) {
    std::optional<std::string> definition_result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && indexed_symbol.identity.kind == "Definition") {
            if (definition_result.has_value()) {
                return std::nullopt;
            }
            definition_result = stable_id;
        }
    }
    if (definition_result.has_value()) {
        return definition_result;
    }

    std::optional<std::string> module_like_result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && isModuleDefinitionKind(indexed_symbol.identity.kind)) {
            if (module_like_result.has_value()) {
                return std::nullopt;
            }
            module_like_result = stable_id;
        }
    }
    return module_like_result;
}

template<typename SnapshotData>
std::optional<std::string> findSymbolIdByNameAndKind(const SnapshotData& data,
                                                     std::string_view name,
                                                     std::string_view kind) {
    std::optional<std::string> result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && indexed_symbol.identity.kind == kind) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = stable_id;
        }
    }
    return result;
}

bool canHaveUserDefinedTypeReference(const SemanticSymbolMetadata& metadata) {
    return metadata.kind == 13 || metadata.kind == 14;
}

bool isTypeDefinitionMetadata(const SemanticSymbolMetadata& metadata) {
    switch (metadata.kind) {
        case 2:
        case 5:
        case 10:
        case 11:
        case 26:
            return true;
        default:
            return false;
    }
}

template<typename SnapshotData>
std::vector<SemanticLocation> typeDefinitionLocationsByName(const SnapshotData& data,
                                                            std::string_view name) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, symbol] : data.symbols_by_id) {
        if (symbol.identity.name == name && isTypeDefinitionKind(symbol.identity.kind) &&
            !isModuleDefinitionKind(symbol.identity.kind)) {
            locations.push_back(symbol.identity.location);
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

template<typename SnapshotData>
bool hasTypeDefinitionSymbol(const SnapshotData& data, std::string_view name) {
    return !typeDefinitionLocationsByName(data, name).empty();
}

template<typename SnapshotData>
std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                              std::string_view uri,
                                              int line,
                                              int character);

bool isAssignableMetadata(const SemanticSymbolMetadata& metadata) {
    return metadata.kind == 13 && metadata.type_display_name.find('[') != std::string::npos;
}

std::optional<std::int64_t> bitWidthFromDisplayName(std::string_view display_name) {
    const auto open = display_name.find('[');
    const auto close = display_name.find(']', open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1) {
        if (display_name == "logic" || display_name == "bit" || display_name == "wire" ||
            display_name == "reg") {
            return 1;
        }
        return std::nullopt;
    }

    const auto colon = display_name.find(':', open + 1);
    if (colon == std::string_view::npos || colon >= close) {
        return std::nullopt;
    }

    try {
        const auto msb = std::stoll(std::string(display_name.substr(open + 1,
                                                                    colon - open - 1)));
        const auto lsb = std::stoll(std::string(display_name.substr(colon + 1,
                                                                    close - colon - 1)));
        return std::llabs(msb - lsb) + 1;
    }
    catch (...) {
        return std::nullopt;
    }
}

template<typename SnapshotData>
std::optional<std::string> symbolIdAtRangeStart(const SnapshotData& data,
                                                std::string_view uri,
                                                const ParseRange& range) {
    return symbolIdAtLocation(data, uri, range.start_line, range.start_character);
}

template<typename SnapshotData>
std::optional<SemanticSymbolMetadata> metadataForSymbolId(const SnapshotData& data,
                                                          std::string_view stable_id) {
    const auto symbol_it = data.symbols_by_id.find(std::string(stable_id));
    if (symbol_it == data.symbols_by_id.end()) {
        return std::nullopt;
    }
    const auto& identity = symbol_it->second.identity;
    const auto metadata_it = data.metadata_by_uri.find(identity.location.uri);
    if (metadata_it == data.metadata_by_uri.end()) {
        return std::nullopt;
    }
    for (const auto& metadata : metadata_it->second) {
        if (metadata.name == identity.name &&
            metadata.selection_range.start_line == identity.location.range.start_line &&
            metadata.selection_range.start_character == identity.location.range.start_character) {
            return metadata;
        }
    }
    return std::nullopt;
}

template<typename SnapshotData>
std::vector<std::string> packageDefinitionIds(const SnapshotData& data,
                                              std::string_view package_name) {
    std::vector<std::string> result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == package_name &&
            indexed_symbol.identity.kind == "Package") {
            result.push_back(stable_id);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

template<typename SnapshotData>
void appendDuplicateSymbolDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                      const SnapshotData& data,
                                      const SemanticEngineDocument& document) {
    const auto metadata_it = data.metadata_by_uri.find(document.uri);
    if (metadata_it == data.metadata_by_uri.end()) {
        return;
    }

    std::map<std::string, std::vector<SemanticSymbolMetadata>> by_name;
    for (const auto& metadata : metadata_it->second) {
        if (metadata.name.empty()) {
            continue;
        }
        by_name[metadata.name].push_back(metadata);
    }
    for (auto& [_, symbols] : by_name) {
        if (symbols.size() < 2) {
            continue;
        }
        std::sort(symbols.begin(), symbols.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.selection_range.start_line != rhs.selection_range.start_line) {
                return lhs.selection_range.start_line < rhs.selection_range.start_line;
            }
            return lhs.selection_range.start_character < rhs.selection_range.start_character;
        });
        for (size_t index = 1; index < symbols.size(); ++index) {
            if (symbols[index].selection_range.start_line != symbols[index - 1].selection_range.start_line + 1) {
                continue;
            }
            result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                      .code = "duplicateSymbol",
                                                      .message = duplicateSymbolMessage(symbols[index].name),
                                                      .range = symbols[index].selection_range,
                                                      .severity = 1});
        }
    }
}

template<typename SnapshotData>
void appendUnresolvedPackageDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                        const SnapshotData& data,
                                        const SemanticEngineDocument& document) {
    const auto imports_it = data.package_imports_by_uri.find(document.uri);
    if (imports_it == data.package_imports_by_uri.end()) {
        return;
    }
    for (const auto& import : imports_it->second) {
        if (!packageDefinitionIds(data, import.package_name).empty()) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                  .code = "unresolvedPackage",
                                                  .message = unresolvedPackageMessage(import.package_name),
                                                  .range = import.package_range,
                                                  .severity = 1});
    }
}

template<typename SnapshotData>
void appendWidthMismatchDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                    const SnapshotData& data,
                                    const SemanticEngineDocument& document) {
    const auto assignments_it = data.assignments_by_uri.find(document.uri);
    if (assignments_it == data.assignments_by_uri.end()) {
        return;
    }
    std::set<std::pair<int, int>> reported;
    for (const auto& assignment : assignments_it->second) {
        const auto left_id = symbolIdAtRangeStart(data, document.uri, assignment.left_range);
        const auto right_id = symbolIdAtRangeStart(data, document.uri, assignment.right_range);
        if (!left_id.has_value() || !right_id.has_value()) {
            continue;
        }
        const auto left_metadata = metadataForSymbolId(data, *left_id);
        const auto right_metadata = metadataForSymbolId(data, *right_id);
        if (!left_metadata.has_value() || !right_metadata.has_value() ||
            !isAssignableMetadata(*left_metadata) || !isAssignableMetadata(*right_metadata)) {
            continue;
        }
        const auto left_width = bitWidthFromDisplayName(left_metadata->type_display_name);
        const auto right_width = bitWidthFromDisplayName(right_metadata->type_display_name);
        if (!left_width.has_value() || !right_width.has_value() || *left_width <= 0 ||
            *right_width <= 0 || *left_width == *right_width) {
            continue;
        }
        const auto key = std::pair(assignment.right_range.start_line,
                                   assignment.right_range.start_character);
        if (!reported.insert(key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                  .code = "widthMismatch",
                                                  .message = widthMismatchMessage(assignment.left_expression,
                                                                                  *left_width,
                                                                                  assignment.right_expression,
                                                                                  *right_width),
                                                  .range = assignment.right_range,
                                                  .severity = 2});
    }
}

template<typename SnapshotData>
void appendAmbiguousReferenceDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                         const SnapshotData& data,
                                         const SemanticEngineDocument& document) {
    const auto imports_it = data.package_imports_by_uri.find(document.uri);
    if (imports_it == data.package_imports_by_uri.end() || imports_it->second.size() < 2) {
        return;
    }

    CompilationService compilation_service;
    for (const auto& identifier : compilation_service.identifiers(document.text)) {
        size_t definition_count = 0;
        for (const auto& import : imports_it->second) {
            for (const auto& package_id : packageDefinitionIds(data, import.package_name)) {
                const auto package_it = data.symbols_by_id.find(package_id);
                if (package_it == data.symbols_by_id.end()) {
                    continue;
                }
                const auto package_location = package_it->second.identity.location;
                const auto candidate_it = data.metadata_by_uri.find(package_location.uri);
                if (candidate_it == data.metadata_by_uri.end()) {
                    continue;
                }
                definition_count += static_cast<size_t>(
                    std::count_if(candidate_it->second.begin(),
                                  candidate_it->second.end(),
                                  [&](const SemanticSymbolMetadata& candidate) {
                                      return candidate.name == identifier.name &&
                                             isTypeDefinitionMetadata(candidate);
                                  }));
            }
        }
        if (definition_count < 2) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                  .code = "ambiguousReference",
                                                  .message = ambiguousReferenceMessage(identifier.name,
                                                                                      definition_count),
                                                  .range = identifier.range,
                                                  .severity = 2});
    }
}

template<typename SnapshotData>
void appendUnresolvedTypeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const SnapshotData& data,
                                     const SemanticEngineDocument& document) {
    CompilationService compilation_service;
    std::set<std::pair<int, int>> reported_type_ranges;
    for (const auto& metadata : compilation_service.semanticSymbolMetadata(document.text, document.uri)) {
        if (!canHaveUserDefinedTypeReference(metadata)) {
            continue;
        }
        const auto type_range = userTypeReferenceRange(document.text, metadata);
        if (!type_range.has_value() || hasTypeDefinitionSymbol(data, metadata.type_name)) {
            continue;
        }
        const auto range_key = std::pair(type_range->start_line, type_range->start_character);
        if (!reported_type_ranges.insert(range_key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                  .code = std::string(kUnresolvedTypeDiagnosticCode),
                                                  .message = unresolvedTypeMessage(metadata.type_name),
                                                  .range = *type_range,
                                                  .severity = 1});
    }
}

template<typename SnapshotData>
void appendUnknownIncludeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const SnapshotData& data,
                                     const SemanticEngineDocument& document,
                                     std::string_view workspace_root_uri) {
    CompilationService compilation_service;
    for (const auto& include : compilation_service.includeDirectives(document.text)) {
        if (resolveIncludeTarget(workspace_root_uri, document.uri, include.target).has_value()) {
            continue;
        }
        const auto key = std::tuple(std::string(kUnknownIncludeDiagnosticCode),
                                    include.range.start_line,
                                    include.range.start_character,
                                    unknownIncludeMessage(include.target));
        const auto already_reported = std::any_of(result.begin(), result.end(), [&](const auto& diagnostic) {
            return std::tuple(diagnostic.code,
                              diagnostic.range.start_line,
                              diagnostic.range.start_character,
                              diagnostic.message) == key;
        });
        if (already_reported) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                  .code = std::string(kUnknownIncludeDiagnosticCode),
                                                  .message = unknownIncludeMessage(include.target),
                                                  .range = include.range,
                                                  .severity = 1});
    }
    (void)data;
}

template<typename SnapshotData>
void appendUnresolvedModuleDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                       const SnapshotData& data,
                                       const SemanticEngineDocument& document) {
    CompilationService compilation_service;
    for (const auto& module : compilation_service.moduleDefinitions(document.text, document.uri)) {
        for (const auto& instance : module.instances) {
            if (data.modules_by_name.contains(instance.module_name) ||
                !isValidIdentifier(instance.module_name)) {
                continue;
            }
            const auto key = std::tuple(std::string(kUnresolvedModuleDiagnosticCode),
                                        instance.module_selection_range.start_line,
                                        instance.module_selection_range.start_character,
                                        unresolvedModuleMessage(instance.module_name));
            const auto already_reported = std::any_of(result.begin(), result.end(), [&](const auto& diagnostic) {
                return std::tuple(diagnostic.code,
                                  diagnostic.range.start_line,
                                  diagnostic.range.start_character,
                                  diagnostic.message) == key;
            });
            if (already_reported) {
                continue;
            }
            result.push_back(SemanticEngineDiagnostic{.uri = document.uri,
                                                      .code = std::string(kUnresolvedModuleDiagnosticCode),
                                                      .message = unresolvedModuleMessage(instance.module_name),
                                                      .range = instance.module_selection_range,
                                                      .severity = 1});
        }
    }
}

template<typename SnapshotData>
void addModuleInstantiationReferences(SnapshotData& data,
                                       const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    for (const auto& [document_uri, instances] : data.module_instances_by_uri) {
        if (!documents.contains(document_uri)) {
            continue;
        }
        for (const auto& instance : instances) {
            const auto target_id = !instance.target_stable_id.empty()
                                       ? std::optional<std::string>{instance.target_stable_id}
                                       : findDefinitionSymbolId(data, instance.module_name);
            if (!target_id.has_value()) {
                continue;
            }
            insertReference(data,
                            *target_id,
                            instance.module_name,
                            SemanticLocation{.uri = document_uri, .range = instance.module_selection_range},
                            false);
        }
    }
}

template<typename SnapshotData>
std::vector<SemanticLocation> moduleImplementationLocations(const SnapshotData& data,
                                                            std::string_view module_name,
                                                            bool& truncated) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            if (instance.module_name != module_name) {
                continue;
            }
            locations.push_back(SemanticLocation{.uri = instance.uri,
                                                 .range = instance.module_selection_range});
            if (locations.size() >= kMaxSemanticLocations) {
                truncated = true;
                break;
            }
        }
        if (truncated) {
            break;
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

template<typename SnapshotData>
std::optional<typename SnapshotData::ModuleInstance> moduleInstanceAt(const SnapshotData& data,
                                                                      std::string_view uri,
                                                                      int line,
                                                                      int character) {
    const auto instances_it = data.module_instances_by_uri.find(std::string(uri));
    if (instances_it == data.module_instances_by_uri.end()) {
        return std::nullopt;
    }
    std::optional<typename SnapshotData::ModuleInstance> best;
    for (const auto& instance : instances_it->second) {
        if (!containsPosition(instance.module_selection_range, line, character)) {
            continue;
        }
        if (!best.has_value() ||
            locationLess(SemanticLocation{.uri = instance.uri, .range = instance.module_selection_range},
                         SemanticLocation{.uri = best->uri, .range = best->module_selection_range})) {
            best = instance;
        }
    }
    return best;
}

template<typename SnapshotData>
void sortSnapshotIndexes(SnapshotData& data) {
    for (auto& [_, indexes] : data.references_by_symbol) {
        std::sort(indexes.begin(), indexes.end(), [&](size_t lhs, size_t rhs) {
            return locationLess(data.references[lhs].location, data.references[rhs].location);
        });
    }
    for (auto& [_, completions] : data.completions_by_uri) {
        std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhs_priority = completionPriorityForDetail(lhs.detail);
            const auto rhs_priority = completionPriorityForDetail(rhs.detail);
            if (lhs_priority != rhs_priority) {
                return lhs_priority < rhs_priority;
            }
            return lhs.label < rhs.label;
        });
    }
    for (auto& [_, ranges] : data.selection_ranges_by_uri) {
        std::sort(ranges.begin(), ranges.end(), rangeLessWideFirst);
        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const ParseRange& lhs,
                                                                 const ParseRange& rhs) {
                         return lhs.start_line == rhs.start_line &&
                                lhs.start_character == rhs.start_character &&
                                lhs.end_line == rhs.end_line &&
                                lhs.end_character == rhs.end_character;
                     }),
                     ranges.end());
    }
}

template<typename SnapshotData>
std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                              std::string_view uri,
                                              int line,
                                              int character) {
    std::optional<std::string> best_id;
    std::optional<SemanticLocation> best_location;
    for (const auto& reference : data.references) {
        if (reference.location.uri != uri || !containsPosition(reference.location.range, line, character)) {
            continue;
        }
        if (!best_location.has_value() ||
            (reference.location.range.start_line >= best_location->range.start_line &&
             reference.location.range.start_character >= best_location->range.start_character)) {
            best_id = reference.stable_id;
            best_location = reference.location;
        }
    }
    return best_id;
}

template<typename SnapshotData>
std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                 std::string_view stable_id,
                                                 bool include_declaration,
                                                 bool& truncated) {
    std::vector<SemanticLocation> locations;
    const auto references_it = data.references_by_symbol.find(std::string(stable_id));
    if (references_it == data.references_by_symbol.end()) {
        return locations;
    }

    for (const auto index : references_it->second) {
        const auto& reference = data.references[index];
        if (!include_declaration && reference.is_declaration) {
            continue;
        }
        locations.push_back(reference.location);
        if (locations.size() >= kMaxSemanticLocations) {
            truncated = true;
            break;
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

bool prefixMatches(std::string_view value, std::string_view prefix) {
    return startsWithInsensitive(prefix, value);
}

void appendCompletionItem(std::vector<SemanticCompletionItem>& items,
                          std::set<std::string>& emitted,
                                                SemanticCompletionItem item,
                                                std::string_view prefix,
                                                bool& truncated) {
    if (item.label == prefix || !prefixMatches(item.label, prefix) || !emitted.insert(item.label).second) {
        return;
    }
    items.push_back(std::move(item));
    if (items.size() >= kMaxSemanticLocations) {
        truncated = true;
    }
}

template<typename SnapshotData>
void appendSymbolCompletion(std::vector<SemanticCompletionItem>& items,
                            std::set<std::string>& emitted,
                            const SnapshotData& data,
                            const typename SnapshotData::IndexedSymbol& symbol,
                            std::string_view prefix,
                            bool& truncated) {
    if (truncated) {
        return;
    }
    appendCompletionItem(items,
                         emitted,
                         SemanticCompletionItem{.stable_id = symbol.identity.stable_id,
                                                 .label = symbol.identity.name,
                                                 .detail = completionDetailForSemanticKind(symbol.identity.kind),
                                                 .documentation = {},
                                                 .insert_text = symbol.identity.name,
                                                 .kind = completionKindForSemanticKind(symbol.identity.kind),
                                                 .unresolved = false},
                         prefix,
                         truncated);
    (void)data;
}

template<typename SnapshotData>
void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const SnapshotData& data,
                                 const ModuleDefinition& module,
                                 std::string_view module_uri,
                                 std::string_view prefix,
                                 const std::set<std::string>& excluded_ports,
                                 bool& truncated) {
    const auto module_id = findSymbolIdByNameAndKind(data, module.name, "Definition")
                               .value_or(std::string("module|") + module.name);
    if (module.port_details.empty()) {
        for (const auto& port_name : module.ports) {
            if (truncated) {
                return;
            }
            if (excluded_ports.contains(port_name)) {
                continue;
            }
            const SchematicPort port{.name = port_name,
                                     .direction = {},
                                     .width_text = {},
                                     .range = module.selection_range,
                                     .selection_range = module.selection_range};
            appendCompletionItem(
                items,
                emitted,
                SemanticCompletionItem{.stable_id = module_id + "|port|" + port_name,
                                        .label = port_name,
                                        .detail = "Port",
                                        .documentation = portDocumentation(module, port, module_uri),
                                        .insert_text = portConnectionSnippet(port_name),
                                        .kind = 5,
                                        .unresolved = false},
                prefix,
                truncated);
        }
        return;
    }

    for (const auto& port : module.port_details) {
        if (truncated) {
            return;
        }
        if (excluded_ports.contains(port.name)) {
            continue;
        }
        appendCompletionItem(
            items,
            emitted,
            SemanticCompletionItem{.stable_id = module_id + "|port|" + port.name,
                                    .label = port.name,
                                    .detail = portSignatureLabel(port),
                                    .documentation = portDocumentation(module, port, module_uri),
                                    .insert_text = portConnectionSnippet(port.name),
                                    .kind = 5,
                                    .unresolved = false},
            prefix,
            truncated);
    }
}

template<typename Map>
std::optional<std::string> firstUninstantiatedModuleName(const Map& modules_by_name) {
    std::set<std::string> instantiated;
    for (const auto& [_, module] : modules_by_name) {
        for (const auto& instance : module.instances) {
            instantiated.insert(instance.module_name);
        }
    }
    for (const auto& [name, _] : modules_by_name) {
        if (!instantiated.contains(name)) {
            return name;
        }
    }
    if (!modules_by_name.empty()) {
        return modules_by_name.begin()->first;
    }
    return std::nullopt;
}

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isLogicOutputPortName(std::string_view port_name) {
    const auto normalized = lowerAsciiCopy(std::string(port_name));
    return normalized == "y" || normalized == "out" || normalized == "o" || normalized == "q";
}

const SchematicPort* findSchematicPortByName(const ModuleSchematic& schematic, std::string_view name) {
    const auto found = std::find_if(schematic.ports.begin(), schematic.ports.end(), [&](const auto& port) {
        return port.name == name;
    });
    return found == schematic.ports.end() ? nullptr : &*found;
}

const SchematicPort* findSchematicPortByIndex(const ModuleSchematic& schematic, int index) {
    if (index < 0 || static_cast<size_t>(index) >= schematic.ports.size()) {
        return nullptr;
    }
    return &schematic.ports[static_cast<size_t>(index)];
}

std::vector<SchematicPort> modulePorts(const ModuleDefinition& module) {
    if (!module.port_details.empty()) {
        return module.port_details;
    }

    std::vector<SchematicPort> ports;
    for (const auto& port_name : module.ports) {
        ports.push_back(SchematicPort{.name = port_name,
                                      .direction = {},
                                      .width_text = {},
                                      .range = module.selection_range,
                                      .selection_range = module.selection_range});
    }
    return ports;
}

std::set<std::string> connectedPortNames(const SchematicCell& cell,
                                         const std::vector<SchematicPort>& ports) {
    std::set<std::string> connected;
    for (const auto& connection : cell.connections) {
        if (!connection.port_name.empty()) {
            connected.insert(connection.port_name);
            continue;
        }
        if (connection.port_index >= 0 && static_cast<size_t>(connection.port_index) < ports.size()) {
            connected.insert(ports[static_cast<size_t>(connection.port_index)].name);
        }
    }
    return connected;
}

std::vector<SchematicPort> missingPorts(const SchematicCell& cell,
                                        const ModuleDefinition& module) {
    const auto ports = modulePorts(module);
    const auto connected = connectedPortNames(cell, ports);
    std::vector<SchematicPort> result;
    for (const auto& port : ports) {
        if (port.name.empty() || connected.contains(port.name)) {
            continue;
        }
        result.push_back(port);
    }
    return result;
}

bool sameParseRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

void appendEndpointByDirection(SemanticSchematicNet& net,
                               std::string direction,
                               SemanticSchematicEndpoint endpoint,
                               bool invert_direction = false) {
    if (invert_direction) {
        if (direction == "input") {
            direction = "output";
        }
        else if (direction == "output") {
            direction = "input";
        }
    }

    if (direction == "output") {
        net.drivers.push_back(std::move(endpoint));
        return;
    }
    if (direction == "input") {
        net.loads.push_back(std::move(endpoint));
        return;
    }

    net.drivers.push_back(endpoint);
    net.loads.push_back(std::move(endpoint));
}

template<typename SnapshotData>
std::vector<SemanticSchematicNet> buildSchematicNets(const ModuleSchematic& schematic,
                                                     const SnapshotData& data) {
    std::map<std::string, SemanticSchematicNet> nets;
    const auto ensure_net = [&](std::string_view signal) -> SemanticSchematicNet& {
        auto [it, inserted] = nets.try_emplace(std::string(signal),
                                               SemanticSchematicNet{.name = std::string(signal)});
        (void)inserted;
        return it->second;
    };

    for (const auto& port : schematic.ports) {
        if (port.name.empty()) {
            continue;
        }
        auto& net = ensure_net(port.name);
        appendEndpointByDirection(net,
                                  port.direction,
                                  SemanticSchematicEndpoint{.node_id = std::string("$port:") + port.name,
                                                            .port_name = port.name},
                                  true);
    }

    for (const auto& cell : schematic.cells) {
        const auto target_it = cell.kind == "module"
                                   ? data.schematics_by_name.find(cell.type)
                                   : data.schematics_by_name.end();
        for (const auto& connection : cell.connections) {
            if (connection.signal.empty()) {
                continue;
            }

            std::string port_name = connection.port_name;
            std::string direction;
            if (target_it != data.schematics_by_name.end()) {
                const auto* port = !port_name.empty()
                                       ? findSchematicPortByName(target_it->second, port_name)
                                       : findSchematicPortByIndex(target_it->second,
                                                                  connection.port_index);
                if (port != nullptr) {
                    port_name = port->name;
                    direction = port->direction;
                }
            }

            if (port_name.empty() && connection.port_index >= 0) {
                port_name = std::to_string(connection.port_index);
            }
            if (direction.empty()) {
                direction = isLogicOutputPortName(port_name) ? "output" : "input";
            }

            auto& net = ensure_net(connection.signal);
            appendEndpointByDirection(net,
                                      direction,
                                      SemanticSchematicEndpoint{.node_id = cell.id,
                                                                .port_name = port_name});
        }
    }

    std::vector<SemanticSchematicNet> result;
    result.reserve(nets.size());
    for (auto& [_, net] : nets) {
        result.push_back(std::move(net));
    }
    return result;
}

std::optional<fs::path> pathFromFileUri(std::string_view uri) {
    if (!isFileUri(uri)) {
        return std::nullopt;
    }
    auto path = fileUriToPath(uri);
    if (path.empty()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) != 0 &&
        path[2] == ':') {
        path.erase(path.begin());
    }
#endif
    return fs::path(path);
}

std::optional<fs::path> resolveIncludeTarget(std::string_view workspace_root_uri,
                                             std::string_view document_uri,
                                             std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    std::vector<fs::path> candidates;

    if (target_path.is_absolute()) {
        candidates.push_back(target_path);
    }
    else if (const auto document_path = pathFromFileUri(document_uri)) {
        candidates.push_back(document_path->parent_path() / target_path);
    }

    if (!target_path.is_absolute()) {
        if (const auto workspace_path = pathFromFileUri(workspace_root_uri)) {
            candidates.push_back(*workspace_path / target_path);
        }
    }

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::exists(candidate, error) && fs::is_regular_file(candidate, error)) {
            return fs::weakly_canonical(candidate, error);
        }
    }

    return std::nullopt;
}

std::optional<fs::path> proposedIncludeTarget(std::string_view workspace_root_uri,
                                              std::string_view document_uri,
                                              std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    if (target_path.is_absolute()) {
        return target_path;
    }
    if (const auto document_path = pathFromFileUri(document_uri)) {
        return document_path->parent_path() / target_path;
    }
    if (const auto workspace_path = pathFromFileUri(workspace_root_uri)) {
        return *workspace_path / target_path;
    }
    return std::nullopt;
}

} // namespace

SemanticEngine::SemanticEngine() = default;

SemanticEngine::~SemanticEngine() = default;

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    includes_.clear();
    reverse_includes_.clear();
    snapshot_.reset();
    snapshot_data_.reset();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    config_.workspace_root_uri = workspace_root_uri_.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>{workspace_root_uri_};
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::configure(SemanticEngineConfig config) {
    config_ = std::move(config);
    if (config_.workspace_root_uri.has_value()) {
        workspace_root_uri_ = withoutTrailingSlash(normalizeFileUri(*config_.workspace_root_uri));
        config_.workspace_root_uri = workspace_root_uri_;
    }
    std::sort(config_.top_modules.begin(), config_.top_modules.end());
    config_.top_modules.erase(std::unique(config_.top_modules.begin(), config_.top_modules.end()),
                              config_.top_modules.end());
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::updateDocument(std::string_view uri,
                                    std::string_view text,
                                    SemanticEngineDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.insert_or_assign(document_uri,
                                SemanticEngineDocument{.uri = document_uri,
                                                       .text = std::string(text),
                                                       .version = state.version,
                                                       .is_open = state.is_open,
                                                       .dirty = state.dirty});
    try {
        rebuildDependenciesFor(document_uri, text);
    }
    catch (...) {
        includes_[document_uri] = {};
        for (auto& [_, including_uris] : reverse_includes_) {
            including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                                 including_uris.end());
        }
    }
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.erase(document_uri);
    includes_.erase(document_uri);
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                             including_uris.end());
    }
    reverse_includes_.erase(document_uri);
    snapshot_dirty_ = true;
    ++generation_;
}

const SemanticEngineDocument* SemanticEngine::document(std::string_view uri) const {
    const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (document_it == documents_.end()) {
        return nullptr;
    }
    return &document_it->second;
}

std::vector<std::string> SemanticEngine::includedUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = includes_.find(document_uri);
    if (include_it == includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> SemanticEngine::includingUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = reverse_includes_.find(document_uri);
    if (include_it == reverse_includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> SemanticEngine::dirtyDocumentUris() const {
    std::vector<std::string> result;
    for (const auto& [uri, document] : documents_) {
        if (document.dirty) {
            result.push_back(uri);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> SemanticEngine::affectedDocumentUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::vector<std::string> pending{document_uri};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        result.push_back(current);
        const auto reverse_it = reverse_includes_.find(current);
        if (reverse_it == reverse_includes_.end()) {
            continue;
        }
        pending.insert(pending.end(), reverse_it->second.begin(), reverse_it->second.end());
    }
    std::sort(result.begin(), result.end());
    return result;
}

const SemanticEngineSnapshot& SemanticEngine::snapshot() const {
    if (!snapshot_.has_value() || snapshot_dirty_) {
        rebuildSnapshot();
    }
    return *snapshot_;
}

std::vector<SemanticEngineDiagnostic> SemanticEngine::diagnosticsFor(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto& current_snapshot = snapshot();
    std::vector<SemanticEngineDiagnostic> result;
    for (const auto& diagnostic : current_snapshot.diagnostics) {
        if (diagnostic.uri == document_uri) {
            result.push_back(diagnostic);
        }
    }
    if (const auto data = snapshotData(); data != nullptr) {
        if (const auto document_it = documents_.find(document_uri); document_it != documents_.end()) {
            appendDuplicateSymbolDiagnostics(result, *data, document_it->second);
            appendUnresolvedPackageDiagnostics(result, *data, document_it->second);
            appendUnknownIncludeDiagnostics(result, *data, document_it->second, workspace_root_uri_);
            appendUnresolvedModuleDiagnostics(result, *data, document_it->second);
            appendUnresolvedTypeDiagnostics(result, *data, document_it->second);
            appendWidthMismatchDiagnostics(result, *data, document_it->second);
            appendAmbiguousReferenceDiagnostics(result, *data, document_it->second);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.range.start_line != rhs.range.start_line) {
            return lhs.range.start_line < rhs.range.start_line;
        }
        if (lhs.range.start_character != rhs.range.start_character) {
            return lhs.range.start_character < rhs.range.start_character;
        }
        if (lhs.code != rhs.code) {
            return lhs.code < rhs.code;
        }
        return lhs.message < rhs.message;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
                     return lhs.uri == rhs.uri && lhs.code == rhs.code && lhs.message == rhs.message &&
                            lhs.range.start_line == rhs.range.start_line &&
                            lhs.range.start_character == rhs.range.start_character &&
                            lhs.range.end_line == rhs.range.end_line &&
                            lhs.range.end_character == rhs.range.end_character;
                 }),
                 result.end());
    return result;
}

const SemanticEngine::SnapshotData* SemanticEngine::snapshotData() const {
    (void)snapshot();
    return snapshot_data_.get();
}

SemanticLookupResult SemanticEngine::lookupAt(std::string_view uri, int line, int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticLookupResult result{.mode = current_snapshot.mode,
                                .generation = current_snapshot.generation,
                                .query_location = SemanticLocation{.uri = document_uri,
                                                                   .range = ParseRange{.start_line = line,
                                                                                       .start_character = character,
                                                                                       .end_line = line,
                                                                                       .end_character = character}},
                                .unresolved = true};
    const auto* data = snapshotData();
    if (data == nullptr || !data->compilation) {
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    if (const auto instance = moduleInstanceAt(*data, document_uri, line, character)) {
        const auto target_id = !instance->target_stable_id.empty()
                                   ? std::optional<std::string>{instance->target_stable_id}
                                   : findDefinitionSymbolId(*data, instance->module_name);
        if (target_id.has_value()) {
            const auto symbol_it = data->symbols_by_id.find(*target_id);
            if (symbol_it != data->symbols_by_id.end()) {
                result.query_location = SemanticLocation{.uri = instance->uri,
                                                         .range = instance->module_selection_range};
                result.symbol = symbol_it->second.identity;
                result.unresolved = false;
                return result;
            }
        }
        result.messages.push_back("module instance target is not indexed");
        return result;
    }

    const auto id = symbolIdAtLocation(*data, document_uri, line, character);
    if (!id.has_value()) {
        result.messages.push_back("no AST symbol at position");
        return result;
    }

    const auto symbol_it = data->symbols_by_id.find(*id);
    if (symbol_it == data->symbols_by_id.end()) {
        result.messages.push_back("AST symbol identity is not indexed");
        return result;
    }

    const auto reference_it = std::find_if(data->references.begin(),
                                           data->references.end(),
                                           [&](const SnapshotData::IndexedReference& reference) {
                                               return reference.stable_id == *id &&
                                                      reference.location.uri == document_uri &&
                                                      containsPosition(reference.location.range, line, character);
                                           });
    if (reference_it != data->references.end()) {
        result.query_location = reference_it->location;
    }
    else {
        result.query_location = symbol_it->second.identity.location;
    }
    result.symbol = symbol_it->second.identity;
    result.unresolved = false;
    return result;
}

SemanticReferenceResult SemanticEngine::definitionsAt(std::string_view uri,
                                                      int line,
                                                      int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (lookup.symbol.has_value()) {
        result.locations.push_back(lookup.symbol->location);
    }
    return result;
}

SemanticReferenceResult SemanticEngine::typeDefinitionsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                    .messages = lookup.messages,
                                    .unresolved = lookup.unresolved};

    const auto* data = snapshotData();
    if (data != nullptr) {
        if (const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
            document_it != documents_.end()) {
            for (const auto& metadata : CompilationService{}.semanticSymbolMetadata(document_it->second.text,
                                                                                    document_it->second.uri)) {
                const auto type_range = userTypeReferenceRange(document_it->second.text, metadata);
                if (!type_range.has_value() || !containsPosition(*type_range, line, character)) {
                    continue;
                }
                auto locations = typeDefinitionLocationsByName(*data, metadata.type_name);
                if (!locations.empty()) {
                    result.locations = std::move(locations);
                    result.unresolved = false;
                    return result;
                }
            }
        }
    }

    if (!lookup.symbol.has_value()) {
        return result;
    }

    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && symbol_it->second.symbol != nullptr) {
            const auto* declared_type = symbol_it->second.symbol->getDeclaredType();
            if (declared_type != nullptr) {
                const auto& type = declared_type->getType();
                if (const auto type_location = declarationLocationForSymbol(*data->source_manager, type)) {
                    result.locations.push_back(*type_location);
                    return result;
                }
            }
        }
    }

    result.locations.push_back(lookup.symbol->location);
    result.messages.push_back("type definition resolved to declaration because the type has no source location");
    return result;
}

SemanticReferenceResult SemanticEngine::referencesAt(std::string_view uri,
                                                     int line,
                                                     int character,
                                                     bool include_declaration) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }

    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto instance = moduleInstanceAt(*data, document_uri, line, character)) {
        result.locations = moduleImplementationLocations(*data,
                                                         instance->module_name,
                                                         result.truncated);
        if (include_declaration) {
            const auto target_id = !instance->target_stable_id.empty()
                                       ? std::optional<std::string>{instance->target_stable_id}
                                       : findDefinitionSymbolId(*data, instance->module_name);
            if (target_id.has_value()) {
                const auto target_it = data->symbols_by_id.find(*target_id);
                if (target_it != data->symbols_by_id.end()) {
                    result.locations.push_back(target_it->second.identity.location);
                }
            }
        }
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
        result.unresolved = false;
        return result;
    }

    result.locations = locationsForSymbol(*data,
                                          lookup.symbol->stable_id,
                                          include_declaration,
                                          result.truncated);
    if (lookup.symbol->kind == "Definition") {
        bool implementation_truncated = false;
        auto implementations = moduleImplementationLocations(*data,
                                                             lookup.symbol->name,
                                                             implementation_truncated);
        for (auto& location : implementations) {
            if (!include_declaration && sameLocation(location, lookup.query_location)) {
                continue;
            }
            if (result.locations.size() >= kMaxSemanticLocations) {
                result.truncated = true;
                break;
            }
            result.locations.push_back(std::move(location));
        }
        result.truncated = result.truncated || implementation_truncated;
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
    }
    return result;
}

SemanticReferenceResult SemanticEngine::documentHighlightsAt(std::string_view uri,
                                                              int line,
                                                              int character) const {
    auto result = referencesAt(uri, line, character, true);
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    result.locations.erase(std::remove_if(result.locations.begin(),
                                          result.locations.end(),
                                          [&](const SemanticLocation& location) {
                                              return location.uri != document_uri;
                                          }),
                           result.locations.end());
    return result;
}

SemanticReferenceResult SemanticEngine::implementationsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    result.locations = moduleImplementationLocations(*data, lookup.symbol->name, result.truncated);
    result.unresolved = false;
    return result;
}

SemanticHoverResult SemanticEngine::hoverAt(std::string_view uri, int line, int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticHoverResult result{.generation = lookup.generation,
                               .messages = lookup.messages,
                               .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }

    result.contents = "**" + lookup.symbol->kind + "** `" + lookup.symbol->name + "`";
    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && !symbol_it->second.type_display.empty()) {
            result.contents += "\n\nType: `" + symbol_it->second.type_display + "`";
        }
    }
    result.range = lookup.query_location.range;
    return result;
}

SemanticPrepareRenameResult SemanticEngine::prepareRenameAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticPrepareRenameResult result{.generation = lookup.generation,
                                       .messages = lookup.messages,
                                       .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }
    result.placeholder = lookup.symbol->name;
    result.range = lookup.query_location.range;
    return result;
}

SemanticRenameResult SemanticEngine::renameAt(std::string_view uri,
                                              int line,
                                              int character,
                                              std::string_view new_name) const {
    const auto references = referencesAt(uri, line, character, true);
    SemanticRenameResult result{.generation = references.generation,
                                .messages = references.messages,
                                .unresolved = references.unresolved,
                                .truncated = references.truncated};
    for (const auto& location : references.locations) {
        result.edits.push_back(SemanticTextEdit{.location = location,
                                                .new_text = std::string(new_name)});
    }
    return result;
}

SemanticCompletionResult SemanticEngine::completionsAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view prefix) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticCompletionResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::set<std::string> emitted;
    const auto document_it = documents_.find(document_uri);
    const auto* document = document_it == documents_.end() ? nullptr : &document_it->second;
    const auto prefix_start = document == nullptr
                                  ? std::optional<size_t>{}
                                  : completionPrefixStartOffset(document->text, line, character, prefix);

    const auto append_items = [&](const std::vector<SemanticCompletionItem>& items) {
        for (const auto& item : items) {
            appendCompletionItem(result.items, emitted, item, prefix, result.truncated);
            if (result.truncated) {
                return;
            }
        }
    };

    if (document != nullptr && prefix_start.has_value() && *prefix_start > 0 &&
        document->text[*prefix_start - 1] == '`') {
        const auto append_macros = [&](const std::vector<MacroDefinition>& macros) {
            for (const auto& macro : macros) {
                appendCompletionItem(result.items,
                                     emitted,
                                     SemanticCompletionItem{.stable_id = document_uri + "|macro|" + macro.name,
                                                            .label = macro.name,
                                                            .detail = macro.function_like ? "Macro function"
                                                                                          : "Macro",
                                                            .documentation = macroDocumentation(macro),
                                                            .insert_text = macroInsertText(macro),
                                                            .kind = macro.function_like ? 3 : 21,
                                                            .unresolved = false},
                                     prefix,
                                     result.truncated);
                if (result.truncated) {
                    return;
                }
            }
        };
        if (const auto macros_it = data->macros_by_uri.find(document_uri); macros_it != data->macros_by_uri.end()) {
            append_macros(macros_it->second);
        }
        for (const auto& [macro_uri, macros] : data->macros_by_uri) {
            if (macro_uri != document_uri) {
                append_macros(macros);
            }
            if (result.truncated) {
                break;
            }
        }
        return result;
    }

    if (document != nullptr) {
        if (const auto package_name = packageQualifierBeforeCompletion(document->text, line, character, prefix)) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                const auto& symbol = indexed_symbol.identity;
                if (symbol.name == *package_name || symbol.location.uri.empty()) {
                    continue;
                }
                if (symbol.stable_id.find("|" + *package_name + "::") == std::string::npos &&
                    symbol.stable_id.find("." + *package_name + ".") == std::string::npos &&
                    symbol.stable_id.find(*package_name) == std::string::npos) {
                    continue;
                }
                appendSymbolCompletion(result.items,
                                       emitted,
                                       *data,
                                       indexed_symbol,
                                       prefix,
                                       result.truncated);
                if (result.truncated) {
                    return result;
                }
            }
            if (result.items.empty()) {
                result.messages.push_back("package completion had no indexed AST members");
            }
            return result;
        }

        if (prefix_start.has_value() && *prefix_start > 0 && document->text[*prefix_start - 1] == '.') {
            const auto instances_it = data->module_instances_by_uri.find(document_uri);
            if (instances_it == data->module_instances_by_uri.end()) {
                result.unresolved = true;
                result.messages.push_back("named member completion has no indexed module instances");
                return result;
            }
            for (const auto& instance : instances_it->second) {
                if (!parseRangeContainsPosition(instance.range, line, character)) {
                    continue;
                }
                const auto module_it = data->modules_by_name.find(instance.module_name);
                if (module_it != data->modules_by_name.end()) {
                    std::set<std::string> connected_ports;
                    const auto position_offset = utf8OffsetAtUtf16Position(document->text, line, character);
                    const auto search_start = utf8OffsetAtUtf16Position(document->text,
                                                                        instance.selection_range.end_line,
                                                                        instance.selection_range.end_character);
                    const auto search_end = utf8OffsetAtUtf16Position(document->text,
                                                                      instance.range.end_line,
                                                                      instance.range.end_character);
                    if (position_offset.has_value() && search_start.has_value() && search_end.has_value()) {
                        const auto open_paren = openParenBeforePosition(document->text,
                                                                        *search_start,
                                                                        std::min(*position_offset, *search_end));
                        if (open_paren.has_value()) {
                            connected_ports = connectedNamedPortsBeforePosition(document->text,
                                                                                *open_paren,
                                                                                *position_offset);
                        }
                    }
                    const auto module_uri_it = data->module_uris_by_name.find(instance.module_name);
                    appendModulePortCompletions(result.items,
                                                emitted,
                                                *data,
                                                module_it->second,
                                                module_uri_it == data->module_uris_by_name.end()
                                                    ? std::string_view{}
                                                    : std::string_view(module_uri_it->second),
                                                prefix,
                                                connected_ports,
                                                result.truncated);
                    return result;
                }
            }
        }

        if (const auto member_name = memberQualifierBeforeCompletion(document->text, line, character, prefix)) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                if (indexed_symbol.identity.name == *member_name) {
                    continue;
                }
                if (indexed_symbol.identity.kind == "Field" || indexed_symbol.identity.kind == "Member" ||
                    indexed_symbol.identity.kind == "Net" || indexed_symbol.identity.kind == "Variable" ||
                    indexed_symbol.identity.kind == "Parameter" || indexed_symbol.identity.kind == "Subroutine") {
                    appendSymbolCompletion(result.items,
                                           emitted,
                                           *data,
                                           indexed_symbol,
                                           prefix,
                                           result.truncated);
                }
                if (result.truncated) {
                    return result;
                }
            }
            result.messages.push_back("member completion used AST symbol index fallback");
            return result;
        }

        if (prefix_start.has_value() && hasOnlyWhitespaceSinceLineStart(document->text, *prefix_start)) {
            std::vector<std::string> module_names;
            module_names.reserve(data->modules_by_name.size());
            for (const auto& [module_name, _] : data->modules_by_name) {
                module_names.push_back(module_name);
            }
            std::sort(module_names.begin(), module_names.end());
            for (const auto& module_name : module_names) {
                const auto& module = data->modules_by_name.at(module_name);
                const auto module_id = findSymbolIdByNameAndKind(*data, module.name, "Definition")
                                           .value_or(std::string("module|") + module.name);
                const auto module_uri_it = data->module_uris_by_name.find(module.name);
                appendCompletionItem(result.items,
                                     emitted,
                                     SemanticCompletionItem{.stable_id = module_id,
                                                            .label = module.name,
                                                            .detail = moduleSignatureLabel(module),
                                                            .documentation = moduleDocumentation(
                                                                module,
                                                                module_uri_it == data->module_uris_by_name.end()
                                                                    ? std::string_view{}
                                                                    : std::string_view(module_uri_it->second)),
                                                            .insert_text = module.name,
                                                            .kind = 9,
                                                            .unresolved = false},
                                     prefix,
                                     result.truncated);
                if (result.truncated) {
                    return result;
                }
            }
        }
    }

    const auto completion_it = data->completions_by_uri.find(document_uri);
    if (completion_it != data->completions_by_uri.end()) {
        append_items(completion_it->second);
    }
    for (const auto& [completion_uri, items] : data->completions_by_uri) {
        if (completion_uri == document_uri) {
            continue;
        }
        append_items(items);
        if (result.truncated) {
            break;
        }
    }
    return result;
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label) const {
    SemanticCompletionItem item{.stable_id = std::string(stable_id),
                                .label = std::string(label),
                                .insert_text = std::string(label)};
    const auto* data = snapshotData();
    if (data == nullptr) {
        item.unresolved = true;
        return item;
    }

    const auto stable_id_text = std::string(stable_id);
    const auto port_marker = stable_id_text.find("|port|");
    if (port_marker != std::string::npos) {
        const auto port_name = stable_id_text.substr(port_marker + 6);
        for (const auto& [_, module] : data->modules_by_name) {
            const auto module_uri_it = data->module_uris_by_name.find(module.name);
            const auto module_uri = module_uri_it == data->module_uris_by_name.end()
                                        ? std::string_view{}
                                        : std::string_view(module_uri_it->second);
            if (module.port_details.empty()) {
                if (std::find(module.ports.begin(), module.ports.end(), port_name) != module.ports.end()) {
                    const SchematicPort port{.name = port_name,
                                             .direction = {},
                                             .width_text = {},
                                             .range = module.selection_range,
                                             .selection_range = module.selection_range};
                    item.detail = "Port";
                    item.documentation = portDocumentation(module, port, module_uri);
                    item.insert_text = portConnectionSnippet(port_name);
                    return item;
                }
            }
            for (const auto& port : module.port_details) {
                if (port.name != port_name) {
                    continue;
                }
                item.detail = portSignatureLabel(port);
                item.documentation = portDocumentation(module, port, module_uri);
                item.insert_text = portConnectionSnippet(port.name);
                return item;
            }
        }
    }

    const auto macro_marker = stable_id_text.find("|macro|");
    if (macro_marker != std::string::npos) {
        const auto macro_name = stable_id_text.substr(macro_marker + 7);
        for (const auto& [_, macros] : data->macros_by_uri) {
            const auto macro_it = std::find_if(macros.begin(), macros.end(), [&](const MacroDefinition& macro) {
                return macro.name == macro_name;
            });
            if (macro_it == macros.end()) {
                continue;
            }
            item.detail = macro_it->function_like ? "Macro function " + macroSignatureLabel(*macro_it)
                                                  : "Macro";
            item.documentation = macroDocumentation(*macro_it);
            item.insert_text = macroInsertText(*macro_it);
            return item;
        }
        item.unresolved = true;
        return item;
    }

    const auto symbol_it = data->symbols_by_id.find(std::string(stable_id));
    if (symbol_it == data->symbols_by_id.end()) {
        item.unresolved = true;
        return item;
    }

    item.detail = symbol_it->second.identity.kind;
    item.documentation = "**" + symbol_it->second.identity.kind + "** `" +
                         symbol_it->second.identity.name + "`";
    if (symbol_it->second.identity.kind == "Definition") {
        const auto module_it = data->modules_by_name.find(symbol_it->second.identity.name);
        if (module_it != data->modules_by_name.end()) {
            const auto module_uri_it = data->module_uris_by_name.find(module_it->first);
            item.detail = moduleSignatureLabel(module_it->second);
            item.documentation = moduleDocumentation(module_it->second,
                                                     module_uri_it == data->module_uris_by_name.end()
                                                         ? std::string_view{}
                                                         : std::string_view(module_uri_it->second));
            item.insert_text = moduleInstantiationSnippet(module_it->second);
            return item;
        }
    }
    if (!symbol_it->second.type_display.empty()) {
        item.documentation += "\n\nType: `" + symbol_it->second.type_display + "`";
    }
    return item;
}

SemanticSignatureHelpResult SemanticEngine::signatureHelpAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticSignatureHelpResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    const auto document_it = documents_.find(document_uri);
    if (document_it == documents_.end()) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }
    const auto position_offset = utf8OffsetAtUtf16Position(document_it->second.text, line, character);
    if (!position_offset.has_value()) {
        result.unresolved = true;
        result.messages.push_back("signature help position could not be mapped to a source offset");
        return result;
    }

    const auto instances_it = data->module_instances_by_uri.find(document_uri);
    if (instances_it != data->module_instances_by_uri.end()) {
        for (const auto& instance : instances_it->second) {
            if (!parseRangeContainsPosition(instance.range, line, character)) {
                continue;
            }
            const auto search_start = utf8OffsetAtUtf16Position(document_it->second.text,
                                                                instance.selection_range.end_line,
                                                                instance.selection_range.end_character);
            const auto search_end = utf8OffsetAtUtf16Position(document_it->second.text,
                                                              instance.range.end_line,
                                                              instance.range.end_character);
            if (!search_start.has_value() || !search_end.has_value()) {
                continue;
            }
            const auto open_paren = openParenBeforePosition(document_it->second.text,
                                                            *search_start,
                                                            std::min(*position_offset, *search_end));
            if (!open_paren.has_value()) {
                continue;
            }
            const auto module_it = data->modules_by_name.find(instance.module_name);
            if (module_it == data->modules_by_name.end()) {
                result.unresolved = true;
                result.messages.push_back("signature target module is not indexed in the AST snapshot");
                return result;
            }

            result.label = moduleSignatureLabel(module_it->second);
            if (module_it->second.port_details.empty()) {
                result.parameters = module_it->second.ports;
            }
            else {
                for (const auto& port : module_it->second.port_details) {
                    result.parameters.push_back(portSignatureLabel(port));
                }
            }
            const auto parameter_count = result.parameters.size();
            result.active_parameter = parameter_count == 0
                                          ? 0
                                          : std::min(activeParameterAt(document_it->second.text,
                                                                       *open_paren,
                                                                       *position_offset),
                                                     static_cast<int>(parameter_count) - 1);
            return result;
        }
    }

    result.unresolved = true;
    result.messages.push_back("no AST-backed signature invocation at position");
    return result;
}

SemanticInlayHintResult SemanticEngine::inlayHints(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticInlayHintResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
        if (indexed_symbol.identity.location.uri != document_uri || indexed_symbol.type_display.empty() ||
            !rangesOverlapOrTouch(indexed_symbol.identity.location.range, range)) {
            continue;
        }
        result.hints.push_back(SemanticInlayHint{.location = indexed_symbol.identity.location,
                                                 .label = ": " + indexed_symbol.type_display,
                                                 .kind = "type",
                                                 .tooltip = "Resolved type"});
    }

    const auto instances_it = data->module_instances_by_uri.find(document_uri);
    if (instances_it != data->module_instances_by_uri.end()) {
        for (const auto& instance : instances_it->second) {
            if (!rangesOverlapOrTouch(instance.selection_range, range)) {
                continue;
            }
            const auto module_it = data->modules_by_name.find(instance.module_name);
            result.hints.push_back(SemanticInlayHint{
                .location = SemanticLocation{.uri = document_uri,
                                             .range = ParseRange{
                                                 .start_line = instance.selection_range.end_line,
                                                 .start_character = instance.selection_range.end_character,
                                                 .end_line = instance.selection_range.end_line,
                                                 .end_character = instance.selection_range.end_character}},
                .label = ": " + instance.module_name,
                .kind = "type",
                .tooltip = module_it == data->modules_by_name.end()
                               ? std::string{}
                               : moduleSignatureLabel(module_it->second)});
        }
    }
    std::sort(result.hints.begin(), result.hints.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticTokenResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& reference : data->references) {
        if (reference.location.uri != document_uri) {
            continue;
        }
        const auto symbol_it = data->symbols_by_id.find(reference.stable_id);
        auto token_type = std::string("variable");
        if (symbol_it != data->symbols_by_id.end()) {
            const auto& kind = symbol_it->second.identity.kind;
            if (kind == "Package" || kind == "Namespace") {
                token_type = "namespace";
            }
            else if (kind == "Definition" || kind == "TypeAlias" || kind == "Type") {
                token_type = "type";
            }
            else if (kind == "ClassType") {
                token_type = "class";
            }
            else if (kind == "EnumType") {
                token_type = "enum";
            }
            else if (kind == "Interface" || kind == "Modport") {
                token_type = "interface";
            }
            else if (kind == "Subroutine" || kind == "SubroutinePort") {
                token_type = "function";
            }
            else if (kind == "Parameter") {
                token_type = "parameter";
            }
            else if (kind == "EnumValue") {
                token_type = "enumMember";
            }
        }
        result.tokens.push_back(SemanticToken{.location = reference.location,
                                              .token_type = std::move(token_type),
                                              .token_modifier = reference.is_declaration
                                                                    ? std::string("declaration")
                                                                    : std::string{}});
    }
    std::sort(result.tokens.begin(), result.tokens.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticSelectionRangeResult result{.generation = lookup.generation,
                                        .messages = lookup.messages,
                                        .unresolved = lookup.unresolved};
    if (lookup.unresolved) {
        return result;
    }

    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::vector<ParseRange> ranges;
    ranges.push_back(lookup.query_location.range);
    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto ranges_it = data->selection_ranges_by_uri.find(document_uri);
        if (ranges_it != data->selection_ranges_by_uri.end()) {
            for (const auto& candidate : ranges_it->second) {
                if (parseRangeContainsPosition(candidate, line, character) &&
                    rangeContainsRange(candidate, lookup.query_location.range)) {
                    ranges.push_back(candidate);
                }
            }
        }
    }
    const auto document_it = documents_.find(document_uri);
    if (document_it != documents_.end()) {
        if (const auto line_range = lineRangeAtPosition(document_it->second.text, line, character)) {
            if (rangeContainsRange(*line_range, lookup.query_location.range)) {
                ranges.push_back(*line_range);
            }
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const ParseRange& lhs, const ParseRange& rhs) {
        if (lhs.start_line != rhs.start_line) {
            return lhs.start_line > rhs.start_line;
        }
        if (lhs.start_character != rhs.start_character) {
            return lhs.start_character > rhs.start_character;
        }
        if (lhs.end_line != rhs.end_line) {
            return lhs.end_line < rhs.end_line;
        }
        return lhs.end_character < rhs.end_character;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const ParseRange& lhs,
                                                             const ParseRange& rhs) {
                     return lhs.start_line == rhs.start_line &&
                            lhs.start_character == rhs.start_character &&
                            lhs.end_line == rhs.end_line &&
                            lhs.end_character == rhs.end_character;
                 }),
                 ranges.end());

    std::vector<ParseRange> chain;
    for (const auto& candidate : ranges) {
        if (chain.empty() || rangeContainsRange(candidate, chain.back())) {
            chain.push_back(candidate);
        }
    }

    for (size_t index = 0; index < chain.size(); ++index) {
        const auto parent = index + 1 < chain.size()
                                ? std::optional<size_t>{index + 1}
                                : std::optional<size_t>{};
        result.ranges.push_back(SemanticSelectionRange{.range = chain[index], .parent = parent});
    }
    return result;
}

SemanticModuleHierarchyResult SemanticEngine::moduleHierarchy(std::optional<std::string_view> module_name,
                                                              int max_depth) const {
    const auto& current_snapshot = snapshot();
    SemanticModuleHierarchyResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::vector<std::string> root_names;
    if (module_name.has_value()) {
        root_names.push_back(std::string(*module_name));
    }
    else if (!config_.top_modules.empty()) {
        root_names = config_.top_modules;
    }
    else if (const auto inferred = firstUninstantiatedModuleName(data->modules_by_name)) {
        root_names.push_back(*inferred);
    }

    if (root_names.empty()) {
        result.unresolved = true;
        result.messages.push_back("No module definitions are indexed in the design snapshot.");
        return result;
    }

    const auto build_node = [&](const auto& self,
                                std::string_view current_name,
                                const SnapshotData::ModuleInstance* instance,
                                std::vector<std::string>& stack,
                                int depth) -> SemanticHierarchyNode {
        const auto definition_it = data->modules_by_name.find(std::string(current_name));
        if (definition_it == data->modules_by_name.end()) {
            result.partial = true;
            auto node = SemanticHierarchyNode{.module_name = std::string(current_name),
                                              .kind = "module",
                                              .unresolved = true};
            if (instance != nullptr) {
                node.instance_name = instance->instance_name;
                node.instance_range = instance->range;
                node.instance_selection_range = instance->selection_range;
                node.module_selection_range = instance->module_selection_range;
            }
            result.messages.push_back("Unresolved module '" + std::string(current_name) +
                                      "' in design hierarchy.");
            return node;
        }

        const auto& definition = definition_it->second;
        const auto uri_it = data->module_uris_by_name.find(definition.name);
        const auto definition_uri = uri_it == data->module_uris_by_name.end()
                                        ? std::string{}
                                        : uri_it->second;
        const auto is_cycle = std::find(stack.begin(), stack.end(), definition.name) != stack.end();
        auto node = SemanticHierarchyNode{
            .module_name = definition.name,
            .kind = definition.kind,
            .location = SemanticLocation{.uri = definition_uri, .range = definition.range},
            .selection_range = definition.selection_range,
            .unresolved = false,
            .cycle = is_cycle};

        if (instance != nullptr) {
            node.instance_name = instance->instance_name;
            node.instance_range = instance->range;
            node.instance_selection_range = instance->selection_range;
            node.module_selection_range = instance->module_selection_range;
        }

        if (is_cycle) {
            result.partial = true;
            result.messages.push_back("Cycle detected while expanding module '" + definition.name + "'.");
            return node;
        }
        if (depth >= max_depth) {
            node.truncated = true;
            result.truncated = true;
            result.partial = true;
            result.messages.push_back("Module hierarchy expansion reached maxDepth.");
            return node;
        }

        stack.push_back(definition.name);
        for (const auto& child_instance : definition.instances) {
            const auto child = SnapshotData::ModuleInstance{.module_name = child_instance.module_name,
                                                            .instance_name = child_instance.instance_name,
                                                            .uri = definition_uri,
                                                            .range = child_instance.range,
                                                            .selection_range = child_instance.selection_range,
                                                            .module_selection_range =
                                                                child_instance.module_selection_range};
            node.children.push_back(self(self, child.module_name, &child, stack, depth + 1));
        }
        stack.pop_back();
        return node;
    };

    for (const auto& root_name : root_names) {
        std::vector<std::string> stack;
        result.roots.push_back(build_node(build_node, root_name, nullptr, stack, 0));
    }
    return result;
}

SemanticSchematicResult SemanticEngine::schematic(std::optional<std::string_view> module_name,
                                                  int max_depth) const {
    const auto& current_snapshot = snapshot();
    SemanticSchematicResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::optional<std::string> root_name;
    if (module_name.has_value()) {
        root_name = std::string(*module_name);
    }
    else if (!config_.top_modules.empty()) {
        root_name = config_.top_modules.front();
    }
    else {
        root_name = firstUninstantiatedModuleName(data->modules_by_name);
        if (!root_name.has_value() && !data->modules_by_name.empty()) {
            root_name = data->modules_by_name.begin()->first;
            result.messages.push_back("No uninstantiated top module could be inferred for this workspace.");
        }
    }

    if (!root_name.has_value()) {
        result.unresolved = true;
        result.messages.push_back("No module definitions are indexed in the design snapshot.");
        return result;
    }
    result.root_module_id = *root_name;

    std::set<std::string> emitted;
    std::vector<std::string> stack;
    const auto collect = [&](const auto& self,
                             std::string_view current_name,
                             int depth) -> void {
        const auto current = std::string(current_name);
        if (emitted.contains(current)) {
            return;
        }
        const auto schematic_it = data->schematics_by_name.find(current);
        if (schematic_it == data->schematics_by_name.end()) {
            result.partial = true;
            result.messages.push_back("No schematic data found for module '" + current + "'.");
            return;
        }

        const auto uri_it = data->schematic_uris_by_name.find(current);
        const auto schematic_uri = uri_it == data->schematic_uris_by_name.end()
                                       ? std::string{}
                                       : uri_it->second;
        emitted.insert(current);
        result.modules.push_back(SemanticSchematicModuleView{
            .module = SemanticSchematicModule{.id = schematic_it->second.name,
                                              .name = schematic_it->second.name,
                                              .uri = schematic_uri,
                                              .range = schematic_it->second.range,
                                              .selection_range = schematic_it->second.selection_range,
                                              .ports = schematic_it->second.ports,
                                              .cells = schematic_it->second.cells},
            .nets = buildSchematicNets(schematic_it->second, *data)});

        if (depth >= max_depth) {
            result.truncated = true;
            result.partial = true;
            result.messages.push_back("Schematic expansion reached maxDepth.");
            return;
        }
        if (std::find(stack.begin(), stack.end(), current) != stack.end()) {
            result.partial = true;
            result.messages.push_back("Cycle detected while expanding schematic module '" + current + "'.");
            return;
        }

        const auto definition_it = data->modules_by_name.find(current);
        if (definition_it == data->modules_by_name.end()) {
            return;
        }
        stack.push_back(current);
        for (const auto& instance : definition_it->second.instances) {
            self(self, instance.module_name, depth + 1);
        }
        stack.pop_back();
    };

    collect(collect, *root_name, 0);
    return result;
}

SemanticCallHierarchyPrepareResult SemanticEngine::prepareCallHierarchy(std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticCallHierarchyPrepareResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto append_module = [&](const ModuleDefinition& definition, const std::string& module_uri) {
        result.items.push_back(SemanticCallHierarchyItem{.name = definition.name,
                                                         .kind = definition.kind == "interface" ? 11 : 2,
                                                         .detail = definition.kind,
                                                         .uri = module_uri,
                                                         .range = definition.range,
                                                         .selection_range = definition.selection_range});
    };

    for (const auto& entry : data->module_entries) {
        const auto& definition = entry.definition;
        if (entry.uri != document_uri) {
            continue;
        }
        if (parseRangeContainsPosition(definition.selection_range, line, character)) {
            append_module(definition, entry.uri);
            return result;
        }
        for (const auto& instance : definition.instances) {
            if (!parseRangeContainsPosition(instance.module_selection_range, line, character) &&
                !parseRangeContainsPosition(instance.selection_range, line, character)) {
                continue;
            }
            const auto target_it = data->modules_by_name.find(instance.module_name);
            if (target_it == data->modules_by_name.end()) {
                result.unresolved = true;
                result.messages.push_back("Call hierarchy target module is unresolved.");
                return result;
            }
            const auto target_uri_it = data->module_uris_by_name.find(target_it->first);
            append_module(target_it->second,
                          target_uri_it == data->module_uris_by_name.end()
                              ? std::string{}
                              : target_uri_it->second);
            return result;
        }
        if (parseRangeContainsPosition(definition.range, line, character)) {
            append_module(definition, entry.uri);
            return result;
        }
    }

    result.unresolved = true;
    result.messages.push_back("No design hierarchy item at position.");
    return result;
}

SemanticCallHierarchyCallsResult SemanticEngine::incomingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    SemanticCallHierarchyCallsResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto target_entry_it = std::find_if(data->module_entries.begin(),
                                              data->module_entries.end(),
                                              [&](const SnapshotData::ModuleEntry& entry) {
                                                  return entry.definition.name == item.name &&
                                                         entry.uri == item.uri &&
                                                         sameParseRange(entry.definition.selection_range,
                                                                        item.selection_range);
                                              });
    if (target_entry_it == data->module_entries.end()) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy target module is not indexed.");
        return result;
    }

    for (const auto& caller_entry : data->module_entries) {
        const auto& caller = caller_entry.definition;
        for (const auto& instance : caller.instances) {
            if (instance.module_name != target_entry_it->definition.name) {
                continue;
            }
            result.calls.push_back(SemanticCallHierarchyCall{
                .item = SemanticCallHierarchyItem{.name = caller.name,
                                                  .kind = caller.kind == "interface" ? 11 : 2,
                                                  .detail = caller.kind,
                                                  .uri = caller_entry.uri,
                                                  .range = caller.range,
                                                  .selection_range = caller.selection_range},
                .from_ranges = {instance.module_selection_range}});
        }
    }
    return result;
}

SemanticCallHierarchyCallsResult SemanticEngine::outgoingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    SemanticCallHierarchyCallsResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto source_entry_it = std::find_if(data->module_entries.begin(),
                                              data->module_entries.end(),
                                              [&](const SnapshotData::ModuleEntry& entry) {
                                                  return entry.definition.name == item.name &&
                                                         entry.uri == item.uri &&
                                                         sameParseRange(entry.definition.selection_range,
                                                                        item.selection_range);
                                              });
    if (source_entry_it == data->module_entries.end()) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy source module is not indexed.");
        return result;
    }

    for (const auto& instance : source_entry_it->definition.instances) {
        const auto target_it = data->modules_by_name.find(instance.module_name);
        if (target_it == data->modules_by_name.end()) {
            continue;
        }
        const auto target_uri_it = data->module_uris_by_name.find(target_it->first);
        result.calls.push_back(SemanticCallHierarchyCall{
            .item = SemanticCallHierarchyItem{.name = target_it->second.name,
                                              .kind = target_it->second.kind == "interface" ? 11 : 2,
                                              .detail = target_it->second.kind,
                                              .uri = target_uri_it == data->module_uris_by_name.end()
                                                         ? std::string{}
                                                         : target_uri_it->second,
                                              .range = target_it->second.range,
                                              .selection_range = target_it->second.selection_range},
            .from_ranges = {instance.module_selection_range}});
    }
    return result;
}

SemanticConeTrace SemanticEngine::backwardConeAt(std::string_view uri,
                                                 int line,
                                                 int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticConeTrace trace{.generation = lookup.generation,
                            .messages = lookup.messages,
                            .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        if (trace.messages.empty()) {
            trace.messages.push_back("No signal symbol was found at the requested position.");
        }
        return trace;
    }

    const auto* data = snapshotData();
    if (data == nullptr) {
        trace.unresolved = true;
        trace.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return trace;
    }

    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto assignments_it = data->assignments_by_uri.find(document_uri);
    if (assignments_it == data->assignments_by_uri.end()) {
        trace.messages.push_back("No continuous assignments are indexed for the current document.");
        return trace;
    }

    const auto symbol_id_at_range = [&](const ParseRange& range) -> std::optional<std::string> {
        return symbolIdAtLocation(*data, document_uri, range.start_line, range.start_character);
    };

    std::vector<Identifier> document_identifiers;
    if (const auto document_it = documents_.find(document_uri); document_it != documents_.end()) {
        CompilationService compilation_service;
        document_identifiers = compilation_service.identifiers(document_it->second.text);
    }

    const auto append_node = [&](const std::string& stable_id) {
        if (std::find_if(trace.nodes.begin(), trace.nodes.end(), [&](const SemanticConeNode& node) {
                return node.id == stable_id;
            }) != trace.nodes.end()) {
            return;
        }
        const auto symbol_it = data->symbols_by_id.find(stable_id);
        if (symbol_it == data->symbols_by_id.end()) {
            return;
        }
        trace.nodes.push_back(SemanticConeNode{.id = stable_id,
                                               .name = symbol_it->second.identity.name,
                                               .location = symbol_it->second.identity.location,
                                               .bit_width = std::nullopt});
    };

    trace.root_symbol_id = lookup.symbol->stable_id;
    append_node(lookup.symbol->stable_id);

    std::vector<std::string> pending{lookup.symbol->stable_id};
    std::set<std::string> visited;
    std::set<std::string> emitted_edges;
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto current_id = pending[index];
        if (!visited.insert(current_id).second) {
            continue;
        }
        const auto current_symbol_it = data->symbols_by_id.find(current_id);
        if (current_symbol_it == data->symbols_by_id.end()) {
            continue;
        }

        for (const auto& assignment : assignments_it->second) {
            const auto left_id = symbol_id_at_range(assignment.left_range);
            if (!left_id.has_value() || *left_id != current_id) {
                continue;
            }

            for (const auto& identifier : document_identifiers) {
                if (!rangeContainsRange(assignment.right_range, identifier.range)) {
                    continue;
                }
                const auto input_id = symbol_id_at_range(identifier.range);
                if (!input_id.has_value()) {
                    continue;
                }

                append_node(*input_id);
                const auto edge_key = current_id + "\n" + *input_id + "\n" +
                                      std::to_string(assignment.range.start_line) + ":" +
                                      std::to_string(assignment.range.start_character);
                if (emitted_edges.insert(edge_key).second) {
                    trace.edges.push_back(SemanticConeEdge{.from_symbol_id = current_id,
                                                           .to_symbol_id = *input_id,
                                                           .location = SemanticLocation{.uri = document_uri,
                                                                                        .range = assignment.range},
                                                           .expression = assignment.right_expression});
                }
                if (!visited.contains(*input_id)) {
                    pending.push_back(*input_id);
                }
            }
        }

        if (trace.nodes.size() >= kMaxSemanticLocations || trace.edges.size() >= kMaxSemanticLocations) {
            trace.truncated = true;
            trace.partial = true;
            trace.messages.push_back("Backward cone reached the result cap.");
            break;
        }
    }

    trace.unresolved = false;
    std::sort(trace.nodes.begin(), trace.nodes.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    std::sort(trace.edges.begin(), trace.edges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.from_symbol_id != rhs.from_symbol_id) {
            return lhs.from_symbol_id < rhs.from_symbol_id;
        }
        if (lhs.to_symbol_id != rhs.to_symbol_id) {
            return lhs.to_symbol_id < rhs.to_symbol_id;
        }
        return locationLess(lhs.location, rhs.location);
    });
    return trace;
}

SemanticCodeActionResult SemanticEngine::codeActionsAt(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticCodeActionResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto document_it = documents_.find(document_uri);
    if (document_it == documents_.end()) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }
    const auto& document = document_it->second;
    const auto insert_range = endOfTextRange(document.text);

    CompilationService compilation_service;
    for (const auto& include : compilation_service.includeDirectives(document.text)) {
        if (!rangesIntersect(include.range, range) ||
            resolveIncludeTarget(workspace_root_uri_, document_uri, include.target).has_value()) {
            continue;
        }
        const auto target = proposedIncludeTarget(workspace_root_uri_, document_uri, include.target);
        if (!target.has_value()) {
            continue;
        }

        result.actions.push_back(SemanticCodeAction{
            .title = "Create include file '" + include.target + "'",
            .kind = "quickfix",
            .is_preferred = true,
            .diagnostics = {SemanticDiagnosticData{.code = std::string(kUnknownIncludeDiagnosticCode),
                                                   .message = unknownIncludeMessage(include.target),
                                                   .range = include.range,
                                                   .severity = 1}},
            .create_files = {SemanticCodeActionCreateFile{.uri = pathToFileUri(*target),
                                                          .ignore_if_exists = true}}});
    }

    const auto modules = compilation_service.moduleDefinitions(document.text, document_uri);
    for (const auto& module : modules) {
        for (const auto& instance : module.instances) {
            if (!rangesIntersect(instance.module_selection_range, range) ||
                data->modules_by_name.contains(instance.module_name) ||
                !isValidIdentifier(instance.module_name)) {
                continue;
            }
            result.actions.push_back(SemanticCodeAction{
                .title = "Create stub module '" + instance.module_name + "'",
                .kind = "quickfix",
                .diagnostics = {SemanticDiagnosticData{.code = std::string(kUnresolvedModuleDiagnosticCode),
                                                       .message = unresolvedModuleMessage(instance.module_name),
                                                       .range = instance.module_selection_range,
                                                       .severity = 1}},
                .edits = {SemanticCodeActionEdit{.uri = document_uri,
                                                 .range = insert_range,
                                                 .new_text = moduleStubInsertionText(document.text,
                                                                                     instance.module_name)}}});
        }
    }

    for (const auto& schematic : compilation_service.moduleSchematics(document.text, document_uri)) {
        for (const auto& cell : schematic.cells) {
            if (cell.kind != "module" || !rangesIntersect(cell.range, range)) {
                continue;
            }
            const auto module_it = data->modules_by_name.find(cell.type);
            if (module_it == data->modules_by_name.end()) {
                continue;
            }
            const auto missing_ports = missingPorts(cell, module_it->second);
            if (missing_ports.empty()) {
                continue;
            }
            const auto insertion_range = instancePortInsertionRange(document.text, cell);
            if (!insertion_range.has_value()) {
                continue;
            }
            result.actions.push_back(SemanticCodeAction{
                .title = "Add missing port connections to '" + cell.name + "'",
                .kind = "quickfix",
                .edits = {SemanticCodeActionEdit{.uri = document_uri,
                                                 .range = *insertion_range,
                                                 .new_text = missingPortConnectionText(missing_ports,
                                                                                       !cell.connections.empty())}}});
        }
    }

    std::set<std::string> emitted_type_names;
    for (const auto& diagnostic : diagnosticsFor(document_uri)) {
        if (diagnostic.code != kUnresolvedTypeDiagnosticCode || !rangesIntersect(diagnostic.range, range)) {
            continue;
        }
        const auto type_name = textForParseRange(document.text, diagnostic.range);
        if (!type_name.has_value() || !isValidIdentifier(*type_name) ||
            !emitted_type_names.insert(*type_name).second) {
            continue;
        }
        result.actions.push_back(SemanticCodeAction{
            .title = "Create typedef '" + *type_name + "'",
            .kind = "quickfix",
            .diagnostics = {SemanticDiagnosticData{.code = diagnostic.code,
                                                   .message = diagnostic.message,
                                                   .range = diagnostic.range,
                                                   .severity = diagnostic.severity}},
            .edits = {SemanticCodeActionEdit{.uri = document_uri,
                                             .range = insert_range,
                                             .new_text = typedefSkeletonInsertionText(document.text,
                                                                                     *type_name)}}});
    }

    return result;
}

SemanticWorkspaceSymbolResult SemanticEngine::workspaceSymbols(std::string_view query,
                                                               size_t limit) const {
    const auto& current_snapshot = snapshot();
    SemanticWorkspaceSymbolResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::set<std::string> emitted_ids;
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        const auto& identity = indexed_symbol.identity;
        if (identity.name.empty() || identity.location.uri.empty() ||
            !fuzzyMatch(query, identity.name) || !emitted_ids.insert(stable_id).second) {
            continue;
        }
        result.symbols.push_back(SemanticWorkspaceSymbol{.name = identity.name,
                                                         .kind = lspSymbolKindForSemanticKind(identity.kind),
                                                         .location = identity.location,
                                                         .selection_range = identity.location.range,
                                                         .stable_id = stable_id});
    }

    std::sort(result.symbols.begin(), result.symbols.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return locationLess(lhs.location, rhs.location);
    });

    if (limit > 0 && result.symbols.size() > limit) {
        result.symbols.resize(limit);
        result.truncated = true;
        result.messages.push_back("workspace/symbol results were truncated at " + std::to_string(limit) +
                                  " entries");
    }
    return result;
}

void SemanticEngine::rebuildDependenciesFor(std::string_view document_uri, std::string_view text) {
    const auto normalized_uri = withoutTrailingSlash(normalizeFileUri(document_uri));
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), normalized_uri),
                             including_uris.end());
    }

    CompilationService compilation_service;
    std::vector<std::string> included_uris;
    for (const auto& include : compilation_service.includeDirectives(text)) {
        included_uris.push_back(joinFileUri(uriDirectory(normalized_uri), include.target));
    }
    std::sort(included_uris.begin(), included_uris.end());
    included_uris.erase(std::unique(included_uris.begin(), included_uris.end()), included_uris.end());
    includes_[normalized_uri] = included_uris;

    for (const auto& included_uri : included_uris) {
        auto& including_uris = reverse_includes_[included_uri];
        including_uris.push_back(normalized_uri);
        std::sort(including_uris.begin(), including_uris.end());
        including_uris.erase(std::unique(including_uris.begin(), including_uris.end()), including_uris.end());
    }
}

void SemanticEngine::rebuildSnapshot() const {
    auto data = std::make_unique<SnapshotData>();
    data->source_manager = std::make_unique<slang::SourceManager>();
    data->source_manager->setDisableProximatePaths(true);
    const auto options = makeCompilationOptions();
    data->syntax_trees.reserve(documents_.size());

    SemanticEngineSnapshot next{};
    next.generation = generation_;
    next.mode = config_.build.has_value() || config_.build_pattern.has_value() ||
                        !config_.top_modules.empty()
                    ? SemanticEngineMode::Design
                    : SemanticEngineMode::Shallow;
    next.top_modules = config_.top_modules;
    next.dirty_document_uris = dirtyDocumentUris();
    next.has_shallow_ast = false;
    next.has_design_ast = false;

    for (const auto& document_entry : documents_) {
        next.document_uris.push_back(document_entry.first);
    }
    std::sort(next.document_uris.begin(), next.document_uris.end());

    for (const auto& uri : next.document_uris) {
        const auto document_it = documents_.find(uri);
        if (document_it == documents_.end()) {
            continue;
        }

        CompilationService compilation_service;
        data->macros_by_uri[uri] = compilation_service.macroDefinitions(document_it->second.text);
        data->package_imports_by_uri[uri] = compilation_service.packageImports(document_it->second.text);
        data->metadata_by_uri[uri] = compilation_service.semanticSymbolMetadata(document_it->second.text,
                                                                                uri);
        const auto append_symbol_ranges = [&](const auto& self,
                                              const std::vector<DocumentSymbol>& symbols) -> void {
            for (const auto& symbol : symbols) {
                data->selection_ranges_by_uri[uri].push_back(symbol.range);
                data->selection_ranges_by_uri[uri].push_back(symbol.selection_range);
                self(self, symbol.children);
            }
        };
        append_symbol_ranges(append_symbol_ranges,
                             compilation_service.documentSymbols(document_it->second.text, uri));

        const auto modules = compilation_service.moduleDefinitions(document_it->second.text, uri);
        for (const auto& module : modules) {
            data->module_entries.push_back(SnapshotData::ModuleEntry{.uri = uri, .definition = module});
            data->modules_by_name.try_emplace(module.name, module);
            data->module_uris_by_name.try_emplace(module.name, uri);
            for (const auto& instance : module.instances) {
                data->module_instances_by_uri[uri].push_back(SnapshotData::ModuleInstance{
                    .module_name = instance.module_name,
                    .instance_name = instance.instance_name,
                    .target_stable_id = {},
                    .uri = uri,
                    .range = instance.range,
                    .selection_range = instance.selection_range,
                    .module_selection_range = instance.module_selection_range});
            }
            data->selection_ranges_by_uri[uri].push_back(module.range);
            data->selection_ranges_by_uri[uri].push_back(module.selection_range);
            for (const auto& instance : module.instances) {
                data->selection_ranges_by_uri[uri].push_back(instance.range);
                data->selection_ranges_by_uri[uri].push_back(instance.selection_range);
                data->selection_ranges_by_uri[uri].push_back(instance.module_selection_range);
            }
        }
        for (const auto& schematic : compilation_service.moduleSchematics(document_it->second.text, uri)) {
            data->schematics_by_name.try_emplace(schematic.name, schematic);
            data->schematic_uris_by_name.try_emplace(schematic.name, uri);
        }
        const auto assignments = compilation_service.continuousAssignments(document_it->second.text, uri);
        data->assignments_by_uri[uri] = assignments;
        for (const auto& assignment : assignments) {
            data->selection_ranges_by_uri[uri].push_back(assignment.range);
            data->selection_ranges_by_uri[uri].push_back(assignment.left_range);
            data->selection_ranges_by_uri[uri].push_back(assignment.right_range);
        }

        auto tree = slang::syntax::SyntaxTree::fromFileInMemory(document_it->second.text,
                                                                *data->source_manager,
                                                                "source",
                                                                fileUriToPath(uri),
                                                                options);
        if (tree) {
            data->syntax_trees.push_back(std::move(tree));
        }
    }

    if (!data->syntax_trees.empty()) {
        try {
            data->compilation = std::make_unique<slang::ast::Compilation>(options);
            for (auto& tree : data->syntax_trees) {
                data->compilation->addSyntaxTree(tree);
            }
            slang::DiagnosticEngine diagnostic_engine(*data->source_manager);
            for (auto& tree : data->syntax_trees) {
                for (const auto& diagnostic : tree->diagnostics()) {
                    const auto uri = diagnosticUri(*data->source_manager, diagnostic);
                    if (uri.empty() || documents_.find(uri) == documents_.end()) {
                        continue;
                    }
                    const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                    next.diagnostics.push_back(
                        SemanticEngineDiagnostic{.uri = uri,
                                                 .code = std::string("slang:") +
                                                         std::string(slang::toString(diagnostic.code)),
                                                 .message = diagnostic_engine.formatMessage(diagnostic),
                                                 .range = sourceRangeForDiagnostic(*data->source_manager, diagnostic),
                                                 .severity = toLspSeverity(severity)});
                }
            }
            next.has_shallow_ast = true;
            next.has_design_ast = next.mode == SemanticEngineMode::Design;

            const auto& root = data->compilation->getRoot();
            SemanticIndexVisitor<SnapshotData> visitor(*data, *data->source_manager, documents_);
            root.visit(visitor);
            for (const auto* definition : data->compilation->getDefinitions()) {
                if (definition != nullptr) {
                    insertSymbol(*data, *data->source_manager, *definition);
                }
            }
            addDeclarationReferences(*data);
            addModuleInstantiationReferences(*data, documents_);
            sortSnapshotIndexes(*data);

            for (const auto& diagnostic : data->compilation->getSemanticDiagnostics()) {
                const auto uri = diagnosticUri(*data->source_manager, diagnostic);
                if (uri.empty() || documents_.find(uri) == documents_.end()) {
                    continue;
                }
                const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                next.diagnostics.push_back(
                    SemanticEngineDiagnostic{.uri = uri,
                                             .code = std::string("slang:") +
                                                     std::string(slang::toString(diagnostic.code)),
                                             .message = diagnostic_engine.formatMessage(diagnostic),
                                             .range = sourceRangeForDiagnostic(*data->source_manager, diagnostic),
                                             .severity = toLspSeverity(severity)});
            }
        }
        catch (...) {
            next.has_shallow_ast = false;
            next.has_design_ast = false;
            data.reset();
        }
    }

    snapshot_ = std::move(next);
    snapshot_data_ = std::move(data);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
