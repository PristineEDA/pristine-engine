#include "pristine/server/ServerSession.h"

#include <nlohmann/json.hpp>

#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/lsp/Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace pristine::server {
namespace {

namespace fs = std::filesystem;

jsonrpc::Json toRangeJson(const analysis::ParseRange& range) {
    return jsonrpc::Json{{"start",
                          jsonrpc::Json{{"line", range.start_line},
                                         {"character", range.start_character}}},
                         {"end",
                          jsonrpc::Json{{"line", range.end_line},
                                         {"character", range.end_character}}}};
}

jsonrpc::Json toLocationJson(const analysis::Location& location) {
    return jsonrpc::Json{{"uri", location.uri}, {"range", toRangeJson(location.range)}};
}

jsonrpc::Json toLocationJson(const analysis::SemanticLocation& location) {
    return jsonrpc::Json{{"uri", location.uri}, {"range", toRangeJson(location.range)}};
}

jsonrpc::Json toTextEditJson(const analysis::ParseRange& range, std::string_view new_text) {
    return jsonrpc::Json{{"range", toRangeJson(range)}, {"newText", new_text}};
}

jsonrpc::Json toPositionJson(int line, int character) {
    return jsonrpc::Json{{"line", line}, {"character", character}};
}

jsonrpc::Json toDocumentHighlightJson(const analysis::Location& location) {
    return jsonrpc::Json{{"range", toRangeJson(location.range)}, {"kind", 1}};
}

jsonrpc::Json toDocumentHighlightJson(const analysis::SemanticLocation& location) {
    return jsonrpc::Json{{"range", toRangeJson(location.range)}, {"kind", 1}};
}

constexpr std::string_view kUnknownIncludeDiagnosticCode = "unknownInclude";
constexpr std::string_view kUnresolvedModuleDiagnosticCode = "unresolvedModule";
constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";

jsonrpc::Json makeDiagnosticJson(const analysis::ParseRange& range,
                                 int severity,
                                 std::string_view code,
                                 std::string_view source,
                                 std::string message) {
    return jsonrpc::Json{{"range", toRangeJson(range)},
                         {"severity", severity},
                         {"code", std::string(code)},
                         {"source", std::string(source)},
                         {"message", std::move(message)}};
}

std::string unknownIncludeMessage(std::string_view target) {
    return std::string("Include file '") + std::string(target) + "' could not be resolved.";
}

jsonrpc::Json makeUnknownIncludeDiagnostic(const analysis::IncludeDirective& include,
                                           std::string_view source) {
    return makeDiagnosticJson(include.range,
                              1,
                              kUnknownIncludeDiagnosticCode,
                              source,
                              unknownIncludeMessage(include.target));
}

std::string unresolvedModuleMessage(std::string_view module_name) {
    return std::string("Module '") + std::string(module_name) + "' could not be resolved.";
}

jsonrpc::Json makeUnresolvedModuleDiagnostic(const analysis::ModuleInstantiation& instance,
                                             std::string_view source) {
    return makeDiagnosticJson(instance.module_selection_range,
                              1,
                              kUnresolvedModuleDiagnosticCode,
                              source,
                              unresolvedModuleMessage(instance.module_name));
}

analysis::ParseRange endOfTextRange(std::string_view text) {
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
    return analysis::ParseRange{.start_line = line,
                                .start_character = character,
                                .end_line = line,
                                .end_character = character};
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

std::string missingPortConnectionText(const std::vector<analysis::SchematicPort>& ports,
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

std::vector<analysis::SchematicPort> modulePorts(const analysis::ModuleDefinition& module) {
    if (!module.port_details.empty()) {
        return module.port_details;
    }

    std::vector<analysis::SchematicPort> ports;
    for (const auto& port_name : module.ports) {
        ports.push_back(analysis::SchematicPort{.name = port_name,
                                                .direction = {},
                                                .width_text = {},
                                                .range = module.selection_range,
                                                .selection_range = module.selection_range});
    }
    return ports;
}

std::set<std::string> connectedPortNames(const analysis::SchematicCell& cell,
                                         const std::vector<analysis::SchematicPort>& ports) {
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

std::vector<analysis::SchematicPort> missingPorts(const analysis::SchematicCell& cell,
                                                  const analysis::ModuleDefinition& module) {
    const auto ports = modulePorts(module);
    const auto connected = connectedPortNames(cell, ports);
    std::vector<analysis::SchematicPort> result;
    for (const auto& port : ports) {
        if (port.name.empty() || connected.contains(port.name)) {
            continue;
        }
        result.push_back(port);
    }
    return result;
}

void appendLabelPart(std::string& label, std::string_view part) {
    if (part.empty()) {
        return;
    }
    if (!label.empty()) {
        label.push_back(' ');
    }
    label += part;
}

std::string portSignatureLabel(const analysis::SchematicPort& port) {
    std::string label;
    appendLabelPart(label, port.direction);
    appendLabelPart(label, port.width_text);
    appendLabelPart(label, port.name);
    return label.empty() ? port.name : label;
}

std::string moduleSignatureLabel(const analysis::ModuleDefinition& module) {
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

jsonrpc::Json toInlayHintJson(const analysis::ModuleInstantiation& instance,
                              const analysis::ModuleDefinition* module) {
    jsonrpc::Json result{{"position", toPositionJson(instance.selection_range.end_line,
                                                      instance.selection_range.end_character)},
                         {"label", std::string(": ") + instance.module_name},
                         {"kind", 1}};
    if (module) {
        result["tooltip"] = moduleSignatureLabel(*module);
    }
    return result;
}

jsonrpc::Json toInlayHintJson(const analysis::SemanticInlayHint& hint) {
    const auto& range = hint.location.range;
    jsonrpc::Json result{{"position", toPositionJson(range.start_line, range.start_character)},
                         {"label", hint.label},
                         {"kind", hint.kind == "parameter" ? 2 : 1}};
    if (!hint.tooltip.empty()) {
        result["tooltip"] = hint.tooltip;
    }
    return result;
}

std::optional<int> semanticTokenTypeForSymbolKind(int symbol_kind) {
    switch (symbol_kind) {
        case 2:
        case 26:
            return 1;
        case 3:
        case 4:
            return 0;
        case 5:
            return 2;
        case 10:
            return 3;
        case 11:
            return 4;
        case 12:
            return 5;
        case 13:
        case 19:
            return 6;
        case 14:
            return 7;
        case 22:
            return 8;
        default:
            return std::nullopt;
    }
}

std::optional<int> semanticTokenTypeForName(std::string_view token_type) {
    if (token_type == "namespace") {
        return 0;
    }
    if (token_type == "type") {
        return 1;
    }
    if (token_type == "class") {
        return 2;
    }
    if (token_type == "enum") {
        return 3;
    }
    if (token_type == "interface") {
        return 4;
    }
    if (token_type == "function") {
        return 5;
    }
    if (token_type == "variable") {
        return 6;
    }
    if (token_type == "parameter") {
        return 7;
    }
    if (token_type == "enumMember") {
        return 8;
    }
    return std::nullopt;
}

struct SemanticToken {
    int line = 0;
    int character = 0;
    int length = 0;
    int type = 0;
};

struct SignatureInvocation {
    std::string module_name;
    std::string instance_name;
    int active_parameter = 0;
    size_t open_paren_offset = 0;
    size_t position_offset = 0;
};

void collectFoldingRanges(jsonrpc::Json& result, const std::vector<analysis::DocumentSymbol>& symbols) {
    for (const auto& symbol : symbols) {
        if (symbol.range.end_line > symbol.range.start_line) {
            result.push_back(jsonrpc::Json{{"startLine", symbol.range.start_line},
                                           {"startCharacter", symbol.range.start_character},
                                           {"endLine", symbol.range.end_line},
                                           {"endCharacter", symbol.range.end_character},
                                           {"kind", "region"}});
        }
        collectFoldingRanges(result, symbol.children);
    }
}

void collectSemanticTokens(std::vector<SemanticToken>& result,
                           const std::vector<analysis::DocumentSymbol>& symbols) {
    for (const auto& symbol : symbols) {
        const auto token_type = semanticTokenTypeForSymbolKind(symbol.kind);
        const auto& range = symbol.selection_range;
        if (token_type.has_value() && range.start_line == range.end_line &&
            range.end_character > range.start_character) {
            result.push_back(SemanticToken{.line = range.start_line,
                                           .character = range.start_character,
                                           .length = range.end_character - range.start_character,
                                           .type = *token_type});
        }
        collectSemanticTokens(result, symbol.children);
    }
}

jsonrpc::Json toSemanticTokensJson(std::vector<SemanticToken> tokens) {
    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& lhs, const SemanticToken& rhs) {
        if (lhs.line != rhs.line) {
            return lhs.line < rhs.line;
        }
        if (lhs.character != rhs.character) {
            return lhs.character < rhs.character;
        }
        if (lhs.length != rhs.length) {
            return lhs.length < rhs.length;
        }
        return lhs.type < rhs.type;
    });
    tokens.erase(std::unique(tokens.begin(), tokens.end(), [](const SemanticToken& lhs,
                                                             const SemanticToken& rhs) {
                     return lhs.line == rhs.line && lhs.character == rhs.character &&
                            lhs.length == rhs.length && lhs.type == rhs.type;
                 }),
                 tokens.end());

    jsonrpc::Json data = jsonrpc::Json::array();
    int previous_line = 0;
    int previous_character = 0;
    bool first = true;
    for (const auto& token : tokens) {
        const auto delta_line = first ? token.line : token.line - previous_line;
        const auto delta_character = first || delta_line != 0 ? token.character
                                                             : token.character - previous_character;
        data.push_back(delta_line);
        data.push_back(delta_character);
        data.push_back(token.length);
        data.push_back(token.type);
        data.push_back(0);
        previous_line = token.line;
        previous_character = token.character;
        first = false;
    }

    return jsonrpc::Json{{"data", std::move(data)}};
}

int toCompletionItemKind(int symbol_kind) {
    switch (symbol_kind) {
        case 2:
            return 9;
        case 5:
            return 7;
        case 10:
            return 13;
        case 11:
            return 8;
        case 12:
            return 3;
        case 13:
            return 6;
        case 14:
            return 21;
        case 22:
            return 20;
        case 26:
            return 25;
        default:
            return 18;
    }
}

std::string completionDetailForSymbolKind(int symbol_kind) {
    switch (symbol_kind) {
        case 2:
            return "Module";
        case 3:
            return "Namespace";
        case 4:
            return "Package";
        case 5:
            return "Class";
        case 10:
            return "Enum";
        case 11:
            return "Interface / Modport";
        case 12:
            return "Callable";
        case 13:
            return "Variable";
        case 14:
            return "Parameter";
        case 19:
            return "Instance";
        case 22:
            return "Enum Member";
        case 26:
            return "Typedef";
        default:
            return "Symbol";
    }
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

std::optional<std::string> jsonStringField(const jsonrpc::Json& object, std::string_view key) {
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto field_it = object.find(std::string(key));
    if (field_it == object.end() || !field_it->is_string()) {
        return std::nullopt;
    }
    return field_it->get<std::string>();
}

std::optional<analysis::ParseRange> jsonRangeField(const jsonrpc::Json& object, std::string_view key) {
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto range_it = object.find(std::string(key));
    if (range_it == object.end() || !range_it->is_object()) {
        return std::nullopt;
    }
    const auto start_it = range_it->find("start");
    const auto end_it = range_it->find("end");
    if (start_it == range_it->end() || end_it == range_it->end() ||
        !start_it->is_object() || !end_it->is_object()) {
        return std::nullopt;
    }

    const auto start_line_it = start_it->find("line");
    const auto start_character_it = start_it->find("character");
    const auto end_line_it = end_it->find("line");
    const auto end_character_it = end_it->find("character");
    if (start_line_it == start_it->end() || start_character_it == start_it->end() ||
        end_line_it == end_it->end() || end_character_it == end_it->end() ||
        !start_line_it->is_number_integer() || !start_character_it->is_number_integer() ||
        !end_line_it->is_number_integer() || !end_character_it->is_number_integer()) {
        return std::nullopt;
    }

    return analysis::ParseRange{.start_line = start_line_it->get<int>(),
                                .start_character = start_character_it->get<int>(),
                                .end_line = end_line_it->get<int>(),
                                .end_character = end_character_it->get<int>()};
}

jsonrpc::Json markdownDocumentation(std::string value) {
    return jsonrpc::Json{{"kind", "markdown"}, {"value", std::move(value)}};
}

std::string declarationLocationLabel(const analysis::Location& location) {
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

std::string moduleInstantiationSnippet(const analysis::ModuleDefinition& module) {
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

std::string portConnectionSnippet(const analysis::SchematicPort& port) {
    return port.name + "(${1:" + snippetEscape(port.name) + "})";
}

std::string macroSignatureLabel(const analysis::MacroEntry& macro) {
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

std::string macroInsertText(const analysis::MacroEntry& macro) {
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

std::string symbolTypeLabel(const analysis::SemanticSymbol& symbol) {
    if (!symbol.type.has_value()) {
        return {};
    }
    if (!symbol.type->display_name.empty()) {
        return symbol.type->display_name;
    }
    return symbol.type->name;
}

std::string richCompletionDetail(const analysis::SemanticSymbol& symbol,
                                 const analysis::ModuleDefinition* module) {
    if (module && symbol.kind == 2) {
        return moduleSignatureLabel(*module);
    }

    auto detail = completionDetailForSymbolKind(symbol.kind);
    const auto type_label = symbolTypeLabel(symbol);
    if (!type_label.empty() && (symbol.kind == 13 || symbol.kind == 14 || symbol.kind == 19 ||
                                symbol.kind == 22 || symbol.kind == 26)) {
        detail += ": ";
        detail += type_label;
    }
    if (symbol.kind == 14 && symbol.constant_value.has_value()) {
        detail += " = ";
        detail += std::to_string(*symbol.constant_value);
    }
    return detail;
}

std::string completionDocumentationForSymbol(const analysis::SemanticSymbol& symbol,
                                             const analysis::ModuleDefinition* module) {
    std::string documentation = "**";
    documentation += completionDetailForSymbolKind(symbol.kind);
    documentation += "** `";
    documentation += module && symbol.kind == 2 ? moduleSignatureLabel(*module) : symbol.name;
    documentation += "`";

    const auto type_label = symbolTypeLabel(symbol);
    if (!type_label.empty() && symbol.kind != 2) {
        documentation += "\n\nType: `" + type_label + "`";
    }
    if (!symbol.direction.empty()) {
        documentation += "\n\nDirection: `" + symbol.direction + "`";
    }
    if (symbol.type.has_value() && symbol.type->bit_width.has_value()) {
        documentation += "\n\nWidth: `" + std::to_string(*symbol.type->bit_width) + " bit";
        if (*symbol.type->bit_width != 1) {
            documentation += "s";
        }
        documentation += "`";
    }
    if (symbol.constant_value.has_value()) {
        documentation += "\n\nValue: `" + std::to_string(*symbol.constant_value) + "`";
    }
    if (symbol.type.has_value() && !symbol.type->alias_target.empty()) {
        documentation += "\n\nAlias: `" + symbol.type->alias_target + "`";
    }
    const auto port_count = module ? (module->port_details.empty() ? module->ports.size()
                                                                   : module->port_details.size())
                                   : 0;
    if (port_count > 0) {
        documentation += "\n\nPorts: `";
        for (size_t index = 0; index < port_count; ++index) {
            if (index != 0) {
                documentation += ", ";
            }
            documentation += module->port_details.empty() ? module->ports[index]
                                                          : portSignatureLabel(module->port_details[index]);
        }
        documentation += "`";
    }
    documentation += "\n\nDeclared: `" + declarationLocationLabel(symbol.location) + "`";
    if (!symbol.scope_path.empty()) {
        documentation += "\n\nScope: `" + symbol.scope_path + "`";
    }
    return documentation;
}

std::string portCompletionDetail(const analysis::SchematicPort& port) {
    return portSignatureLabel(port);
}

std::string completionDocumentationForPort(const analysis::ModuleDefinition& module,
                                           const analysis::SchematicPort& port,
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

std::string completionDocumentationForMacro(const analysis::MacroEntry& macro) {
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
    documentation += "\n\nDeclared: `" + declarationLocationLabel(macro.location) + "`";
    return documentation;
}

jsonrpc::Json semanticCompletionData(const analysis::SemanticSymbol& symbol) {
    return jsonrpc::Json{{"source", "semantic"},
                         {"symbolId", symbol.id},
                         {"uri", symbol.location.uri},
                         {"range", toRangeJson(symbol.location.range)},
                         {"selectionRange", toRangeJson(symbol.selection_range)},
                         {"kind", symbol.kind},
                         {"scopePath", symbol.scope_path}};
}

jsonrpc::Json indexCompletionData(const analysis::CompletionEntry& entry) {
    return jsonrpc::Json{{"source", "index"},
                         {"label", entry.label},
                         {"uri", entry.location.uri},
                         {"range", toRangeJson(entry.location.range)},
                         {"selectionRange", toRangeJson(entry.selection_range)},
                         {"kind", entry.kind}};
}

jsonrpc::Json portCompletionData(const analysis::ModuleDefinition& module,
                                 const analysis::SchematicPort& port,
                                 std::string_view declaration_uri) {
    return jsonrpc::Json{{"source", "port"},
                         {"moduleName", module.name},
                         {"portName", port.name},
                         {"uri", std::string(declaration_uri)},
                         {"range", toRangeJson(port.range)},
                         {"selectionRange", toRangeJson(port.selection_range)}};
}

jsonrpc::Json macroCompletionData(const analysis::MacroEntry& macro) {
    jsonrpc::Json parameters = jsonrpc::Json::array();
    for (const auto& parameter : macro.parameters) {
        parameters.push_back(parameter);
    }
    return jsonrpc::Json{{"source", "macro"},
                         {"name", macro.name},
                         {"parameters", std::move(parameters)},
                         {"body", macro.body},
                         {"functionLike", macro.function_like},
                         {"uri", macro.location.uri},
                         {"range", toRangeJson(macro.location.range)},
                         {"selectionRange", toRangeJson(macro.selection_range)}};
}

jsonrpc::Json toSemanticCompletionItem(const analysis::SemanticSymbol& symbol) {
    return jsonrpc::Json{{"label", symbol.name},
                         {"kind", toCompletionItemKind(symbol.kind)},
                         {"detail", completionDetailForSymbolKind(symbol.kind)},
                         {"data", semanticCompletionData(symbol)}};
}

jsonrpc::Json semanticEngineCompletionData(const analysis::SemanticCompletionItem& item) {
    return jsonrpc::Json{{"source", "semanticEngine"},
                         {"stableId", item.stable_id},
                         {"label", item.label}};
}

jsonrpc::Json toSemanticEngineCompletionItem(const analysis::SemanticCompletionItem& item) {
    jsonrpc::Json result{{"label", item.label},
                         {"kind", item.kind == 0 ? 18 : item.kind},
                         {"detail", item.detail},
                         {"data", semanticEngineCompletionData(item)}};
    if (!item.insert_text.empty() && item.insert_text != item.label) {
        result["insertText"] = item.insert_text;
        if (item.insert_text.find("${") != std::string::npos) {
            result["insertTextFormat"] = 2;
        }
    }
    return result;
}

jsonrpc::Json toIndexCompletionItem(const analysis::CompletionEntry& entry) {
    return jsonrpc::Json{{"label", entry.label},
                         {"kind", toCompletionItemKind(entry.kind)},
                         {"detail", entry.detail},
                         {"data", indexCompletionData(entry)}};
}

jsonrpc::Json toPortCompletionItem(const analysis::ModuleDefinition& module,
                                   const analysis::SchematicPort& port,
                                   std::string_view declaration_uri) {
    return jsonrpc::Json{{"label", port.name},
                         {"kind", 5},
                         {"detail", portCompletionDetail(port)},
                         {"data", portCompletionData(module, port, declaration_uri)}};
}

jsonrpc::Json toMacroCompletionItem(const analysis::MacroEntry& macro) {
    return jsonrpc::Json{{"label", macro.name},
                         {"kind", macro.function_like ? 3 : 21},
                         {"detail", macro.function_like ? "Macro function" : "Macro"},
                         {"data", macroCompletionData(macro)}};
}

std::optional<analysis::MacroEntry> macroEntryFromCompletionData(const jsonrpc::Json& data) {
    const auto name = jsonStringField(data, "name");
    const auto uri = jsonStringField(data, "uri");
    if (!name.has_value() || !uri.has_value()) {
        return std::nullopt;
    }

    std::vector<std::string> parameters;
    const auto parameters_it = data.find("parameters");
    if (parameters_it != data.end() && parameters_it->is_array()) {
        for (const auto& parameter : *parameters_it) {
            if (parameter.is_string()) {
                parameters.push_back(parameter.get<std::string>());
            }
        }
    }

    auto body = jsonStringField(data, "body").value_or("");
    bool function_like = false;
    const auto function_like_it = data.find("functionLike");
    if (function_like_it != data.end() && function_like_it->is_boolean()) {
        function_like = function_like_it->get<bool>();
    }

    const auto range = jsonRangeField(data, "range").value_or(analysis::ParseRange{});
    const auto selection_range = jsonRangeField(data, "selectionRange").value_or(range);

    return analysis::MacroEntry{.name = *name,
                                .parameters = std::move(parameters),
                                .body = std::move(body),
                                .location = analysis::Location{.uri = *uri, .range = range},
                                .selection_range = selection_range,
                                .function_like = function_like};
}

bool isModuleLikeCompletionSymbol(int symbol_kind) {
    return symbol_kind == 2 || symbol_kind == 11;
}

bool isTypeDefinitionSymbol(int symbol_kind) {
    switch (symbol_kind) {
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

std::string percentEncodePath(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':') {
            result.push_back(static_cast<char>(ch));
            continue;
        }

        result.push_back('%');
        result.push_back(hex[(ch >> 4U) & 0x0FU]);
        result.push_back(hex[ch & 0x0FU]);
    }

    return result;
}

std::string toFileUri(const fs::path& path) {
    std::error_code error;
    auto normalized = fs::weakly_canonical(path, error);
    if (error) {
        normalized = fs::absolute(path, error);
    }
    const auto generic = normalized.generic_string();
    return std::string("file://") + (generic.starts_with('/') ? "" : "/") + percentEncodePath(generic);
}

std::optional<std::string> readFileText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool isIndexableSourcePath(const fs::path& path) {
    const auto extension = path.extension().string();
    return extension == ".sv" || extension == ".svh" || extension == ".v" || extension == ".vh";
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

int comparePosition(int lhs_line, int lhs_character, int rhs_line, int rhs_character) {
    if (lhs_line != rhs_line) {
        return lhs_line < rhs_line ? -1 : 1;
    }
    if (lhs_character == rhs_character) {
        return 0;
    }
    return lhs_character < rhs_character ? -1 : 1;
}

bool positionInRange(int line, int character, const lsp::Range& range) {
    return comparePosition(line, character, range.start.line, range.start.character) >= 0 &&
           comparePosition(line, character, range.end.line, range.end.character) <= 0;
}

bool isEmptyRange(const lsp::Range& range) {
    return range.start.line == range.end.line && range.start.character == range.end.character;
}

bool parseRangeContainsPosition(const analysis::ParseRange& range, const lsp::Position& position) {
    return comparePosition(position.line, position.character, range.start_line, range.start_character) >= 0 &&
           comparePosition(position.line, position.character, range.end_line, range.end_character) < 0;
}

bool parseRangeIntersects(const analysis::ParseRange& target, const lsp::Range& range) {
    if (isEmptyRange(range)) {
        return parseRangeContainsPosition(target, range.start);
    }

    return comparePosition(target.end_line, target.end_character,
                           range.start.line, range.start.character) > 0 &&
           comparePosition(range.end.line, range.end.character,
                           target.start_line, target.start_character) > 0;
}

bool sameParseRange(const analysis::ParseRange& lhs, const analysis::ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

void appendDistinctRange(std::vector<analysis::ParseRange>& ranges, const analysis::ParseRange& range) {
    if (std::none_of(ranges.begin(), ranges.end(), [&](const analysis::ParseRange& existing) {
            return sameParseRange(existing, range);
        })) {
        ranges.push_back(range);
    }
}

analysis::ParseRange pointRange(const lsp::Position& position) {
    return analysis::ParseRange{.start_line = position.line,
                                .start_character = position.character,
                                .end_line = position.line,
                                .end_character = position.character};
}

analysis::ParseRange pointRangeAtOffset(std::string_view text, size_t target_offset) {
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
    return analysis::ParseRange{.start_line = line,
                                .start_character = character,
                                .end_line = line,
                                .end_character = character};
}

std::optional<analysis::ParseRange> lineRangeAtPosition(std::string_view text, const lsp::Position& position) {
    if (position.line < 0) {
        return std::nullopt;
    }

    int line = 0;
    size_t line_start = 0;
    for (size_t offset = 0; offset < text.size() && line < position.line; ++offset) {
        if (text[offset] == '\n') {
            ++line;
            line_start = offset + 1;
        }
    }
    if (line != position.line || line_start > text.size()) {
        return std::nullopt;
    }

    size_t line_end = line_start;
    while (line_end < text.size() && text[line_end] != '\n' && text[line_end] != '\r') {
        ++line_end;
    }

    size_t trimmed_start = line_start;
    while (trimmed_start < line_end && (text[trimmed_start] == ' ' || text[trimmed_start] == '\t')) {
        ++trimmed_start;
    }
    size_t trimmed_end = line_end;
    while (trimmed_end > trimmed_start && (text[trimmed_end - 1] == ' ' || text[trimmed_end - 1] == '\t')) {
        --trimmed_end;
    }
    if (trimmed_start == trimmed_end) {
        return std::nullopt;
    }

    return analysis::ParseRange{.start_line = position.line,
                                .start_character = static_cast<int>(trimmed_start - line_start),
                                .end_line = position.line,
                                .end_character = static_cast<int>(trimmed_end - line_start)};
}

std::optional<size_t> offsetAtPosition(std::string_view text, const lsp::Position& position) {
    if (position.line < 0 || position.character < 0) {
        return std::nullopt;
    }

    int line = 0;
    int character = 0;
    for (size_t offset = 0; offset < text.size(); ++offset) {
        if (line == position.line && character == position.character) {
            return offset;
        }

        const char value = text[offset];
        if (value == '\r') {
            if (offset + 1 < text.size() && text[offset + 1] == '\n') {
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

    if (line == position.line && character == position.character) {
        return text.size();
    }
    return std::nullopt;
}

std::optional<size_t> offsetAtParsePosition(std::string_view text, int line, int character) {
    return offsetAtPosition(text, lsp::Position{.line = line, .character = character});
}

std::optional<std::string> textForParseRange(std::string_view text, const analysis::ParseRange& range) {
    const auto start_offset = offsetAtParsePosition(text, range.start_line, range.start_character);
    const auto end_offset = offsetAtParsePosition(text, range.end_line, range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value() || *start_offset > *end_offset) {
        return std::nullopt;
    }
    return std::string(text.substr(*start_offset, *end_offset - *start_offset));
}

std::optional<analysis::ParseRange> instancePortInsertionRange(std::string_view text,
                                                               const analysis::SchematicCell& cell) {
    const auto start_offset = offsetAtParsePosition(text, cell.range.start_line, cell.range.start_character);
    const auto end_offset = offsetAtParsePosition(text, cell.range.end_line, cell.range.end_character);
    if (!start_offset.has_value() || !end_offset.has_value() || *start_offset >= *end_offset) {
        return std::nullopt;
    }

    for (size_t offset = *end_offset; offset > *start_offset; --offset) {
        if (text[offset - 1] == ')') {
            return pointRangeAtOffset(text, offset - 1);
        }
    }
    return std::nullopt;
}

std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                  const lsp::Position& position,
                                                  std::string_view prefix) {
    const auto offset = offsetAtPosition(text, position);
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
        const auto ch = static_cast<unsigned char>(text[index]);
        if (std::isspace(ch) == 0) {
            return false;
        }
    }
    return true;
}

bool isModuleInstantiationCompletionContext(std::string_view text,
                                            const lsp::Position& position,
                                            std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, position, prefix);
    return prefix_start.has_value() && hasOnlyWhitespaceSinceLineStart(text, *prefix_start);
}

bool isMacroCompletionContext(std::string_view text,
                              const lsp::Position& position,
                              std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, position, prefix);
    return prefix_start.has_value() && *prefix_start > 0 && text[*prefix_start - 1] == '`';
}

bool isNamedPortCompletionContext(std::string_view text,
                                  const lsp::Position& position,
                                  std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, position, prefix);
    return prefix_start.has_value() && *prefix_start > 0 && text[*prefix_start - 1] == '.';
}

std::optional<std::string> packageQualifierBeforeCompletion(std::string_view text,
                                                            const lsp::Position& position,
                                                            std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, position, prefix);
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
    if (!isValidIdentifier(qualifier)) {
        return std::nullopt;
    }
    return std::string(qualifier);
}

std::set<std::string> connectedNamedPortsBeforePosition(std::string_view text,
                                                        const SignatureInvocation& invocation) {
    std::set<std::string> connected_ports;
    int depth = 0;
    for (size_t offset = invocation.open_paren_offset + 1;
         offset < invocation.position_offset && offset < text.size();
         ++offset) {
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
        if (name_start >= invocation.position_offset || !isIdentifierStart(text[name_start])) {
            continue;
        }

        size_t name_end = name_start + 1;
        while (name_end < invocation.position_offset && isIdentifierContinue(text[name_end])) {
            ++name_end;
        }

        size_t cursor = name_end;
        while (cursor < invocation.position_offset &&
               std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < invocation.position_offset && text[cursor] == '(') {
            connected_ports.insert(std::string(text.substr(name_start, name_end - name_start)));
        }

        offset = name_end;
    }
    return connected_ports;
}

bool appendCompletionItem(jsonrpc::Json& result,
                          std::set<std::string>& emitted_labels,
                          jsonrpc::Json item) {
    const auto label_it = item.find("label");
    if (label_it == item.end() || !label_it->is_string()) {
        return false;
    }
    if (!emitted_labels.insert(label_it->get<std::string>()).second) {
        return false;
    }
    result.push_back(std::move(item));
    return true;
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

std::optional<SignatureInvocation> findSignatureInvocation(
    const analysis::CompilationService& compilation_service,
    const document::TextDocument& document,
    const lsp::Position& position) {
    const auto position_offset = offsetAtPosition(document.text, position);
    if (!position_offset.has_value()) {
        return std::nullopt;
    }

    for (const auto& module : compilation_service.moduleDefinitions(document.text, document.uri)) {
        for (const auto& instance : module.instances) {
            if (!parseRangeContainsPosition(instance.range, position)) {
                continue;
            }

            const auto search_start = offsetAtParsePosition(document.text,
                                                            instance.selection_range.end_line,
                                                            instance.selection_range.end_character);
            const auto search_end = offsetAtParsePosition(document.text,
                                                          instance.range.end_line,
                                                          instance.range.end_character);
            if (!search_start.has_value() || !search_end.has_value() || *position_offset < *search_start) {
                continue;
            }

            const auto bounded_position = std::min(*position_offset, *search_end);
            const auto open_paren_it = std::find(document.text.begin() + static_cast<std::ptrdiff_t>(*search_start),
                                                 document.text.begin() + static_cast<std::ptrdiff_t>(bounded_position),
                                                 '(');
            if (open_paren_it == document.text.begin() + static_cast<std::ptrdiff_t>(bounded_position)) {
                continue;
            }

            const auto open_paren_offset = static_cast<size_t>(std::distance(document.text.begin(), open_paren_it));
            return SignatureInvocation{.module_name = instance.module_name,
                                       .instance_name = instance.instance_name,
                                       .active_parameter = activeParameterAt(document.text,
                                                                            open_paren_offset,
                                                                            *position_offset),
                                       .open_paren_offset = open_paren_offset,
                                       .position_offset = *position_offset};
        }
    }

    return std::nullopt;
}

jsonrpc::Json toSignatureHelpJson(const analysis::ModuleDefinition& module, int active_parameter) {
    jsonrpc::Json parameters = jsonrpc::Json::array();
    if (module.port_details.empty()) {
        for (const auto& port : module.ports) {
            parameters.push_back(jsonrpc::Json{{"label", port}});
        }
    }
    else {
        for (const auto& port : module.port_details) {
            parameters.push_back(jsonrpc::Json{{"label", portSignatureLabel(port)}});
        }
    }

    const auto parameter_count = module.port_details.empty() ? module.ports.size() : module.port_details.size();
    const auto bounded_parameter = parameter_count == 0
        ? 0
        : std::min(active_parameter, static_cast<int>(parameter_count) - 1);

    return jsonrpc::Json{{"signatures",
                          jsonrpc::Json::array({jsonrpc::Json{{"label", moduleSignatureLabel(module)},
                                                               {"parameters", std::move(parameters)}}})},
                         {"activeSignature", 0},
                         {"activeParameter", bounded_parameter}};
}

jsonrpc::Json toSignatureHelpJson(const analysis::SemanticSignatureHelpResult& help) {
    jsonrpc::Json parameters = jsonrpc::Json::array();
    for (const auto& parameter : help.parameters) {
        parameters.push_back(jsonrpc::Json{{"label", parameter}});
    }
    const auto parameter_count = help.parameters.size();
    const auto bounded_parameter = parameter_count == 0
        ? 0
        : std::min(help.active_parameter, static_cast<int>(parameter_count) - 1);
    return jsonrpc::Json{{"signatures",
                          jsonrpc::Json::array({jsonrpc::Json{{"label", help.label},
                                                               {"parameters", std::move(parameters)}}})},
                         {"activeSignature", 0},
                         {"activeParameter", bounded_parameter}};
}

void collectSelectionSymbolRanges(std::vector<analysis::ParseRange>& ranges,
                                  const std::vector<analysis::DocumentSymbol>& symbols,
                                  const lsp::Position& position) {
    for (const auto& symbol : symbols) {
        if (!parseRangeContainsPosition(symbol.range, position) &&
            !parseRangeContainsPosition(symbol.selection_range, position)) {
            continue;
        }

        collectSelectionSymbolRanges(ranges, symbol.children, position);
        if (parseRangeContainsPosition(symbol.selection_range, position)) {
            appendDistinctRange(ranges, symbol.selection_range);
        }
        if (parseRangeContainsPosition(symbol.range, position)) {
            appendDistinctRange(ranges, symbol.range);
        }
    }
}

jsonrpc::Json toSelectionRangeJson(const std::vector<analysis::ParseRange>& ranges) {
    jsonrpc::Json current;
    for (auto range_it = ranges.rbegin(); range_it != ranges.rend(); ++range_it) {
        jsonrpc::Json next{{"range", toRangeJson(*range_it)}};
        if (!current.is_null()) {
            next["parent"] = std::move(current);
        }
        current = std::move(next);
    }
    return current;
}

jsonrpc::Json toSelectionRangeJson(const std::vector<analysis::SemanticSelectionRange>& ranges,
                                   size_t index) {
    const auto& range = ranges[index];
    jsonrpc::Json result{{"range", toRangeJson(range.range)}};
    if (range.parent.has_value() && *range.parent < ranges.size() && *range.parent != index) {
        result["parent"] = toSelectionRangeJson(ranges, *range.parent);
    }
    return result;
}

bool hasStructuredSelectionParent(const std::vector<analysis::SemanticSelectionRange>& ranges) {
    if (ranges.empty() || !ranges.front().parent.has_value() || *ranges.front().parent >= ranges.size()) {
        return false;
    }
    const auto& child = ranges.front().range;
    const auto& parent = ranges[*ranges.front().parent].range;
    return parent.start_line != child.start_line || parent.start_character != 0;
}

jsonrpc::Json selectionRangeForPosition(const analysis::CompilationService& compilation_service,
                                        const document::TextDocument& document,
                                        const lsp::Position& position) {
    std::vector<analysis::ParseRange> ranges;
    if (const auto identifier = compilation_service.identifierAt(document.text, position.line,
                                                                 position.character)) {
        appendDistinctRange(ranges, identifier->range);
    }
    if (const auto line_range = lineRangeAtPosition(document.text, position)) {
        appendDistinctRange(ranges, *line_range);
    }
    collectSelectionSymbolRanges(ranges,
                                 compilation_service.documentSymbols(document.text, document.uri),
                                 position);
    if (ranges.empty()) {
        ranges.push_back(pointRange(position));
    }
    return toSelectionRangeJson(ranges);
}

std::optional<fs::path> resolveIncludeTarget(const workspace::WorkspaceManager& workspace_manager,
                                             std::string_view document_uri,
                                             std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    std::vector<fs::path> candidates;

    if (target_path.is_absolute()) {
        candidates.push_back(target_path);
    }
    else if (const auto document_path = workspace::WorkspaceManager::pathFromFileUri(document_uri)) {
        candidates.push_back(document_path->parent_path() / target_path);
    }

    if (!target_path.is_absolute()) {
        const auto& workspace_state = workspace_manager.state();
        if (workspace_state.root_path.has_value()) {
            candidates.push_back(*workspace_state.root_path / target_path);
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

std::optional<fs::path> proposedIncludeTarget(const workspace::WorkspaceManager& workspace_manager,
                                              std::string_view document_uri,
                                              std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    if (target_path.is_absolute()) {
        return target_path;
    }

    if (const auto document_path = workspace::WorkspaceManager::pathFromFileUri(document_uri)) {
        return document_path->parent_path() / target_path;
    }

    const auto& workspace_state = workspace_manager.state();
    if (workspace_state.root_path.has_value()) {
        return *workspace_state.root_path / target_path;
    }

    return std::nullopt;
}

jsonrpc::Json toDocumentSymbolJson(const analysis::DocumentSymbol& symbol) {
    jsonrpc::Json result{{"name", symbol.name},
                         {"kind", symbol.kind},
                         {"range", toRangeJson(symbol.range)},
                         {"selectionRange", toRangeJson(symbol.selection_range)}};

    if (!symbol.children.empty()) {
        result["children"] = jsonrpc::Json::array();
        for (const auto& child : symbol.children) {
            result["children"].push_back(toDocumentSymbolJson(child));
        }
    }

    return result;
}

struct IndexedModuleDefinition {
    std::string uri;
    analysis::ModuleDefinition definition;
};

std::vector<IndexedModuleDefinition> sortedModuleDefinitions(
    const std::unordered_map<std::string, std::vector<analysis::ModuleDefinition>>& documents) {
    std::vector<IndexedModuleDefinition> result;
    for (const auto& [uri, definitions] : documents) {
        for (const auto& definition : definitions) {
            result.push_back(IndexedModuleDefinition{.uri = uri, .definition = definition});
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.definition.name != rhs.definition.name) {
            return lhs.definition.name < rhs.definition.name;
        }
        if (lhs.uri != rhs.uri) {
            return lhs.uri < rhs.uri;
        }
        return lhs.definition.range.start_line < rhs.definition.range.start_line;
    });

    return result;
}

std::map<std::string, IndexedModuleDefinition> buildModuleLookup(
    const std::vector<IndexedModuleDefinition>& definitions) {
    std::map<std::string, IndexedModuleDefinition> modules;
    for (const auto& definition : definitions) {
        modules.try_emplace(definition.definition.name, definition);
    }
    return modules;
}

jsonrpc::Json toSchematicPortJson(const analysis::SchematicPort& port) {
    return jsonrpc::Json{{"name", port.name},
                         {"direction", port.direction},
                         {"widthText", port.width_text},
                         {"range", toRangeJson(port.range)},
                         {"selectionRange", toRangeJson(port.selection_range)}};
}

jsonrpc::Json toSchematicConnectionJson(const analysis::SchematicConnection& connection) {
    return jsonrpc::Json{{"portName", connection.port_name},
                         {"portIndex", connection.port_index},
                         {"signal", connection.signal},
                         {"range", toRangeJson(connection.range)}};
}

jsonrpc::Json toSchematicCellJson(const analysis::SchematicCell& cell) {
    jsonrpc::Json connections = jsonrpc::Json::array();
    for (const auto& connection : cell.connections) {
        connections.push_back(toSchematicConnectionJson(connection));
    }

    return jsonrpc::Json{{"id", cell.id},
                         {"name", cell.name},
                         {"type", cell.type},
                         {"kind", cell.kind},
                         {"range", toRangeJson(cell.range)},
                         {"selectionRange", toRangeJson(cell.selection_range)},
                         {"connections", std::move(connections)}};
}

jsonrpc::Json toHierarchyNodeJson(const analysis::SemanticHierarchyNode& node) {
    jsonrpc::Json children = jsonrpc::Json::array();
    for (const auto& child : node.children) {
        children.push_back(toHierarchyNodeJson(child));
    }

    jsonrpc::Json result{{"moduleName", node.module_name},
                         {"kind", node.kind.empty() ? "module" : node.kind},
                         {"uri", node.location.uri.empty() ? jsonrpc::Json(nullptr) : jsonrpc::Json(node.location.uri)},
                         {"range", node.location.uri.empty() ? jsonrpc::Json(nullptr)
                                                              : toRangeJson(node.location.range)},
                         {"selectionRange", node.location.uri.empty() ? jsonrpc::Json(nullptr)
                                                                      : toRangeJson(node.selection_range)},
                         {"unresolved", node.unresolved},
                         {"cycle", node.cycle},
                         {"children", std::move(children)}};
    if (!node.instance_name.empty()) {
        result["instanceName"] = node.instance_name;
    }
    if (node.instance_range.has_value()) {
        result["instanceRange"] = toRangeJson(*node.instance_range);
    }
    if (node.instance_selection_range.has_value()) {
        result["instanceSelectionRange"] = toRangeJson(*node.instance_selection_range);
    }
    if (node.module_selection_range.has_value()) {
        result["moduleSelectionRange"] = toRangeJson(*node.module_selection_range);
    }
    if (node.truncated) {
        result["truncated"] = true;
    }
    return result;
}

jsonrpc::Json toSchematicEndpointJson(const analysis::SemanticSchematicEndpoint& endpoint) {
    return jsonrpc::Json{{"nodeId", endpoint.node_id}, {"portName", endpoint.port_name}};
}

jsonrpc::Json toSchematicNetJson(const analysis::SemanticSchematicNet& net) {
    jsonrpc::Json drivers = jsonrpc::Json::array();
    for (const auto& driver : net.drivers) {
        drivers.push_back(toSchematicEndpointJson(driver));
    }

    jsonrpc::Json loads = jsonrpc::Json::array();
    for (const auto& load : net.loads) {
        loads.push_back(toSchematicEndpointJson(load));
    }

    return jsonrpc::Json{{"name", net.name},
                         {"drivers", std::move(drivers)},
                         {"loads", std::move(loads)}};
}

jsonrpc::Json toSchematicModuleJson(const analysis::SemanticSchematicModuleView& view) {
    jsonrpc::Json ports = jsonrpc::Json::array();
    for (const auto& port : view.module.ports) {
        ports.push_back(toSchematicPortJson(port));
    }

    jsonrpc::Json cells = jsonrpc::Json::array();
    for (const auto& cell : view.module.cells) {
        cells.push_back(toSchematicCellJson(cell));
    }

    jsonrpc::Json nets = jsonrpc::Json::array();
    for (const auto& net : view.nets) {
        nets.push_back(toSchematicNetJson(net));
    }

    return jsonrpc::Json{{"id", view.module.id},
                         {"name", view.module.name},
                         {"uri", view.module.uri},
                         {"range", toRangeJson(view.module.range)},
                         {"selectionRange", toRangeJson(view.module.selection_range)},
                         {"ports", std::move(ports)},
                         {"cells", std::move(cells)},
                         {"nets", std::move(nets)}};
}

jsonrpc::Json toConeNodeJson(const analysis::SemanticConeNode& node) {
    jsonrpc::Json result{{"id", node.id},
                         {"name", node.name},
                         {"uri", node.location.uri},
                         {"range", toRangeJson(node.location.range)}};
    result["bitWidth"] = node.bit_width.has_value() ? jsonrpc::Json(*node.bit_width) : jsonrpc::Json(nullptr);
    return result;
}

jsonrpc::Json toConeEdgeJson(const analysis::SemanticConeEdge& edge) {
    return jsonrpc::Json{{"from", edge.from_symbol_id},
                         {"to", edge.to_symbol_id},
                         {"range", toRangeJson(edge.location.range)},
                         {"expression", edge.expression}};
}

jsonrpc::Json toConeTraceJson(const analysis::SemanticConeTrace& trace) {
    jsonrpc::Json nodes = jsonrpc::Json::array();
    for (const auto& node : trace.nodes) {
        nodes.push_back(toConeNodeJson(node));
    }

    jsonrpc::Json edges = jsonrpc::Json::array();
    for (const auto& edge : trace.edges) {
        edges.push_back(toConeEdgeJson(edge));
    }

    jsonrpc::Json messages = jsonrpc::Json::array();
    for (const auto& message : trace.messages) {
        messages.push_back(message);
    }

    return jsonrpc::Json{{"rootSymbolId",
                          trace.root_symbol_id.has_value() ? jsonrpc::Json(*trace.root_symbol_id)
                                                            : jsonrpc::Json(nullptr)},
                         {"nodes", std::move(nodes)},
                         {"edges", std::move(edges)},
                         {"messages", std::move(messages)}};
}

jsonrpc::Json toCallHierarchyItemJson(const analysis::SemanticCallHierarchyItem& item) {
    return jsonrpc::Json{{"name", item.name},
                         {"kind", item.kind},
                         {"detail", item.detail},
                         {"uri", item.uri},
                         {"range", toRangeJson(item.range)},
                         {"selectionRange", toRangeJson(item.selection_range)}};
}

analysis::ParseRange parseRangeFromLspRange(const lsp::Range& range) {
    return analysis::ParseRange{.start_line = range.start.line,
                                .start_character = range.start.character,
                                .end_line = range.end.line,
                                .end_character = range.end.character};
}

analysis::SemanticCallHierarchyItem toSemanticCallHierarchyItem(const lsp::CallHierarchyItem& item) {
    return analysis::SemanticCallHierarchyItem{.name = item.name,
                                               .uri = item.uri,
                                               .range = parseRangeFromLspRange(item.range),
                                               .selection_range = parseRangeFromLspRange(item.selection_range)};
}

std::optional<std::string> parseOptionalModuleName(const jsonrpc::Json& params) {
    const auto module_name_it = params.find("moduleName");
    if (module_name_it == params.end() || module_name_it->is_null()) {
        return std::nullopt;
    }
    if (!module_name_it->is_string()) {
        throw std::runtime_error("Expected 'moduleName' to be a string");
    }
    return module_name_it->get<std::string>();
}

int parseMaxDepth(const jsonrpc::Json& params) {
    const auto max_depth_it = params.find("maxDepth");
    if (max_depth_it == params.end() || max_depth_it->is_null()) {
        return 64;
    }
    if (!max_depth_it->is_number_integer()) {
        throw std::runtime_error("Expected 'maxDepth' to be an integer");
    }
    return std::max(1, max_depth_it->get<int>());
}

} // namespace

ServerSession::ServerSession(std::string server_name, std::string server_version) :
    server_name_(std::move(server_name)), server_version_(std::move(server_version)) {}

void ServerSession::bind(jsonrpc::JsonRpcServer& server) {
    server_ = &server;

    server.registerRequestHandler("initialize", [this](const jsonrpc::Json& params) {
        return handleInitialize(params);
    });
    server.registerRequestHandler("textDocument/documentSymbol", [this](const jsonrpc::Json& params) {
        return handleDocumentSymbol(params);
    });
    server.registerRequestHandler("systemverilog/moduleHierarchy", [this](const jsonrpc::Json& params) {
        return handleModuleHierarchy(params);
    });
    server.registerRequestHandler("systemverilog/schematic", [this](const jsonrpc::Json& params) {
        return handleSchematic(params);
    });
    server.registerRequestHandler("systemverilog/backwardCone", [this](const jsonrpc::Json& params) {
        return handleBackwardCone(params);
    });
    server.registerRequestHandler("textDocument/hover", [this](const jsonrpc::Json& params) {
        return handleHover(params);
    });
    server.registerRequestHandler("textDocument/definition", [this](const jsonrpc::Json& params) {
        return handleDefinition(params);
    });
    server.registerRequestHandler("textDocument/typeDefinition", [this](const jsonrpc::Json& params) {
        return handleTypeDefinition(params);
    });
    server.registerRequestHandler("textDocument/implementation", [this](const jsonrpc::Json& params) {
        return handleImplementation(params);
    });
    server.registerRequestHandler("textDocument/documentHighlight", [this](const jsonrpc::Json& params) {
        return handleDocumentHighlight(params);
    });
    server.registerRequestHandler("textDocument/documentLink", [this](const jsonrpc::Json& params) {
        return handleDocumentLink(params);
    });
    server.registerRequestHandler("textDocument/inlayHint", [this](const jsonrpc::Json& params) {
        return handleInlayHint(params);
    });
    server.registerRequestHandler("textDocument/codeAction", [this](const jsonrpc::Json& params) {
        return handleCodeAction(params);
    });
    server.registerRequestHandler("textDocument/foldingRange", [this](const jsonrpc::Json& params) {
        return handleFoldingRange(params);
    });
    server.registerRequestHandler("textDocument/semanticTokens/full", [this](const jsonrpc::Json& params) {
        return handleSemanticTokensFull(params);
    });
    server.registerRequestHandler("textDocument/selectionRange", [this](const jsonrpc::Json& params) {
        return handleSelectionRange(params);
    });
    server.registerRequestHandler("textDocument/signatureHelp", [this](const jsonrpc::Json& params) {
        return handleSignatureHelp(params);
    });
    server.registerRequestHandler("textDocument/prepareCallHierarchy", [this](const jsonrpc::Json& params) {
        return handlePrepareCallHierarchy(params);
    });
    server.registerRequestHandler("callHierarchy/incomingCalls", [this](const jsonrpc::Json& params) {
        return handleIncomingCalls(params);
    });
    server.registerRequestHandler("callHierarchy/outgoingCalls", [this](const jsonrpc::Json& params) {
        return handleOutgoingCalls(params);
    });
    server.registerRequestHandler("textDocument/references", [this](const jsonrpc::Json& params) {
        return handleReferences(params);
    });
    server.registerRequestHandler("workspace/symbol", [this](const jsonrpc::Json& params) {
        return handleWorkspaceSymbol(params);
    });
    server.registerRequestHandler("textDocument/completion", [this](const jsonrpc::Json& params) {
        return handleCompletion(params);
    });
    server.registerRequestHandler("completionItem/resolve", [this](const jsonrpc::Json& params) {
        return handleCompletionItemResolve(params);
    });
    server.registerRequestHandler("textDocument/prepareRename", [this](const jsonrpc::Json& params) {
        return handlePrepareRename(params);
    });
    server.registerRequestHandler("textDocument/rename", [this](const jsonrpc::Json& params) {
        return handleRename(params);
    });
    server.registerRequestHandler("shutdown", [this](const jsonrpc::Json& params) {
        return handleShutdown(params);
    });

    server.registerNotificationHandler("initialized", [this](const jsonrpc::Json& params) {
        handleInitialized(params);
    });
    server.registerNotificationHandler("textDocument/didOpen", [this](const jsonrpc::Json& params) {
        handleDidOpen(params);
    });
    server.registerNotificationHandler("textDocument/didChange", [this](const jsonrpc::Json& params) {
        handleDidChange(params);
    });
    server.registerNotificationHandler("textDocument/didSave", [this](const jsonrpc::Json& params) {
        handleDidSave(params);
    });
    server.registerNotificationHandler("textDocument/didClose", [this](const jsonrpc::Json& params) {
        handleDidClose(params);
    });
    server.registerNotificationHandler("workspace/didChangeWatchedFiles", [this](const jsonrpc::Json& params) {
        handleDidChangeWatchedFiles(params);
    });
    server.registerNotificationHandler("exit", [this](const jsonrpc::Json& params) {
        handleExit(params);
    });
}

jsonrpc::Json ServerSession::handleInitialize(const jsonrpc::Json& params) {
    workspace_manager_.initialize(lsp::parseInitializeParams(params));
    semantic_workspace_.clear();
    const auto& workspace_config = workspace_manager_.state().config;
    semantic_workspace_.configureSemanticEngine(
        analysis::SemanticEngineConfig{.build = workspace_config.build,
                                       .build_pattern = workspace_config.build_pattern,
                                       .build_relative_paths = workspace_config.build_relative_paths,
                                       .flags = workspace_config.flags,
                                       .top_modules = workspace_config.top_modules});
    if (const auto& root_path = workspace_manager_.state().root_path) {
        semantic_workspace_.setWorkspaceRoot(toFileUri(*root_path));
    }
    else {
        semantic_workspace_.setWorkspaceRoot({});
    }
    symbol_index_.clear();
    hierarchy_documents_.clear();
    indexWorkspaceSources();
    initialized_ = true;
    shutdown_requested_ = false;
    return lsp::makeInitializeResult(server_name_, server_version_);
}

jsonrpc::Json ServerSession::handleDocumentSymbol(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/documentSymbol received before initialize");
    }

    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto* document = document_store_.find(uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : compilation_service_.documentSymbols(document->text, document->uri)) {
        result.push_back(toDocumentSymbolJson(symbol));
    }

    return result;
}

jsonrpc::Json ServerSession::handleModuleHierarchy(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/moduleHierarchy received before initialize");
    }

    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);
    const auto hierarchy = semantic_workspace_.engineModuleHierarchy(
        requested_module_name.has_value()
            ? std::optional<std::string_view>{std::string_view(*requested_module_name)}
            : std::optional<std::string_view>{},
        max_depth);

    jsonrpc::Json roots = jsonrpc::Json::array();
    jsonrpc::Json messages = jsonrpc::Json::array();
    for (const auto& root : hierarchy.roots) {
        roots.push_back(toHierarchyNodeJson(root));
    }
    for (const auto& message : hierarchy.messages) {
        messages.push_back(message);
    }

    jsonrpc::Json result{{"roots", std::move(roots)}, {"messages", std::move(messages)}};
    if (hierarchy.unresolved) {
        result["unresolved"] = true;
    }
    if (hierarchy.partial) {
        result["partial"] = true;
    }
    if (hierarchy.truncated) {
        result["truncated"] = true;
    }
    return result;
}

jsonrpc::Json ServerSession::handleSchematic(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/schematic received before initialize");
    }

    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);
    const auto schematic = semantic_workspace_.engineSchematic(
        requested_module_name.has_value()
            ? std::optional<std::string_view>{std::string_view(*requested_module_name)}
            : std::optional<std::string_view>{},
        max_depth);

    jsonrpc::Json messages = jsonrpc::Json::array();
    jsonrpc::Json modules = jsonrpc::Json::array();
    for (const auto& module : schematic.modules) {
        modules.push_back(toSchematicModuleJson(module));
    }
    for (const auto& message : schematic.messages) {
        messages.push_back(message);
    }

    jsonrpc::Json result{{"rootModuleId",
                          schematic.root_module_id.has_value() ? jsonrpc::Json(*schematic.root_module_id)
                                                                : jsonrpc::Json(nullptr)},
                         {"modules", std::move(modules)},
                         {"messages", std::move(messages)}};
    if (schematic.unresolved) {
        result["unresolved"] = true;
    }
    if (schematic.partial) {
        result["partial"] = true;
    }
    if (schematic.truncated) {
        result["truncated"] = true;
    }
    return result;
}

jsonrpc::Json ServerSession::handleBackwardCone(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/backwardCone received before initialize");
    }

    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto& position = params.at("position");
    const auto line = position.at("line").get<int>();
    const auto character = position.at("character").get<int>();
    return toConeTraceJson(semantic_workspace_.backwardConeAt(uri, line, character));
}

jsonrpc::Json ServerSession::handleHover(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/hover received before initialize");
    }

    const auto hover = lsp::parseHoverParams(params);
    const auto* document = document_store_.find(hover.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto semantic_result = semantic_workspace_.hoverAt(document->uri, hover.position.line,
                                                             hover.position.character);
    if (semantic_result) {
        return jsonrpc::Json{{"contents", jsonrpc::Json{{"kind", "markdown"},
                                                         {"value", semantic_result->contents}}},
                             {"range", toRangeJson(semantic_result->range)}};
    }

    const auto result = compilation_service_.hover(document->text, document->uri, hover.position.line,
                                                   hover.position.character);
    if (!result) {
        return nullptr;
    }

    return jsonrpc::Json{{"contents", jsonrpc::Json{{"kind", "markdown"}, {"value", result->contents}}},
                         {"range", toRangeJson(result->range)}};
}

jsonrpc::Json ServerSession::handleDefinition(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/definition received before initialize");
    }

    const auto definition = lsp::parseDefinitionParams(params);
    const auto* document = document_store_.find(definition.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto identifier = compilation_service_.identifierAt(document->text, definition.position.line,
                                                              definition.position.character);
    if (!identifier) {
        return nullptr;
    }

    const auto semantic_definitions = semantic_workspace_.findDefinitionsAt(document->uri,
                                                                            definition.position.line,
                                                                            definition.position.character);
    if (!semantic_definitions.empty()) {
        jsonrpc::Json result = jsonrpc::Json::array();
        for (const auto& symbol : semantic_definitions) {
            result.push_back(toLocationJson(symbol.location));
        }
        return result;
    }

    const auto definitions = symbol_index_.definitions(identifier->name, document->uri);
    if (definitions.empty()) {
        return nullptr;
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : definitions) {
        result.push_back(toLocationJson(symbol.location));
    }

    return result;
}

jsonrpc::Json ServerSession::handleTypeDefinition(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/typeDefinition received before initialize");
    }

    const auto type_definition = lsp::parseTypeDefinitionParams(params);
    const auto* document = document_store_.find(type_definition.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto identifier = compilation_service_.identifierAt(document->text,
                                                              type_definition.position.line,
                                                              type_definition.position.character);
    if (!identifier) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : semantic_workspace_.findTypeDefinitionsAt(document->uri,
                                                                        type_definition.position.line,
                                                                        type_definition.position.character)) {
        result.push_back(toLocationJson(symbol.location));
    }
    if (!result.empty()) {
        return result;
    }

    for (const auto& symbol : symbol_index_.definitions(identifier->name, document->uri)) {
        if (isTypeDefinitionSymbol(symbol.kind)) {
            result.push_back(toLocationJson(symbol.location));
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleReferences(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/references received before initialize");
    }

    const auto references = lsp::parseReferenceParams(params);
    const auto* document = document_store_.find(references.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto identifier = compilation_service_.identifierAt(document->text, references.position.line,
                                                              references.position.character);
    if (!identifier) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    const auto engine_references = semantic_workspace_.engineReferencesAt(
        document->uri, references.position.line, references.position.character,
        references.context.include_declaration);
    const auto semantic_references = semantic_workspace_.findReferencesAt(
        document->uri, references.position.line, references.position.character,
        references.context.include_declaration);
    if (!engine_references.unresolved &&
        semantic_workspace_.findResolvedSymbolAt(document->uri, references.position.line,
                                                 references.position.character).has_value()) {
        for (const auto& reference : semantic_references) {
            result.push_back(toLocationJson(reference.location));
        }
        return result;
    }

    for (const auto& reference : symbol_index_.references(identifier->name,
                                                          references.context.include_declaration)) {
        result.push_back(toLocationJson(reference.location));
    }

    return result;
}

jsonrpc::Json ServerSession::handleImplementation(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/implementation received before initialize");
    }

    const auto implementation = lsp::parseImplementationParams(params);
    const auto* document = document_store_.find(implementation.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto identifier = compilation_service_.identifierAt(document->text,
                                                              implementation.position.line,
                                                              implementation.position.character);
    if (!identifier) {
        return jsonrpc::Json::array();
    }

    const auto definitions = sortedModuleDefinitions(hierarchy_documents_);
    const auto modules = buildModuleLookup(definitions);
    if (!modules.contains(identifier->name)) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& definition : definitions) {
        for (const auto& instance : definition.definition.instances) {
            if (instance.module_name == identifier->name) {
                result.push_back(toLocationJson(analysis::Location{.uri = definition.uri,
                                                                    .range = instance.module_selection_range}));
            }
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleDocumentHighlight(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/documentHighlight received before initialize");
    }

    const auto highlight = lsp::parseDocumentHighlightParams(params);
    const auto* document = document_store_.find(highlight.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto identifier = compilation_service_.identifierAt(document->text, highlight.position.line,
                                                              highlight.position.character);
    if (!identifier) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    const auto engine_highlights = semantic_workspace_.engineReferencesAt(
        document->uri, highlight.position.line, highlight.position.character, true);
    const auto semantic_references = semantic_workspace_.findDocumentReferencesAt(
        document->uri, highlight.position.line, highlight.position.character, true);
    if (!engine_highlights.unresolved &&
        semantic_workspace_.findResolvedSymbolAt(document->uri, highlight.position.line,
                                                 highlight.position.character).has_value()) {
        for (const auto& reference : semantic_references) {
            result.push_back(toDocumentHighlightJson(reference.location));
        }
        return result;
    }

    for (const auto& reference : symbol_index_.documentReferences(document->uri, identifier->name, true)) {
        result.push_back(toDocumentHighlightJson(reference.location));
    }

    return result;
}

jsonrpc::Json ServerSession::handleDocumentLink(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/documentLink received before initialize");
    }

    const auto links = lsp::parseDocumentLinkParams(params);
    const auto* document = document_store_.find(links.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& include : compilation_service_.includeDirectives(document->text)) {
        const auto target = resolveIncludeTarget(workspace_manager_, document->uri, include.target);
        if (!target.has_value()) {
            continue;
        }
        result.push_back(jsonrpc::Json{{"range", toRangeJson(include.range)},
                                       {"target", toFileUri(*target)}});
    }

    return result;
}

jsonrpc::Json ServerSession::handleInlayHint(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/inlayHint received before initialize");
    }

    const auto hints = lsp::parseInlayHintParams(params);
    const auto* document = document_store_.find(hints.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    const auto engine_hints = semantic_workspace_.engineInlayHints(
        document->uri,
        analysis::ParseRange{.start_line = hints.range.start.line,
                             .start_character = hints.range.start.character,
                             .end_line = hints.range.end.line,
                             .end_character = hints.range.end.character});
    if (!engine_hints.unresolved && !engine_hints.hints.empty()) {
        for (const auto& hint : engine_hints.hints) {
            if (hint.label.starts_with(": ")) {
                continue;
            }
            result.push_back(toInlayHintJson(hint));
        }
    }

    const auto module_lookup = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
    for (const auto& module : compilation_service_.moduleDefinitions(document->text, document->uri)) {
        for (const auto& instance : module.instances) {
            if (positionInRange(instance.selection_range.end_line,
                                instance.selection_range.end_character,
                                hints.range)) {
                const auto definition_it = module_lookup.find(instance.module_name);
                result.push_back(toInlayHintJson(instance, definition_it == module_lookup.end()
                                                              ? nullptr
                                                              : &definition_it->second.definition));
            }
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleCodeAction(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/codeAction received before initialize");
    }

    const auto action = lsp::parseCodeActionParams(params);
    const auto* document = document_store_.find(action.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& include : compilation_service_.includeDirectives(document->text)) {
        if (!parseRangeIntersects(include.range, action.range) ||
            resolveIncludeTarget(workspace_manager_, document->uri, include.target).has_value()) {
            continue;
        }

        const auto target = proposedIncludeTarget(workspace_manager_, document->uri, include.target);
        if (!target.has_value()) {
            continue;
        }

        jsonrpc::Json create_file{{"kind", "create"},
                                  {"uri", toFileUri(*target)},
                                  {"options", jsonrpc::Json{{"ignoreIfExists", true}}}};
        jsonrpc::Json edit{{"documentChanges", jsonrpc::Json::array({std::move(create_file)})}};
        result.push_back(jsonrpc::Json{
            {"title", std::string("Create include file '") + include.target + "'"},
            {"kind", "quickfix"},
            {"isPreferred", true},
            {"diagnostics", jsonrpc::Json::array({makeUnknownIncludeDiagnostic(include, server_name_)})},
            {"edit", std::move(edit)}});
    }

    try {
        const auto module_lookup = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
        const auto insert_range = endOfTextRange(document->text);
        for (const auto& module : compilation_service_.moduleDefinitions(document->text, document->uri)) {
            for (const auto& instance : module.instances) {
                if (!parseRangeIntersects(instance.module_selection_range, action.range) ||
                    module_lookup.contains(instance.module_name) || !isValidIdentifier(instance.module_name)) {
                    continue;
                }

                jsonrpc::Json edit{{"changes",
                                    jsonrpc::Json{{document->uri,
                                                   jsonrpc::Json::array({toTextEditJson(
                                                       insert_range,
                                                       moduleStubInsertionText(document->text,
                                                                               instance.module_name))})}}}};
                result.push_back(jsonrpc::Json{
                    {"title", std::string("Create stub module '") + instance.module_name + "'"},
                    {"kind", "quickfix"},
                    {"diagnostics",
                     jsonrpc::Json::array({makeUnresolvedModuleDiagnostic(instance, server_name_)})},
                    {"edit", std::move(edit)}});
            }
        }
    }
    catch (...) {
    }

    try {
        const auto module_lookup = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
        for (const auto& schematic : compilation_service_.moduleSchematics(document->text, document->uri)) {
            for (const auto& cell : schematic.cells) {
                if (cell.kind != "module" || !parseRangeIntersects(cell.range, action.range)) {
                    continue;
                }

                const auto module_it = module_lookup.find(cell.type);
                if (module_it == module_lookup.end()) {
                    continue;
                }

                const auto missing_ports = missingPorts(cell, module_it->second.definition);
                if (missing_ports.empty()) {
                    continue;
                }

                const auto insertion_range = instancePortInsertionRange(document->text, cell);
                if (!insertion_range.has_value()) {
                    continue;
                }

                jsonrpc::Json edit{{"changes",
                                    jsonrpc::Json{{document->uri,
                                                   jsonrpc::Json::array({toTextEditJson(
                                                       *insertion_range,
                                                       missingPortConnectionText(missing_ports,
                                                                                 !cell.connections.empty()))})}}}};
                result.push_back(jsonrpc::Json{
                    {"title", std::string("Add missing port connections to '") + cell.name + "'"},
                    {"kind", "quickfix"},
                    {"edit", std::move(edit)}});
            }
        }
    }
    catch (...) {
    }

    try {
        const auto insert_range = endOfTextRange(document->text);
        std::set<std::string> emitted_type_names;
        for (const auto& diagnostic : semantic_workspace_.diagnosticsFor(document->uri)) {
            if (diagnostic.code != kUnresolvedTypeDiagnosticCode ||
                !parseRangeIntersects(diagnostic.range, action.range)) {
                continue;
            }

            const auto type_name = textForParseRange(document->text, diagnostic.range);
            if (!type_name.has_value() || !isValidIdentifier(*type_name) ||
                !emitted_type_names.insert(*type_name).second) {
                continue;
            }

            jsonrpc::Json edit{{"changes",
                                jsonrpc::Json{{document->uri,
                                               jsonrpc::Json::array({toTextEditJson(
                                                   insert_range,
                                                   typedefSkeletonInsertionText(document->text,
                                                                               *type_name))})}}}};
            result.push_back(jsonrpc::Json{
                {"title", std::string("Create typedef '") + *type_name + "'"},
                {"kind", "quickfix"},
                {"diagnostics",
                 jsonrpc::Json::array({makeDiagnosticJson(diagnostic.range,
                                                          diagnostic.severity,
                                                          diagnostic.code,
                                                          server_name_,
                                                          diagnostic.message)})},
                {"edit", std::move(edit)}});
        }
    }
    catch (...) {
    }

    return result;
}

jsonrpc::Json ServerSession::handleFoldingRange(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/foldingRange received before initialize");
    }

    const auto folding_range = lsp::parseFoldingRangeParams(params);
    const auto* document = document_store_.find(folding_range.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    collectFoldingRanges(result, compilation_service_.documentSymbols(document->text, document->uri));
    return result;
}

jsonrpc::Json ServerSession::handleSemanticTokensFull(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/semanticTokens/full received before initialize");
    }

    const auto semantic_tokens = lsp::parseSemanticTokensParams(params);
    const auto* document = document_store_.find(semantic_tokens.text_document.uri);
    if (!document) {
        return jsonrpc::Json{{"data", jsonrpc::Json::array()}};
    }

    const auto engine_tokens = semantic_workspace_.engineSemanticTokens(document->uri);
    if (!engine_tokens.unresolved && !engine_tokens.tokens.empty()) {
        std::vector<SemanticToken> tokens;
        for (const auto& token : engine_tokens.tokens) {
            const auto token_type = semanticTokenTypeForName(token.token_type);
            if (!token_type.has_value() ||
                token.location.range.start_line != token.location.range.end_line ||
                token.location.range.end_character <= token.location.range.start_character) {
                continue;
            }
            tokens.push_back(SemanticToken{.line = token.location.range.start_line,
                                           .character = token.location.range.start_character,
                                           .length = token.location.range.end_character -
                                                     token.location.range.start_character,
                                           .type = *token_type});
        }
        if (!tokens.empty()) {
            return toSemanticTokensJson(std::move(tokens));
        }
    }

    std::vector<SemanticToken> tokens;
    collectSemanticTokens(tokens, compilation_service_.documentSymbols(document->text, document->uri));
    return toSemanticTokensJson(std::move(tokens));
}

jsonrpc::Json ServerSession::handleSelectionRange(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/selectionRange received before initialize");
    }

    const auto selection_range = lsp::parseSelectionRangeParams(params);
    const auto* document = document_store_.find(selection_range.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& position : selection_range.positions) {
        const auto engine_selection = semantic_workspace_.engineSelectionRangesAt(document->uri,
                                                                                  position.line,
                                                                                  position.character);
        if (!engine_selection.unresolved && hasStructuredSelectionParent(engine_selection.ranges)) {
            result.push_back(toSelectionRangeJson(engine_selection.ranges, 0));
            continue;
        }
        result.push_back(selectionRangeForPosition(compilation_service_, *document, position));
    }
    return result;
}

jsonrpc::Json ServerSession::handleSignatureHelp(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/signatureHelp received before initialize");
    }

    const auto signature_help = lsp::parseSignatureHelpParams(params);
    const auto* document = document_store_.find(signature_help.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto engine_help = semantic_workspace_.engineSignatureHelpAt(document->uri,
                                                                       signature_help.position.line,
                                                                       signature_help.position.character);
    if (!engine_help.unresolved && !engine_help.label.empty()) {
        return toSignatureHelpJson(engine_help);
    }

    const auto invocation = findSignatureInvocation(compilation_service_, *document, signature_help.position);
    if (!invocation.has_value()) {
        return nullptr;
    }

    const auto modules = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
    const auto module_it = modules.find(invocation->module_name);
    if (module_it == modules.end()) {
        return nullptr;
    }

    return toSignatureHelpJson(module_it->second.definition, invocation->active_parameter);
}

jsonrpc::Json ServerSession::handlePrepareCallHierarchy(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/prepareCallHierarchy received before initialize");
    }

    const auto prepare = lsp::parseCallHierarchyPrepareParams(params);
    const auto prepared = semantic_workspace_.enginePrepareCallHierarchy(prepare.text_document.uri,
                                                                         prepare.position.line,
                                                                         prepare.position.character);
    if (prepared.items.empty()) {
        return nullptr;
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& item : prepared.items) {
        result.push_back(toCallHierarchyItemJson(item));
    }
    return result;
}

jsonrpc::Json ServerSession::handleIncomingCalls(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("callHierarchy/incomingCalls received before initialize");
    }

    const auto calls = lsp::parseCallHierarchyCallsParams(params);
    const auto incoming = semantic_workspace_.engineIncomingCalls(toSemanticCallHierarchyItem(calls.item));

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& call : incoming.calls) {
        jsonrpc::Json from_ranges = jsonrpc::Json::array();
        for (const auto& range : call.from_ranges) {
            from_ranges.push_back(toRangeJson(range));
        }
        result.push_back(jsonrpc::Json{{"from", toCallHierarchyItemJson(call.item)},
                                       {"fromRanges", std::move(from_ranges)}});
    }

    return result;
}

jsonrpc::Json ServerSession::handleOutgoingCalls(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("callHierarchy/outgoingCalls received before initialize");
    }

    const auto calls = lsp::parseCallHierarchyCallsParams(params);
    const auto outgoing = semantic_workspace_.engineOutgoingCalls(toSemanticCallHierarchyItem(calls.item));
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& call : outgoing.calls) {
        jsonrpc::Json from_ranges = jsonrpc::Json::array();
        for (const auto& range : call.from_ranges) {
            from_ranges.push_back(toRangeJson(range));
        }
        result.push_back(jsonrpc::Json{{"to", toCallHierarchyItemJson(call.item)},
                                       {"fromRanges", std::move(from_ranges)}});
    }

    return result;
}

jsonrpc::Json ServerSession::handleWorkspaceSymbol(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("workspace/symbol received before initialize");
    }

    const auto workspace_symbol = lsp::parseWorkspaceSymbolParams(params);
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : symbol_index_.workspaceSymbols(workspace_symbol.query)) {
        result.push_back(jsonrpc::Json{{"name", symbol.name},
                                       {"kind", symbol.kind},
                                       {"location", toLocationJson(symbol.location)}});
    }

