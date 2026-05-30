#include "CodeActionProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>

namespace pristine::analysis::semantic {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUnknownIncludeDiagnosticCode = "unknownInclude";
constexpr std::string_view kUnresolvedModuleDiagnosticCode = "unresolvedModule";
constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";

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

std::string unknownIncludeMessage(std::string_view target) {
    return std::string("Include file '") + std::string(target) + "' could not be resolved.";
}

std::string unresolvedModuleMessage(std::string_view module_name) {
    return std::string("Module '") + std::string(module_name) + "' could not be resolved.";
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
