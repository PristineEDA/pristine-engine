#include "pristine/server/ServerSession.h"

#include <nlohmann/json.hpp>

#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/layout/LayoutSource.h"
#include "pristine/lsp/Protocol.h"
#include "pristine/waveform/FstWaveformSource.h"
#include "../analysis/semantic/DebugTrace.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pristine::server {
namespace {

namespace fs = std::filesystem;
constexpr size_t kSyntaxFirstDiagnosticsDocumentThreshold = 128;

jsonrpc::Json toRangeJson(const analysis::ParseRange& range) {
    return jsonrpc::Json{{"start",
                          jsonrpc::Json{{"line", range.start_line},
                                         {"character", range.start_character}}},
                         {"end",
                          jsonrpc::Json{{"line", range.end_line},
                                         {"character", range.end_character}}}};
}

jsonrpc::Json toLocationJson(const analysis::SemanticLocation& location) {
    return jsonrpc::Json{{"uri", location.uri}, {"range", toRangeJson(location.range)}};
}

jsonrpc::Json toDiagnosticJson(const analysis::SemanticDiagnosticData& diagnostic,
                               std::string_view source) {
    return jsonrpc::Json{{"range", toRangeJson(diagnostic.range)},
                         {"severity", diagnostic.severity},
                         {"code", diagnostic.code},
                         {"source", std::string(source)},
                         {"message", diagnostic.message}};
}

jsonrpc::Json toTextEditJson(const analysis::ParseRange& range, std::string_view new_text) {
    return jsonrpc::Json{{"range", toRangeJson(range)}, {"newText", new_text}};
}

jsonrpc::Json toPositionJson(int line, int character) {
    return jsonrpc::Json{{"line", line}, {"character", character}};
}

jsonrpc::Json toDocumentHighlightJson(const analysis::SemanticLocation& location) {
    return jsonrpc::Json{{"range", toRangeJson(location.range)}, {"kind", 1}};
}

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

jsonrpc::Json toHoverResultJson(const analysis::HoverResult& hover) {
    return jsonrpc::Json{{"contents", jsonrpc::Json{{"kind", "markdown"},
                                                     {"value", hover.contents}}},
                         {"range", toRangeJson(hover.range)}};
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

jsonrpc::Json markdownDocumentation(std::string value) {
    return jsonrpc::Json{{"kind", "markdown"}, {"value", std::move(value)}};
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

analysis::ParseRange pointRange(const lsp::Position& position) {
    return analysis::ParseRange{.start_line = position.line,
                                .start_character = position.character,
                                .end_line = position.line,
                                .end_character = position.character};
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

jsonrpc::Json toCodeActionJson(const analysis::SemanticCodeAction& action,
                               std::string_view diagnostic_source) {
    jsonrpc::Json diagnostics = jsonrpc::Json::array();
    for (const auto& diagnostic : action.diagnostics) {
        diagnostics.push_back(toDiagnosticJson(diagnostic, diagnostic_source));
    }

    jsonrpc::Json item{{"title", action.title},
                       {"kind", action.kind}};
    if (action.is_preferred) {
        item["isPreferred"] = true;
    }
    if (!diagnostics.empty()) {
        item["diagnostics"] = std::move(diagnostics);
    }

    if (!action.create_files.empty()) {
        jsonrpc::Json document_changes = jsonrpc::Json::array();
        for (const auto& create_file : action.create_files) {
            document_changes.push_back(jsonrpc::Json{{"kind", "create"},
                                                     {"uri", create_file.uri},
                                                     {"options", jsonrpc::Json{{"ignoreIfExists",
                                                                                 create_file.ignore_if_exists}}}});
        }
        item["edit"] = jsonrpc::Json{{"documentChanges", std::move(document_changes)}};
        return item;
    }

    if (!action.edits.empty()) {
        jsonrpc::Json changes = jsonrpc::Json::object();
        for (const auto& edit : action.edits) {
            changes[edit.uri].push_back(toTextEditJson(edit.range, edit.new_text));
        }
        item["edit"] = jsonrpc::Json{{"changes", std::move(changes)}};
    }
    return item;
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

jsonrpc::Json toOutlineItemJson(const analysis::OutlineItem& item, bool include_children) {
    jsonrpc::Json result{{"id", item.id},
                         {"parentId",
                          item.parent_id.has_value() ? jsonrpc::Json(*item.parent_id)
                                                     : jsonrpc::Json(nullptr)},
                         {"name", item.name},
                         {"kind", item.kind},
                         {"symbolKind", item.symbol_kind},
                         {"range", toRangeJson(item.range)},
                         {"selectionRange", toRangeJson(item.selection_range)},
                         {"depth", item.depth}};

    if (!item.metadata.detail.empty()) {
        result["detail"] = item.metadata.detail;
    }
    if (!item.metadata.declaration.empty()) {
        result["declaration"] = item.metadata.declaration;
    }
    if (!item.metadata.type.empty()) {
        result["type"] = item.metadata.type;
    }
    if (!item.metadata.direction.empty()) {
        result["direction"] = item.metadata.direction;
    }
    if (!item.metadata.value.empty()) {
        result["value"] = item.metadata.value;
    }
    if (!item.metadata.module_name.empty()) {
        result["moduleName"] = item.metadata.module_name;
    }

    if (include_children) {
        result["children"] = jsonrpc::Json::array();
        for (const auto& child : item.children) {
            result["children"].push_back(toOutlineItemJson(child, true));
        }
    }

    return result;
}

jsonrpc::Json toOutlineResultJson(const analysis::OutlineResult& outline,
                                  bool include_children,
                                  bool include_flat) {
    jsonrpc::Json roots = jsonrpc::Json::array();
    if (include_children) {
        for (const auto& root : outline.roots) {
            roots.push_back(toOutlineItemJson(root, true));
        }
    }

    jsonrpc::Json items = jsonrpc::Json::array();
    if (include_flat) {
        for (const auto& item : outline.items) {
            items.push_back(toOutlineItemJson(item, false));
        }
    }

    jsonrpc::Json messages = jsonrpc::Json::array();
    for (const auto& message : outline.messages) {
        messages.push_back(message);
    }

    return jsonrpc::Json{{"uri", outline.uri},
                         {"version", outline.version},
                         {"generation", outline.generation},
                         {"roots", std::move(roots)},
                         {"items", std::move(items)},
                         {"partial", outline.partial},
                         {"truncated", outline.truncated},
                         {"messages", std::move(messages)}};
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

void appendQueryCacheTelemetry(jsonrpc::Json& result,
                               const analysis::SemanticQueryCacheStats& stats) {
    result["queryCacheHits"] = stats.hits;
    result["queryCacheMisses"] = stats.misses;
    result["queryCacheStores"] = stats.stores;
    result["queryCacheEvictions"] = stats.evictions;
    result["queryCacheEntries"] = stats.total_entries;
    result["queryCacheWorkspaceSymbolEntries"] = stats.workspace_symbols_entries;
    result["queryCacheModuleHierarchyEntries"] = stats.module_hierarchy_entries;
    result["queryCacheSchematicEntries"] = stats.schematic_entries;
    result["queryCacheBackwardConeEntries"] = stats.backward_cone_entries;
}

void appendSyntaxCacheTelemetry(jsonrpc::Json& result,
                                const analysis::SyntaxDocumentCacheStats& stats) {
    result["syntaxCacheHits"] = stats.hits;
    result["syntaxCacheMisses"] = stats.misses;
    result["syntaxCacheStores"] = stats.stores;
    result["syntaxCacheInvalidations"] = stats.invalidations;
    result["syntaxCacheEntries"] = stats.entries;
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
    analysis::SemanticCallHierarchyItem result;
    result.name = item.name;
    result.uri = item.uri;
    result.range = parseRangeFromLspRange(item.range);
    result.selection_range = parseRangeFromLspRange(item.selection_range);
    return result;
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

jsonrpc::Json toWaveformSessionJson(const waveform::WaveformSessionInfo& info) {
    jsonrpc::Json result{{"sessionId", info.session_id},
                         {"protocol", info.protocol},
                         {"endpoint",
                          jsonrpc::Json{{"kind", info.endpoint.kind}, {"path", info.endpoint.path}}},
                         {"title", info.title},
                         {"duration", info.duration},
                         {"timescaleUnit", info.timescale_unit},
                         {"groupCount", info.group_count},
                         {"signalCount", info.signal_count},
                         {"source", info.source}};
    if (info.file_uri.has_value()) {
        result["fileUri"] = *info.file_uri;
    }
    return result;
}

jsonrpc::Json toLayoutBoundsJson(const layout::LayoutRect& bounds,
                                 std::uint32_t units_per_micron) {
    const auto scale = units_per_micron == 0 ? 1.0 : static_cast<double>(units_per_micron);
    return jsonrpc::Json{{"x0", static_cast<double>(bounds.x0) / scale},
                         {"y0", static_cast<double>(bounds.y0) / scale},
                         {"x1", static_cast<double>(bounds.x1) / scale},
                         {"y1", static_cast<double>(bounds.y1) / scale}};
}

jsonrpc::Json toLayoutSessionJson(const layout::LayoutSessionInfo& info) {
    jsonrpc::Json messages = jsonrpc::Json::array();
    for (const auto& message : info.messages) {
        messages.push_back(message);
    }
    jsonrpc::Json file_uris = jsonrpc::Json::array();
    for (const auto& uri : info.file_uris) {
        file_uris.push_back(uri);
    }
    jsonrpc::Json result{{"sessionId", info.session_id},
                         {"protocol", info.protocol},
                         {"endpoint",
                         jsonrpc::Json{{"kind", info.endpoint.kind}, {"path", info.endpoint.path}}},
                         {"title", info.title},
                         {"source", info.source},
                         {"lefCount", info.lef_count},
                         {"defPresent", info.def_present},
                         {"unitsPerMicron", info.units_per_micron},
                         {"bbox",
                          info.bounds.has_value()
                              ? toLayoutBoundsJson(*info.bounds, info.units_per_micron)
                              : jsonrpc::Json(nullptr)},
                         {"layerCount", info.layer_count},
                         {"macroCount", info.macro_count},
                         {"componentCount", info.component_count},
                         {"netCount", info.net_count},
                         {"cellCount", info.cell_count},
                         {"referenceCount", info.reference_count},
                         {"elementCount", info.element_count},
                         {"diagnosticCount", info.diagnostic_count},
                         {"messages", std::move(messages)},
                         {"fileUris", std::move(file_uris)}};
    return result;
}

std::string parseWaveformSource(const jsonrpc::Json& params) {
    const auto source_it = params.find("source");
    if (source_it == params.end() || source_it->is_null()) {
        return "mock";
    }
    if (!source_it->is_string()) {
        throw std::runtime_error("Expected 'source' to be a string");
    }
    return source_it->get<std::string>();
}

std::string parseRequiredString(const jsonrpc::Json& params, std::string_view field_name) {
    const auto it = params.find(std::string(field_name));
    if (it == params.end() || !it->is_string()) {
        throw std::runtime_error("Expected '" + std::string(field_name) + "' to be a string");
    }
    return it->get<std::string>();
}

std::vector<std::string> parseRequiredStringArray(const jsonrpc::Json& params,
                                                  std::string_view field_name) {
    const auto it = params.find(std::string(field_name));
    if (it == params.end() || !it->is_array()) {
        throw std::runtime_error("Expected '" + std::string(field_name) + "' to be an array");
    }
    std::vector<std::string> result;
    for (const auto& value : *it) {
        if (!value.is_string()) {
            throw std::runtime_error("Expected '" + std::string(field_name) +
                                     "' entries to be strings");
        }
        result.push_back(value.get<std::string>());
    }
    return result;
}

bool isPathInsideRoot(const fs::path& path, const fs::path& root) {
    std::error_code error;
    const auto canonical_path = fs::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const auto canonical_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }

    auto root_it = canonical_root.begin();
    auto path_it = canonical_path.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++path_it) {
        if (path_it == canonical_path.end() || *root_it != *path_it) {
            return false;
        }
    }
    return true;
}

void addIndexedSourcePath(std::vector<fs::path>& paths, const fs::path& path) {
    const auto normalized = fs::absolute(path).lexically_normal();
    paths.push_back(normalized);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

void removeIndexedSourcePath(std::vector<fs::path>& paths, const fs::path& path) {
    const auto normalized = fs::absolute(path).lexically_normal();
    paths.erase(std::remove(paths.begin(), paths.end(), normalized), paths.end());
}

analysis::SemanticEngineConfig semanticConfigForWorkspace(const workspace::State& state) {
    std::vector<analysis::SemanticEngineConfig::IndexConfig> semantic_index_config;
    semantic_index_config.reserve(state.config.index.size());
    for (const auto& index_config : state.config.index) {
        semantic_index_config.push_back(
            analysis::SemanticEngineConfig::IndexConfig{.dirs = index_config.dirs,
                                                        .exclude_dirs = index_config.exclude_dirs});
    }

    return analysis::SemanticEngineConfig{.build = state.config.build,
                                          .build_pattern = state.config.build_pattern,
                                          .build_relative_paths = state.config.build_relative_paths,
                                          .flags = state.config.flags,
                                          .workspace_root_uri =
                                              state.root_path.has_value()
                                                  ? std::optional<std::string>{toFileUri(*state.root_path)}
                                                  : std::optional<std::string>{},
                                          .top_modules = state.config.top_modules,
                                          .index = std::move(semantic_index_config)};
}

} // namespace

ServerSession::ServerSession(std::string server_name, std::string server_version) :
    server_name_(std::move(server_name)), server_version_(std::move(server_version)) {
    diagnostics_thread_ = std::thread([this]() { backgroundDiagnosticsLoop(); });
}

ServerSession::~ServerSession() {
    stopBackgroundDiagnostics();
    waveform_service_.stop();
    layout_service_.stop();
}

void ServerSession::bind(jsonrpc::JsonRpcServer& server) {
    server_ = &server;

    server.registerRequestHandler("initialize", [this](const jsonrpc::Json& params) {
        return handleInitialize(params);
    });
    server.registerRequestHandler("textDocument/documentSymbol", [this](const jsonrpc::Json& params) {
        return handleDocumentSymbol(params);
    });
    server.registerRequestHandler("systemverilog/outline", [this](const jsonrpc::Json& params) {
        return handleOutline(params);
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
    server.registerRequestHandler("systemverilog/waveform/open", [this](const jsonrpc::Json& params) {
        return handleWaveformOpen(params);
    });
    server.registerRequestHandler("systemverilog/waveform/close", [this](const jsonrpc::Json& params) {
        return handleWaveformClose(params);
    });
    server.registerRequestHandler("systemverilog/layout/open", [this](const jsonrpc::Json& params) {
        return handleLayoutOpen(params);
    });
    server.registerRequestHandler("systemverilog/layout/close", [this](const jsonrpc::Json& params) {
        return handleLayoutClose(params);
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
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.initialize");
    workspace_manager_.initialize(lsp::parseInitializeParams(params));
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        semantic_workspace_.clear();
        semantic_workspace_.configureSemanticEngine(semanticConfigForWorkspace(workspace_manager_.state()));
        if (const auto& root_path = workspace_manager_.state().root_path) {
            semantic_workspace_.setWorkspaceRoot(toFileUri(*root_path));
        }
        else {
            semantic_workspace_.setWorkspaceRoot({});
        }
        semantic_generation_cache_.store(semantic_workspace_.engineGeneration(), std::memory_order_relaxed);
    }
    syntax_cache_.clear();
    indexed_source_paths_ = workspace_manager_.sourceFilesForIndex();
    indexWorkspaceSources();
    initialized_ = true;
    shutdown_requested_ = false;
    return lsp::makeInitializeResult(server_name_, server_version_);
}

jsonrpc::Json ServerSession::handleDocumentSymbol(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.documentSymbol");
    if (!initialized_) {
        throw std::runtime_error("textDocument/documentSymbol received before initialize");
    }

    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto* document = document_store_.find(uri);
    if (!document) {
        return jsonrpc::Json::array();
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& symbol : cachedDocumentSymbols(*document)) {
        result.push_back(toDocumentSymbolJson(symbol));
    }

    return result;
}

jsonrpc::Json ServerSession::handleOutline(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.outline");
    if (!initialized_) {
        throw std::runtime_error("systemverilog/outline received before initialize");
    }

    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    analysis::OutlineOptions options;
    if (params.contains("maxDepth") && params.at("maxDepth").is_number_integer()) {
        options.max_depth = params.at("maxDepth").get<int>();
    }
    if (params.contains("limit") && params.at("limit").is_number_unsigned()) {
        options.limit = params.at("limit").get<size_t>();
    }
    else if (params.contains("limit") && params.at("limit").is_number_integer()) {
        const auto limit = params.at("limit").get<int>();
        options.limit = limit <= 0 ? 0 : static_cast<size_t>(limit);
    }
    if (params.contains("includeChildren") && params.at("includeChildren").is_boolean()) {
        options.include_children = params.at("includeChildren").get<bool>();
    }
    if (params.contains("includeFlat") && params.at("includeFlat").is_boolean()) {
        options.include_flat = params.at("includeFlat").get<bool>();
    }

    const auto* document = document_store_.find(uri);
    if (!document) {
        analysis::OutlineResult result;
        result.uri = uri;
        result.messages.push_back("Document is not open; systemverilog/outline only operates on opened documents.");
        auto response = toOutlineResultJson(result, options.include_children, options.include_flat);
        appendSyntaxCacheTelemetry(response, syntax_cache_.stats());
        return response;
    }

    const auto outline = compilation_service_.outline(document->text,
                                                      document->uri,
                                                      document->version,
                                                      semantic_generation_cache_.load(std::memory_order_relaxed),
                                                      options);
    auto response = toOutlineResultJson(outline, options.include_children, options.include_flat);
    appendSyntaxCacheTelemetry(response, syntax_cache_.stats());
    return response;
}

jsonrpc::Json ServerSession::handleModuleHierarchy(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.moduleHierarchy");
    if (!initialized_) {
        throw std::runtime_error("systemverilog/moduleHierarchy received before initialize");
    }

    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);
    analysis::SemanticModuleHierarchyResult hierarchy;
    analysis::SemanticQueryCacheStats query_cache_stats;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        hierarchy = semantic_workspace_.engineModuleHierarchy(
            requested_module_name.has_value()
                ? std::optional<std::string_view>{std::string_view(*requested_module_name)}
                : std::optional<std::string_view>{},
            max_depth);
        query_cache_stats = semantic_workspace_.engineQueryCacheStats();
    }

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
    if (hierarchy.discovery_closure_used) {
        result["discoveryClosureUsed"] = true;
        result["discoveryClosureRoot"] = hierarchy.discovery_closure_root_name;
        result["discoveryClosureCandidateDocumentCount"] =
            hierarchy.discovery_closure_candidate_document_count;
        result["discoveryClosureDocumentCount"] = hierarchy.discovery_closure_document_count;
        result["discoveryClosureMissingCandidateCount"] =
            hierarchy.discovery_closure_missing_candidate_count;
        result["discoveryClosureDedupedDocumentCount"] =
            hierarchy.discovery_closure_deduped_document_count;
        result["discoveryClosureBuildMicros"] = hierarchy.discovery_closure_build_micros;
        result["discoveryClosureQueryMicros"] = hierarchy.discovery_closure_query_micros;
        result["discoveryClosureCacheHit"] = hierarchy.discovery_closure_cache_hit;
    }
    appendQueryCacheTelemetry(result, query_cache_stats);
    return result;
}

jsonrpc::Json ServerSession::handleSchematic(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.schematic");
    if (!initialized_) {
        throw std::runtime_error("systemverilog/schematic received before initialize");
    }

    const auto requested_module_name = parseOptionalModuleName(params);
    const auto max_depth = parseMaxDepth(params);
    analysis::SemanticSchematicResult schematic;
    analysis::SemanticQueryCacheStats query_cache_stats;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        schematic = semantic_workspace_.engineSchematic(
            requested_module_name.has_value()
                ? std::optional<std::string_view>{std::string_view(*requested_module_name)}
                : std::optional<std::string_view>{},
            max_depth);
        query_cache_stats = semantic_workspace_.engineQueryCacheStats();
    }

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
    if (schematic.discovery_closure_used) {
        result["discoveryClosureUsed"] = true;
        result["discoveryClosureRoot"] = schematic.discovery_closure_root_name;
        result["discoveryClosureCandidateDocumentCount"] =
            schematic.discovery_closure_candidate_document_count;
        result["discoveryClosureDocumentCount"] = schematic.discovery_closure_document_count;
        result["discoveryClosureMissingCandidateCount"] =
            schematic.discovery_closure_missing_candidate_count;
        result["discoveryClosureDedupedDocumentCount"] =
            schematic.discovery_closure_deduped_document_count;
        result["discoveryClosureBuildMicros"] = schematic.discovery_closure_build_micros;
        result["discoveryClosureQueryMicros"] = schematic.discovery_closure_query_micros;
        result["discoveryClosureCacheHit"] = schematic.discovery_closure_cache_hit;
    }
    appendQueryCacheTelemetry(result, query_cache_stats);
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
    analysis::SemanticConeTrace trace;
    analysis::SemanticQueryCacheStats query_cache_stats;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        trace = semantic_workspace_.engineBackwardConeAt(uri, line, character);
        query_cache_stats = semantic_workspace_.engineQueryCacheStats();
    }
    auto result = toConeTraceJson(trace);
    appendQueryCacheTelemetry(result, query_cache_stats);
    return result;
}

jsonrpc::Json ServerSession::handleWaveformOpen(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/waveform/open received before initialize");
    }

    const auto source = parseWaveformSource(params);
    if (source == "mock") {
        if (params.contains("fstUri")) {
            throw std::runtime_error("'fstUri' is only valid for source 'fst'");
        }
        return toWaveformSessionJson(waveform_service_.openMockSession());
    }
    if (source != "fst") {
        throw std::runtime_error("Unsupported waveform source: " + source);
    }

    const auto fst_uri = parseRequiredString(params, "fstUri");
    const auto fst_path = workspace::WorkspaceManager::pathFromFileUri(fst_uri);
    if (!fst_path.has_value()) {
        throw std::runtime_error("Expected 'fstUri' to be a file URI");
    }
    const auto& workspace_state = workspace_manager_.state();
    if (!workspace_state.root_path.has_value()) {
        throw std::runtime_error("FST waveform source requires an initialized workspace root");
    }
    if (!isPathInsideRoot(*fst_path, *workspace_state.root_path)) {
        throw std::runtime_error("FST waveform file must be inside the workspace root");
    }

    return toWaveformSessionJson(waveform_service_.openSession(
        waveform::openFstWaveformSource(*fst_path, fst_uri, workspace_state.root_path)));
}

jsonrpc::Json ServerSession::handleWaveformClose(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/waveform/close received before initialize");
    }

    const auto session_id_it = params.find("sessionId");
    if (session_id_it == params.end() || !session_id_it->is_string()) {
        throw std::runtime_error("Expected 'sessionId' to be a string");
    }

    const auto closed = waveform_service_.closeSession(session_id_it->get<std::string>());
    return jsonrpc::Json{{"closed", closed}};
}

jsonrpc::Json ServerSession::handleLayoutOpen(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/layout/open received before initialize");
    }

    std::vector<std::string> lef_uris;
    const auto lef_uris_it = params.find("lefUris");
    if (lef_uris_it != params.end()) {
        lef_uris = parseRequiredStringArray(params, "lefUris");
    }
    const auto def_uri = jsonStringField(params, "defUri");
    const auto gds_uri = jsonStringField(params, "gdsUri");
    const auto title = jsonStringField(params, "title").value_or("");
    if (gds_uri.has_value() && (!lef_uris.empty() || def_uri.has_value())) {
        throw std::runtime_error("Layout open does not support mixing GDS with LEF/DEF inputs");
    }
    if (!gds_uri.has_value() && lef_uris.empty() && !def_uri.has_value()) {
        throw std::runtime_error("Layout open requires at least one LEF URI, a DEF URI, or a GDS URI");
    }

    const auto& workspace_state = workspace_manager_.state();
    if (!workspace_state.root_path.has_value()) {
        throw std::runtime_error("Layout source requires an initialized workspace root");
    }

    if (gds_uri.has_value()) {
        auto path = workspace::WorkspaceManager::pathFromFileUri(*gds_uri);
        if (!path.has_value()) {
            throw std::runtime_error("Expected 'gdsUri' to be a file URI");
        }
        if (!isPathInsideRoot(*path, *workspace_state.root_path)) {
            throw std::runtime_error("GDS file must be inside the workspace root");
        }
        auto source = layout::openGdsLayoutSource(*path, *gds_uri, title);
        return toLayoutSessionJson(layout_service_.openSession(std::move(source), 0, false));
    }

    std::vector<fs::path> lef_paths;
    lef_paths.reserve(lef_uris.size());
    for (const auto& uri : lef_uris) {
        const auto path = workspace::WorkspaceManager::pathFromFileUri(uri);
        if (!path.has_value()) {
            throw std::runtime_error("Expected LEF URI to be a file URI");
        }
        if (!isPathInsideRoot(*path, *workspace_state.root_path)) {
            throw std::runtime_error("LEF file must be inside the workspace root");
        }
        lef_paths.push_back(*path);
    }

    std::optional<fs::path> def_path;
    if (def_uri.has_value()) {
        auto path = workspace::WorkspaceManager::pathFromFileUri(*def_uri);
        if (!path.has_value()) {
            throw std::runtime_error("Expected 'defUri' to be a file URI");
        }
        if (!isPathInsideRoot(*path, *workspace_state.root_path)) {
            throw std::runtime_error("DEF file must be inside the workspace root");
        }
        def_path = *path;
    }

    auto source = layout::openLefDefLayoutSource(lef_paths,
                                                lef_uris,
                                                def_path,
                                                def_uri,
                                                title);
    return toLayoutSessionJson(
        layout_service_.openSession(std::move(source), lef_uris.size(), def_uri.has_value()));
}

jsonrpc::Json ServerSession::handleLayoutClose(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("systemverilog/layout/close received before initialize");
    }

