#include "DesignGraphProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <map>
#include <set>

namespace pristine::analysis::semantic {
namespace {

std::optional<std::string> firstUninstantiatedModuleName(
    const std::unordered_map<std::string, ModuleDefinition>& modules_by_name) {
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

std::vector<SemanticSchematicNet> buildSchematicNets(const ModuleSchematic& schematic,
                                                     const DesignGraphContext& context) {
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
                                   ? context.schematics_by_name.find(cell.type)
                                   : context.schematics_by_name.end();
        for (const auto& connection : cell.connections) {
            if (connection.signal.empty()) {
                continue;
            }

            std::string port_name = connection.port_name;
            std::string direction;
            if (target_it != context.schematics_by_name.end()) {
                const auto* port = !port_name.empty()
                                       ? findSchematicPortByName(target_it->second, port_name)
                                       : findSchematicPortByIndex(target_it->second, connection.port_index);
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

SemanticCallHierarchyItem callHierarchyItemFor(const ModuleDefinition& definition,
                                               const std::string& uri) {
    return SemanticCallHierarchyItem{.name = definition.name,
                                     .kind = definition.kind == "interface" ? 11 : 2,
                                     .detail = definition.kind,
                                     .uri = uri,
                                     .range = definition.range,
                                     .selection_range = definition.selection_range};
}

} // namespace

SemanticModuleHierarchyResult moduleHierarchy(const DesignGraphContext& context,
                                              std::optional<std::string_view> module_name,
                                              int max_depth) {
    SemanticModuleHierarchyResult result{.generation = context.generation};
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
    else if (const auto inferred = firstUninstantiatedModuleName(context.modules_by_name)) {
        root_names.push_back(*inferred);
    }

    if (root_names.empty()) {
        result.unresolved = true;
        result.messages.push_back("No module definitions are indexed in the design snapshot.");
        return result;
    }

    const auto build_node = [&](const auto& self,
                                std::string_view current_name,
                                const ModuleInstantiation* instance,
                                std::string_view instance_uri,
                                std::vector<std::string>& stack,
                                int depth) -> SemanticHierarchyNode {
        const auto definition_it = context.modules_by_name.find(std::string(current_name));
        if (definition_it == context.modules_by_name.end()) {
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
        const auto uri_it = context.module_uris_by_name.find(definition.name);
        const auto definition_uri = uri_it == context.module_uris_by_name.end()
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
            (void)instance_uri;
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
            node.children.push_back(self(self,
                                         child_instance.module_name,
                                         &child_instance,
                                         definition_uri,
                                         stack,
                                         depth + 1));
        }
        stack.pop_back();
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
    SemanticSchematicResult result{.generation = context.generation};
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
        root_name = firstUninstantiatedModuleName(context.modules_by_name);
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
        const auto schematic_it = context.schematics_by_name.find(current);
        if (schematic_it == context.schematics_by_name.end()) {
            result.partial = true;
            result.messages.push_back("No schematic data found for module '" + current + "'.");
            return;
        }

        const auto uri_it = context.schematic_uris_by_name.find(current);
        const auto schematic_uri = uri_it == context.schematic_uris_by_name.end()
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
            .nets = buildSchematicNets(schematic_it->second, context)});

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
    SemanticCallHierarchyPrepareResult result{.generation = context.generation};
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& entry : context.module_entries) {
        const auto& definition = entry.definition;
        if (entry.uri != document_uri) {
            continue;
        }
        if (parseRangeContainsPosition(definition.selection_range, line, character)) {
            result.items.push_back(callHierarchyItemFor(definition, entry.uri));
            return result;
        }
        for (const auto& instance : definition.instances) {
            if (!parseRangeContainsPosition(instance.module_selection_range, line, character) &&
                !parseRangeContainsPosition(instance.selection_range, line, character)) {
                continue;
            }
            const auto target_it = context.modules_by_name.find(instance.module_name);
            if (target_it == context.modules_by_name.end()) {
                result.unresolved = true;
                result.messages.push_back("Call hierarchy target module is unresolved.");
                return result;
            }
            const auto target_uri_it = context.module_uris_by_name.find(target_it->first);
            result.items.push_back(callHierarchyItemFor(
                target_it->second,
                target_uri_it == context.module_uris_by_name.end() ? std::string{} : target_uri_it->second));
            return result;
        }
        if (parseRangeContainsPosition(definition.range, line, character)) {
            result.items.push_back(callHierarchyItemFor(definition, entry.uri));
            return result;
        }
    }

    result.unresolved = true;
    result.messages.push_back("No design hierarchy item at position.");
    return result;
}

SemanticCallHierarchyCallsResult incomingCalls(const DesignGraphContext& context,
                                               const SemanticCallHierarchyItem& item) {
    SemanticCallHierarchyCallsResult result{.generation = context.generation};
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto target_entry_it = std::find_if(context.module_entries.begin(),
                                              context.module_entries.end(),
                                              [&](const DesignGraphModuleEntry& entry) {
                                                  return entry.definition.name == item.name &&
                                                         entry.uri == item.uri &&
                                                         sameParseRange(entry.definition.selection_range,
                                                                        item.selection_range);
                                              });
    if (target_entry_it == context.module_entries.end()) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy target module is not indexed.");
        return result;
    }

    for (const auto& caller_entry : context.module_entries) {
        const auto& caller = caller_entry.definition;
        for (const auto& instance : caller.instances) {
            if (instance.module_name != target_entry_it->definition.name) {
                continue;
            }
            result.calls.push_back(SemanticCallHierarchyCall{
                .item = callHierarchyItemFor(caller, caller_entry.uri),
                .from_ranges = {instance.module_selection_range}});
        }
    }
    return result;
}

SemanticCallHierarchyCallsResult outgoingCalls(const DesignGraphContext& context,
                                               const SemanticCallHierarchyItem& item) {
    SemanticCallHierarchyCallsResult result{.generation = context.generation};
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto source_entry_it = std::find_if(context.module_entries.begin(),
                                              context.module_entries.end(),
                                              [&](const DesignGraphModuleEntry& entry) {
                                                  return entry.definition.name == item.name &&
                                                         entry.uri == item.uri &&
                                                         sameParseRange(entry.definition.selection_range,
                                                                        item.selection_range);
                                              });
    if (source_entry_it == context.module_entries.end()) {
        result.unresolved = true;
        result.messages.push_back("Call hierarchy source module is not indexed.");
        return result;
    }

    for (const auto& instance : source_entry_it->definition.instances) {
        const auto target_it = context.modules_by_name.find(instance.module_name);
        if (target_it == context.modules_by_name.end()) {
            continue;
        }
        const auto target_uri_it = context.module_uris_by_name.find(target_it->first);
        result.calls.push_back(SemanticCallHierarchyCall{
            .item = callHierarchyItemFor(
                target_it->second,
                target_uri_it == context.module_uris_by_name.end() ? std::string{} : target_uri_it->second),
            .from_ranges = {instance.module_selection_range}});
    }
    return result;
}

} // namespace pristine::analysis::semantic
