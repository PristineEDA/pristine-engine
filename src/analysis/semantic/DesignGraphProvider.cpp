#include "DesignGraphProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {
namespace {

std::vector<std::string> inferredHierarchyRootNames(
    const std::unordered_map<std::string, ModuleDefinition>& modules_by_name) {
    std::set<std::string> instantiated;
    for (const auto& [_, module] : modules_by_name) {
        for (const auto& instance : module.instances) {
            instantiated.insert(instance.module_name);
        }
    }
    std::vector<std::string> roots;
    for (const auto& [name, _] : modules_by_name) {
        if (!instantiated.contains(name)) {
            roots.push_back(name);
        }
    }
    if (roots.empty()) {
        roots.reserve(modules_by_name.size());
        for (const auto& [name, _] : modules_by_name) {
            roots.push_back(name);
        }
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
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

const SchematicPort* findSchematicPortByName(const ModuleSchematic& schematic,
                                             std::string_view name) {
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

const SchematicPort* findModulePortByConnection(const ModuleDefinition& module,
                                                const SchematicConnection& connection) {
    if (!connection.port_name.empty()) {
        const auto found = std::find_if(module.port_details.begin(),
                                        module.port_details.end(),
                                        [&](const SchematicPort& port) {
                                            return port.name == connection.port_name;
                                        });
        if (found != module.port_details.end()) {
            return &*found;
        }
    }
    if (connection.port_index >= 0 &&
        static_cast<size_t>(connection.port_index) < module.port_details.size()) {
        return &module.port_details[static_cast<size_t>(connection.port_index)];
    }
    return nullptr;
}

bool sameParseRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

bool rangeContainsRange(const ParseRange& outer, const ParseRange& inner) {
    if (outer.start_line > inner.start_line || outer.end_line < inner.end_line) {
        return false;
    }
    if (outer.start_line == inner.start_line && outer.start_character > inner.start_character) {
        return false;
    }
    if (outer.end_line == inner.end_line && outer.end_character < inner.end_character) {
        return false;
    }
    return true;
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

bool isSimpleSystemVerilogIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto is_start = [](unsigned char ch) {
        return std::isalpha(ch) != 0 || ch == '_' || ch == '$';
    };
    const auto is_continue = [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '$';
    };
    if (!is_start(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [&](char ch) {
        return is_continue(static_cast<unsigned char>(ch));
    });
}

std::optional<std::string> symbolIdForRange(const DesignGraphContext& context,
                                            std::string_view uri,
                                            const ParseRange& range) {
    const auto ranges_it = context.symbol_ranges_by_uri.find(std::string(uri));
    if (ranges_it == context.symbol_ranges_by_uri.end()) {
        return std::nullopt;
    }
    std::optional<DesignGraphRangeSymbol> best;
    for (const auto& candidate : ranges_it->second) {
        if (!rangeContainsRange(range, candidate.range)) {
            continue;
        }
        if (!best.has_value() || locationLess(SemanticLocation{.uri = std::string(uri),
                                                               .range = candidate.range},
                                              SemanticLocation{.uri = std::string(uri),
                                                               .range = best->range})) {
            best = candidate;
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    return best->stable_id;
}

std::optional<std::string> symbolIdForNameInRange(const DesignGraphContext& context,
                                                  std::string_view uri,
                                                  const ParseRange& scope_range,
                                                  std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::optional<std::string> best_id;
    std::optional<SemanticLocation> best_location;
    for (const auto& [stable_id, symbol] : context.symbols_by_id) {
        const auto& identity = symbol.identity;
        if (identity.name != name || identity.location.uri != uri ||
            !rangeContainsRange(scope_range, identity.location.range)) {
            continue;
        }
        if (!best_location.has_value() || locationLess(identity.location, *best_location) ||
            (sameParseRange(identity.location.range, best_location->range) && stable_id < *best_id)) {
            best_id = stable_id;
            best_location = identity.location;
        }
    }
    return best_id;
}

std::optional<std::string> symbolIdForNameInModule(const DesignGraphContext& context,
                                                   std::string_view uri,
                                                   const ModuleDefinition& module,
                                                   const SchematicPort& port) {
    if (auto id = symbolIdForRange(context, uri, port.selection_range)) {
        return id;
    }
    const auto name = std::string_view(port.name);
    if (name.empty()) {
        return std::nullopt;
    }
    return symbolIdForNameInRange(context, uri, module.range, name);
}

std::optional<std::string> symbolIdForPortFromAssignments(const DesignGraphContext& context,
                                                          std::string_view uri,
                                                          std::string_view port_name,
                                                          bool output_port) {
    const auto edges_it = context.assignment_edges_by_uri.find(std::string(uri));
    if (edges_it == context.assignment_edges_by_uri.end()) {
        return std::nullopt;
    }
    for (const auto& edge : edges_it->second) {
        const auto& stable_id = output_port ? edge.from_symbol_id : edge.to_symbol_id;
        const auto symbol_it = context.symbols_by_id.find(stable_id);
        if (symbol_it != context.symbols_by_id.end() &&
            symbol_it->second.identity.name == port_name &&
            symbol_it->second.identity.location.uri == uri) {
            return stable_id;
        }
    }
    return std::nullopt;
}

std::vector<SnapshotAssignmentEdge> coneEdgesForContext(const DesignGraphContext& context) {
    std::vector<SnapshotAssignmentEdge> edges;
    for (const auto& [_, document_edges] : context.assignment_edges_by_uri) {
        edges.insert(edges.end(), document_edges.begin(), document_edges.end());
    }

    std::set<std::string> emitted;
    for (const auto& [parent_name, parent_signature] : context.module_signatures_by_name) {
        const auto parent_uri = parent_signature.uri;
        if (parent_uri.empty()) {
            continue;
        }
        for (const auto& cell : parent_signature.schematic.cells) {
            const auto child_signature_it = context.module_signatures_by_name.find(cell.type);
            if (child_signature_it == context.module_signatures_by_name.end()) {
                continue;
            }
            const auto& child_signature = child_signature_it->second;
            if (child_signature.uri.empty()) {
                continue;
            }
            for (const auto& connection : cell.connections) {
                const auto* child_port = findModulePortByConnection(child_signature.definition, connection);
                if (child_port == nullptr) {
                    continue;
                }
                auto parent_signal_id = symbolIdForRange(context, parent_uri, connection.range);
                if (!parent_signal_id.has_value() &&
                    isSimpleSystemVerilogIdentifier(connection.signal)) {
                    parent_signal_id = symbolIdForNameInRange(context,
                                                              parent_uri,
                                                              parent_signature.definition.range,
                                                              connection.signal);
                }
                auto child_port_id = symbolIdForPortFromAssignments(context,
                                                                    child_signature.uri,
                                                                    child_port->name,
                                                                    child_port->direction == "output");
                if (!child_port_id.has_value()) {
                    child_port_id = symbolIdForNameInModule(context,
                                                            child_signature.uri,
                                                            child_signature.definition,
                                                            *child_port);
                }
                if (!parent_signal_id.has_value() || !child_port_id.has_value()) {
                    continue;
                }

                std::string from_symbol_id;
                std::string to_symbol_id;
                if (child_port->direction == "output") {
                    from_symbol_id = *parent_signal_id;
                    to_symbol_id = *child_port_id;
                }
                else {
                    from_symbol_id = *child_port_id;
                    to_symbol_id = *parent_signal_id;
                }
                const auto key = from_symbol_id + "\n" + to_symbol_id + "\n" + parent_name + "\n" +
                                 cell.name + "\n" + child_port->name;
                if (!emitted.insert(key).second) {
                    continue;
                }
                edges.push_back(SnapshotAssignmentEdge{
                    .from_symbol_id = std::move(from_symbol_id),
                    .to_symbol_id = std::move(to_symbol_id),
                    .location = SemanticLocation{.uri = parent_uri, .range = connection.range},
                    .expression_location = SemanticLocation{.uri = parent_uri, .range = connection.range},
                    .expression = connection.signal.empty() ? child_port->name : connection.signal});
            }
        }
    }
    return edges;
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

std::vector<SemanticSchematicNet> buildSchematicNets(const ModuleSchematic& schematic,
                                                     const DesignGraphContext& context) {
    std::map<std::string, SemanticSchematicNet> nets;
    const auto ensure_net = [&](std::string_view signal) -> SemanticSchematicNet& {
        auto [it, inserted] = nets.try_emplace(std::string(signal));
        if (inserted) {
            it->second.name = std::string(signal);
        }
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

    const auto signature_it = context.module_signatures_by_name.find(schematic.name);
    const auto& cells = signature_it == context.module_signatures_by_name.end()
                            ? schematic.cells
                            : signature_it->second.schematic.cells;

    for (const auto& cell : cells) {
        if (cell.kind == "interface" && !cell.name.empty()) {
            auto& net = ensure_net(cell.name);
            appendEndpointByDirection(net,
                                      "interface",
                                      SemanticSchematicEndpoint{.node_id = cell.id,
                                                                .port_name = "interface"});
        }

        const auto target_it = cell.kind == "module"
                                   ? context.module_signatures_by_name.find(cell.type)
                                   : context.module_signatures_by_name.end();
        for (const auto& connection : cell.connections) {
            if (connection.signal.empty()) {
                continue;
            }

            std::string port_name = connection.port_name;
            std::string direction;
            if (target_it != context.module_signatures_by_name.end()) {
                const auto& target_schematic = target_it->second.schematic;
                const auto* port = !port_name.empty()
                                       ? findSchematicPortByName(target_schematic, port_name)
                                       : findSchematicPortByIndex(target_schematic, connection.port_index);
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

void appendUniqueMessage(std::vector<std::string>& messages, std::string message) {
    if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
        messages.push_back(std::move(message));
    }
}

int callHierarchyRangeLengthScore(const ParseRange& range) {
    return (range.end_line - range.start_line) * 100000 +
           (range.end_character - range.start_character);
}

SemanticCallHierarchyItem callHierarchyItemFor(const SnapshotModuleCallHierarchyItem& item,
                                               std::uint64_t generation) {
    return SemanticCallHierarchyItem{.name = item.name,
                                     .kind = item.kind == "interface" ? 11 : 2,
                                     .detail = item.kind,
                                     .uri = item.uri,
                                     .range = item.range,
                                     .selection_range = item.selection_range,
                                     .opaque_id = item.id,
                                     .generation = generation};
}

std::optional<std::string> callHierarchyItemIdAt(const SnapshotModuleCallEdgeIndex& index,
                                                  std::string_view uri,
                                                  int line,
                                                  int character,
                                                  size_t& scanned_ranges) {
    const auto ranges_it = index.items_by_uri.find(std::string(uri));
    if (ranges_it == index.items_by_uri.end()) {
        return std::nullopt;
    }

    const SnapshotModuleCallHierarchyRange* best = nullptr;
    for (const auto& candidate : ranges_it->second) {
        ++scanned_ranges;
        if (!parseRangeContainsPosition(candidate.range, line, character)) {
            continue;
        }
        if (best == nullptr || callHierarchyRangeLengthScore(candidate.range) <
                                   callHierarchyRangeLengthScore(best->range) ||
            (callHierarchyRangeLengthScore(candidate.range) ==
                 callHierarchyRangeLengthScore(best->range) &&
             candidate.item_id < best->item_id)) {
            best = &candidate;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<std::string>{best->item_id};
}

std::string hierarchyMemoKey(std::string_view module_name,
                             int depth,
                             int max_depth,
                             const std::vector<std::string>& stack) {
    std::string key;
    key.reserve(module_name.size() + stack.size() * 16 + 32);
    key.append(module_name);
    key.push_back('\n');
    key.append(std::to_string(depth));
    key.push_back('/');
    key.append(std::to_string(max_depth));
    for (const auto& entry : stack) {
        key.push_back('\n');
        key.append(entry);
    }
    return key;
}

SemanticHierarchyNode instantiateHierarchyTemplate(SemanticHierarchyNode node,
                                                   const ModuleInstantiation* instance) {
    if (instance != nullptr) {
        node.instance_name = instance->instance_name;
        node.instance_range = instance->range;
        node.instance_selection_range = instance->selection_range;
        node.module_selection_range = instance->module_selection_range;
    }
    return node;
}

} // namespace

SemanticModuleHierarchyResult moduleHierarchy(const DesignGraphContext& context,
                                              std::optional<std::string_view> module_name,
                                              int max_depth) {
    SemanticModuleHierarchyResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::vector<std::string> root_names;
    if (module_name.has_value()) {
        root_names.push_back(std::string(*module_name));
    }
    else if (!context.top_modules.empty()) {
        root_names = context.top_modules;
    }
    else {
        root_names = inferredHierarchyRootNames(context.modules_by_name);
    }

    if (root_names.empty()) {
        result.unresolved = true;
        result.messages.push_back("No module definitions are indexed in the design snapshot.");
        return result;
    }

    std::unordered_map<std::string, SemanticHierarchyNode> hierarchy_memo;

    const auto build_node = [&](const auto& self,
                                std::string_view current_name,
                                const ModuleInstantiation* instance,
                                std::string_view instance_uri,
                                std::vector<std::string>& stack,
                                int depth) -> SemanticHierarchyNode {
        const auto memo_key = hierarchyMemoKey(current_name, depth, max_depth, stack);
        if (const auto memo_it = hierarchy_memo.find(memo_key); memo_it != hierarchy_memo.end()) {
            return instantiateHierarchyTemplate(memo_it->second, instance);
        }

        const auto definition_it = context.modules_by_name.find(std::string(current_name));
        if (definition_it == context.modules_by_name.end()) {
            result.partial = true;
            SemanticHierarchyNode node;
            node.module_name = std::string(current_name);
            node.kind = "module";
            node.unresolved = true;
            if (instance != nullptr) {
                node.instance_name = instance->instance_name;
                node.instance_range = instance->range;
                node.instance_selection_range = instance->selection_range;
                node.module_selection_range = instance->module_selection_range;
            }
            appendUniqueMessage(result.messages,
                                "Unresolved module '" + std::string(current_name) +
                                    "' in design hierarchy.");
            hierarchy_memo.emplace(memo_key, instantiateHierarchyTemplate(node, nullptr));
            return node;
        }

        const auto& definition = definition_it->second;
        const auto uri_it = context.module_uris_by_name.find(definition.name);
        const auto definition_uri = uri_it == context.module_uris_by_name.end()
                                        ? std::string{}
                                        : uri_it->second;
        const auto is_cycle = std::find(stack.begin(), stack.end(), definition.name) != stack.end();
        SemanticHierarchyNode node;
        node.module_name = definition.name;
        node.kind = definition.kind;
        node.location = SemanticLocation{.uri = definition_uri, .range = definition.range};
        node.selection_range = definition.selection_range;
        node.unresolved = false;
        node.cycle = is_cycle;

        if (instance != nullptr) {
            node.instance_name = instance->instance_name;
            node.instance_range = instance->range;
            node.instance_selection_range = instance->selection_range;
            node.module_selection_range = instance->module_selection_range;
            (void)instance_uri;
        }

        if (is_cycle) {
            result.partial = true;
            appendUniqueMessage(result.messages,
                                "Cycle detected while expanding module '" + definition.name + "'.");
            hierarchy_memo.emplace(memo_key, instantiateHierarchyTemplate(node, nullptr));
            return node;
        }
        if (depth >= max_depth) {
            node.truncated = true;
            result.truncated = true;
            result.partial = true;
            appendUniqueMessage(result.messages, "Module hierarchy expansion reached maxDepth.");
            hierarchy_memo.emplace(memo_key, instantiateHierarchyTemplate(node, nullptr));
            return node;
        }

        stack.push_back(definition.name);
        for (const auto& child_instance : definition.instances) {
            node.children.push_back(self(self,
                                         child_instance.module_name,
                                         &child_instance,
                                         definition_uri,
                                         stack,
                                         depth + 1));
        }
        stack.pop_back();
        hierarchy_memo.emplace(memo_key, instantiateHierarchyTemplate(node, nullptr));
        return node;
    };

    for (const auto& root_name : root_names) {
        std::vector<std::string> stack;
        result.roots.push_back(build_node(build_node, root_name, nullptr, {}, stack, 0));
    }
    return result;
}

SemanticSchematicResult schematic(const DesignGraphContext& context,
                                  std::optional<std::string_view> module_name,
                                  int max_depth) {
    SemanticSchematicResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::optional<std::string> root_name;
    if (module_name.has_value()) {
        root_name = std::string(*module_name);
    }
    else if (!context.top_modules.empty()) {
        root_name = context.top_modules.front();
    }
    else {
        const auto inferred_roots = inferredHierarchyRootNames(context.modules_by_name);
        if (!inferred_roots.empty()) {
            root_name = inferred_roots.front();
        }
        if (!root_name.has_value() && !context.modules_by_name.empty()) {
            root_name = context.modules_by_name.begin()->first;
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
    const auto collect = [&](const auto& self, std::string_view current_name, int depth) -> void {
        const auto current = std::string(current_name);
        if (emitted.contains(current)) {
            return;
        }
        const auto signature_it = context.module_signatures_by_name.find(current);
        if (signature_it == context.module_signatures_by_name.end()) {
            result.partial = true;
            appendUniqueMessage(result.messages,
                                "No schematic data found for module '" + current + "'.");
            return;
        }
        const auto& schematic = signature_it->second.schematic;
        const auto& schematic_uri = signature_it->second.uri;
        emitted.insert(current);
        result.modules.push_back(SemanticSchematicModuleView{
            .module = SemanticSchematicModule{.id = schematic.name,
                                              .name = schematic.name,
                                              .uri = schematic_uri,
                                              .range = schematic.range,
                                              .selection_range = schematic.selection_range,
                                              .ports = schematic.ports,
                                              .cells = schematic.cells},
            .nets = buildSchematicNets(schematic, context)});

        if (depth >= max_depth) {
            result.truncated = true;
            result.partial = true;
            appendUniqueMessage(result.messages, "Schematic expansion reached maxDepth.");
            return;
        }
        if (std::find(stack.begin(), stack.end(), current) != stack.end()) {
            result.partial = true;
            appendUniqueMessage(result.messages,
                                "Cycle detected while expanding schematic module '" + current + "'.");
            return;
        }

        const auto definition_it = context.modules_by_name.find(current);
        if (definition_it == context.modules_by_name.end()) {
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

SemanticCallHierarchyPrepareResult prepareCallHierarchy(const DesignGraphContext& context,
                                                        std::string_view document_uri,
                                                        int line,
                                                        int character) {
    SemanticCallHierarchyPrepareResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto item_id = callHierarchyItemIdAt(context.module_call_edge_index,
                                               document_uri,
                                               line,
                                               character,
                                               result.scanned_edge_count);
    if (item_id.has_value()) {
        const auto item_it = context.module_call_edge_index.items_by_id.find(*item_id);
        if (item_it != context.module_call_edge_index.items_by_id.end()) {
            result.items.push_back(callHierarchyItemFor(item_it->second, context.generation));
            return result;
        }
    }

    result.unresolved = true;
    result.messages.push_back("No indexed module call hierarchy item at position.");
    return result;
}

SemanticCallHierarchyCallsResult incomingCalls(const DesignGraphContext& context,
                                               const SemanticCallHierarchyItem& item) {
    SemanticCallHierarchyCallsResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    if (item.opaque_id.empty() || item.generation != context.generation) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy item identity is missing or stale.");
        return result;
    }

    const auto incoming_it = context.module_call_edge_index.edges_by_callee_item_id.find(item.opaque_id);
    if (incoming_it == context.module_call_edge_index.edges_by_callee_item_id.end()) {
        if (!context.module_call_edge_index.items_by_id.contains(item.opaque_id)) {
            result.unresolved = true;
            result.messages.push_back("Call hierarchy target module is not indexed.");
        }
        return result;
    }

    for (const auto edge_index : incoming_it->second) {
        ++result.scanned_edge_count;
        const auto& edge = context.module_call_edge_index.edges[edge_index];
        const auto caller_it = context.module_call_edge_index.items_by_id.find(edge.caller_item_id);
        if (caller_it == context.module_call_edge_index.items_by_id.end()) {
            continue;
        }
        result.calls.push_back(SemanticCallHierarchyCall{
            .item = callHierarchyItemFor(caller_it->second, context.generation),
            .from_ranges = {edge.selection_range}});
    }
    return result;
}

SemanticCallHierarchyCallsResult outgoingCalls(const DesignGraphContext& context,
                                               const SemanticCallHierarchyItem& item) {
    SemanticCallHierarchyCallsResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    if (item.opaque_id.empty() || item.generation != context.generation) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy item identity is missing or stale.");
        return result;
    }

    const auto outgoing_it = context.module_call_edge_index.edges_by_caller_item_id.find(item.opaque_id);
    if (outgoing_it == context.module_call_edge_index.edges_by_caller_item_id.end()) {
        if (!context.module_call_edge_index.items_by_id.contains(item.opaque_id)) {
            result.unresolved = true;
            result.messages.push_back("Call hierarchy source module is not indexed.");
        }
        return result;
    }

    for (const auto edge_index : outgoing_it->second) {
        ++result.scanned_edge_count;
        const auto& edge = context.module_call_edge_index.edges[edge_index];
        const auto callee_it = context.module_call_edge_index.items_by_id.find(edge.callee_item_id);
        if (callee_it == context.module_call_edge_index.items_by_id.end()) {
            continue;
        }
        result.calls.push_back(SemanticCallHierarchyCall{
            .item = callHierarchyItemFor(callee_it->second, context.generation),
            .from_ranges = {edge.selection_range}});
    }
    return result;
}

SemanticConeTrace backwardCone(const DesignGraphContext& context,
                               std::string_view document_uri,
                               const SemanticLookupResult& lookup,
                               size_t max_results) {
    (void)document_uri;
    SemanticConeTrace trace;
    trace.generation = lookup.generation;
    trace.messages = lookup.messages;
    trace.unresolved = lookup.unresolved;
    if (!lookup.symbol.has_value()) {
        if (trace.messages.empty()) {
            trace.messages.push_back("No signal symbol was found at the requested position.");
        }
        return trace;
    }
    if (!context.snapshot_available) {
        trace.unresolved = true;
        trace.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return trace;
    }

    const auto cone_edges = coneEdgesForContext(context);
    if (cone_edges.empty()) {
        trace.messages.push_back(
            "No AST assignment edges or instance port edges are indexed for the current snapshot.");
        return trace;
    }

    const auto mark_truncated = [&]() {
        if (!trace.truncated) {
            trace.truncated = true;
            trace.partial = true;
            trace.messages.push_back("Backward cone reached the result cap.");
        }
    };
    const auto reached_cap = [&]() {
        return max_results > 0 && (trace.nodes.size() >= max_results ||
                                   trace.edges.size() >= max_results);
    };
    const auto has_node = [&](const std::string& stable_id) {
        if (std::find_if(trace.nodes.begin(), trace.nodes.end(), [&](const SemanticConeNode& node) {
                return node.id == stable_id;
            }) != trace.nodes.end()) {
            return true;
        }
        return false;
    };
    const auto append_node = [&](const std::string& stable_id) {
        if (has_node(stable_id)) {
            return true;
        }
        if (reached_cap()) {
            mark_truncated();
            return false;
        }
        const auto symbol_it = context.symbols_by_id.find(stable_id);
        if (symbol_it == context.symbols_by_id.end()) {
            return true;
        }
        trace.nodes.push_back(SemanticConeNode{.id = stable_id,
                                               .name = symbol_it->second.identity.name,
                                               .location = symbol_it->second.identity.location,
                                               .bit_width = std::nullopt});
        return true;
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
        if (!context.symbols_by_id.contains(current_id)) {
            continue;
        }
        if (reached_cap()) {
            mark_truncated();
            break;
        }

        for (const auto& edge : cone_edges) {
            if (edge.from_symbol_id != current_id) {
                continue;
            }
            if (reached_cap()) {
                mark_truncated();
                break;
            }

            const auto node_available = append_node(edge.to_symbol_id);
            const auto edge_key = edge.from_symbol_id + "\n" + edge.to_symbol_id + "\n" +
                                  std::to_string(edge.location.range.start_line) + ":" +
                                  std::to_string(edge.location.range.start_character);
            if (node_available && emitted_edges.insert(edge_key).second) {
                if (max_results > 0 && trace.edges.size() >= max_results) {
                    mark_truncated();
                    break;
                }
                trace.edges.push_back(SemanticConeEdge{.from_symbol_id = edge.from_symbol_id,
                                                       .to_symbol_id = edge.to_symbol_id,
                                                       .location = edge.location,
                                                       .expression = edge.expression});
            }
            if (node_available && !visited.contains(edge.to_symbol_id) && !reached_cap()) {
                pending.push_back(edge.to_symbol_id);
            }
        }

        if (reached_cap()) {
            mark_truncated();
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

} // namespace pristine::analysis::semantic