    const auto session_id_it = params.find("sessionId");
    if (session_id_it == params.end() || !session_id_it->is_string()) {
        throw std::runtime_error("Expected 'sessionId' to be a string");
    }

    const auto closed = layout_service_.closeSession(session_id_it->get<std::string>());
    return jsonrpc::Json{{"closed", closed}};
}

jsonrpc::Json ServerSession::handleHover(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.hover");
    if (!initialized_) {
        throw std::runtime_error("textDocument/hover received before initialize");
    }

    const auto hover = lsp::parseHoverParams(params);
    const auto* document = document_store_.find(hover.text_document.uri);
    if (!document) {
        return nullptr;
    }

    bool use_syntax_hover = false;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        use_syntax_hover =
            semantic_workspace_.documentCount() > kSyntaxFirstDiagnosticsDocumentThreshold &&
            !semantic_workspace_.engineHasFreshSnapshot();
    }
    if (use_syntax_hover) {
        analysis::semantic::debugTraceInstant("server.hover.syntaxFastPath", document->uri);
        const auto syntax_hover = compilation_service_.hover(document->text,
                                                             document->uri,
                                                             hover.position.line,
                                                             hover.position.character);
        if (!syntax_hover.has_value()) {
            return nullptr;
        }
        return toHoverResultJson(*syntax_hover);
    }

    analysis::SemanticHoverResult semantic_result;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        semantic_result = semantic_workspace_.engineHoverAt(document->uri,
                                                            hover.position.line,
                                                            hover.position.character);
    }
    if (semantic_result.unresolved || semantic_result.contents.empty()) {
        return nullptr;
    }

    return jsonrpc::Json{{"contents", jsonrpc::Json{{"kind", "markdown"},
                                                     {"value", semantic_result.contents}}},
                         {"range", toRangeJson(semantic_result.range)}};
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

    analysis::SemanticReferenceResult definitions;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        definitions = semantic_workspace_.engineDefinitionsAt(document->uri,
                                                              definition.position.line,
                                                              definition.position.character);
    }
    if (definitions.unresolved || definitions.locations.empty()) {
        return nullptr;
    }

    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& location : definitions.locations) {
        result.push_back(toLocationJson(location));
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

    jsonrpc::Json result = jsonrpc::Json::array();
    analysis::SemanticReferenceResult definitions;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        definitions = semantic_workspace_.engineTypeDefinitionsAt(document->uri,
                                                                  type_definition.position.line,
                                                                  type_definition.position.character);
    }
    if (definitions.unresolved) {
        return result;
    }
    for (const auto& location : definitions.locations) {
        result.push_back(toLocationJson(location));
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

    jsonrpc::Json result = jsonrpc::Json::array();
    analysis::SemanticReferenceResult engine_references;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_references = semantic_workspace_.engineReferencesAt(
            document->uri, references.position.line, references.position.character,
            references.context.include_declaration);
    }
    if (engine_references.unresolved) {
        return result;
    }
    for (const auto& location : engine_references.locations) {
        result.push_back(toLocationJson(location));
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

    jsonrpc::Json result = jsonrpc::Json::array();
    analysis::SemanticReferenceResult references;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        references = semantic_workspace_.engineImplementationsAt(document->uri,
                                                                 implementation.position.line,
                                                                 implementation.position.character);
    }
    if (references.unresolved) {
        return result;
    }
    for (const auto& location : references.locations) {
        result.push_back(toLocationJson(location));
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

    jsonrpc::Json result = jsonrpc::Json::array();
    analysis::SemanticReferenceResult highlights;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        highlights = semantic_workspace_.engineDocumentHighlightsAt(document->uri,
                                                                    highlight.position.line,
                                                                    highlight.position.character);
    }
    if (highlights.unresolved) {
        return result;
    }
    for (const auto& location : highlights.locations) {
        if (location.uri == document->uri) {
            result.push_back(toDocumentHighlightJson(location));
        }
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
    analysis::SemanticInlayHintResult engine_hints;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_hints = semantic_workspace_.engineInlayHints(
            document->uri,
            analysis::ParseRange{.start_line = hints.range.start.line,
                                 .start_character = hints.range.start.character,
                                 .end_line = hints.range.end.line,
                                 .end_character = hints.range.end.character});
    }
    if (!engine_hints.unresolved && !engine_hints.hints.empty()) {
        for (const auto& hint : engine_hints.hints) {
            result.push_back(toInlayHintJson(hint));
        }
    }

    return result;
}