    return result;
}

jsonrpc::Json ServerSession::handleCompletion(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/completion received before initialize");
    }

    const auto completion = lsp::parseCompletionParams(params);
    const auto* document = document_store_.find(completion.text_document.uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    const auto prefix = compilation_service_.completionPrefix(document->text, completion.position.line,
                                                              completion.position.character);
    jsonrpc::Json result = jsonrpc::Json::array();
    std::set<std::string> emitted_labels;

    if (isMacroCompletionContext(document->text, completion.position, prefix)) {
        for (const auto& macro : symbol_index_.macroCompletions(prefix, document->uri)) {
            appendCompletionItem(result, emitted_labels, toMacroCompletionItem(macro));
        }
        if (result.empty()) {
            const auto engine_completions = semantic_workspace_.engineCompletionsAt(document->uri,
                                                                                    completion.position.line,
                                                                                    completion.position.character,
                                                                                    prefix);
            for (const auto& item : engine_completions.items) {
                appendCompletionItem(result, emitted_labels, toSemanticEngineCompletionItem(item));
            }
        }
        return result;
    }

    if (isModuleInstantiationCompletionContext(document->text, completion.position, prefix)) {
        for (const auto& symbol : semantic_workspace_.visibleSymbolsAt(document->uri,
                                                                       completion.position.line,
                                                                       completion.position.character,
                                                                       prefix)) {
            if (!isModuleLikeCompletionSymbol(symbol.kind)) {
                continue;
            }
            appendCompletionItem(result, emitted_labels, toSemanticCompletionItem(symbol));
        }

        for (const auto& item : symbol_index_.completions(prefix, document->uri)) {
            if (!isModuleLikeCompletionSymbol(item.kind)) {
                continue;
            }
            appendCompletionItem(result, emitted_labels, toIndexCompletionItem(item));
        }
    }

    if (isNamedPortCompletionContext(document->text, completion.position, prefix)) {
        const auto invocation = findSignatureInvocation(compilation_service_, *document, completion.position);
        if (invocation.has_value()) {
            const auto modules = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
            const auto module_it = modules.find(invocation->module_name);
            if (module_it != modules.end()) {
                const auto& module = module_it->second.definition;
                const auto connected_ports = connectedNamedPortsBeforePosition(document->text, *invocation);
                if (module.port_details.empty()) {
                    for (const auto& port_name : module.ports) {
                        if (!startsWithInsensitive(prefix, port_name) ||
                            connected_ports.contains(port_name)) {
                            continue;
                        }
                        analysis::SchematicPort port{.name = port_name,
                                                     .direction = {},
                                                     .width_text = {},
                                                     .range = module.selection_range,
                                                     .selection_range = module.selection_range};
                        appendCompletionItem(result, emitted_labels,
                                             toPortCompletionItem(module, port, module_it->second.uri));
                    }
                }
                else {
                    for (const auto& port : module.port_details) {
                        if (!startsWithInsensitive(prefix, port.name) ||
                            connected_ports.contains(port.name)) {
                            continue;
                        }
                        appendCompletionItem(result, emitted_labels,
                                             toPortCompletionItem(module, port, module_it->second.uri));
                    }
                }
            }
        }
        if (result.empty()) {
            const auto engine_completions = semantic_workspace_.engineCompletionsAt(document->uri,
                                                                                    completion.position.line,
                                                                                    completion.position.character,
                                                                                    prefix);
            for (const auto& item : engine_completions.items) {
                appendCompletionItem(result, emitted_labels, toSemanticEngineCompletionItem(item));
            }
        }
        return result;
    }

    if (const auto package_name = packageQualifierBeforeCompletion(document->text, completion.position, prefix)) {
        for (const auto& symbol : semantic_workspace_.packageMembersAt(document->uri,
                                                                       completion.position.line,
                                                                       completion.position.character,
                                                                       *package_name,
                                                                       prefix)) {
            appendCompletionItem(result, emitted_labels, toSemanticCompletionItem(symbol));
        }
        if (result.empty()) {
            const auto engine_completions = semantic_workspace_.engineCompletionsAt(document->uri,
                                                                                    completion.position.line,
                                                                                    completion.position.character,
                                                                                    prefix);
            for (const auto& item : engine_completions.items) {
                appendCompletionItem(result, emitted_labels, toSemanticEngineCompletionItem(item));
            }
        }
        return result;
    }

    for (const auto& symbol : semantic_workspace_.visibleSymbolsAt(document->uri, completion.position.line,
                                                                   completion.position.character, prefix)) {
        appendCompletionItem(result, emitted_labels, toSemanticCompletionItem(symbol));
    }

    for (const auto& item : symbol_index_.completions(prefix, document->uri)) {
        appendCompletionItem(result, emitted_labels, toIndexCompletionItem(item));
    }

    if (result.empty()) {
        const auto engine_completions = semantic_workspace_.engineCompletionsAt(document->uri,
                                                                                completion.position.line,
                                                                                completion.position.character,
                                                                                prefix);
        for (const auto& item : engine_completions.items) {
            appendCompletionItem(result, emitted_labels, toSemanticEngineCompletionItem(item));
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleCompletionItemResolve(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("completionItem/resolve received before initialize");
    }

    jsonrpc::Json item = params;
    const auto data_it = item.find("data");
    if (data_it == item.end() || !data_it->is_object()) {
        return item;
    }

    const auto source = jsonStringField(*data_it, "source");
    if (!source.has_value()) {
        return item;
    }

    if (*source == "semantic") {
        const auto symbol_id = jsonStringField(*data_it, "symbolId");
        if (!symbol_id.has_value()) {
            return item;
        }
        const auto symbol = semantic_workspace_.findSymbolById(*symbol_id);
        if (!symbol.has_value()) {
            return item;
        }

        const analysis::ModuleDefinition* module = nullptr;
        const auto modules = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
        const auto module_it = modules.find(symbol->name);
        if (symbol->kind == 2 && module_it != modules.end()) {
            module = &module_it->second.definition;
        }

        item["detail"] = richCompletionDetail(*symbol, module);
        item["documentation"] = markdownDocumentation(completionDocumentationForSymbol(*symbol, module));
        if (module) {
            item["insertText"] = moduleInstantiationSnippet(*module);
            item["insertTextFormat"] = 2;
        }
        return item;
    }

    if (*source == "semanticEngine") {
        const auto stable_id = jsonStringField(*data_it, "stableId");
        const auto label = jsonStringField(*data_it, "label").value_or(item.value("label", ""));
        if (!stable_id.has_value()) {
            return item;
        }
        const auto resolved = semantic_workspace_.engineResolveCompletion(*stable_id, label);
        if (resolved.unresolved) {
            return item;
        }
        if (!resolved.detail.empty()) {
            item["detail"] = resolved.detail;
        }
        if (!resolved.documentation.empty()) {
            item["documentation"] = markdownDocumentation(resolved.documentation);
        }
        if (!resolved.insert_text.empty() && resolved.insert_text != label) {
            item["insertText"] = resolved.insert_text;
            if (resolved.insert_text.find("${") != std::string::npos) {
                item["insertTextFormat"] = 2;
            }
        }
        return item;
    }

    if (*source == "index") {
        const auto label = jsonStringField(*data_it, "label");
        const auto uri = jsonStringField(*data_it, "uri");
        if (label.has_value() && uri.has_value()) {
            std::string documentation = "**Indexed Symbol** `";
            documentation += *label;
            documentation += "`";
            documentation += "\n\nDeclared: `" + *uri + "`";
            item["documentation"] = markdownDocumentation(std::move(documentation));
        }
        return item;
    }

    if (*source == "port") {
        const auto module_name = jsonStringField(*data_it, "moduleName");
        const auto port_name = jsonStringField(*data_it, "portName");
        if (!module_name.has_value() || !port_name.has_value()) {
            return item;
        }

        const auto modules = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
        const auto module_it = modules.find(*module_name);
        if (module_it == modules.end()) {
            return item;
        }

        const auto& module = module_it->second.definition;
        if (module.port_details.empty()) {
            for (const auto& fallback_port_name : module.ports) {
                if (fallback_port_name != *port_name) {
                    continue;
                }
                analysis::SchematicPort port{.name = fallback_port_name,
                                             .direction = {},
                                             .width_text = {},
                                             .range = module.selection_range,
                                             .selection_range = module.selection_range};
                item["detail"] = portCompletionDetail(port);
                item["documentation"] = markdownDocumentation(
                    completionDocumentationForPort(module, port, module_it->second.uri));
                item["insertText"] = portConnectionSnippet(port);
                item["insertTextFormat"] = 2;
                return item;
            }
        }
        for (const auto& port : module.port_details) {
            if (port.name != *port_name) {
                continue;
            }
            item["detail"] = portCompletionDetail(port);
            item["documentation"] = markdownDocumentation(
                completionDocumentationForPort(module, port, module_it->second.uri));
            item["insertText"] = portConnectionSnippet(port);
            item["insertTextFormat"] = 2;
            return item;
        }
    }

    if (*source == "macro") {
        const auto macro = macroEntryFromCompletionData(*data_it);
        if (!macro.has_value()) {
            return item;
        }
        item["detail"] = macro->function_like ? "Macro function " + macroSignatureLabel(*macro)
                                                : "Macro";
        item["documentation"] = markdownDocumentation(completionDocumentationForMacro(*macro));
        item["insertText"] = macroInsertText(*macro);
        if (macro->function_like) {
            item["insertTextFormat"] = 2;
        }
        return item;
    }

    return item;
}

jsonrpc::Json ServerSession::handlePrepareRename(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/prepareRename received before initialize");
    }

    const auto prepare = lsp::parsePrepareRenameParams(params);
    const auto* document = document_store_.find(prepare.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto identifier = compilation_service_.identifierAt(document->text, prepare.position.line,
                                                              prepare.position.character);
    if (!identifier) {
        return nullptr;
    }

    const auto engine_prepare = semantic_workspace_.enginePrepareRenameAt(document->uri,
                                                                          prepare.position.line,
                                                                          prepare.position.character);
    if (!engine_prepare.unresolved &&
        semantic_workspace_.findResolvedSymbolAt(document->uri, prepare.position.line,
                                                 prepare.position.character).has_value()) {
        return jsonrpc::Json{{"range", toRangeJson(engine_prepare.range)},
                             {"placeholder", engine_prepare.placeholder}};
    }

    const auto definitions = symbol_index_.definitions(identifier->name, document->uri);
    if (definitions.empty() || symbol_index_.hasAmbiguousDefinitions(identifier->name, document->uri)) {
        return nullptr;
    }

    return jsonrpc::Json{{"range", toRangeJson(identifier->range)}, {"placeholder", identifier->name}};
}

jsonrpc::Json ServerSession::handleRename(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/rename received before initialize");
    }

    const auto rename = lsp::parseRenameParams(params);
    if (!isValidIdentifier(rename.new_name)) {
        return nullptr;
    }

    const auto* document = document_store_.find(rename.text_document.uri);
    if (!document) {
        return nullptr;
    }

    const auto identifier = compilation_service_.identifierAt(document->text, rename.position.line,
                                                              rename.position.character);
    if (!identifier) {
        return nullptr;
    }

    const auto engine_rename = semantic_workspace_.engineRenameAt(document->uri,
                                                                  rename.position.line,
                                                                  rename.position.character,
                                                                  rename.new_name);
    if (!engine_rename.unresolved &&
        semantic_workspace_.findResolvedSymbolAt(document->uri, rename.position.line,
                                                 rename.position.character).has_value()) {
        std::map<std::string, jsonrpc::Json> changes;
        for (const auto& reference : semantic_workspace_.findReferencesAt(document->uri,
                                                                          rename.position.line,
                                                                          rename.position.character,
                                                                          true)) {
            auto [entry_it, inserted] = changes.try_emplace(reference.location.uri, jsonrpc::Json::array());
            entry_it->second.push_back(toTextEditJson(reference.location.range, rename.new_name));
        }

        if (changes.empty()) {
            return nullptr;
        }

        jsonrpc::Json changes_json = jsonrpc::Json::object();
        for (auto& [uri, edits] : changes) {
            changes_json[uri] = std::move(edits);
        }

        return jsonrpc::Json{{"changes", std::move(changes_json)}};
    }

    const auto definitions = symbol_index_.definitions(identifier->name, document->uri);
    if (definitions.empty() || symbol_index_.hasAmbiguousDefinitions(identifier->name, document->uri)) {
        return nullptr;
    }

    std::map<std::string, jsonrpc::Json> changes;
    for (const auto& reference : symbol_index_.references(identifier->name, true)) {
        auto [entry_it, inserted] = changes.try_emplace(reference.location.uri, jsonrpc::Json::array());
        entry_it->second.push_back(toTextEditJson(reference.location.range, rename.new_name));
    }

    if (changes.empty()) {
        return nullptr;
    }

    jsonrpc::Json changes_json = jsonrpc::Json::object();
    for (auto& [uri, edits] : changes) {
        changes_json[uri] = std::move(edits);
    }

    return jsonrpc::Json{{"changes", std::move(changes_json)}};
}

jsonrpc::Json ServerSession::handleShutdown(const jsonrpc::Json&) {
    shutdown_requested_ = true;
    return nullptr;
}

void ServerSession::handleInitialized(const jsonrpc::Json&) {}

void ServerSession::handleDidOpen(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didOpen received before initialize");
    }

    const auto did_open = lsp::parseDidOpenTextDocumentParams(params);
    document_store_.open(did_open);
    updateSymbolIndex(did_open.text_document.uri, did_open.text_document.text,
                      analysis::SemanticDocumentState{.version = did_open.text_document.version,
                                                      .is_open = true,
                                                      .dirty = false,
                                                      .invalidate_dependents = true});
    publishDiagnostics(did_open.text_document.uri);
}

