#include "CodeActionProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <unordered_map>

namespace pristine::analysis::semantic {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUnknownIncludeDiagnosticCode = "unknownInclude";
constexpr std::string_view kUnresolvedModuleDiagnosticCode = "unresolvedModule";
constexpr std::string_view kUnresolvedPackageDiagnosticCode = "unresolvedPackage";
constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";
constexpr std::string_view kMissingImportDiagnosticCode = "missingImport";

struct MacroInvocation {
    std::string name;
    bool function_like = false;
    size_t argument_count = 0;
    std::vector<std::string> arguments;
    ParseRange range;
};

int comparePosition(int lhs_line, int lhs_character, int rhs_line, int rhs_character) {
    if (lhs_line != rhs_line) {
        return lhs_line < rhs_line ? -1 : 1;
    }
    if (lhs_character == rhs_character) {
        return 0;
    }
    return lhs_character < rhs_character ? -1 : 1;
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
        }
        else {
            ++character;
        }
    }
    return ParseRange{.start_line = line,
                      .start_character = character,
                      .end_line = line,
                      .end_character = character};
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

std::string lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

bool looksLikeUndefinedMacroDiagnostic(const SemanticEngineDiagnostic& diagnostic) {
    if (!diagnostic.code.starts_with("slang:")) {
        return false;
    }
    const auto code = lowercase(diagnostic.code);
    const auto message = lowercase(diagnostic.message);
    return code.find("macro") != std::string::npos ||
           (message.find("macro") != std::string::npos &&
            (message.find("undefined") != std::string::npos ||
             message.find("not defined") != std::string::npos ||
             message.find("unknown") != std::string::npos));
}

bool offsetsIntersect(size_t lhs_start, size_t lhs_end, size_t rhs_start, size_t rhs_end) {
    return lhs_end > rhs_start && rhs_end > lhs_start;
}

ParseRange pointRangeAtUtf8Offset(std::string_view text, size_t target_offset);

bool offsetRangeIntersectsSelection(size_t candidate_start,
                                    size_t candidate_end,
                                    size_t selection_start,
                                    size_t selection_end) {
    if (selection_start == selection_end) {
        return selection_start >= candidate_start && selection_start <= candidate_end;
    }
    return offsetsIntersect(candidate_start, candidate_end, selection_start, selection_end);
}

std::string trimCopy(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

ParseRange rangeForUtf8Offsets(std::string_view text, size_t start_offset, size_t end_offset) {
    auto start = pointRangeAtUtf8Offset(text, start_offset);
    auto end = pointRangeAtUtf8Offset(text, end_offset);
    return ParseRange{.start_line = start.start_line,
                      .start_character = start.start_character,
                      .end_line = end.start_line,
                      .end_character = end.start_character};
}

struct MacroArgumentList {
    std::vector<std::string> arguments;
    size_t end_offset = 0;
};

std::optional<MacroArgumentList> parseMacroArgumentList(std::string_view text,
                                                        size_t open_paren_offset) {
    if (open_paren_offset >= text.size() || text[open_paren_offset] != '(') {
        return std::nullopt;
    }

    std::vector<std::string> arguments;
    size_t depth = 0;
    size_t argument_start = open_paren_offset + 1;
    bool saw_argument_token = false;
    for (size_t offset = open_paren_offset; offset < text.size(); ++offset) {
        const char ch = text[offset];
        if (ch == '(') {
            ++depth;
            if (depth == 1) {
                argument_start = offset + 1;
            }
            continue;
        }
        if (ch == ')') {
            if (depth == 0) {
                return std::nullopt;
            }
            if (depth == 1) {
                if (saw_argument_token) {
                    arguments.push_back(trimCopy(text.substr(argument_start, offset - argument_start)));
                }
                return MacroArgumentList{.arguments = std::move(arguments),
                                         .end_offset = offset + 1};
            }
            --depth;
            continue;
        }
        if (depth == 1 && ch == ',') {
            arguments.push_back(trimCopy(text.substr(argument_start, offset - argument_start)));
            argument_start = offset + 1;
            saw_argument_token = false;
            continue;
        }
        if (depth >= 1 && std::isspace(static_cast<unsigned char>(ch)) == 0) {
            saw_argument_token = true;
        }
    }
    return std::nullopt;
}

size_t countMacroArguments(std::string_view text, size_t open_paren_offset) {
    size_t depth = 0;
    size_t argument_count = 0;
    bool saw_argument_token = false;
    for (size_t offset = open_paren_offset; offset < text.size(); ++offset) {
        const char ch = text[offset];
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')') {
            if (depth == 0) {
                return argument_count;
            }
            --depth;
            if (depth == 0) {
                return saw_argument_token ? argument_count + 1 : 0;
            }
            continue;
        }
        if (depth != 1) {
            continue;
        }
        if (ch == ',') {
            ++argument_count;
            saw_argument_token = false;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            saw_argument_token = true;
        }
    }
    return saw_argument_token ? argument_count + 1 : argument_count;
}