jsonrpc::Json ServerSession::handleCodeAction(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/codeAction received before initialize");
    }

    const auto action = lsp::parseCodeActionParams(params);
    analysis::SemanticCodeActionResult actions;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        actions = semantic_workspace_.engineCodeActionsAt(
            action.text_document.uri,
            analysis::ParseRange{.start_line = action.range.start.line,
                                 .start_character = action.range.start.character,
                                 .end_line = action.range.end.line,
                                 .end_character = action.range.end.character});
    }
    jsonrpc::Json result = jsonrpc::Json::array();
    for (const auto& item : actions.actions) {
        result.push_back(toCodeActionJson(item, server_name_));
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
    collectFoldingRanges(result, cachedDocumentSymbols(*document));
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

    analysis::SemanticTokenResult engine_tokens;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_tokens = semantic_workspace_.engineSemanticTokens(document->uri);
    }
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
    collectSemanticTokens(tokens, cachedDocumentSymbols(*document));
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
        analysis::SemanticSelectionRangeResult engine_selection;
        {
            std::lock_guard semantic_lock(semantic_mutex_);
            engine_selection = semantic_workspace_.engineSelectionRangesAt(document->uri,
                                                                          position.line,
                                                                          position.character);
        }
        if (!engine_selection.unresolved && !engine_selection.ranges.empty()) {
            result.push_back(toSelectionRangeJson(engine_selection.ranges, 0));
            continue;
        }
        result.push_back(toSelectionRangeJson(std::vector<analysis::ParseRange>{pointRange(position)}));
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

    analysis::SemanticSignatureHelpResult engine_help;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_help = semantic_workspace_.engineSignatureHelpAt(document->uri,
                                                                signature_help.position.line,
                                                                signature_help.position.character);
    }
    if (!engine_help.unresolved && !engine_help.label.empty()) {
        return toSignatureHelpJson(engine_help);
    }
    return nullptr;
}