void ServerSession::handleDidChange(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didChange received before initialize");
    }

    const auto did_change = lsp::parseDidChangeTextDocumentParams(params);
    document_store_.applyChanges(did_change);
    if (const auto* document = document_store_.find(did_change.text_document.uri)) {
        updateSymbolIndex(document->uri, document->text,
                          analysis::SemanticDocumentState{.version = document->version,
                                                          .is_open = true,
                                                          .dirty = document->dirty,
                                                          .invalidate_dependents = true});
    }
    publishDiagnostics(did_change.text_document.uri);
}

void ServerSession::handleDidSave(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didSave received before initialize");
    }

    const auto did_save = lsp::parseDidSaveTextDocumentParams(params);
    document_store_.save(did_save);
    if (const auto* document = document_store_.find(did_save.text_document.uri)) {
        updateSymbolIndex(document->uri, document->text,
                          analysis::SemanticDocumentState{.version = document->version,
                                                          .is_open = true,
                                                          .dirty = document->dirty,
                                                          .invalidate_dependents = true});
    }
    publishDiagnostics(did_save.text_document.uri);
}

void ServerSession::handleDidClose(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didClose received before initialize");
    }

    const auto did_close = lsp::parseDidCloseTextDocumentParams(params);
    clearDiagnostics(did_close.text_document.uri);
    document_store_.close(did_close);
    restoreClosedDocumentIndex(did_close.text_document.uri);
}