std::optional<MacroInvocation> macroInvocationAtRange(std::string_view text,
                                                      const ParseRange& range) {
    const auto start_offset = utf8OffsetAtUtf16Position(text,
                                                        range.start_line,
                                                        range.start_character);
    const auto end_offset = utf8OffsetAtUtf16Position(text,
                                                      range.end_line,
                                                      range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value()) {
        return std::nullopt;
    }

    const auto line_start = text.rfind('\n', *start_offset);
    const size_t search_begin = line_start == std::string_view::npos ? 0 : line_start + 1;
    const auto line_end = text.find('\n', *start_offset);
    const size_t search_end = line_end == std::string_view::npos ? text.size() : line_end;
    auto tick = text.find('`', search_begin);
    while (tick != std::string_view::npos && tick < search_end) {
        const size_t name_start = tick + 1;
        if (name_start < text.size() && isIdentifierStart(text[name_start])) {
            size_t name_end = name_start + 1;
            while (name_end < text.size() && isIdentifierContinue(text[name_end])) {
                ++name_end;
            }

            size_t invocation_end = name_end;
            std::vector<std::string> arguments;
            bool function_like = false;
            size_t cursor = name_end;
            while (cursor < search_end && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
                ++cursor;
            }
            if (cursor < text.size() && text[cursor] == '(') {
                if (const auto argument_list = parseMacroArgumentList(text, cursor)) {
                    function_like = true;
                    arguments = argument_list->arguments;
                    invocation_end = argument_list->end_offset;
                }
            }

            if (offsetRangeIntersectsSelection(tick,
                                               invocation_end,
                                               *start_offset,
                                               *end_offset)) {
                return MacroInvocation{
                    .name = std::string(text.substr(name_start, name_end - name_start)),
                    .function_like = function_like,
                    .argument_count = arguments.size(),
                    .arguments = std::move(arguments),
                    .range = rangeForUtf8Offsets(text, tick, invocation_end)};
            }
        }
        tick = text.find('`', tick + 1);
    }
    return std::nullopt;
}