jsonrpc::Json ServerSession::handlePrepareCallHierarchy(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/prepareCallHierarchy received before initialize");
    }

    const auto prepare = lsp::parseCallHierarchyPrepareParams(params);
    analysis::SemanticCallHierarchyPrepareResult prepared;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        prepared = semantic_workspace_.enginePrepareCallHierarchy(prepare.text_document.uri,
                                                                  prepare.position.line,
                                                                  prepare.position.character);
    }
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
    analysis::SemanticCallHierarchyCallsResult incoming;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        incoming = semantic_workspace_.engineIncomingCalls(toSemanticCallHierarchyItem(calls.item));
    }

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
    analysis::SemanticCallHierarchyCallsResult outgoing;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        outgoing = semantic_workspace_.engineOutgoingCalls(toSemanticCallHierarchyItem(calls.item));
    }
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
    analysis::SemanticWorkspaceSymbolResult symbols;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        symbols = semantic_workspace_.engineWorkspaceSymbols(workspace_symbol.query);
    }
    for (const auto& symbol : symbols.symbols) {
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
    analysis::SemanticCompletionResult engine_completions;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_completions = semantic_workspace_.engineCompletionsAt(document->uri,
                                                                     completion.position.line,
                                                                     completion.position.character,
                                                                     prefix);
    }
    if (engine_completions.unresolved) {
        return result;
    }
    for (const auto& item : engine_completions.items) {
        appendCompletionItem(result, emitted_labels, toSemanticEngineCompletionItem(item));
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

    if (*source != "semanticEngine") {
        return item;
    }

    const auto stable_id = jsonStringField(*data_it, "stableId");
    const auto label = jsonStringField(*data_it, "label").value_or(item.value("label", ""));
    if (!stable_id.has_value()) {
        return item;
    }
    analysis::SemanticCompletionItem resolved;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        resolved = semantic_workspace_.engineResolveCompletion(*stable_id, label);
    }
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

