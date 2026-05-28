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

jsonrpc::Json toTextEditJson(const analysis::ParseRange& range, std::string_view new_text) {
    return jsonrpc::Json{{"range", toRangeJson(range)}, {"newText", new_text}};
}

jsonrpc::Json toPositionJson(int line, int character) {
    return jsonrpc::Json{{"line", line}, {"character", character}};
}

jsonrpc::Json toDocumentHighlightJson(const analysis::Location& location) {
    return jsonrpc::Json{{"range", toRangeJson(location.range)}, {"kind", 1}};
}

jsonrpc::Json toInlayHintJson(const analysis::ModuleInstantiation& instance) {
    return jsonrpc::Json{{"position", toPositionJson(instance.selection_range.end_line,
                                                      instance.selection_range.end_character)},
                         {"label", std::string(": ") + instance.module_name},
                         {"kind", 1}};
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
                                                                            *position_offset)};
        }
    }

    return std::nullopt;
}

std::string signatureLabel(const analysis::ModuleDefinition& module) {
    std::string label = module.name + "(";
    for (size_t index = 0; index < module.ports.size(); ++index) {
        if (index != 0) {
            label += ", ";
        }
        label += module.ports[index];
    }
    label += ")";
    return label;
}

jsonrpc::Json toSignatureHelpJson(const analysis::ModuleDefinition& module, int active_parameter) {
    jsonrpc::Json parameters = jsonrpc::Json::array();
    for (const auto& port : module.ports) {
        parameters.push_back(jsonrpc::Json{{"label", port}});
    }

    const auto bounded_parameter = module.ports.empty()
        ? 0
        : std::min(active_parameter, static_cast<int>(module.ports.size()) - 1);

    return jsonrpc::Json{{"signatures",
                          jsonrpc::Json::array({jsonrpc::Json{{"label", signatureLabel(module)},
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

struct IndexedModuleSchematic {
    std::string uri;
    analysis::ModuleSchematic schematic;
};

bool sameRange(const analysis::ParseRange& lhs, const lsp::Range& rhs) {
    return lhs.start_line == rhs.start.line && lhs.start_character == rhs.start.character &&
           lhs.end_line == rhs.end.line && lhs.end_character == rhs.end.character;
}

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

std::vector<IndexedModuleSchematic> sortedModuleSchematics(
    const std::unordered_map<std::string, std::vector<analysis::ModuleSchematic>>& documents) {
    std::vector<IndexedModuleSchematic> result;
    for (const auto& [uri, schematics] : documents) {
        for (const auto& schematic : schematics) {
            result.push_back(IndexedModuleSchematic{.uri = uri, .schematic = schematic});
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.schematic.name != rhs.schematic.name) {
            return lhs.schematic.name < rhs.schematic.name;
        }
        if (lhs.uri != rhs.uri) {
            return lhs.uri < rhs.uri;
        }
        return lhs.schematic.range.start_line < rhs.schematic.range.start_line;
    });

    return result;
}

std::map<std::string, IndexedModuleSchematic> buildSchematicLookup(
    const std::vector<IndexedModuleSchematic>& schematics) {
    std::map<std::string, IndexedModuleSchematic> modules;
    for (const auto& schematic : schematics) {
        modules.try_emplace(schematic.schematic.name, schematic);
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

const analysis::SchematicPort* findSchematicPort(const analysis::ModuleSchematic& schematic,
                                                 std::string_view name) {
    const auto found = std::find_if(schematic.ports.begin(), schematic.ports.end(), [&](const auto& port) {
        return port.name == name;
    });
    return found == schematic.ports.end() ? nullptr : &*found;
}

const analysis::SchematicPort* findSchematicPortByIndex(const analysis::ModuleSchematic& schematic,
                                                        int index) {
    if (index < 0 || static_cast<size_t>(index) >= schematic.ports.size()) {
        return nullptr;
    }
    return &schematic.ports[static_cast<size_t>(index)];
}

jsonrpc::Json makeSchematicEndpoint(std::string node_id, std::string port_name) {
    return jsonrpc::Json{{"nodeId", std::move(node_id)}, {"portName", std::move(port_name)}};
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void appendEndpointByDirection(jsonrpc::Json& net,
                               std::string direction,
                               jsonrpc::Json endpoint,
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
        net["drivers"].push_back(std::move(endpoint));
        return;
    }
    if (direction == "input") {
        net["loads"].push_back(std::move(endpoint));
        return;
    }

    net["drivers"].push_back(endpoint);
    net["loads"].push_back(std::move(endpoint));
}

bool isLogicOutputPort(std::string_view port_name) {
    const auto normalized = lowerAscii(std::string(port_name));
    return normalized == "y" || normalized == "out" || normalized == "o" || normalized == "q";
}

jsonrpc::Json buildSchematicNetsJson(
    const analysis::ModuleSchematic& schematic,
    const std::map<std::string, IndexedModuleSchematic>& modules) {
    std::map<std::string, jsonrpc::Json> nets;
    const auto ensure_net = [&](std::string_view signal) -> jsonrpc::Json& {
        auto [it, inserted] = nets.try_emplace(std::string(signal),
                                               jsonrpc::Json{{"name", std::string(signal)},
                                                             {"drivers", jsonrpc::Json::array()},
                                                             {"loads", jsonrpc::Json::array()}});
        (void)inserted;
        return it->second;
    };

    for (const auto& port : schematic.ports) {
        if (port.name.empty()) {
            continue;
        }
        auto& net = ensure_net(port.name);
        appendEndpointByDirection(net, port.direction,
                                  makeSchematicEndpoint(std::string("$port:") + port.name, port.name), true);
    }

    for (const auto& cell : schematic.cells) {
        const auto target_it = cell.kind == "module" ? modules.find(cell.type) : modules.end();
        for (const auto& connection : cell.connections) {
            if (connection.signal.empty()) {
                continue;
            }

            std::string port_name = connection.port_name;
            std::string direction;
            if (target_it != modules.end()) {
                const auto* port = !port_name.empty()
                                       ? findSchematicPort(target_it->second.schematic, port_name)
                                       : findSchematicPortByIndex(target_it->second.schematic,
                                                                  connection.port_index);
                if (port) {
                    port_name = port->name;
                    direction = port->direction;
                }
            }

            if (port_name.empty() && connection.port_index >= 0) {
                port_name = std::to_string(connection.port_index);
            }
            if (direction.empty()) {
                direction = isLogicOutputPort(port_name) ? "output" : "input";
            }

            auto& net = ensure_net(connection.signal);
            appendEndpointByDirection(net, direction, makeSchematicEndpoint(cell.id, port_name));
        }
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (auto& [_, net] : nets) {
        result.push_back(std::move(net));
    }
    return result;
}

jsonrpc::Json toSchematicModuleJson(const IndexedModuleSchematic& indexed,
                                    const std::map<std::string, IndexedModuleSchematic>& modules) {
    jsonrpc::Json ports = jsonrpc::Json::array();
    for (const auto& port : indexed.schematic.ports) {
        ports.push_back(toSchematicPortJson(port));
    }

    jsonrpc::Json cells = jsonrpc::Json::array();
    for (const auto& cell : indexed.schematic.cells) {
        cells.push_back(toSchematicCellJson(cell));
    }

    return jsonrpc::Json{{"id", indexed.schematic.name},
                         {"name", indexed.schematic.name},
                         {"uri", indexed.uri},
                         {"range", toRangeJson(indexed.schematic.range)},
                         {"selectionRange", toRangeJson(indexed.schematic.selection_range)},
                         {"ports", std::move(ports)},
                         {"cells", std::move(cells)},
                         {"nets", buildSchematicNetsJson(indexed.schematic, modules)}};
}

jsonrpc::Json toCallHierarchyItemJson(const IndexedModuleDefinition& module) {
    return jsonrpc::Json{{"name", module.definition.name},
                         {"kind", module.definition.kind == "interface" ? 11 : 2},
                         {"detail", module.definition.kind},
                         {"uri", module.uri},
                         {"range", toRangeJson(module.definition.range)},
                         {"selectionRange", toRangeJson(module.definition.selection_range)}};
}

std::optional<IndexedModuleDefinition> findCallHierarchyModule(
    const std::vector<IndexedModuleDefinition>& definitions,
    const lsp::CallHierarchyItem& item) {
    for (const auto& definition : definitions) {
        if (definition.definition.name == item.name && definition.uri == item.uri &&
            sameRange(definition.definition.selection_range, item.selection_range)) {
            return definition;
        }
    }
    return std::nullopt;
}

std::optional<IndexedModuleDefinition> findCallHierarchyModuleAt(
    const std::vector<IndexedModuleDefinition>& definitions,
    std::string_view uri,
    const lsp::Position& position) {
    const auto modules = buildModuleLookup(definitions);
    for (const auto& definition : definitions) {
        if (definition.uri != uri) {
            continue;
        }
        if (parseRangeContainsPosition(definition.definition.selection_range, position)) {
            return definition;
        }
        for (const auto& instance : definition.definition.instances) {
            if (!parseRangeContainsPosition(instance.module_selection_range, position) &&
                !parseRangeContainsPosition(instance.selection_range, position)) {
                continue;
            }
            const auto target_it = modules.find(instance.module_name);
            if (target_it != modules.end()) {
                return target_it->second;
            }
        }
    }
    return std::nullopt;
}

jsonrpc::Json makeUnresolvedHierarchyNode(const analysis::ModuleInstantiation& instance) {
    return jsonrpc::Json{{"moduleName", instance.module_name},
                         {"kind", "module"},
                         {"instanceName", instance.instance_name},
                         {"uri", nullptr},
                         {"range", nullptr},
                         {"selectionRange", nullptr},
                         {"instanceRange", toRangeJson(instance.range)},
                         {"instanceSelectionRange", toRangeJson(instance.selection_range)},
                         {"moduleSelectionRange", toRangeJson(instance.module_selection_range)},
                         {"unresolved", true},
                         {"cycle", false},
                         {"children", jsonrpc::Json::array()}};
}

jsonrpc::Json buildHierarchyNode(const std::map<std::string, IndexedModuleDefinition>& modules,
                                 std::string_view module_name,
                                 const analysis::ModuleInstantiation* instance,
                                 std::vector<std::string>& stack,
                                 int depth,
                                 int max_depth) {
    const auto definition_it = modules.find(std::string(module_name));
    if (definition_it == modules.end()) {
        if (instance) {
            return makeUnresolvedHierarchyNode(*instance);
        }

        return jsonrpc::Json{{"moduleName", std::string(module_name)},
                             {"kind", "module"},
                             {"uri", nullptr},
                             {"range", nullptr},
                             {"selectionRange", nullptr},
                             {"unresolved", true},
                             {"cycle", false},
                             {"children", jsonrpc::Json::array()}};
    }

    const auto& indexed_definition = definition_it->second;
    const auto& definition = indexed_definition.definition;
    const auto is_cycle = std::find(stack.begin(), stack.end(), definition.name) != stack.end();

    jsonrpc::Json node{{"moduleName", definition.name},
                       {"kind", definition.kind},
                       {"uri", indexed_definition.uri},
                       {"range", toRangeJson(definition.range)},
                       {"selectionRange", toRangeJson(definition.selection_range)},
                       {"unresolved", false},
                       {"cycle", is_cycle},
                       {"children", jsonrpc::Json::array()}};

    if (instance) {
        node["instanceName"] = instance->instance_name;
        node["instanceRange"] = toRangeJson(instance->range);
        node["instanceSelectionRange"] = toRangeJson(instance->selection_range);
        node["moduleSelectionRange"] = toRangeJson(instance->module_selection_range);
    }

    if (is_cycle || depth >= max_depth) {
        if (depth >= max_depth) {
            node["truncated"] = true;
        }
        return node;
    }

    stack.push_back(definition.name);
    for (const auto& child_instance : definition.instances) {
        node["children"].push_back(buildHierarchyNode(modules, child_instance.module_name,
                                                       &child_instance, stack, depth + 1, max_depth));
    }
    stack.pop_back();

    return node;
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

std::optional<std::string> inferRootModuleName(
    const std::map<std::string, IndexedModuleDefinition>& modules,
    const std::vector<IndexedModuleDefinition>& definitions,
    jsonrpc::Json& messages) {
    std::set<std::string> instantiated_modules;
    for (const auto& definition : definitions) {
        for (const auto& instance : definition.definition.instances) {
            instantiated_modules.insert(instance.module_name);
        }
    }

    for (const auto& [name, _] : modules) {
        if (!instantiated_modules.contains(name)) {
            return name;
        }
    }

    if (!modules.empty()) {
        messages.push_back("No uninstantiated top module could be inferred for this workspace.");
        return modules.begin()->first;
    }

    return std::nullopt;
}

void collectReachableSchematicModules(
    jsonrpc::Json& result,
    jsonrpc::Json& messages,
    const std::map<std::string, IndexedModuleDefinition>& definitions,
    const std::map<std::string, IndexedModuleSchematic>& schematics,
    std::set<std::string>& emitted,
    std::vector<std::string>& stack,
    std::string_view module_name,
    int depth,
    int max_depth) {
    if (emitted.contains(std::string(module_name))) {
        return;
    }

    const auto schematic_it = schematics.find(std::string(module_name));
    if (schematic_it == schematics.end()) {
        messages.push_back(std::string("No schematic data found for module '") + std::string(module_name) + "'.");
        return;
    }

    emitted.insert(std::string(module_name));
    result.push_back(toSchematicModuleJson(schematic_it->second, schematics));

    if (depth >= max_depth || std::find(stack.begin(), stack.end(), module_name) != stack.end()) {
        return;
    }

    const auto definition_it = definitions.find(std::string(module_name));
    if (definition_it == definitions.end()) {
        return;
    }

    stack.push_back(std::string(module_name));
    for (const auto& instance : definition_it->second.definition.instances) {
        collectReachableSchematicModules(result, messages, definitions, schematics, emitted, stack,
                                         instance.module_name, depth + 1, max_depth);
    }
    stack.pop_back();
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
    if (const auto& root_path = workspace_manager_.state().root_path) {
        semantic_workspace_.setWorkspaceRoot(toFileUri(*root_path));
    }
    else {
        semantic_workspace_.setWorkspaceRoot({});
    }
    symbol_index_.clear();
    hierarchy_documents_.clear();
    schematic_documents_.clear();
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

    const auto sorted_definitions = sortedModuleDefinitions(hierarchy_documents_);
    auto modules = buildModuleLookup(sorted_definitions);
    std::set<std::string> instantiated_modules;
    for (const auto& definition : sorted_definitions) {
        for (const auto& instance : definition.definition.instances) {
            instantiated_modules.insert(instance.module_name);
        }
    }

    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);
    jsonrpc::Json roots = jsonrpc::Json::array();
    jsonrpc::Json messages = jsonrpc::Json::array();

    std::vector<std::string> root_names;
    if (requested_module_name.has_value()) {
        root_names.push_back(*requested_module_name);
    }
    else {
        for (const auto& module : modules) {
            if (!instantiated_modules.contains(module.first)) {
                root_names.push_back(module.first);
            }
        }
    }

    if (root_names.empty() && !modules.empty()) {
        messages.push_back("No uninstantiated top module could be inferred for this workspace.");
    }

    for (const auto& root_name : root_names) {
        std::vector<std::string> stack;
        roots.push_back(buildHierarchyNode(modules, root_name, nullptr, stack, 0, max_depth));
    }

    return jsonrpc::Json{{"roots", std::move(roots)}, {"messages", std::move(messages)}};
}

jsonrpc::Json ServerSession::handleSchematic(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/schematic received before initialize");
    }

    const auto sorted_definitions = sortedModuleDefinitions(hierarchy_documents_);
    const auto definition_lookup = buildModuleLookup(sorted_definitions);
    const auto schematic_lookup = buildSchematicLookup(sortedModuleSchematics(schematic_documents_));
    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);

    jsonrpc::Json messages = jsonrpc::Json::array();
    const auto root_module_name = requested_module_name.has_value()
                                      ? requested_module_name
                                      : inferRootModuleName(definition_lookup, sorted_definitions, messages);
    jsonrpc::Json modules = jsonrpc::Json::array();
    if (!root_module_name.has_value()) {
        return jsonrpc::Json{{"rootModuleId", nullptr},
                             {"modules", std::move(modules)},
                             {"messages", std::move(messages)}};
    }

    std::set<std::string> emitted;
    std::vector<std::string> stack;
    collectReachableSchematicModules(modules, messages, definition_lookup, schematic_lookup, emitted, stack,
                                     *root_module_name, 0, max_depth);

    return jsonrpc::Json{{"rootModuleId", *root_module_name},
                         {"modules", std::move(modules)},
                         {"messages", std::move(messages)}};
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

    const auto semantic_definitions = semantic_workspace_.definitionsAt(document->uri,
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
    for (const auto& symbol : semantic_workspace_.definitionsAt(document->uri,
                                                                type_definition.position.line,
                                                                type_definition.position.character)) {
        if (isTypeDefinitionSymbol(symbol.kind)) {
            result.push_back(toLocationJson(symbol.location));
        }
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
    const auto semantic_references = semantic_workspace_.referencesAt(
        document->uri, references.position.line, references.position.character,
        references.context.include_declaration);
    if (semantic_workspace_.resolvedSymbolAt(document->uri, references.position.line,
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
    const auto semantic_references = semantic_workspace_.documentReferencesAt(
        document->uri, highlight.position.line, highlight.position.character, true);
    if (semantic_workspace_.resolvedSymbolAt(document->uri, highlight.position.line,
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
    for (const auto& module : compilation_service_.moduleDefinitions(document->text, document->uri)) {
        for (const auto& instance : module.instances) {
            if (positionInRange(instance.selection_range.end_line,
                                instance.selection_range.end_character,
                                hints.range)) {
                result.push_back(toInlayHintJson(instance));
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
            {"edit", std::move(edit)}});
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
    const auto definitions = sortedModuleDefinitions(hierarchy_documents_);
    const auto module = findCallHierarchyModuleAt(definitions, prepare.text_document.uri, prepare.position);
    if (!module.has_value()) {
        return nullptr;
    }

    return jsonrpc::Json::array({toCallHierarchyItemJson(*module)});
}

jsonrpc::Json ServerSession::handleIncomingCalls(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("callHierarchy/incomingCalls received before initialize");
    }

    const auto calls = lsp::parseCallHierarchyCallsParams(params);
    const auto definitions = sortedModuleDefinitions(hierarchy_documents_);
    const auto target = findCallHierarchyModule(definitions, calls.item);
    if (!target.has_value()) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& caller : definitions) {
        for (const auto& instance : caller.definition.instances) {
            if (instance.module_name == target->definition.name) {
                result.push_back(jsonrpc::Json{{"from", toCallHierarchyItemJson(caller)},
                                               {"fromRanges", jsonrpc::Json::array(
                                                                  {toRangeJson(instance.module_selection_range)})}});
            }
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleOutgoingCalls(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("callHierarchy/outgoingCalls received before initialize");
    }

    const auto calls = lsp::parseCallHierarchyCallsParams(params);
    const auto definitions = sortedModuleDefinitions(hierarchy_documents_);
    const auto source = findCallHierarchyModule(definitions, calls.item);
    if (!source.has_value()) {
        return jsonrpc::Json::array();
    }

    const auto modules = buildModuleLookup(definitions);
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& instance : source->definition.instances) {
        const auto target_it = modules.find(instance.module_name);
        if (target_it == modules.end()) {
            continue;
        }
        result.push_back(jsonrpc::Json{{"to", toCallHierarchyItemJson(target_it->second)},
                                       {"fromRanges", jsonrpc::Json::array(
                                                          {toRangeJson(instance.module_selection_range)})}});
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
    for (const auto& symbol : semantic_workspace_.visibleSymbolsAt(document->uri, completion.position.line,
                                                                   completion.position.character, prefix)) {
        if (!emitted_labels.insert(symbol.name).second) {
            continue;
        }
        result.push_back(jsonrpc::Json{{"label", symbol.name},
                                       {"kind", toCompletionItemKind(symbol.kind)},
                                       {"detail", completionDetailForSymbolKind(symbol.kind)}});
    }

    for (const auto& item : symbol_index_.completions(prefix, document->uri)) {
        if (!emitted_labels.insert(item.label).second) {
            continue;
        }
        result.push_back(jsonrpc::Json{{"label", item.label},
                                       {"kind", toCompletionItemKind(item.kind)},
                                       {"detail", item.detail}});
    }

    return result;
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

    if (semantic_workspace_.resolvedSymbolAt(document->uri, prepare.position.line,
                                             prepare.position.character).has_value()) {
        return jsonrpc::Json{{"range", toRangeJson(identifier->range)}, {"placeholder", identifier->name}};
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

    if (semantic_workspace_.resolvedSymbolAt(document->uri, rename.position.line,
                                             rename.position.character).has_value()) {
        std::map<std::string, jsonrpc::Json> changes;
        for (const auto& reference : semantic_workspace_.referencesAt(document->uri,
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
    std::vector<analysis::ModuleSchematic> schematics;
    try {
        definitions = compilation_service_.moduleDefinitions(text, uri);
        schematics = compilation_service_.moduleSchematics(text, uri);
    }
    catch (...) {
        definitions.clear();
        schematics.clear();
    }

    hierarchy_documents_.insert_or_assign(std::string(uri), std::move(definitions));
    schematic_documents_.insert_or_assign(std::string(uri), std::move(schematics));
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
    schematic_documents_.erase(std::string(uri));
}

void ServerSession::publishDiagnostics(std::string_view uri) {
    if (!server_) {
        return;
    }

    const auto* document = document_store_.find(uri);
    if (!document) {
        return;
    }

    const auto parse_result = compilation_service_.parse(document->text, document->uri);

    jsonrpc::Json diagnostics = jsonrpc::Json::array();
    for (const auto& diagnostic : parse_result.diagnostics) {
        diagnostics.push_back(jsonrpc::Json{{"range",
                                             jsonrpc::Json{{"start",
                                                            jsonrpc::Json{{"line", diagnostic.range.start_line},
                                                                           {"character", diagnostic.range.start_character}}},
                                                           {"end",
                                                            jsonrpc::Json{{"line", diagnostic.range.end_line},
                                                                           {"character", diagnostic.range.end_character}}}}},
                                            {"severity", diagnostic.severity},
                                            {"code", diagnostic.code},
                                            {"source", server_name_},
                                            {"message", diagnostic.message}});
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