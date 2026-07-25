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

bool caseInsensitiveLess(std::string_view left, std::string_view right) {
    const auto common_size = std::min(left.size(), right.size());
    for (size_t index = 0; index < common_size; ++index) {
        const auto lhs = std::tolower(static_cast<unsigned char>(left[index]));
        const auto rhs = std::tolower(static_cast<unsigned char>(right[index]));
        if (lhs != rhs) {
            return lhs < rhs;
        }
    }
    return left.size() < right.size();
}

std::vector<SnapshotVisibilityCandidate>::const_iterator workspaceCandidateStart(
    const std::vector<SnapshotVisibilityCandidate>& candidates,
    std::string_view prefix) {
    if (prefix.empty()) {
        return candidates.begin();
    }
    return std::lower_bound(candidates.begin(),
                            candidates.end(),
                            prefix,
                            [](const SnapshotVisibilityCandidate& candidate,
                               std::string_view value) {
                                return caseInsensitiveLess(candidate.identity.name, value);
                            });
}
bool scopeStartsBeforePosition(const SnapshotScopeVisibility& scope, int line, int character) {
    if (scope.range.start_line != line) {
        return scope.range.start_line < line;
    }
    return scope.range.start_character <= character;
}

const SnapshotScopeVisibility* nearestSourceBackedScopeBeforePosition(
    const std::vector<SnapshotScopeVisibility>& scopes,
    int line,
    int character) {
    const SnapshotScopeVisibility* result = nullptr;
    for (const auto& scope : scopes) {
        if (!scopeStartsBeforePosition(scope, line, character)) {
            continue;
        }
        if (result == nullptr || scope.lexical_depth < result->lexical_depth ||
            (scope.lexical_depth == result->lexical_depth &&
             (scope.range.start_line > result->range.start_line ||
              (scope.range.start_line == result->range.start_line &&
               scope.range.start_character > result->range.start_character)))) {
            result = &scope;
        }
    }
    return result;
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
    if (kind == "Property" || kind == "Sequence" || kind == "LetDecl" || kind == "Checker") {
        return 3;
    }
    if (kind == "ClockingBlock") {
        return 8;
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
    if (kind == "Property") {
        return "Property";
    }
    if (kind == "Sequence") {
        return "Sequence";
    }
    if (kind == "LetDecl") {
        return "Let";
    }
    if (kind == "Checker") {
        return "Checker";
    }
    if (kind == "ClockingBlock") {
        return "Clocking Block";
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
        detail == "Callable" || detail == "Property" || detail == "Sequence" ||
        detail == "Let" || detail == "Checker") {
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

std::string macroCompletionResolveId(const SnapshotVisibleMacro& macro) {
    const auto& range = macro.definition.selection_range;
    const auto& available_after = macro.available_after;
    return "completion-macro:" + macro.source_uri + ":" + macro.definition.name + ":" +
           std::to_string(range.start_line) + ":" + std::to_string(range.start_character) + ":" +
           std::to_string(available_after.start_line) + ":" +
           std::to_string(available_after.start_character);
}

std::string portCompletionResolveId(std::string_view module_stable_id, const SchematicPort& port) {
    return "completion-port:" + std::string(module_stable_id) + ":" + port.name;
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
    if (context.facts_by_id == nullptr) {
        item.unresolved = true;
        return item;
    }
    const auto fact_it = context.facts_by_id->find(std::string(stable_id));
    if (fact_it == context.facts_by_id->end()) {
        item.unresolved = true;
        return item;
    }

    const auto& fact = fact_it->second;
    if (fact.kind == SnapshotCompletionResolveKind::Macro && fact.macro.has_value()) {
        const auto& macro = *fact.macro;
        item.detail = macro.function_like ? "Macro function " + macroSignatureLabel(macro) : "Macro";
        item.documentation = macroDocumentation(macro);
        item.insert_text = macroInsertText(macro);
        return item;
    }
    if (fact.kind == SnapshotCompletionResolveKind::Port && fact.module.has_value() &&
        fact.port.has_value()) {
        item.detail = fact.port->direction.empty() && fact.port->width_text.empty()
                          ? "Port"
                          : portSignatureLabel(*fact.port);
        item.documentation = portDocumentation(*fact.module, *fact.port, fact.module_uri);
        item.insert_text = portConnectionSnippet(fact.port->name);
        return item;
    }
    if (fact.kind == SnapshotCompletionResolveKind::Module && fact.module.has_value()) {
        item.detail = moduleSignatureLabel(*fact.module);
        item.documentation = moduleDocumentation(*fact.module, fact.module_uri);
        item.insert_text = moduleInstantiationSnippet(*fact.module);
        return item;
    }

    const auto& symbol = fact.identity;
    if (symbol.stable_id.empty()) {
        item.unresolved = true;
        return item;
    }
    if (fact.kind == SnapshotCompletionResolveKind::Member) {
        item.detail = completionDetailForSemanticKind(symbol.kind);
        if (item.detail.empty()) {
            item.detail = symbol.kind;
        }
        item.documentation = "**" + symbol.kind + "** `" + symbol.name + "`";
        if (!fact.type_display.empty()) {
            item.documentation += "\n\nType: `" + fact.type_display + "`";
        }
        if (!symbol.location.uri.empty()) {
            item.documentation += "\n\nDeclared: `" + symbol.location.uri + ":" +
                                  std::to_string(symbol.location.range.start_line + 1) + ":" +
                                  std::to_string(symbol.location.range.start_character + 1) + "`";
        }
        return item;
    }
    item.detail = symbol.kind;
    item.documentation = "**" + symbol.kind + "** `" + symbol.name + "`";
    if (!fact.type_display.empty()) {
        item.documentation += "\n\nType: `" + fact.type_display + "`";
    }
    if (!fact.value_display.empty()) {
        item.documentation += "\n\nValue: `" + fact.value_display + "`";
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
    auto offset = utf8OffsetAtUtf16Position(text, line, character);
    if (!offset.has_value() && line >= 0 && character >= 0) {
        size_t line_start = 0;
        for (int current_line = 0; current_line < line; ++current_line) {
            const auto newline = text.find('\n', line_start);
            if (newline == std::string_view::npos) {
                return std::nullopt;
            }
            line_start = newline + 1;
        }
        auto line_end = text.find_first_of("\r\n", line_start);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }
        offset = line_end;
    }
    if (!offset.has_value()) {
        return std::nullopt;
    }
    auto prefix_end = *offset;
    while (prefix_end > 0 &&
           (text[prefix_end - 1] == '\n' || text[prefix_end - 1] == '\r')) {
        --prefix_end;
    }
    if (prefix_end < prefix.size()) {
        return std::nullopt;
    }
    return prefix_end - prefix.size();
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

std::string normalizedMemberQualifier(std::string_view qualifier) {
    std::string result;
    int bracket_depth = 0;
    for (const auto value : qualifier) {
        if (value == '[') {
            ++bracket_depth;
            continue;
        }
        if (value == ']') {
            bracket_depth = std::max(0, bracket_depth - 1);
            continue;
        }
        if (bracket_depth == 0) {
            result.push_back(value);
        }
    }
    return result;
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
                SemanticCompletionItem{.stable_id = portCompletionResolveId(module_stable_id, port),
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
            SemanticCompletionItem{.stable_id = portCompletionResolveId(module_stable_id, port),
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

SemanticCompletionResult completeAt(const CompletionQueryContext& context,
                                    int line,
                                    int character,
                                    std::string_view prefix) {
    SemanticCompletionResult result;
    result.generation = context.generation;
    result.scope_visibility_count = context.scope_visibility_count;
    result.package_visibility_count = context.package_visibility_count;
    result.member_visibility_count = context.member_visibility_count;
    result.callable_visibility_count = context.callable_visibility_count;
    result.scope_visibility_build_micros = context.scope_visibility_build_micros;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    if (context.document_text == nullptr) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }

    std::set<std::string> emitted;
    const auto completion_context = detectCompletionContext(*context.document_text,
                                                            line,
                                                            character,
                                                            prefix);
    const auto append_candidate = [&](const SnapshotVisibilityCandidate& candidate,
                                      bool workspace_candidate) {
        ++result.scanned_candidate_count;
        if (workspace_candidate) {
            ++result.scanned_workspace_candidate_count;
        }
        else {
            ++result.scanned_scope_candidate_count;
        }
        appendSymbolCompletion(result.items,
                               emitted,
                               candidate.identity,
                               prefix,
                               result.truncated);
    };

    if (completion_context.macro_invocation) {
        if (context.macros != nullptr) {
            std::unordered_map<std::string, size_t> latest_visible_by_name;
            for (size_t index = 0; index < context.macros->size(); ++index) {
                const auto& visible_macro = context.macros->at(index);
                if (visible_macro.available_after.start_line > line ||
                    (visible_macro.available_after.start_line == line &&
                     visible_macro.available_after.start_character >= character)) {
                    continue;
                }
                if (visible_macro.unavailable_after.has_value() &&
                    (visible_macro.unavailable_after->start_line < line ||
                     (visible_macro.unavailable_after->start_line == line &&
                      visible_macro.unavailable_after->start_character <= character))) {
                    continue;
                }
                latest_visible_by_name[visible_macro.definition.name] = index;
            }
            for (size_t index = 0; index < context.macros->size(); ++index) {
                const auto& visible_macro = context.macros->at(index);
                const auto latest = latest_visible_by_name.find(visible_macro.definition.name);
                if (latest == latest_visible_by_name.end() || latest->second != index) {
                    continue;
                }
                const auto& macro = visible_macro.definition;
                ++result.scanned_candidate_count;
                appendCompletionItem(
                    result.items,
                    emitted,
                    SemanticCompletionItem{.stable_id = macroCompletionResolveId(visible_macro),
                                            .label = macro.name,
                                            .detail = macro.function_like ? "Macro function" : "Macro",
                                            .documentation = macroDocumentation(macro),
                                            .insert_text = macroInsertText(macro),
                                            .kind = macro.function_like ? 3 : 21,
                                            .unresolved = false},
                    prefix,
                    result.truncated);
                if (result.truncated) {
                    break;
                }
            }
        }
        return result;
    }

    if (completion_context.package_qualifier.has_value()) {
        const auto package_it = context.packages == nullptr
                                    ? std::unordered_map<std::string, SnapshotPackageVisibility>::const_iterator{}
                                    : context.packages->find(*completion_context.package_qualifier);
        if (context.packages == nullptr || package_it == context.packages->end()) {
            result.messages.push_back("package completion target is not indexed in scope visibility");
            return result;
        }
        for (const auto& candidate : package_it->second.candidates) {
            append_candidate(candidate, false);
            if (result.truncated) {
                break;
            }
        }
        return result;
    }

    if (completion_context.member_access && !completion_context.member_qualifier.has_value() &&
        context.module_instances != nullptr &&
        context.modules_by_name != nullptr) {
        for (const auto& instance : *context.module_instances) {
            if (!parseRangeContainsPosition(instance.range, line, character)) {
                continue;
            }
            const auto module_it = context.modules_by_name->find(instance.module_name);
            if (module_it == context.modules_by_name->end()) {
                continue;
            }
            auto connected_ports = connectedNamedPortsForInstance(*context.document_text,
                                                                   line,
                                                                   character,
                                                                   instance.selection_range,
                                                                   instance.range);
            std::string module_stable_id = instance.target_stable_id;
            if (module_stable_id.empty() && context.module_definition_ids_by_name != nullptr) {
                if (const auto definition = context.module_definition_ids_by_name->find(instance.module_name);
                    definition != context.module_definition_ids_by_name->end()) {
                    module_stable_id = definition->second;
                }
            }
            if (module_stable_id.empty()) {
                module_stable_id = "module|" + instance.module_name;
            }
            const auto module_uri_it = context.module_uris_by_name == nullptr
                                           ? std::unordered_map<std::string, std::string>::const_iterator{}
                                           : context.module_uris_by_name->find(instance.module_name);
            appendModulePortCompletions(
                result.items,
                emitted,
                module_stable_id,
                module_it->second,
                context.module_uris_by_name == nullptr || module_uri_it == context.module_uris_by_name->end()
                    ? std::string_view{}
                    : std::string_view(module_uri_it->second),
                prefix,
                connected_ports,
                result.truncated);
            result.scanned_candidate_count += module_it->second.port_details.empty()
                                                  ? module_it->second.ports.size()
                                                  : module_it->second.port_details.size();
            return result;
        }
    }

    if (completion_context.member_qualifier.has_value()) {
        const auto qualifier = normalizedMemberQualifier(*completion_context.member_qualifier);
        CompletionMemberContext member_context;
        member_context.qualifier = qualifier;
        if (context.member_candidates_by_qualifier != nullptr) {
            auto candidates = context.member_candidates_by_qualifier->find(qualifier);
            if (candidates == context.member_candidates_by_qualifier->end()) {
                member_context.qualifier = baseMemberQualifier(qualifier);
                candidates = context.member_candidates_by_qualifier->find(member_context.qualifier);
            }
            if (candidates != context.member_candidates_by_qualifier->end()) {
                result.scanned_candidate_count += candidates->second.size();
                result.scanned_scope_candidate_count += candidates->second.size();
                for (const auto& candidate : candidates->second) {
                    member_context.candidates.push_back(
                        CompletionMemberCandidate{.identity = candidate.identity});
                }
            }
        }
        appendMemberCompletions(result.items,
                                emitted,
                                member_context,
                                prefix,
                                result.truncated);
        if (!result.items.empty()) {
            result.messages.push_back("member completion used typed AstIndex member view");
        }
        else {
            result.messages.push_back("member completion receiver is not indexed in scope visibility");
        }
        return result;
    }

    bool module_candidates_emitted = false;
    if (completion_context.module_instantiation_position && context.modules_by_name != nullptr &&
        context.workspace_candidates_by_name != nullptr) {
        const auto item_count_before_modules = result.items.size();
        for (auto candidate = workspaceCandidateStart(*context.workspace_candidates_by_name, prefix);
             candidate != context.workspace_candidates_by_name->end() &&
             prefixMatches(candidate->identity.name, prefix);
             ++candidate) {
            if (candidate->identity.kind != "Definition") {
                continue;
            }
            ++result.scanned_candidate_count;
            ++result.scanned_workspace_candidate_count;
            const auto module_it = context.modules_by_name->find(candidate->identity.name);
            if (module_it == context.modules_by_name->end()) {
                continue;
            }
            const auto& module_name = candidate->identity.name;
            const auto& module = module_it->second;
            const auto& stable_id = candidate->identity.stable_id;
            const auto module_uri_it = context.module_uris_by_name == nullptr
                                           ? std::unordered_map<std::string, std::string>::const_iterator{}
                                           : context.module_uris_by_name->find(module_name);
            appendCompletionItem(
                result.items,
                emitted,
                SemanticCompletionItem{.stable_id = std::move(stable_id),
                                        .label = module.name,
                                        .detail = moduleSignatureLabel(module),
                                        .documentation = moduleDocumentation(
                                            module,
                                            context.module_uris_by_name == nullptr ||
                                                    module_uri_it == context.module_uris_by_name->end()
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
        module_candidates_emitted = result.items.size() != item_count_before_modules;
    }

    bool matched_scope = false;
    if (context.scopes != nullptr) {
        for (const auto& scope : *context.scopes) {
            if (!parseRangeContainsPosition(scope.range, line, character)) {
                continue;
            }
            matched_scope = true;
            for (const auto& candidate : scope.candidates) {
                append_candidate(candidate, false);
                if (result.truncated) {
                    return result;
                }
            }
        }
        if (!matched_scope) {
            // A recoverable trailing edit can leave slang's source range before the cursor.
            // Keep completion semantic by using the nearest outer AST scope, never text/global fallback.
            if (const auto* scope = nearestSourceBackedScopeBeforePosition(*context.scopes, line, character)) {
                for (const auto& candidate : scope->candidates) {
                    append_candidate(candidate, false);
                    if (result.truncated) {
                        return result;
                    }
                }
            }
        }
    }
    if (!matched_scope && context.document_candidates != nullptr) {
        for (const auto& candidate : *context.document_candidates) {
            append_candidate(candidate, false);
            if (result.truncated) {
                return result;
            }
        }
    }
    if (!module_candidates_emitted && context.workspace_candidates_by_name != nullptr) {
        for (auto candidate = workspaceCandidateStart(*context.workspace_candidates_by_name, prefix);
             candidate != context.workspace_candidates_by_name->end() &&
             prefixMatches(candidate->identity.name, prefix);
             ++candidate) {
            append_candidate(*candidate, true);
            if (result.truncated) {
                break;
            }
        }
    }
    return result;
}

} // namespace pristine::analysis::semantic