jsonrpc::Json ServerSession::handlePrepareRename(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/prepareRename received before initialize");
    }

    const auto prepare = lsp::parsePrepareRenameParams(params);
    const auto* document = document_store_.find(prepare.text_document.uri);
    if (!document) {
        return nullptr;
    }

    analysis::SemanticPrepareRenameResult engine_prepare;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_prepare = semantic_workspace_.enginePrepareRenameAt(document->uri,
                                                                   prepare.position.line,
                                                                   prepare.position.character);
    }
    if (engine_prepare.unresolved) {
        return nullptr;
    }

    return jsonrpc::Json{{"range", toRangeJson(engine_prepare.range)},
                         {"placeholder", engine_prepare.placeholder}};
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

    analysis::SemanticRenameResult engine_rename;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        engine_rename = semantic_workspace_.engineRenameAt(document->uri,
                                                           rename.position.line,
                                                           rename.position.character,
                                                           rename.new_name);
    }
    if (engine_rename.unresolved || engine_rename.edits.empty()) {
        return nullptr;
    }

    std::map<std::string, jsonrpc::Json> changes;
    for (const auto& edit : engine_rename.edits) {
        auto [entry_it, inserted] = changes.try_emplace(edit.location.uri, jsonrpc::Json::array());
        entry_it->second.push_back(toTextEditJson(edit.location.range, edit.new_text));
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
    stopBackgroundDiagnostics();
    waveform_service_.stop();
    layout_service_.stop();
    shutdown_requested_ = true;
    return nullptr;
}