void ServerSession::handleDidChangeWatchedFiles(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("workspace/didChangeWatchedFiles received before initialize");
    }

    const auto watched_files = lsp::parseDidChangeWatchedFilesParams(params);
    for (const auto& change : watched_files.changes) {
        const auto path = workspace::WorkspaceManager::pathFromFileUri(change.uri);
        if (!path.has_value() || !isIndexableSourcePath(*path)) {
            continue;
        }

        if (change.type == lsp::FileChangeType::Deleted) {
            removeDocumentIndexes(change.uri);
            continue;
        }

        if (const auto* document = document_store_.find(change.uri)) {
            updateSymbolIndex(document->uri, document->text,
                              analysis::SemanticDocumentState{.version = document->version,
                                                              .is_open = true,
                                                              .dirty = document->dirty,
                                                              .invalidate_dependents = true});
            continue;
        }

        std::error_code error;
        if (!fs::exists(*path, error) || !fs::is_regular_file(*path, error)) {
            removeDocumentIndexes(change.uri);
            continue;
        }

        const auto text = readFileText(*path);
        if (!text.has_value()) {
            removeDocumentIndexes(change.uri);
            continue;
        }

        updateSymbolIndex(change.uri, *text,
                          analysis::SemanticDocumentState{.version = -1,
                                                          .is_open = false,
                                                          .dirty = false,
                                                          .invalidate_dependents = true});
    }
}

