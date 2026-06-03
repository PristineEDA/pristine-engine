#include "CompletionProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>

namespace pristine::analysis::semantic {
namespace {

constexpr size_t kMaxCompletionItems = 2000;

bool startsWithInsensitive(std::string_view prefix, std::string_view candidate) {
    if (prefix.size() > candidate.size()) {
        return false;
    }
    for (size_t index = 0; index < prefix.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(prefix[index]);
        const auto rhs = static_cast<unsigned char>(candidate[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

bool prefixMatches(std::string_view value, std::string_view prefix) {
    return startsWithInsensitive(prefix, value);
}

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
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

std::optional<std::string> qualifierBefore(std::string_view text, size_t qualifier_end) {
    size_t name_start = qualifier_end;
    while (name_start > 0) {
        const auto previous = text[name_start - 1];
        if (isIdentifierContinue(previous)) {
            --name_start;
            continue;
        }
        if (previous == ']') {
            int depth = 1;
            --name_start;
            while (name_start > 0 && depth > 0) {
                const auto value = text[name_start - 1];
                --name_start;
                if (value == ']') {
                    ++depth;
                }
                else if (value == '[') {
                    --depth;
                }
            }
            continue;
        }
        if (previous == '.') {
            --name_start;
            continue;
        }
        break;
    }
    const auto qualifier = text.substr(name_start, qualifier_end - name_start);
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
            offset = cursor - 1;
            continue;
        }

        offset = name_end;
    }
    return connected_ports;
}

std::string_view moduleUriFor(const CompletionResolveContext& context,
                              std::string_view module_name) {
    if (context.module_uris_by_name == nullptr) {
        return {};
    }
    const auto module_uri_it = context.module_uris_by_name->find(std::string(module_name));
    if (module_uri_it == context.module_uris_by_name->end()) {
        return {};
    }
    return module_uri_it->second;
}

} // namespace

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

SemanticCompletionItem resolveCompletionItem(std::string_view stable_id,
                                             std::string_view label,
                                             const CompletionResolveContext& context) {
    SemanticCompletionItem item;
    item.stable_id = std::string(stable_id);
    item.label = std::string(label);
    item.insert_text = std::string(label);
    const auto stable_id_text = std::string(stable_id);
    const auto port_marker = stable_id_text.find("|port|");
    if (port_marker != std::string::npos && context.modules_by_name != nullptr) {
        const auto port_name = stable_id_text.substr(port_marker + 6);
        for (const auto& [_, module] : *context.modules_by_name) {
            const auto module_uri = moduleUriFor(context, module.name);
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
        if (context.macros_by_uri != nullptr) {
            for (const auto& [_, macros] : *context.macros_by_uri) {
                const auto macro_it = std::find_if(macros.begin(),
                                                   macros.end(),
                                                   [&](const MacroDefinition& macro) {
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
        }
        item.unresolved = true;
        return item;
    }

    if (!context.symbol.has_value()) {
        item.unresolved = true;
        return item;
    }

    const auto& symbol = *context.symbol;
    item.detail = symbol.identity.kind;
    item.documentation = "**" + symbol.identity.kind + "** `" + symbol.identity.name + "`";
    if (symbol.identity.kind == "Definition" && context.modules_by_name != nullptr) {
        const auto module_it = context.modules_by_name->find(symbol.identity.name);
        if (module_it != context.modules_by_name->end()) {
            item.detail = moduleSignatureLabel(module_it->second);
            item.documentation = moduleDocumentation(module_it->second,
                                                    moduleUriFor(context, module_it->second.name));
            item.insert_text = moduleInstantiationSnippet(module_it->second);
            return item;
        }
    }
    if (!symbol.type_display.empty()) {
        item.documentation += "\n\nType: `" + symbol.type_display + "`";
    }
    return item;
}

std::set<std::string> connectedNamedPortsForInstance(std::string_view text,
                                                     int line,
                                                     int character,
                                                     ParseRange instance_selection_range,
                                                     ParseRange instance_range) {
    const auto position_offset = utf8OffsetAtUtf16Position(text, line, character);
    const auto search_start = utf8OffsetAtUtf16Position(text,
                                                        instance_selection_range.end_line,
                                                        instance_selection_range.end_character);
    const auto search_end = utf8OffsetAtUtf16Position(text,
                                                      instance_range.end_line,
                                                      instance_range.end_character);
    if (!position_offset.has_value() || !search_start.has_value() || !search_end.has_value()) {
        return {};
    }

    const auto open_paren = openParenBeforePosition(text,
                                                    *search_start,
                                                    std::min(*position_offset, *search_end));
    if (!open_paren.has_value()) {
        return {};
    }
    return connectedNamedPortsBeforePosition(text, *open_paren, *position_offset);
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

CompletionContext detectCompletionContext(std::string_view text,
                                          int line,
                                          int character,
                                          std::string_view prefix) {
    CompletionContext context;
    context.prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!context.prefix_start.has_value()) {
        return context;
    }

    const auto prefix_start = *context.prefix_start;
    context.macro_invocation = prefix_start > 0 && text[prefix_start - 1] == '`';
    context.member_access = prefix_start > 0 && text[prefix_start - 1] == '.';
    context.module_instantiation_position = hasOnlyWhitespaceSinceLineStart(text, prefix_start);

    if (prefix_start >= 2 && text[prefix_start - 1] == ':' && text[prefix_start - 2] == ':') {
        context.package_qualifier = qualifierBefore(text, prefix_start - 2);
    }
    if (context.member_access) {
        context.member_qualifier = qualifierBefore(text, prefix_start - 1);
    }
    return context;
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
    if (items.size() >= kMaxCompletionItems) {
        truncated = true;
    }
}

void appendSymbolCompletion(std::vector<SemanticCompletionItem>& items,
                            std::set<std::string>& emitted,
                            const SemanticSymbolIdentity& symbol,
                            std::string_view prefix,
                            bool& truncated) {
    if (truncated) {
        return;
    }
    appendCompletionItem(items,
                         emitted,
                         SemanticCompletionItem{.stable_id = symbol.stable_id,
                                                 .label = symbol.name,
                                                 .detail = completionDetailForSemanticKind(symbol.kind),
                                                 .documentation = {},
                                                 .insert_text = symbol.name,
                                                 .kind = completionKindForSemanticKind(symbol.kind),
                                                 .unresolved = false},
                         prefix,
                         truncated);
}

std::string baseMemberQualifier(std::string_view qualifier) {
    const auto dot = qualifier.rfind('.');
    const auto segment = dot == std::string_view::npos ? qualifier : qualifier.substr(dot + 1);
    const auto bracket = segment.find('[');
    const auto base = bracket == std::string_view::npos ? segment : segment.substr(0, bracket);
    return std::string(base);
}

bool isMemberCompletionKind(std::string_view kind) {
    return kind == "Field" || kind == "Member" || kind == "Net" || kind == "Variable" ||
           kind == "Parameter" || kind == "Subroutine";
}

void appendMemberCompletions(std::vector<SemanticCompletionItem>& items,
                             std::set<std::string>& emitted,
                             const CompletionMemberContext& context,
                             std::string_view prefix,
                             bool& truncated) {
    for (const auto& candidate : context.candidates) {
        if (truncated) {
            return;
        }
        if (candidate.identity.name == context.qualifier ||
            !isMemberCompletionKind(candidate.identity.kind)) {
            continue;
        }
        appendSymbolCompletion(items, emitted, candidate.identity, prefix, truncated);
    }
}

void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const std::string& module_stable_id,
                                 const ModuleDefinition& module,
                                 std::string_view module_uri,
                                 std::string_view prefix,
                                 const std::set<std::string>& excluded_ports,
                                 bool& truncated) {
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
                SemanticCompletionItem{.stable_id = module_stable_id + "|port|" + port_name,
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
            SemanticCompletionItem{.stable_id = module_stable_id + "|port|" + port.name,
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

} // namespace pristine::analysis::semantic
