#include "DesignGraphProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
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

std::string directionLabel(SnapshotGraphPortDirection direction) {
    switch (direction) {
    case SnapshotGraphPortDirection::Input:
        return "input";
    case SnapshotGraphPortDirection::Output:
        return "output";
    case SnapshotGraphPortDirection::Inout:
        return "inout";
    case SnapshotGraphPortDirection::Ref:
        return "ref";
    case SnapshotGraphPortDirection::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string coneEdgeKindLabel(SnapshotConeEdgeKind kind) {
    switch (kind) {
    case SnapshotConeEdgeKind::Assignment:
        return "assignment";
    case SnapshotConeEdgeKind::InstancePort:
        return "instancePort";
    case SnapshotConeEdgeKind::InterfaceMember:
        return "interfaceMember";
    case SnapshotConeEdgeKind::ParameterOverride:
        return "parameterOverride";
    case SnapshotConeEdgeKind::ControlDependency:
        return "controlDependency";
    case SnapshotConeEdgeKind::PrimitiveCell:
        return "primitiveCell";
    case SnapshotConeEdgeKind::AssertionSample:
        return "assertionSample";
    default:
        return "assignment";
    }
}

std::string coneSourceRoleLabel(SnapshotConeSourceRole role) {
    switch (role) {
    case SnapshotConeSourceRole::Data:
        return "data";
    case SnapshotConeSourceRole::Control:
        return "control";
    case SnapshotConeSourceRole::Sampled:
        return "sampled";
    case SnapshotConeSourceRole::Clock:
        return "clock";
    case SnapshotConeSourceRole::Disable:
        return "disable";
    case SnapshotConeSourceRole::Abort:
        return "abort";
    }
    return "data";
}

std::string coneControlOriginLabel(SnapshotConeControlOrigin origin) {
    switch (origin) {
    case SnapshotConeControlOrigin::None:
        return {};
    case SnapshotConeControlOrigin::ConditionalStatement:
        return "if";
    case SnapshotConeControlOrigin::CaseStatement:
        return "case";
    case SnapshotConeControlOrigin::TernaryCondition:
        return "ternary";
    case SnapshotConeControlOrigin::DynamicSelect:
        return "dynamicSelect";
    case SnapshotConeControlOrigin::PrimitiveControl:
        return "primitiveControl";
    case SnapshotConeControlOrigin::EventControl:
        return "eventControl";
    case SnapshotConeControlOrigin::EventIff:
        return "eventIff";
    case SnapshotConeControlOrigin::AssertionClock:
        return "assertionClock";
    case SnapshotConeControlOrigin::AssertionDisable:
        return "assertionDisable";
    case SnapshotConeControlOrigin::AssertionAbort:
        return "assertionAbort";
    }
    return {};
}

std::string coneEventKindLabel(SnapshotConeEventKind kind) {
    switch (kind) {
    case SnapshotConeEventKind::None:
        return {};
    case SnapshotConeEventKind::Any:
        return "any";
    case SnapshotConeEventKind::PosEdge:
        return "posedge";
    case SnapshotConeEventKind::NegEdge:
        return "negedge";
    case SnapshotConeEventKind::BothEdges:
        return "bothEdges";
    case SnapshotConeEventKind::EventList:
        return "eventList";
    case SnapshotConeEventKind::Implicit:
        return "implicit";
    case SnapshotConeEventKind::Repeated:
        return "repeated";
    case SnapshotConeEventKind::Unsupported:
        return "unsupported";
    }
    return {};
}

std::string coneSliceKindLabel(SnapshotConeSliceKind kind) {
    switch (kind) {
    case SnapshotConeSliceKind::Whole:
        return "whole";
    case SnapshotConeSliceKind::ElementSelect:
        return "elementSelect";
    case SnapshotConeSliceKind::RangeSelect:
        return "rangeSelect";
    case SnapshotConeSliceKind::Concatenation:
        return "concatenation";
    case SnapshotConeSliceKind::MemberAccess:
        return "memberAccess";
    case SnapshotConeSliceKind::DynamicSelect:
        return "dynamicSelect";
    }
    return "whole";
}

std::string coneSlicePrecisionLabel(SnapshotConeSlicePrecision precision) {
    switch (precision) {
    case SnapshotConeSlicePrecision::Whole:
        return "whole";
    case SnapshotConeSlicePrecision::Exact:
        return "exact";
    case SnapshotConeSlicePrecision::Aggregate:
        return "aggregate";
    case SnapshotConeSlicePrecision::Dynamic:
        return "dynamic";
    case SnapshotConeSlicePrecision::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

std::optional<SemanticConeSlice> coneSlice(const SnapshotConeSliceFact& fact) {
    if (fact.precision == SnapshotConeSlicePrecision::Whole) {
        return std::nullopt;
    }
    return SemanticConeSlice{.precision = coneSlicePrecisionLabel(fact.precision),
                             .msb = fact.msb,
                             .lsb = fact.lsb};
}

std::string coneSliceKey(const SnapshotConeSliceFact& fact) {
    return coneSlicePrecisionLabel(fact.precision) + ":" +
           (fact.msb ? std::to_string(*fact.msb) : std::string{}) + ":" +
           (fact.lsb ? std::to_string(*fact.lsb) : std::string{});
}

bool exactSlicesIntersect(const SnapshotConeSliceFact& lhs, const SnapshotConeSliceFact& rhs) {
    if (lhs.precision != SnapshotConeSlicePrecision::Exact ||
        rhs.precision != SnapshotConeSlicePrecision::Exact || !lhs.msb || !lhs.lsb || !rhs.msb || !rhs.lsb) {
        return true;
    }
    const auto lhs_low = std::min(*lhs.msb, *lhs.lsb);
    const auto lhs_high = std::max(*lhs.msb, *lhs.lsb);
    const auto rhs_low = std::min(*rhs.msb, *rhs.lsb);
    const auto rhs_high = std::max(*rhs.msb, *rhs.lsb);
    return lhs_low <= rhs_high && rhs_low <= lhs_high;
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

void appendUniqueMessage(std::vector<std::string>& messages, std::string message);

struct SchematicNetBuildResult {
    std::vector<SemanticSchematicNet> nets;
    std::vector<std::string> messages;
    size_t typed_connection_fact_lookups = 0;
    size_t source_part_scans = 0;
    size_t partial_connection_facts = 0;
    size_t typed_cell_pin_fact_lookups = 0;
    size_t cell_pin_scans = 0;
    size_t partial_cell_pin_facts = 0;
    size_t interface_member_connection_fact_lookups = 0;
    size_t interface_member_connection_scans = 0;
    size_t partial_interface_member_connection_facts = 0;
    bool partial = false;
    size_t primitive_cell_pin_fact_lookups = 0;
    size_t primitive_control_pin_facts = 0;
};

bool isStaticSchematicSource(const SnapshotGraphConnectionBindingFact::SourcePart& part) {
    return !part.unresolved && !part.source_symbol_id.empty() &&
           (part.source_slice.precision == SnapshotConeSlicePrecision::Whole ||
            part.source_slice.precision == SnapshotConeSlicePrecision::Exact);
}

std::string schematicNetKey(const SnapshotGraphConnectionBindingFact::SourcePart& part) {
    return part.source_symbol_id + "\x1f" + coneSliceKey(part.source_slice);
}

std::string stableShortId(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(8, '0');
    for (size_t index = 0; index < result.size(); ++index) {
        result[result.size() - index - 1] = digits[hash & 0xfU];
        hash >>= 4U;
    }
    return result;
}

SchematicNetBuildResult buildSchematicNets(const ModuleSchematic& schematic,
                                           const DesignGraphContext& context) {
    std::map<std::string, SemanticSchematicNet> nets;
    SchematicNetBuildResult result;
    std::unordered_map<std::string, std::string> net_key_by_display;
    const auto ensure_net = [&](std::string key, std::string label) -> SemanticSchematicNet& {
        auto [it, inserted] = nets.try_emplace(std::move(key));
        if (inserted) {
            const auto displayed = net_key_by_display.find(label);
            if (displayed != net_key_by_display.end() && displayed->second != it->first) {
                label += "@" + stableShortId(it->first);
            }
            else {
                net_key_by_display.try_emplace(label, it->first);
            }
            it->second.name = std::move(label);
        }
        return it->second;
    };

    const auto signature_it = context.module_signatures_by_name.find(schematic.name);
    const auto& cells = signature_it == context.module_signatures_by_name.end()
                            ? schematic.cells
                            : signature_it->second.schematic.cells;
    std::unordered_map<std::string, std::string> cell_names_by_id;
    for (const auto& cell : cells) {
        cell_names_by_id.try_emplace(cell.id, cell.name);
    }

    std::unordered_map<std::string, const SchematicPort*> ports_by_symbol_id;
    for (const auto& port : schematic.ports) {
        const auto stable_id = context.binding_index.port_symbol_ids_by_module_port.find(
            schematic.name + "\x1f" + port.name);
        if (stable_id != context.binding_index.port_symbol_ids_by_module_port.end()) {
            ports_by_symbol_id.try_emplace(stable_id->second, &port);
        }
    }

    std::set<std::string> connected_port_ids;
    std::set<std::string> typed_interface_endpoint_ids;
    std::set<std::string> typed_interface_cell_ids;
    const auto interface_facts = context.binding_index.interface_member_connections_by_module.find(schematic.name);
    if (interface_facts != context.binding_index.interface_member_connections_by_module.end()) {
        for (const auto& fact : interface_facts->second) {
            ++result.typed_connection_fact_lookups;
            ++result.interface_member_connection_fact_lookups;
            ++result.interface_member_connection_scans;
            if (fact.unresolved || fact.parent_member_stable_id.empty() ||
                fact.child_member_stable_id.empty()) {
                result.partial = true;
                ++result.partial_connection_facts;
                ++result.partial_interface_member_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No resolved interface/modport member fact for instance '" +
                                        fact.child_instance_name + "'.");
                continue;
            }
            const auto parent_cell = context.binding_index.schematic_cell_ids_by_instance_id.find(
                fact.parent_interface_instance_stable_id);
            const auto child_cell = context.binding_index.schematic_cell_ids_by_instance_id.find(
                fact.child_instance_stable_id);
            if (parent_cell == context.binding_index.schematic_cell_ids_by_instance_id.end() ||
                child_cell == context.binding_index.schematic_cell_ids_by_instance_id.end()) {
                result.partial = true;
                ++result.partial_connection_facts;
                ++result.partial_interface_member_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No typed schematic cell binding for interface/modport member '" +
                                        fact.member_name + "'.");
                continue;
            }
            typed_interface_endpoint_ids.insert(fact.child_endpoint_stable_id);
            typed_interface_cell_ids.insert(parent_cell->second);
            const auto parent_name = cell_names_by_id.find(parent_cell->second);
            const auto display_name = (parent_name == cell_names_by_id.end() || parent_name->second.empty())
                                          ? fact.member_name
                                          : parent_name->second + "." + fact.member_name;
            auto& net = ensure_net(fact.parent_member_stable_id + "\x1fwhole", display_name);
            const auto parent_endpoint = SemanticSchematicEndpoint{.node_id = parent_cell->second,
                                                                     .port_name = fact.member_name};
            const auto child_endpoint = SemanticSchematicEndpoint{
                .node_id = child_cell->second,
                .port_name = fact.child_endpoint_name.empty()
                                 ? fact.member_name
                                 : fact.child_endpoint_name + "." + fact.member_name};
            if (fact.direction == SnapshotGraphPortDirection::Input) {
                net.drivers.push_back(parent_endpoint);
                net.loads.push_back(child_endpoint);
            }
            else if (fact.direction == SnapshotGraphPortDirection::Output) {
                net.drivers.push_back(child_endpoint);
                net.loads.push_back(parent_endpoint);
            }
            else if (fact.direction == SnapshotGraphPortDirection::Inout ||
                     fact.direction == SnapshotGraphPortDirection::Ref) {
                net.drivers.push_back(parent_endpoint);
                net.loads.push_back(parent_endpoint);
                net.drivers.push_back(child_endpoint);
                net.loads.push_back(child_endpoint);
            }
            else {
                result.partial = true;
                ++result.partial_connection_facts;
                ++result.partial_interface_member_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No direction for interface/modport member '" + fact.member_name + "'.");
            }
        }
    }
    const auto typed_facts = context.binding_index.schematic_connections_by_module.find(schematic.name);
    if (typed_facts != context.binding_index.schematic_connections_by_module.end()) {
        for (const auto& fact : typed_facts->second) {
            ++result.typed_connection_fact_lookups;
            if (fact.kind != SnapshotConeEdgeKind::InstancePort) {
                continue;
            }
            const auto endpoint = context.binding_index.endpoints_by_stable_id.find(fact.endpoint_stable_id);
            if (fact.endpoint_stable_id.empty() ||
                endpoint == context.binding_index.endpoints_by_stable_id.end()) {
                result.partial = true;
                ++result.partial_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No AST-backed endpoint binding for instance '" +
                                        fact.instance_name + "'.");
                continue;
            }
            if (endpoint->second.kind == SnapshotGraphEndpointKind::InterfacePort) {
                if (typed_interface_endpoint_ids.contains(fact.endpoint_stable_id)) {
                    continue;
                }
                result.partial = true;
                ++result.partial_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No resolved interface/modport member fact for instance '" +
                                        fact.instance_name + "'.");
                continue;
            }
            if (fact.unresolved || fact.source_parts.empty()) {
                result.partial = true;
                ++result.partial_connection_facts;
                appendUniqueMessage(result.messages,
                                    "No resolved schematic source fact for instance '" +
                                        fact.instance_name + "'.");
                continue;
            }

            for (const auto& part : fact.source_parts) {
                ++result.source_part_scans;
                if (!isStaticSchematicSource(part)) {
                    result.partial = true;
                    ++result.partial_connection_facts;
                    appendUniqueMessage(result.messages,
                                        "Partial schematic connection for instance '" +
                                            fact.instance_name + "'.");
                    continue;
                }
                auto& net = ensure_net(schematicNetKey(part), fact.display_label);
                appendEndpointByDirection(net,
                                          directionLabel(fact.endpoint_direction),
                                          SemanticSchematicEndpoint{.node_id = fact.instance_name,
                                                                    .port_name = fact.endpoint_name});
                if (const auto port = ports_by_symbol_id.find(part.source_symbol_id);
                    port != ports_by_symbol_id.end()) {
                    appendEndpointByDirection(net,
                                              port->second->direction,
                                              SemanticSchematicEndpoint{.node_id =
                                                                            std::string("$port:") +
                                                                                port->second->name,
                                                                        .port_name = port->second->name},
                                              true);
                    connected_port_ids.insert(part.source_symbol_id);
                }
            }
        }
    }

    std::set<std::string> typed_cell_ids;
    const auto typed_cell_pins = context.binding_index.schematic_cell_pins_by_module.find(schematic.name);
    if (typed_cell_pins != context.binding_index.schematic_cell_pins_by_module.end()) {
        const auto static_slice = [](const SnapshotConeSliceFact& slice) {
            return slice.precision == SnapshotConeSlicePrecision::Whole ||
                   slice.precision == SnapshotConeSlicePrecision::Exact;
        };
        const auto cell_net_key = [](std::string_view symbol_id, const SnapshotConeSliceFact& slice) {
            return std::string(symbol_id) + "\x1f" + coneSliceKey(slice);
        };
        const auto append_port_endpoint = [&](SemanticSchematicNet& net, std::string_view symbol_id) {
            const auto port = ports_by_symbol_id.find(std::string(symbol_id));
            if (port == ports_by_symbol_id.end()) {
                return;
            }
            appendEndpointByDirection(net,
                                      port->second->direction,
                                      SemanticSchematicEndpoint{.node_id = std::string("$port:") +
                                                                            port->second->name,
                                                                .port_name = port->second->name},
                                      true);
            connected_port_ids.insert(std::string(symbol_id));
        };
        for (const auto& fact : typed_cell_pins->second) {
            ++result.typed_connection_fact_lookups;
            ++result.typed_cell_pin_fact_lookups;
            typed_cell_ids.insert(fact.cell_id);
            if (fact.cell_kind == SnapshotSchematicCellKind::Primitive) {
                ++result.primitive_cell_pin_fact_lookups;
                if (fact.pin_direction == SnapshotSchematicCellPinDirection::Control) {
                    ++result.primitive_control_pin_facts;
                }
            }
            ++result.source_part_scans;
            ++result.cell_pin_scans;
            if (fact.unresolved || fact.literal || fact.net_symbol_id.empty() ||
                !static_slice(fact.net_slice) ||
                fact.pin_direction == SnapshotSchematicCellPinDirection::Unknown) {
                result.partial = true;
                ++result.partial_connection_facts;
                ++result.partial_cell_pin_facts;
                appendUniqueMessage(result.messages,
                                    "Partial AST-backed schematic cell pin for cell '" + fact.cell_id + "'.");
                continue;
            }
            auto& net = ensure_net(cell_net_key(fact.net_symbol_id, fact.net_slice),
                                   fact.display_label.empty() ? "<unresolved>" : fact.display_label);
            const auto endpoint = SemanticSchematicEndpoint{.node_id = fact.cell_id,
                                                              .port_name = fact.pin_name};
            switch (fact.pin_direction) {
                case SnapshotSchematicCellPinDirection::Input:
                case SnapshotSchematicCellPinDirection::Control:
                    net.loads.push_back(endpoint);
                    break;
                case SnapshotSchematicCellPinDirection::Output:
                    net.drivers.push_back(endpoint);
                    break;
                case SnapshotSchematicCellPinDirection::Inout:
                    net.drivers.push_back(endpoint);
                    net.loads.push_back(endpoint);
                    break;
                case SnapshotSchematicCellPinDirection::Unknown:
                    break;
            }
            append_port_endpoint(net, fact.net_symbol_id);
        }
    }
    for (const auto& port : schematic.ports) {
        const auto stable_id = context.binding_index.port_symbol_ids_by_module_port.find(
            schematic.name + "\x1f" + port.name);
        if (port.name.empty() || stable_id == context.binding_index.port_symbol_ids_by_module_port.end() ||
            connected_port_ids.contains(stable_id->second)) {
            continue;
        }
        auto& net = ensure_net(stable_id->second + "\x1f" + "whole", port.name);
        appendEndpointByDirection(net,
                                  port.direction,
                                  SemanticSchematicEndpoint{.node_id = std::string("$port:") + port.name,
                                                            .port_name = port.name},
                                  true);
    }

    for (const auto& cell : cells) {
        if (cell.kind == "interface") {
            continue;
        }
        if (cell.kind == "module" || typed_cell_ids.contains(cell.id)) {
            continue;
        }
        if (!typed_cell_ids.contains(cell.id)) {
            result.partial = true;
            ++result.partial_cell_pin_facts;
            appendUniqueMessage(result.messages,
                                "No AST-backed schematic cell pin facts for cell '" + cell.id + "'.");
        }
    }

    result.nets.reserve(nets.size());
    for (auto& [_, net] : nets) {
        const auto sort_endpoints = [](std::vector<SemanticSchematicEndpoint>& endpoints) {
            std::sort(endpoints.begin(), endpoints.end(), [](const auto& lhs, const auto& rhs) {
                return std::tie(lhs.node_id, lhs.port_name) < std::tie(rhs.node_id, rhs.port_name);
            });
            endpoints.erase(std::unique(endpoints.begin(), endpoints.end(), [](const auto& lhs, const auto& rhs) {
                                return lhs.node_id == rhs.node_id && lhs.port_name == rhs.port_name;
                            }),
                            endpoints.end());
        };
        sort_endpoints(net.drivers);
        sort_endpoints(net.loads);
        result.nets.push_back(std::move(net));
    }
    return result;
}

std::vector<SemanticSchematicCell> projectSchematicCells(
    const ModuleSchematic& schematic,
    const DesignGraphContext& context) {
    std::vector<SemanticSchematicCell> cells;
    cells.reserve(schematic.cells.size());
    std::unordered_map<std::string, size_t> cell_indexes;
    for (const auto& cell : schematic.cells) {
        cell_indexes.try_emplace(cell.id, cells.size());
        cells.push_back(SemanticSchematicCell{.id = cell.id,
                                              .name = cell.name,
                                              .type = cell.type,
                                              .kind = cell.kind,
                                              .range = cell.range,
                                              .selection_range = cell.selection_range,
                                              .connections = {}});
    }

    const auto append = [&](std::string_view cell_id,
                            std::string port_name,
                            int port_index,
                            std::string signal,
                            ParseRange range) {
        const auto cell = cell_indexes.find(std::string(cell_id));
        if (cell == cell_indexes.end()) {
            return;
        }
        cells[cell->second].connections.push_back(SemanticSchematicConnection{
            .port_name = std::move(port_name),
            .port_index = port_index,
            .signal = std::move(signal),
            .range = range});
    };

    if (const auto connections = context.binding_index.schematic_connections_by_module.find(schematic.name);
        connections != context.binding_index.schematic_connections_by_module.end()) {
        for (const auto& fact : connections->second) {
            const auto cell = context.binding_index.schematic_cell_ids_by_instance_id.find(
                fact.instance_stable_id);
            if (cell == context.binding_index.schematic_cell_ids_by_instance_id.end()) {
                continue;
            }
            append(cell->second,
                   fact.endpoint_name,
                   fact.endpoint_index,
                   fact.display_label.empty() ? "<partial>" : fact.display_label,
                   fact.location.range);
        }
    }

    if (const auto interface_members =
            context.binding_index.interface_member_connections_by_module.find(schematic.name);
        interface_members != context.binding_index.interface_member_connections_by_module.end()) {
        for (const auto& fact : interface_members->second) {
            if (fact.unresolved || fact.member_name.empty()) {
                continue;
            }
            const auto parent = context.binding_index.schematic_cell_ids_by_instance_id.find(
                fact.parent_interface_instance_stable_id);
            const auto child = context.binding_index.schematic_cell_ids_by_instance_id.find(
                fact.child_instance_stable_id);
            if (parent == context.binding_index.schematic_cell_ids_by_instance_id.end() ||
                child == context.binding_index.schematic_cell_ids_by_instance_id.end()) {
                continue;
            }
            const auto parent_cell = cell_indexes.find(parent->second);
            const auto display = parent_cell == cell_indexes.end()
                                     ? fact.member_name
                                     : cells[parent_cell->second].name + "." + fact.member_name;
            append(parent->second, fact.member_name, -1, display, fact.location.range);
            append(child->second,
                   fact.child_endpoint_name.empty() ? fact.member_name
                                                    : fact.child_endpoint_name + "." + fact.member_name,
                   -1,
                   display,
                   fact.location.range);
        }
    }

    if (const auto pins = context.binding_index.schematic_cell_pins_by_module.find(schematic.name);
        pins != context.binding_index.schematic_cell_pins_by_module.end()) {
        for (const auto& fact : pins->second) {
            append(fact.cell_id,
                   fact.pin_name,
                   fact.pin_index,
                   fact.display_label.empty() ? (fact.literal ? "<literal>" : "<partial>")
                                              : fact.display_label,
                   fact.location.range);
        }
    }

    for (auto& cell : cells) {
        std::sort(cell.connections.begin(), cell.connections.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.port_index, lhs.port_name, lhs.range.start_line, lhs.range.start_character,
                            lhs.range.end_line, lhs.range.end_character, lhs.signal) <
                   std::tie(rhs.port_index, rhs.port_name, rhs.range.start_line, rhs.range.start_character,
                            rhs.range.end_line, rhs.range.end_character, rhs.signal);
        });
        cell.connections.erase(std::unique(cell.connections.begin(), cell.connections.end(), [](const auto& lhs,
                                                                                                 const auto& rhs) {
                                   return lhs.port_name == rhs.port_name && lhs.port_index == rhs.port_index &&
                                          lhs.signal == rhs.signal && lhs.range.start_line == rhs.range.start_line &&
                                           lhs.range.start_character == rhs.range.start_character &&
                                           lhs.range.end_line == rhs.range.end_line &&
                                           lhs.range.end_character == rhs.range.end_character;
                               }),
                               cell.connections.end());
    }
    return cells;
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
        auto net_result = buildSchematicNets(schematic, context);
        result.partial = result.partial || net_result.partial;
        result.graph_binding_lookup_scanned_facts += net_result.typed_connection_fact_lookups;
        result.schematic_connection_fact_lookup_count += net_result.typed_connection_fact_lookups;
        result.schematic_source_part_scan_count += net_result.source_part_scans;
        result.schematic_partial_connection_fact_count += net_result.partial_connection_facts;
        result.schematic_cell_pin_fact_lookup_count += net_result.typed_cell_pin_fact_lookups;
        result.schematic_cell_pin_scan_count += net_result.cell_pin_scans;
        result.schematic_partial_cell_pin_fact_count += net_result.partial_cell_pin_facts;
        result.schematic_primitive_cell_pin_fact_lookup_count += net_result.primitive_cell_pin_fact_lookups;
        result.schematic_primitive_control_pin_fact_count += net_result.primitive_control_pin_facts;
        result.schematic_interface_member_connection_fact_lookup_count +=
            net_result.interface_member_connection_fact_lookups;
        result.schematic_interface_member_connection_scan_count +=
            net_result.interface_member_connection_scans;
        result.schematic_partial_interface_member_connection_fact_count +=
            net_result.partial_interface_member_connection_facts;
        for (auto& message : net_result.messages) {
            appendUniqueMessage(result.messages, std::move(message));
        }
        result.modules.push_back(SemanticSchematicModuleView{
            .module = SemanticSchematicModule{.id = schematic.name,
                                              .name = schematic.name,
                                              .uri = schematic_uri,
                                              .range = schematic.range,
                                              .selection_range = schematic.selection_range,
                                              .ports = schematic.ports,
                                              .cells = projectSchematicCells(schematic, context)},
            .nets = std::move(net_result.nets)});

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
                               size_t max_results,
                               std::optional<SnapshotConeSliceFact> root_slice) {
    (void)document_uri;
    SemanticConeTrace trace;
    const auto is_connection_edge = [](SnapshotConeEdgeKind kind) {
        return kind == SnapshotConeEdgeKind::InstancePort ||
               kind == SnapshotConeEdgeKind::ParameterOverride;
    };
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

    if (context.cone_adjacency_index.edges.empty() &&
        context.cone_adjacency_index.unresolved_sources_by_from_symbol_id.empty()) {
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
    trace.graph_build_scoped_symbol_candidates = context.binding_index.scoped_symbol_candidate_count;
    trace.graph_build_connection_reference_candidates =
        context.binding_index.connection_reference_candidate_count;
    append_node(lookup.symbol->stable_id);
    if (root_slice.has_value() && root_slice->precision == SnapshotConeSlicePrecision::Dynamic) {
        trace.partial = true;
        appendUniqueMessage(trace.messages,
                            "Backward cone root uses a dynamic select; bit precision is partial.");
    }

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

        const auto unresolved_sources =
            context.cone_adjacency_index.unresolved_sources_by_from_symbol_id.find(current_id);
        if (unresolved_sources != context.cone_adjacency_index.unresolved_sources_by_from_symbol_id.end()) {
            for (const auto& source : unresolved_sources->second) {
                trace.partial = true;
                ++trace.cone_unresolved_source_fact_count;
                if (is_connection_edge(source.kind)) {
                    ++trace.cone_unresolved_connection_fact_count;
                }
                const auto origin = coneControlOriginLabel(source.control_origin);
                appendUniqueMessage(trace.messages,
                                    "Unresolved " +
                                        (origin.empty() ? std::string("cone") : origin + " control") +
                                        " source '" + source.expression + "'.");
            }
        }

        const auto adjacency = context.cone_adjacency_index.edges_by_from_symbol_id.find(current_id);
        if (adjacency == context.cone_adjacency_index.edges_by_from_symbol_id.end()) {
            continue;
        }
        for (const auto edge_index : adjacency->second) {
            ++trace.cone_adjacency_scanned_edges;
            const auto& edge = context.cone_adjacency_index.edges[edge_index];
            if (is_connection_edge(edge.kind)) {
                ++trace.cone_connection_slice_adjacency_scanned_edges;
            }
            if (reached_cap()) {
                mark_truncated();
                break;
            }
            if (current_id == *trace.root_symbol_id && root_slice.has_value() &&
                root_slice->precision == SnapshotConeSlicePrecision::Exact &&
                edge.sink_slice.precision == SnapshotConeSlicePrecision::Exact) {
                if (!exactSlicesIntersect(*root_slice, edge.sink_slice)) {
                    continue;
                }
                ++trace.cone_static_slice_match_count;
            }
            else if (current_id == *trace.root_symbol_id && root_slice.has_value() &&
                     root_slice->precision == SnapshotConeSlicePrecision::Exact &&
                     (edge.sink_slice.precision == SnapshotConeSlicePrecision::Aggregate ||
                      edge.sink_slice.precision == SnapshotConeSlicePrecision::Dynamic ||
                      edge.sink_slice.precision == SnapshotConeSlicePrecision::Unresolved)) {
                trace.partial = true;
                appendUniqueMessage(trace.messages,
                                    "Backward cone root cannot prove a static slice match for one dependency.");
            }

            const auto node_available = append_node(edge.to_symbol_id);
            const auto edge_key = edge.from_symbol_id + "\n" + edge.to_symbol_id + "\n" +
                                  std::to_string(edge.location.range.start_line) + ":" +
                                  std::to_string(edge.location.range.start_character) + "\n" +
                                  coneEdgeKindLabel(edge.kind) + "\n" +
                                   coneSourceRoleLabel(edge.source_role) + "\n" +
                                   coneControlOriginLabel(edge.control_origin) + "\n" +
                                   coneEventKindLabel(edge.event_kind) + "\n" +
                                   coneSliceKindLabel(edge.slice_kind) + "\n" +
                                  coneSliceKey(edge.source_slice) + "\n" +
                                  coneSliceKey(edge.sink_slice);
            if (node_available && emitted_edges.insert(edge_key).second) {
                if (max_results > 0 && trace.edges.size() >= max_results) {
                    mark_truncated();
                    break;
                }
                trace.edges.push_back(SemanticConeEdge{.from_symbol_id = edge.from_symbol_id,
                                                       .to_symbol_id = edge.to_symbol_id,
                                                       .location = edge.location,
                                                       .expression = edge.expression,
                                                       .kind = coneEdgeKindLabel(edge.kind),
                                                       .source_role = coneSourceRoleLabel(edge.source_role),
                                                       .slice_kind = coneSliceKindLabel(edge.slice_kind),
                                                       .control_origin = coneControlOriginLabel(edge.control_origin),
                                                       .event_kind = coneEventKindLabel(edge.event_kind),
                                                       .source_range = edge.expression_location.range,
                                                       .source_slice = coneSlice(edge.source_slice),
                                                       .sink_slice = coneSlice(edge.sink_slice)});
                if (edge.kind == SnapshotConeEdgeKind::PrimitiveCell) {
                    if (edge.source_role == SnapshotConeSourceRole::Control) {
                        ++trace.cone_primitive_control_edge_count;
                    }
                    else {
                        ++trace.cone_primitive_data_edge_count;
                    }
                }
                if (edge.kind == SnapshotConeEdgeKind::AssertionSample) {
                    ++trace.cone_assertion_sample_edge_count;
                    switch (edge.source_role) {
                    case SnapshotConeSourceRole::Clock:
                        ++trace.cone_assertion_clock_edge_count;
                        break;
                    case SnapshotConeSourceRole::Disable:
                        ++trace.cone_assertion_disable_edge_count;
                        break;
                    case SnapshotConeSourceRole::Abort:
                        ++trace.cone_assertion_abort_edge_count;
                        break;
                    default:
                        break;
                    }
                }
                if (edge.source_role == SnapshotConeSourceRole::Control) {
                    ++trace.cone_control_edge_count;
                    if (edge.control_origin == SnapshotConeControlOrigin::TernaryCondition) {
                        ++trace.cone_ternary_control_edge_count;
                    }
                    if (edge.control_origin == SnapshotConeControlOrigin::EventControl) {
                        ++trace.cone_event_control_edge_count;
                        ++trace.cone_timing_fact_lookup_count;
                    }
                    if (edge.control_origin == SnapshotConeControlOrigin::EventIff) {
                        ++trace.cone_event_iff_edge_count;
                        ++trace.cone_timing_fact_lookup_count;
                    }
                }
                if (edge.slice_kind != SnapshotConeSliceKind::Whole) {
                    ++trace.cone_slice_fact_count;
                }
                if (edge.source_slice.precision == SnapshotConeSlicePrecision::Exact ||
                    edge.sink_slice.precision == SnapshotConeSlicePrecision::Exact) {
                    ++trace.cone_exact_slice_edge_count;
                    if (is_connection_edge(edge.kind)) {
                        ++trace.cone_exact_connection_edge_count;
                        if (edge.kind == SnapshotConeEdgeKind::ParameterOverride) {
                            ++trace.cone_parameter_override_exact_mapping_count;
                        }
                    }
                }
                if (edge.source_slice.precision == SnapshotConeSlicePrecision::Dynamic ||
                    edge.sink_slice.precision == SnapshotConeSlicePrecision::Dynamic) {
                    ++trace.cone_dynamic_slice_fact_count;
                    if (is_connection_edge(edge.kind)) {
                        ++trace.cone_dynamic_connection_fact_count;
                    }
                    trace.partial = true;
                    appendUniqueMessage(trace.messages,
                                        "Backward cone includes a dynamic select; bit precision is partial.");
                }
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