std::optional<MacroInvocation> macroInvocationFromDiagnostic(std::string_view text,
                                                             const ParseRange& range) {
    const auto start_offset = utf8OffsetAtUtf16Position(text,
                                                        range.start_line,
                                                        range.start_character);
    const auto end_offset = utf8OffsetAtUtf16Position(text,
                                                      range.end_line,
                                                      range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value()) {
        return std::nullopt;
    }

    const auto line_start = text.rfind('\n', *start_offset);
    const size_t search_begin = line_start == std::string_view::npos ? 0 : line_start + 1;
    const auto line_end = text.find('\n', *start_offset);
    const size_t search_end = line_end == std::string_view::npos ? text.size() : line_end;
    auto tick = text.find('`', search_begin);
    while (tick != std::string_view::npos && tick < search_end) {
        const size_t name_start = tick + 1;
        if (name_start < text.size() && isIdentifierStart(text[name_start])) {
            size_t name_end = name_start + 1;
            while (name_end < text.size() && isIdentifierContinue(text[name_end])) {
                ++name_end;
            }
            if (offsetsIntersect(name_start, name_end, *start_offset, *end_offset) ||
                offsetsIntersect(tick, name_end, *start_offset, *end_offset)) {
                MacroInvocation invocation;
                invocation.name = std::string(text.substr(name_start, name_end - name_start));
                invocation.function_like = name_end < text.size() && text[name_end] == '(';
                invocation.argument_count =
                    invocation.function_like ? countMacroArguments(text, name_end) : 0;
                invocation.range = rangeForUtf8Offsets(text, tick, name_end);
                return invocation;
            }
        }
        tick = text.find('`', tick + 1);
    }
    return std::nullopt;
}

std::string unknownIncludeMessage(std::string_view target) {
    return std::string("Include file '") + std::string(target) + "' could not be resolved.";
}

std::string unresolvedModuleMessage(std::string_view module_name) {
    return std::string("Module '") + std::string(module_name) + "' could not be resolved.";
}

std::string packageStubInsertionText(std::string_view text, std::string_view package_name) {
    std::string insertion = text.empty() || text.back() == '\n' ? "\n" : "\n\n";
    insertion += "package ";
    insertion += package_name;
    insertion += ";\nendpackage\n";
    return insertion;
}