void ServerSession::handleExit(const jsonrpc::Json&) {
    if (!server_) {
        return;
    }

    server_->requestStop(shutdown_requested_ ? 0 : 1);
}

void ServerSession::indexWorkspaceSources() {
    for (const auto& path : workspace_manager_.sourceFilesForIndex()) {
        const auto text = readFileText(path);
        if (!text.has_value()) {
            continue;
        }
        updateSymbolIndex(toFileUri(path), *text);
    }
}

void ServerSession::updateSymbolIndex(std::string_view uri,
                                      std::string_view text,
                                      analysis::SemanticDocumentState semantic_state) {
    semantic_workspace_.updateDocument(uri, text, semantic_state);
    symbol_index_.updateDocument(uri, text);
    updateHierarchyIndex(uri, text);
}

void ServerSession::updateHierarchyIndex(std::string_view uri, std::string_view text) {
    std::vector<analysis::ModuleDefinition> definitions;
    try {
        definitions = compilation_service_.moduleDefinitions(text, uri);
    }
    catch (...) {
        definitions.clear();
    }

    hierarchy_documents_.insert_or_assign(std::string(uri), std::move(definitions));
}

void ServerSession::restoreClosedDocumentIndex(std::string_view uri) {
    const auto path = workspace::WorkspaceManager::pathFromFileUri(uri);
    if (!path.has_value()) {
        removeDocumentIndexes(uri);
        return;
    }

    std::error_code error;
    if (!fs::exists(*path, error) || !fs::is_regular_file(*path, error)) {
        removeDocumentIndexes(uri);
        return;
    }

    const auto text = readFileText(*path);
    if (!text.has_value()) {
        removeDocumentIndexes(uri);
        return;
    }

    updateSymbolIndex(uri, *text,
                      analysis::SemanticDocumentState{.version = -1,
                                                      .is_open = false,
                                                      .dirty = false,
                                                      .invalidate_dependents = true});
}