void ServerSession::handleInitialized(const jsonrpc::Json&) {}

void ServerSession::handleDidOpen(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.didOpen");
    if (!initialized_) {
        throw std::runtime_error("textDocument/didOpen received before initialize");
    }

    const auto did_open = lsp::parseDidOpenTextDocumentParams(params);
    {
        std::lock_guard state_lock(state_mutex_);
        document_store_.open(did_open);
    }
    invalidateSyntaxCache(did_open.text_document.uri);
    updateSemanticDocument(did_open.text_document.uri,
                           did_open.text_document.text,
                           analysis::SemanticDocumentState{.version = did_open.text_document.version,
                                                           .is_open = true,
                                                           .dirty = false,
                                                           .invalidate_dependents = true});
    publishDiagnostics(did_open.text_document.uri);
}

void ServerSession::handleDidChange(const jsonrpc::Json& params) {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.didChange");
    if (!initialized_) {
        throw std::runtime_error("textDocument/didChange received before initialize");
    }

    const auto did_change = lsp::parseDidChangeTextDocumentParams(params);
    std::string document_uri;
    std::string document_text;
    int document_version = -1;
    bool document_dirty = false;
    {
        std::lock_guard state_lock(state_mutex_);
        document_store_.applyChanges(did_change);
        if (const auto* document = document_store_.find(did_change.text_document.uri)) {
            document_uri = document->uri;
            document_text = document->text;
            document_version = document->version;
            document_dirty = document->dirty;
        }
    }
    invalidateSyntaxCache(did_change.text_document.uri);
    if (!document_uri.empty()) {
        updateSemanticDocument(document_uri,
                               document_text,
                               analysis::SemanticDocumentState{.version = document_version,
                                                               .is_open = true,
                                                               .dirty = document_dirty,
                                                               .invalidate_dependents = true});
    }
    publishDiagnostics(did_change.text_document.uri);
}