std::string importInsertionText(std::string_view text, std::string_view package_name) {
    std::string insertion;
    if (!text.empty() && text.front() != '\n') {
        insertion += "\n";
    }
    insertion += "import ";
    insertion += package_name;
    insertion += "::*;\n";
    return insertion;
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

std::string macroDefinitionInsertionText(const MacroInvocation& invocation) {
    std::string insertion = "`define ";
    insertion += invocation.name;
    if (invocation.function_like) {
        insertion += "(";
        for (size_t index = 0; index < invocation.argument_count; ++index) {
            if (index != 0) {
                insertion += ", ";
            }
            insertion += "arg";
            insertion += std::to_string(index);
        }
        insertion += ")";
        if (invocation.argument_count > 0) {
            insertion += " arg0";
        }
    }
    else {
        insertion += " 1";
    }
    insertion += "\n\n";
    return insertion;
}

bool macroMatchesInvocation(const MacroDefinition& macro, const MacroInvocation& invocation) {
    if (macro.name != invocation.name || macro.function_like != invocation.function_like) {
        return false;
    }
    if (macro.function_like && macro.parameters.size() != invocation.arguments.size()) {
        return false;
    }
    return true;
}

bool rangeStartsBefore(const ParseRange& lhs, const ParseRange& rhs) {
    return comparePosition(lhs.start_line,
                           lhs.start_character,
                           rhs.start_line,
                           rhs.start_character) < 0;
}

const MacroDefinition* findMacroDefinition(const CodeActionContext& context,
                                           const MacroInvocation& invocation) {
    if (context.macros_by_uri == nullptr) {
        return nullptr;
    }

    const MacroDefinition* fallback = nullptr;
    for (const auto& [uri, macros] : *context.macros_by_uri) {
        for (const auto& macro : macros) {
            if (!macroMatchesInvocation(macro, invocation)) {
                continue;
            }
            if (uri == context.document.uri && rangeStartsBefore(macro.range, invocation.range)) {
                fallback = &macro;
                continue;
            }
            if (fallback == nullptr) {
                fallback = &macro;
            }
        }
    }
    return fallback;
}

std::string expandMacroBody(const MacroDefinition& macro, const MacroInvocation& invocation) {
    if (!macro.function_like) {
        return macro.body;
    }

    std::unordered_map<std::string_view, std::string_view> arguments_by_parameter;
    for (size_t index = 0; index < macro.parameters.size(); ++index) {
        arguments_by_parameter.emplace(macro.parameters[index], invocation.arguments[index]);
    }

    std::string expanded;
    expanded.reserve(macro.body.size());
    for (size_t offset = 0; offset < macro.body.size();) {
        if (isIdentifierStart(macro.body[offset])) {
            size_t end = offset + 1;
            while (end < macro.body.size() && isIdentifierContinue(macro.body[end])) {
                ++end;
            }
            const std::string_view token(macro.body.data() + offset, end - offset);
            if (const auto argument_it = arguments_by_parameter.find(token);
                argument_it != arguments_by_parameter.end()) {
                expanded.append(argument_it->second);
            }
            else {
                expanded.append(token);
            }
            offset = end;
            continue;
        }
        expanded.push_back(macro.body[offset]);
        ++offset;
    }
    return expanded;
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

std::optional<fs::path> resolveIncludeTarget(std::string_view workspace_root_uri,
                                             std::string_view document_uri,
                                             std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    std::vector<fs::path> candidates;

    if (target_path.is_absolute()) {
        candidates.push_back(target_path);
    }
    else {
        const auto document_path = fs::path(fileUriToPath(document_uri));
        candidates.push_back(document_path.parent_path() / target_path);
    }

    if (!target_path.is_absolute() && !workspace_root_uri.empty()) {
        candidates.push_back(fs::path(fileUriToPath(workspace_root_uri)) / target_path);
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
    const auto document_path = fs::path(fileUriToPath(document_uri));
    if (!document_path.empty()) {
        return document_path.parent_path() / target_path;
    }
    if (!workspace_root_uri.empty()) {
        return fs::path(fileUriToPath(workspace_root_uri)) / target_path;
    }
    return std::nullopt;
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

std::optional<std::string> packageNameFromMissingImportDiagnostic(const CodeActionContext& context,
                                                                 const SemanticEngineDiagnostic& diagnostic) {
    const auto type_name = textForParseRange(context.document.text, diagnostic.range);
    if (!type_name.has_value()) {
        return std::nullopt;
    }
    std::vector<std::string> packages;
    for (const auto& [_, symbol] : context.symbols_by_id) {
        if (symbol.identity.name != *type_name) {
            continue;
        }
        for (const auto& [__, package_symbol] : context.symbols_by_id) {
            if (package_symbol.identity.kind == "Package" &&
                package_symbol.identity.location.uri == symbol.identity.location.uri) {
                packages.push_back(package_symbol.identity.name);
            }
        }
    }
    std::sort(packages.begin(), packages.end());
    packages.erase(std::unique(packages.begin(), packages.end()), packages.end());
    if (packages.size() != 1) {
        return std::nullopt;
    }
    const auto imports_it = context.package_imports_by_uri.find(context.document.uri);
    if (imports_it != context.package_imports_by_uri.end() &&
        std::any_of(imports_it->second.begin(),
                    imports_it->second.end(),
                    [&](const PackageImport& import) {
                        return import.package_name == packages.front();
                    })) {
        return std::nullopt;
    }
    return packages.front();
}

bool hasPortListSyntax(std::string_view text, const SchematicCell& cell) {
    const auto start_offset = utf8OffsetAtUtf16Position(text,
                                                        cell.range.start_line,
                                                        cell.range.start_character);
    const auto end_offset = utf8OffsetAtUtf16Position(text,
                                                      cell.range.end_line,
                                                      cell.range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value() || *start_offset >= *end_offset) {
        return false;
    }
    const auto bounded_end = std::min(*end_offset, text.size());
    return std::find(text.begin() + static_cast<std::ptrdiff_t>(*start_offset),
                     text.begin() + static_cast<std::ptrdiff_t>(bounded_end),
                     '(') != text.begin() + static_cast<std::ptrdiff_t>(bounded_end);
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

} // namespace

SemanticCodeActionResult codeActionsAt(const CodeActionContext& context) {
    SemanticCodeActionResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    if (context.document.uri.empty()) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }

    const auto insert_range = endOfTextRange(context.document.text);
    for (const auto& include : context.include_directives) {
        if (!rangesIntersect(include.range, context.range) ||
            resolveIncludeTarget(context.workspace_root_uri,
                                 context.document.uri,
                                 include.target).has_value()) {
            continue;
        }
        const auto target = proposedIncludeTarget(context.workspace_root_uri,
                                                  context.document.uri,
                                                  include.target);
        if (!target.has_value()) {
            continue;
        }

        SemanticCodeAction action;
        action.title = "Create include file '" + include.target + "'";
        action.kind = "quickfix";
        action.is_preferred = true;
        action.diagnostics.push_back(SemanticDiagnosticData{.code = std::string(kUnknownIncludeDiagnosticCode),
                                                            .message = unknownIncludeMessage(include.target),
                                                            .range = include.range,
                                                            .severity = 1});
        action.create_files.push_back(SemanticCodeActionCreateFile{.uri = pathToFileUri(*target),
                                                                   .ignore_if_exists = true});
        result.actions.push_back(std::move(action));
    }

    for (const auto& instance : context.module_instances) {
        if (!rangesIntersect(instance.module_selection_range, context.range) ||
            context.modules_by_name.contains(instance.module_name) ||
            !isValidIdentifier(instance.module_name)) {
            continue;
        }
        SemanticCodeAction action;
        action.title = "Create stub module '" + instance.module_name + "'";
        action.kind = "quickfix";
        action.diagnostics.push_back(SemanticDiagnosticData{
            .code = std::string(kUnresolvedModuleDiagnosticCode),
            .message = unresolvedModuleMessage(instance.module_name),
            .range = instance.module_selection_range,
            .severity = 1});
        action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                      .range = insert_range,
                                                      .new_text = moduleStubInsertionText(context.document.text,
                                                                                          instance.module_name)});
        result.actions.push_back(std::move(action));
    }

    for (const auto& schematic : context.document_schematics) {
        for (const auto& cell : schematic.cells) {
            if (cell.kind != "module" || !rangesIntersect(cell.range, context.range) ||
                !hasPortListSyntax(context.document.text, cell)) {
                continue;
            }
            const auto module_it = context.modules_by_name.find(cell.type);
            if (module_it == context.modules_by_name.end()) {
                continue;
            }
            const auto missing_ports = missingPorts(cell, module_it->second);
            if (missing_ports.empty()) {
                continue;
            }
            const auto insertion_range = instancePortInsertionRange(context.document.text, cell);
            if (!insertion_range.has_value()) {
                continue;
            }
            SemanticCodeAction action;
            action.title = "Add missing port connections to '" + cell.name + "'";
            action.kind = "quickfix";
            action.edits.push_back(SemanticCodeActionEdit{
                .uri = context.document.uri,
                .range = *insertion_range,
                .new_text = missingPortConnectionText(missing_ports, !cell.connections.empty())});
            result.actions.push_back(std::move(action));
        }
    }

    if (const auto invocation = macroInvocationAtRange(context.document.text, context.range)) {
        if (const auto* macro = findMacroDefinition(context, *invocation); macro != nullptr) {
            const auto expanded = expandMacroBody(*macro, *invocation);
            if (!expanded.empty()) {
                SemanticCodeAction action;
                action.title = "Expand macro '" + invocation->name + "'";
                action.kind = "quickfix";
                action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                              .range = invocation->range,
                                                              .new_text = expanded});
                result.actions.push_back(std::move(action));
            }
        }
    }

    std::set<std::string> emitted_macro_names;
    for (const auto& diagnostic : context.diagnostics) {
        if (!looksLikeUndefinedMacroDiagnostic(diagnostic) ||
            !rangesIntersect(diagnostic.range, context.range)) {
            continue;
        }
        const auto invocation = macroInvocationFromDiagnostic(context.document.text, diagnostic.range);
        if (!invocation.has_value() || !isValidIdentifier(invocation->name) ||
            !emitted_macro_names.insert(invocation->name).second) {
            continue;
        }
        SemanticCodeAction action;
        action.title = "Define macro '" + invocation->name + "'";
        action.kind = "quickfix";
        action.diagnostics.push_back(SemanticDiagnosticData{.code = diagnostic.code,
                                                            .message = diagnostic.message,
                                                            .range = diagnostic.range,
                                                            .severity = diagnostic.severity});
        action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                      .range = ParseRange{},
                                                      .new_text = macroDefinitionInsertionText(*invocation)});
        result.actions.push_back(std::move(action));
    }

    std::set<std::string> emitted_package_names;
    for (const auto& diagnostic : context.diagnostics) {
        if (diagnostic.code != kUnresolvedPackageDiagnosticCode ||
            !rangesIntersect(diagnostic.range, context.range)) {
            continue;
        }
        const auto package_name = textForParseRange(context.document.text, diagnostic.range);
        if (!package_name.has_value() || !isValidIdentifier(*package_name) ||
            !emitted_package_names.insert(*package_name).second) {
            continue;
        }
        SemanticCodeAction action;
        action.title = "Create package '" + *package_name + "'";
        action.kind = "quickfix";
        action.diagnostics.push_back(SemanticDiagnosticData{.code = diagnostic.code,
                                                            .message = diagnostic.message,
                                                            .range = diagnostic.range,
                                                            .severity = diagnostic.severity});
        action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                      .range = insert_range,
                                                      .new_text = packageStubInsertionText(context.document.text,
                                                                                          *package_name)});
        result.actions.push_back(std::move(action));
    }

    std::set<std::string> emitted_imports;
    for (const auto& diagnostic : context.diagnostics) {
        if (diagnostic.code != kMissingImportDiagnosticCode ||
            !rangesIntersect(diagnostic.range, context.range)) {
            continue;
        }
        const auto package_name = packageNameFromMissingImportDiagnostic(context, diagnostic);
        if (!package_name.has_value() || !isValidIdentifier(*package_name) ||
            !emitted_imports.insert(*package_name).second) {
            continue;
        }
        SemanticCodeAction action;
        action.title = "Import package '" + *package_name + "'";
        action.kind = "quickfix";
        action.diagnostics.push_back(SemanticDiagnosticData{.code = diagnostic.code,
                                                            .message = diagnostic.message,
                                                            .range = diagnostic.range,
                                                            .severity = diagnostic.severity});
        action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                      .range = ParseRange{},
                                                      .new_text = importInsertionText(context.document.text,
                                                                                      *package_name)});
        result.actions.push_back(std::move(action));
    }

    std::set<std::string> emitted_type_names;
    for (const auto& diagnostic : context.diagnostics) {
        if (diagnostic.code != kUnresolvedTypeDiagnosticCode ||
            !rangesIntersect(diagnostic.range, context.range)) {
            continue;
        }
        const auto type_name = textForParseRange(context.document.text, diagnostic.range);
        if (!type_name.has_value() || !isValidIdentifier(*type_name) ||
            !emitted_type_names.insert(*type_name).second) {
            continue;
        }
        SemanticCodeAction action;
        action.title = "Create typedef '" + *type_name + "'";
        action.kind = "quickfix";
        action.diagnostics.push_back(SemanticDiagnosticData{.code = diagnostic.code,
                                                            .message = diagnostic.message,
                                                            .range = diagnostic.range,
                                                            .severity = diagnostic.severity});
        action.edits.push_back(SemanticCodeActionEdit{.uri = context.document.uri,
                                                      .range = insert_range,
                                                      .new_text = typedefSkeletonInsertionText(context.document.text,
                                                                                              *type_name)});
        result.actions.push_back(std::move(action));
    }

    return result;
}

} // namespace pristine::analysis::semantic