void ServerSession::removeDocumentIndexes(std::string_view uri) {
    semantic_workspace_.removeDocument(uri);
    symbol_index_.removeDocument(uri);
    hierarchy_documents_.erase(std::string(uri));
}

void ServerSession::publishDiagnostics(std::string_view uri) {
    if (!server_) {
        return;
    }

    const auto* document = document_store_.find(uri);
    if (!document) {
        return;
    }

    jsonrpc::Json diagnostics = jsonrpc::Json::array();
    try {
        const auto parse_result = compilation_service_.parse(document->text, document->uri);
        for (const auto& diagnostic : parse_result.diagnostics) {
            diagnostics.push_back(makeDiagnosticJson(diagnostic.range,
                                                     diagnostic.severity,
                                                     diagnostic.code,
                                                     server_name_,
                                                     diagnostic.message));
        }
    }
    catch (...) {
    }

    try {
        for (const auto& include : compilation_service_.includeDirectives(document->text)) {
            if (resolveIncludeTarget(workspace_manager_, document->uri, include.target).has_value()) {
                continue;
            }
            diagnostics.push_back(makeUnknownIncludeDiagnostic(include, server_name_));
        }
    }
    catch (...) {
    }

    try {
        const auto module_lookup = buildModuleLookup(sortedModuleDefinitions(hierarchy_documents_));
        for (const auto& module : compilation_service_.moduleDefinitions(document->text, document->uri)) {
            for (const auto& instance : module.instances) {
                if (module_lookup.contains(instance.module_name)) {
                    continue;
                }
                diagnostics.push_back(makeUnresolvedModuleDiagnostic(instance, server_name_));
            }
        }
    }
    catch (...) {
    }

    for (const auto& diagnostic : semantic_workspace_.diagnosticsFor(document->uri)) {
        diagnostics.push_back(makeDiagnosticJson(diagnostic.range,
                                                 diagnostic.severity,
                                                 diagnostic.code,
                                                 server_name_,
                                                 diagnostic.message));
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", document->uri},
                                            {"diagnostics", std::move(diagnostics)}});
}

void ServerSession::clearDiagnostics(std::string_view uri) {
    if (!server_) {
        return;
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", std::string(uri)},
                                            {"diagnostics", jsonrpc::Json::array()}});
}

} // namespace pristine::server