void ServerSession::handleDidSave(const jsonrpc::Json& params) {
    if (!initialized_) {
        throw std::runtime_error("textDocument/didSave received before initialize");
    }

    const auto did_save = lsp::parseDidSaveTextDocumentParams(params);
    std::string document_uri;
    std::string document_text;
    int document_version = -1;
    bool document_dirty = false;
    {
        std::lock_guard state_lock(state_mutex_);
        document_store_.save(did_save);
        if (const auto* document = document_store_.find(did_save.text_document.uri)) {
            document_uri = document->uri;
            document_text = document->text;
            document_version = document->version;
            document_dirty = document->dirty;
        }
    }
    invalidateSyntaxCache(did_save.text_document.uri);
    if (!document_uri.empty()) {
        updateSemanticDocument(document_uri,
                               document_text,
                               analysis::SemanticDocumentState{.version = document_version,
                                                               .is_open = true,
                                                               .dirty = document_dirty,
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
    {
        std::lock_guard state_lock(state_mutex_);
        document_store_.close(did_close);
    }
    invalidateSyntaxCache(did_close.text_document.uri);
    restoreClosedDocument(did_close.text_document.uri);
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
            invalidateSyntaxCache(change.uri);
            removeIndexedSourcePath(indexed_source_paths_, *path);
            removeSemanticDocument(change.uri);
            continue;
        }

        addIndexedSourcePath(indexed_source_paths_, *path);
        if (const auto* document = document_store_.find(change.uri)) {
            updateSemanticDocument(document->uri,
                                   document->text,
                                   analysis::SemanticDocumentState{.version = document->version,
                                                                   .is_open = true,
                                                                   .dirty = document->dirty,
                                                                   .invalidate_dependents = true});
            continue;
        }

        std::error_code error;
        if (!fs::exists(*path, error) || !fs::is_regular_file(*path, error)) {
            invalidateSyntaxCache(change.uri);
            removeIndexedSourcePath(indexed_source_paths_, *path);
            removeSemanticDocument(change.uri);
            continue;
        }

        const auto text = readFileText(*path);
        if (!text.has_value()) {
            invalidateSyntaxCache(change.uri);
            removeIndexedSourcePath(indexed_source_paths_, *path);
            removeSemanticDocument(change.uri);
            continue;
        }

        updateSemanticDocument(change.uri,
                               *text,
                               analysis::SemanticDocumentState{.version = -1,
                                                               .is_open = false,
                                                               .dirty = false,
                                                               .invalidate_dependents = true});
    }
}

void ServerSession::handleExit(const jsonrpc::Json&) {
    stopBackgroundDiagnostics();
    waveform_service_.stop();
    layout_service_.stop();
    if (!server_) {
        return;
    }

    server_->requestStop(shutdown_requested_ ? 0 : 1);
}

void ServerSession::indexWorkspaceSources() {
    PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.indexWorkspaceSources");
    for (const auto& path : indexed_source_paths_) {
        const auto text = readFileText(path);
        if (!text.has_value()) {
            continue;
        }
        updateSemanticDocument(toFileUri(path), *text);
    }
}

void ServerSession::updateSemanticDocument(std::string_view uri,
                                           std::string_view text,
                                           analysis::SemanticDocumentState semantic_state) {
    std::lock_guard semantic_lock(semantic_mutex_);
    semantic_workspace_.updateDocument(uri, text, semantic_state);
    semantic_generation_cache_.store(semantic_workspace_.engineGeneration(), std::memory_order_relaxed);
}

const std::vector<analysis::DocumentSymbol>&
ServerSession::cachedDocumentSymbols(const document::TextDocument& document) {
    return syntax_cache_.documentSymbols(compilation_service_,
                                         document.uri,
                                         document.version,
                                         document.text);
}

void ServerSession::invalidateSyntaxCache(std::string_view uri) {
    syntax_cache_.invalidate(uri);
}

void ServerSession::restoreClosedDocument(std::string_view uri) {
    const auto path = workspace::WorkspaceManager::pathFromFileUri(uri);
    if (!path.has_value()) {
        removeSemanticDocument(uri);
        return;
    }

    std::error_code error;
    if (!fs::exists(*path, error) || !fs::is_regular_file(*path, error)) {
        removeSemanticDocument(uri);
        return;
    }

    const auto text = readFileText(*path);
    if (!text.has_value()) {
        removeSemanticDocument(uri);
        return;
    }

    updateSemanticDocument(uri,
                           *text,
                           analysis::SemanticDocumentState{.version = -1,
                                                           .is_open = false,
                                                           .dirty = false,
                                                           .invalidate_dependents = true});
}

void ServerSession::removeSemanticDocument(std::string_view uri) {
    std::lock_guard semantic_lock(semantic_mutex_);
    semantic_workspace_.removeDocument(uri);
    semantic_generation_cache_.store(semantic_workspace_.engineGeneration(), std::memory_order_relaxed);
}

void ServerSession::publishDiagnostics(std::string_view uri) {
    PRISTINE_DEBUG_TRACE_SCOPE("server.publishDiagnostics", std::string(uri));
    if (!server_) {
        return;
    }

    std::string document_uri;
    std::string document_text;
    {
        std::lock_guard state_lock(state_mutex_);
        const auto* document = document_store_.find(uri);
        if (!document) {
            return;
        }
        document_uri = document->uri;
        document_text = document->text;
    }

    jsonrpc::Json diagnostics = jsonrpc::Json::array();
    bool syntax_first = false;
    size_t semantic_document_count = 0;
    {
        std::lock_guard semantic_lock(semantic_mutex_);
        semantic_document_count = semantic_workspace_.documentCount();
        syntax_first = semantic_document_count > kSyntaxFirstDiagnosticsDocumentThreshold &&
                       !semantic_workspace_.engineHasFreshSnapshot();
    }
    if (syntax_first) {
        PRISTINE_DEBUG_TRACE_SCOPE("server.publishDiagnostics.syntaxFastPath", document_uri);
        for (const auto& diagnostic : compilation_service_.parse(document_text, document_uri).diagnostics) {
            diagnostics.push_back(makeDiagnosticJson(diagnostic.range,
                                                     diagnostic.severity,
                                                     diagnostic.code,
                                                     server_name_,
                                                     diagnostic.message));
        }
    }
    else {
        std::vector<analysis::SemanticEngineDiagnostic> semantic_diagnostics;
        {
            std::lock_guard semantic_lock(semantic_mutex_);
            semantic_diagnostics = semantic_workspace_.engineDiagnosticsFor(document_uri);
        }
        for (const auto& diagnostic : semantic_diagnostics) {
            diagnostics.push_back(makeDiagnosticJson(diagnostic.range,
                                                     diagnostic.severity,
                                                     diagnostic.code,
                                                     server_name_,
                                                     diagnostic.message));
        }
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", document_uri},
                                            {"diagnostics", std::move(diagnostics)}});
    if (syntax_first) {
        (void)semantic_document_count;
        scheduleSemanticDiagnosticsPublish(true);
    }
}

void ServerSession::publishDiagnostics(std::string_view uri,
                                       std::vector<analysis::SemanticEngineDiagnostic> diagnostics) {
    if (!server_) {
        return;
    }

    jsonrpc::Json diagnostics_json = jsonrpc::Json::array();
    for (const auto& diagnostic : diagnostics) {
        diagnostics_json.push_back(makeDiagnosticJson(diagnostic.range,
                                                      diagnostic.severity,
                                                      diagnostic.code,
                                                      server_name_,
                                                      diagnostic.message));
    }

    server_->sendNotification("textDocument/publishDiagnostics",
                              jsonrpc::Json{{"uri", std::string(uri)},
                                            {"diagnostics", std::move(diagnostics_json)}});
}

void ServerSession::scheduleSemanticDiagnosticsPublish(bool allow_cold_snapshot_build) {
    BackgroundDiagnosticsJob job;
    job.allow_cold_snapshot_build = allow_cold_snapshot_build;
    job.config = semanticConfigForWorkspace(workspace_manager_.state());
    job.indexed_source_paths = indexed_source_paths_;
    job.workspace_root_uri = workspace_manager_.state().root_path.has_value()
                                 ? toFileUri(*workspace_manager_.state().root_path)
                                 : std::string{};
    {
        std::lock_guard state_lock(state_mutex_);
        for (const auto& [uri, document] : document_store_.documents()) {
            job.open_documents.push_back(BackgroundDiagnosticsDocument{.uri = uri,
                                                                       .text = document.text,
                                                                       .version = document.version,
                                                                       .dirty = document.dirty});
        }
    }
    std::sort(job.open_documents.begin(), job.open_documents.end(), [](const auto& lhs,
                                                                       const auto& rhs) {
        return lhs.uri < rhs.uri;
    });
    if (job.open_documents.empty()) {
        return;
    }
    std::set<std::string> open_document_uris;
    for (const auto& document : job.open_documents) {
        open_document_uris.insert(document.uri);
    }
    job.indexed_source_paths.erase(
        std::remove_if(job.indexed_source_paths.begin(),
                       job.indexed_source_paths.end(),
                       [&](const fs::path& path) {
                           return open_document_uris.contains(toFileUri(path));
                       }),
        job.indexed_source_paths.end());

    {
        std::lock_guard semantic_lock(semantic_mutex_);
        job.semantic_generation = semantic_workspace_.engineGeneration();
        job.workspace_had_fresh_snapshot = semantic_workspace_.engineHasFreshSnapshot();
    }

    {
        std::lock_guard background_lock(background_mutex_);
        diagnostics_stop_requested_ = false;
        job.request_generation = ++diagnostics_request_generation_;
        pending_diagnostics_job_ = std::move(job);
    }
    background_cv_.notify_one();
}

void ServerSession::backgroundDiagnosticsLoop() {
    while (true) {
        BackgroundDiagnosticsJob job;
        {
            std::unique_lock background_lock(background_mutex_);
            background_cv_.wait(background_lock, [this]() {
                return diagnostics_stop_requested_ || pending_diagnostics_job_.has_value();
            });
            if (diagnostics_stop_requested_) {
                return;
            }
            job = std::move(*pending_diagnostics_job_);
            pending_diagnostics_job_.reset();
        }

        try {
            PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("server.backgroundDiagnostics");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                std::lock_guard background_lock(background_mutex_);
                if (diagnostics_stop_requested_ ||
                    job.request_generation != diagnostics_request_generation_) {
                    analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                          "stale-before-build");
                    continue;
                }
            }
            if (!job.allow_cold_snapshot_build && !job.workspace_had_fresh_snapshot) {
                analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                      "large-workspace-cold-snapshot");
                continue;
            }

            analysis::SemanticWorkspace background_workspace;
            background_workspace.configureSemanticEngine(job.config);
            background_workspace.setWorkspaceRoot(job.workspace_root_uri);
            for (const auto& path : job.indexed_source_paths) {
                const auto text = readFileText(path);
                if (!text.has_value()) {
                    continue;
                }
                background_workspace.updateDocument(toFileUri(path), *text);
                {
                    std::lock_guard background_lock(background_mutex_);
                    if (diagnostics_stop_requested_ ||
                        job.request_generation != diagnostics_request_generation_) {
                        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                              "stale-during-index");
                        break;
                    }
                }
            }
            {
                std::lock_guard background_lock(background_mutex_);
                if (diagnostics_stop_requested_ ||
                    job.request_generation != diagnostics_request_generation_) {
                    continue;
                }
            }
            for (const auto& document : job.open_documents) {
                background_workspace.updateDocument(document.uri,
                                                    document.text,
                                                    analysis::SemanticDocumentState{
                                                        .version = document.version,
                                                        .is_open = true,
                                                        .dirty = document.dirty,
                                                        .invalidate_dependents = true});
                {
                    std::lock_guard background_lock(background_mutex_);
                    if (diagnostics_stop_requested_ ||
                        job.request_generation != diagnostics_request_generation_) {
                        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                              "stale-during-open-documents");
                        break;
                    }
                }
            }

            for (const auto& document : job.open_documents) {
                {
                    std::lock_guard background_lock(background_mutex_);
                    if (diagnostics_stop_requested_ ||
                        job.request_generation != diagnostics_request_generation_) {
                        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                              "stale-before-publish");
                        break;
                    }
                }

                auto diagnostics = background_workspace.engineDiagnosticsFor(document.uri);

                {
                    std::lock_guard background_lock(background_mutex_);
                    if (diagnostics_stop_requested_ ||
                        job.request_generation != diagnostics_request_generation_) {
                        break;
                    }
                }
                {
                    std::lock_guard semantic_lock(semantic_mutex_);
                    if (semantic_workspace_.engineGeneration() != job.semantic_generation) {
                        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                              "semantic-generation-changed");
                        continue;
                    }
                }
                {
                    std::lock_guard state_lock(state_mutex_);
                    const auto* current_document = document_store_.find(document.uri);
                    if (!current_document || current_document->version != document.version) {
                        analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.skip",
                                                              "document-stale-or-closed");
                        continue;
                    }
                }
                publishDiagnostics(document.uri, std::move(diagnostics));
            }
        }
        catch (const std::exception& error) {
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", error.what());
        }
        catch (...) {
            analysis::semantic::debugTraceInstant("server.backgroundDiagnostics.error", "unknown");
        }
    }
}

void ServerSession::stopBackgroundDiagnostics() {
    {
        std::lock_guard background_lock(background_mutex_);
        diagnostics_stop_requested_ = true;
        pending_diagnostics_job_.reset();
        ++diagnostics_request_generation_;
    }
    background_cv_.notify_one();
    if (diagnostics_thread_.joinable()) {
        diagnostics_thread_.join();
    }
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
