#include "DesignGraphIndexBuilder.h"

#include "SnapshotBuilder.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pristine::analysis::semantic {
namespace {

bool locationLess(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    if (lhs.uri != rhs.uri) return lhs.uri < rhs.uri;
    if (lhs.range.start_line != rhs.range.start_line) return lhs.range.start_line < rhs.range.start_line;
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.range.end_line != rhs.range.end_line) return lhs.range.end_line < rhs.range.end_line;
    return lhs.range.end_character < rhs.range.end_character;
}

bool sameLocation(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    return lhs.uri == rhs.uri && lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line && lhs.range.end_character == rhs.range.end_character;
}

bool sameRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

bool rangeContainsRange(const ParseRange& outer, const ParseRange& inner) {
    if (inner.start_line < outer.start_line || inner.end_line > outer.end_line) return false;
    if (inner.start_line == outer.start_line && inner.start_character < outer.start_character) return false;
    return inner.end_line != outer.end_line || inner.end_character <= outer.end_character;
}

bool rangeStartLess(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.start_line) return lhs.start_line < rhs.start_line;
    return lhs.start_character < rhs.start_character;
}

bool rangeStartsAfter(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.end_line) return lhs.start_line > rhs.end_line;
    return lhs.start_character > rhs.end_character;
}

SnapshotConeSliceFact mergeRootSlices(const SnapshotConeSliceFact& lhs,
                                      const SnapshotConeSliceFact& rhs) {
    if (lhs.precision != SnapshotConeSlicePrecision::Exact ||
        rhs.precision != SnapshotConeSlicePrecision::Exact || !lhs.msb || !lhs.lsb || !rhs.msb ||
        !rhs.lsb) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Aggregate,
                                     .msb = {},
                                     .lsb = {}};
    }

    const auto low = std::min({*lhs.msb, *lhs.lsb, *rhs.msb, *rhs.lsb});
    const auto high = std::max({*lhs.msb, *lhs.lsb, *rhs.msb, *rhs.lsb});
    if (*lhs.msb >= *lhs.lsb) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                     .msb = high,
                                     .lsb = low};
    }
    return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                 .msb = low,
                                 .lsb = high};
}

std::string rangeKey(const ParseRange& range) {
    return std::to_string(range.start_line) + ":" + std::to_string(range.start_character) + ":" +
           std::to_string(range.end_line) + ":" + std::to_string(range.end_character);
}

std::string uriRangeKey(std::string_view uri, const ParseRange& range) {
    return std::string(uri) + "\x1f" + rangeKey(range);
}

std::string scopeNameKey(std::string_view uri, const ParseRange& scope, std::string_view name) {
    return std::string(uri) + "\x1f" + rangeKey(scope) + "\x1f" + std::string(name);
}

std::string moduleMemberKey(std::string_view module, std::string_view name) {
    return std::string(module) + "\x1f" + std::string(name);
}

void upsertBinding(std::unordered_map<std::string, std::string>& values,
                   std::string key,
                   std::string stable_id) {
    if (stable_id.empty()) return;
    const auto found = values.find(key);
    if (found == values.end() || stable_id < found->second) {
        values.insert_or_assign(std::move(key), std::move(stable_id));
    }
}

void upsertEndpoint(SnapshotDesignGraphBindingIndex& index, SnapshotGraphEndpointFact fact) {
    if (fact.stable_id.empty() || fact.module_name.empty() || fact.name.empty()) return;
    const auto key = moduleMemberKey(fact.module_name, fact.name);
    const auto existing = index.endpoints_by_module_member.find(key);
    if (existing == index.endpoints_by_module_member.end() || fact.stable_id < existing->second.stable_id) {
        index.endpoints_by_module_member.insert_or_assign(key, fact);
    }
    const auto by_id = index.endpoints_by_stable_id.find(fact.stable_id);
    if (by_id == index.endpoints_by_stable_id.end() ||
        key < moduleMemberKey(by_id->second.module_name, by_id->second.name)) {
        index.endpoints_by_stable_id.insert_or_assign(fact.stable_id, std::move(fact));
    }
}

SnapshotGraphEndpointKind endpointKindFor(const SchematicPort& port) {
    if (port.direction == "interface") return SnapshotGraphEndpointKind::InterfacePort;
    if (port.direction == "modport") return SnapshotGraphEndpointKind::ModportPort;
    return SnapshotGraphEndpointKind::Port;
}

SnapshotGraphPortDirection directionFor(const SchematicPort& port) {
    if (port.direction == "input") return SnapshotGraphPortDirection::Input;
    if (port.direction == "output") return SnapshotGraphPortDirection::Output;
    if (port.direction == "inout") return SnapshotGraphPortDirection::Inout;
    if (port.direction == "ref") return SnapshotGraphPortDirection::Ref;
    return SnapshotGraphPortDirection::Unknown;
}

std::vector<std::string> sourceIdsForRange(const std::vector<SnapshotUriReferenceRangeFact>& references,
                                           const ParseRange& range,
                                           size_t& candidate_count) {
    std::vector<std::string> ids;
    const auto first = std::lower_bound(references.begin(),
                                        references.end(),
                                        range,
                                        [](const auto& reference, const ParseRange& candidate) {
                                            return rangeStartLess(reference.range, candidate);
                                        });
    for (auto it = first; it != references.end(); ++it) {
        const auto& reference = *it;
        if (rangeStartsAfter(reference.range, range)) break;
        ++candidate_count;
        if (!reference.is_declaration && rangeContainsRange(range, reference.range) &&
            !reference.stable_id.empty()) {
            ids.push_back(reference.stable_id);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void appendConnectionBinding(SnapshotDesignGraphBindingIndex& index,
                             SnapshotGraphConnectionBindingFact binding) {
    if (binding.instance_stable_id.empty() || binding.endpoint_stable_id.empty() ||
        binding.location.uri.empty() || binding.source_symbol_ids.empty()) {
        return;
    }
    std::sort(binding.source_symbol_ids.begin(), binding.source_symbol_ids.end());
    binding.source_symbol_ids.erase(std::unique(binding.source_symbol_ids.begin(), binding.source_symbol_ids.end()),
                                    binding.source_symbol_ids.end());
    const auto key = uriRangeKey(binding.location.uri, binding.location.range);
    index.connection_bindings_by_uri_range[key].push_back(index.connection_bindings.size());
    index.connection_bindings.push_back(std::move(binding));
}

std::optional<std::string> endpointStableId(const SnapshotDesignGraphBindingIndex& bindings,
                                            const SemanticModuleSignature& signature,
                                            const SchematicPort& port) {
    const auto direct = bindings.symbol_ids_by_uri_range.find(uriRangeKey(signature.uri, port.selection_range));
    if (direct != bindings.symbol_ids_by_uri_range.end()) return direct->second;
    const auto scoped = bindings.symbol_ids_by_module_scope_name.find(
        scopeNameKey(signature.uri, signature.definition.range, port.name));
    return scoped == bindings.symbol_ids_by_module_scope_name.end()
               ? std::nullopt
               : std::optional<std::string>(scoped->second);
}

void appendEdge(SnapshotConeAdjacencyIndex& index, SnapshotConeAdjacencyEdge edge) {
    if (!edge.from_symbol_id.empty() && !edge.to_symbol_id.empty()) index.edges.push_back(std::move(edge));
}

} // namespace

void buildDesignGraphIndexes(SnapshotData& data) {
    data.design_graph_binding_index = {};
    data.cone_adjacency_index = {};
    auto& bindings = data.design_graph_binding_index;

    for (const auto& [uri, references] : data.graph_references_by_uri) {
        for (const auto& reference : references) {
            if (!reference.stable_id.empty()) {
                upsertBinding(bindings.symbol_ids_by_uri_range,
                              uriRangeKey(uri, reference.range),
                              reference.stable_id);
            }
        }
    }

    for (const auto& [module_name, signature] : data.ast_module_signatures_by_name) {
        if (signature.uri.empty()) continue;
        const auto symbols = data.graph_symbols_by_uri.find(signature.uri);
        if (symbols != data.graph_symbols_by_uri.end()) {
            const auto first = std::lower_bound(symbols->second.begin(),
                                                symbols->second.end(),
                                                signature.definition.range,
                                                [](const SnapshotUriSymbolRangeFact& symbol,
                                                   const ParseRange& range) {
                                                    return rangeStartLess(symbol.range, range);
                                                });
            for (auto it = first; it != symbols->second.end(); ++it) {
                const auto indexed = data.symbols_by_id.find(it->stable_id);
                if (indexed == data.symbols_by_id.end()) continue;
                const auto& stable_id = it->stable_id;
                const auto& identity = indexed->second.identity;
                if (rangeStartsAfter(it->range, signature.definition.range)) break;
                ++bindings.scoped_symbol_candidate_count;
                if (identity.name.empty() ||
                    !rangeContainsRange(signature.definition.range, identity.location.range)) continue;
            upsertBinding(bindings.symbol_ids_by_module_scope_name,
                          scopeNameKey(signature.uri, signature.definition.range, identity.name), stable_id);
            if (identity.kind.find("Parameter") != std::string::npos ||
                identity.kind.find("Param") != std::string::npos) {
                upsertBinding(bindings.parameter_symbol_ids_by_module_parameter,
                              moduleMemberKey(module_name, identity.name), stable_id);
            }
            }
        }
        for (const auto& port : signature.definition.port_details) {
            const auto stable_id = endpointStableId(bindings, signature, port);
            if (!stable_id.has_value()) continue;
            const auto interface_binding = data.interface_modport_binding_index.ports_by_stable_id.find(*stable_id);
            const auto interface_definition_id =
                interface_binding == data.interface_modport_binding_index.ports_by_stable_id.end()
                    ? std::string{}
                    : interface_binding->second.interface_definition_stable_id;
            const auto modport_id =
                interface_binding == data.interface_modport_binding_index.ports_by_stable_id.end()
                    ? std::string{}
                    : interface_binding->second.modport_stable_id;
            upsertBinding(bindings.port_symbol_ids_by_module_port,
                          moduleMemberKey(module_name, port.name), *stable_id);
            upsertEndpoint(bindings,
                           SnapshotGraphEndpointFact{.stable_id = *stable_id,
                                                     .module_name = module_name,
                                                     .name = port.name,
                                                     .kind = endpointKindFor(port),
                                                     .direction = directionFor(port),
                                                     .location = SemanticLocation{.uri = signature.uri,
                                                                                  .range = port.selection_range},
                                                     .generated_instance_id = {},
                                                     .interface_definition_stable_id = interface_definition_id,
                                                     .modport_stable_id = modport_id});
        }
        for (const auto& parameter : signature.definition.parameter_details) {
            const auto stable_id = bindings.parameter_symbol_ids_by_module_parameter.find(
                moduleMemberKey(module_name, parameter.name));
            if (stable_id == bindings.parameter_symbol_ids_by_module_parameter.end()) continue;
            upsertEndpoint(bindings,
                           SnapshotGraphEndpointFact{.stable_id = stable_id->second,
                                                     .module_name = module_name,
                                                     .name = parameter.name,
                                                     .kind = SnapshotGraphEndpointKind::Parameter,
                                                     .direction = SnapshotGraphPortDirection::Unknown,
                                                     .location = SemanticLocation{.uri = signature.uri,
                                                                                  .range = parameter.selection_range},
                                                     .generated_instance_id = {},
                                                     .interface_definition_stable_id = {},
                                                     .modport_stable_id = {}});
        }
    }

    for (const auto& [uri, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            if (instance.instance_stable_id.empty()) continue;
            upsertBinding(bindings.instance_ids_by_uri_range, uriRangeKey(uri, instance.range), instance.instance_stable_id);
            upsertBinding(bindings.instance_ids_by_uri_range,
                          uriRangeKey(uri, instance.selection_range), instance.instance_stable_id);
            upsertBinding(bindings.instance_ids_by_uri_range,
                          uriRangeKey(uri, instance.module_selection_range), instance.instance_stable_id);
            upsertEndpoint(bindings,
                           SnapshotGraphEndpointFact{.stable_id = instance.instance_stable_id,
                                                     .module_name = instance.module_name,
                                                     .name = instance.instance_name,
                                                     .kind = SnapshotGraphEndpointKind::Instance,
                                                     .direction = SnapshotGraphPortDirection::Unknown,
                                                     .location = SemanticLocation{.uri = uri,
                                                                                  .range = instance.selection_range},
                                                     .generated_instance_id = instance.instance_stable_id,
                                                     .interface_definition_stable_id = {},
                                                     .modport_stable_id = {}});
        }
    }

    auto& adjacency = data.cone_adjacency_index;
    for (const auto& [_, edges] : data.assignment_edges_by_uri) {
        for (const auto& edge : edges) {
            appendEdge(adjacency,
                       SnapshotConeAdjacencyEdge{.from_symbol_id = edge.from_symbol_id,
                                                  .to_symbol_id = edge.to_symbol_id,
                                                  .location = edge.location,
                                                  .target_location = edge.target_location,
                                                  .expression_location = edge.expression_location,
                                                  .expression = edge.expression,
                                                  .kind = edge.kind,
                                                  .source_role = edge.source_role,
                                                  .slice_kind = edge.slice_kind,
                                                  .control_origin = edge.control_origin,
                                                  .source_slice = edge.source_slice,
                                                  .sink_slice = edge.sink_slice,
                                                  .generated_instance_id = {}});
        }
    }
    for (const auto& source : data.unresolved_cone_sources) {
        if (!source.from_symbol_id.empty()) {
            adjacency.unresolved_sources_by_from_symbol_id[source.from_symbol_id].push_back(source);
        }
    }

    std::unordered_map<std::string, std::string> caller_module_by_instance_id;
    for (const auto& edge : data.module_call_edge_index.edges) {
        const auto caller = data.module_call_edge_index.items_by_id.find(edge.caller_item_id);
        if (caller != data.module_call_edge_index.items_by_id.end() && !edge.instance_id.empty()) {
            caller_module_by_instance_id.insert_or_assign(edge.instance_id, caller->second.name);
        }
    }
    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            const auto caller = caller_module_by_instance_id.find(instance.instance_stable_id);
            const auto child = data.ast_module_signatures_by_name.find(instance.module_name);
            if (instance.instance_stable_id.empty() || caller == caller_module_by_instance_id.end() ||
                child == data.ast_module_signatures_by_name.end()) continue;
            const auto parent = data.ast_module_signatures_by_name.find(caller->second);
            if (parent == data.ast_module_signatures_by_name.end() || parent->second.uri.empty()) continue;
            const auto endpointFor = [&](const SchematicConnection& connection,
                                         const std::vector<SchematicPort>& details)
                -> const SnapshotGraphEndpointFact* {
                if (!connection.port_name.empty()) {
                    const auto found = bindings.endpoints_by_module_member.find(
                        moduleMemberKey(instance.module_name, connection.port_name));
                    if (found != bindings.endpoints_by_module_member.end()) return &found->second;
                }
                if (connection.port_index >= 0 &&
                    static_cast<size_t>(connection.port_index) < details.size()) {
                    const auto found = bindings.endpoints_by_module_member.find(
                        moduleMemberKey(instance.module_name,
                                        details[static_cast<size_t>(connection.port_index)].name));
                    if (found != bindings.endpoints_by_module_member.end()) return &found->second;
                }
                return nullptr;
            };
            const auto sourceIdsForConnection = [&](const SchematicConnection& connection) {
                static const std::vector<SnapshotUriReferenceRangeFact> kNoReferences;
                const auto references = data.graph_references_by_uri.find(parent->second.uri);
                auto source_ids = sourceIdsForRange(references == data.graph_references_by_uri.end()
                                                        ? kNoReferences
                                                        : references->second,
                                                    connection.range,
                                                    bindings.connection_reference_candidate_count);
                std::sort(source_ids.begin(), source_ids.end());
                source_ids.erase(std::unique(source_ids.begin(), source_ids.end()), source_ids.end());
                return source_ids;
            };
            const auto appendConnections = [&](const std::vector<SchematicConnection>& connections,
                                               const std::vector<SchematicPort>& details,
                                               SnapshotConeEdgeKind kind) {
                for (const auto& connection : connections) {
                    const auto* child_endpoint = endpointFor(connection, details);
                    if (!child_endpoint) continue;
                    const auto location = SemanticLocation{.uri = parent->second.uri, .range = connection.range};
                    if (kind == SnapshotConeEdgeKind::InstancePort &&
                        child_endpoint->kind == SnapshotGraphEndpointKind::InterfacePort) {
                        const auto port_binding = data.interface_modport_binding_index.ports_by_stable_id.find(
                            child_endpoint->stable_id);
                        if (port_binding == data.interface_modport_binding_index.ports_by_stable_id.end() ||
                            !port_binding->second.resolved ||
                            port_binding->second.modport_stable_id.empty() ||
                            port_binding->second.connected_modport_stable_id.empty()) {
                            continue;
                        }
                        const auto child_members =
                            data.interface_modport_binding_index.members_by_modport_stable_id.find(
                                port_binding->second.modport_stable_id);
                        const auto parent_members =
                            data.interface_modport_binding_index.members_by_modport_stable_id.find(
                                port_binding->second.connected_modport_stable_id);
                        if (child_members == data.interface_modport_binding_index.members_by_modport_stable_id.end() ||
                            parent_members == data.interface_modport_binding_index.members_by_modport_stable_id.end()) {
                            continue;
                        }
                        for (const auto& child_member : child_members->second) {
                            const auto parent_member = std::find_if(parent_members->second.begin(),
                                                                    parent_members->second.end(),
                                                                    [&](const auto& candidate) {
                                                                        return candidate.name == child_member.name;
                                                                    });
                            if (parent_member == parent_members->second.end()) {
                                continue;
                            }
                            const auto append_member_edge = [&](std::string from, std::string to) {
                                appendEdge(adjacency,
                                           SnapshotConeAdjacencyEdge{
                                               .from_symbol_id = std::move(from),
                                               .to_symbol_id = std::move(to),
                                               .location = location,
                                               .target_location = {},
                                               .expression_location = location,
                                               .expression = child_member.name,
                                               .kind = SnapshotConeEdgeKind::InstancePort,
                                               .source_role = SnapshotConeSourceRole::Data,
                                               .slice_kind = SnapshotConeSliceKind::Whole,
                                               .source_slice = {},
                                               .sink_slice = {},
                                               .generated_instance_id = instance.instance_stable_id});
                            };
                            if (child_member.direction == SnapshotGraphPortDirection::Input) {
                                append_member_edge(child_member.stable_id, parent_member->stable_id);
                            }
                            else if (child_member.direction == SnapshotGraphPortDirection::Output) {
                                append_member_edge(parent_member->stable_id, child_member.stable_id);
                            }
                            else if (child_member.direction == SnapshotGraphPortDirection::Inout ||
                                     child_member.direction == SnapshotGraphPortDirection::Ref) {
                                append_member_edge(child_member.stable_id, parent_member->stable_id);
                                append_member_edge(parent_member->stable_id, child_member.stable_id);
                            }
                        }
                        continue;
                    }
                    const auto source_ids = sourceIdsForConnection(connection);
                    appendConnectionBinding(bindings,
                                            SnapshotGraphConnectionBindingFact{
                                                .instance_stable_id = instance.instance_stable_id,
                                                .endpoint_stable_id = child_endpoint->stable_id,
                                                .location = location,
                                                .kind = kind,
                                                .source_symbol_ids = source_ids});
                    for (const auto& source_id : source_ids) {
                        if (kind == SnapshotConeEdgeKind::ParameterOverride) {
                            appendEdge(adjacency,
                                       SnapshotConeAdjacencyEdge{
                                           .from_symbol_id = child_endpoint->stable_id,
                                           .to_symbol_id = source_id,
                                           .location = location,
                                           .target_location = {},
                                           .expression_location = location,
                                           .expression = connection.signal.empty() ? child_endpoint->name : connection.signal,
                                           .kind = kind,
                                           .source_role = SnapshotConeSourceRole::Data,
                                           .slice_kind = SnapshotConeSliceKind::Whole,
                                           .source_slice = {},
                                           .sink_slice = {},
                                           .generated_instance_id = instance.instance_stable_id});
                            continue;
                        }
                        const bool output = child_endpoint->direction == SnapshotGraphPortDirection::Output;
                        const bool input_or_ref = child_endpoint->direction == SnapshotGraphPortDirection::Input ||
                                                  child_endpoint->direction == SnapshotGraphPortDirection::Ref;
                        if (!output && !input_or_ref) continue;
                        appendEdge(adjacency,
                                   SnapshotConeAdjacencyEdge{
                                       .from_symbol_id = output ? source_id : child_endpoint->stable_id,
                                       .to_symbol_id = output ? child_endpoint->stable_id : source_id,
                                       .location = location,
                                       .target_location = {},
                                       .expression_location = location,
                                       .expression = connection.signal.empty() ? child_endpoint->name : connection.signal,
                                       .kind = kind,
                                       .source_role = SnapshotConeSourceRole::Data,
                                       .slice_kind = SnapshotConeSliceKind::Whole,
                                       .source_slice = {},
                                       .sink_slice = {},
                                       .generated_instance_id = instance.instance_stable_id});
                    }
                }
            };
            appendConnections(instance.port_connections,
                              child->second.definition.port_details,
                              SnapshotConeEdgeKind::InstancePort);
            appendConnections(instance.parameter_connections,
                              child->second.definition.parameter_details,
                              SnapshotConeEdgeKind::ParameterOverride);
        }
    }

    std::sort(adjacency.edges.begin(), adjacency.edges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.from_symbol_id != rhs.from_symbol_id) return lhs.from_symbol_id < rhs.from_symbol_id;
        if (lhs.to_symbol_id != rhs.to_symbol_id) return lhs.to_symbol_id < rhs.to_symbol_id;
        if (!sameLocation(lhs.location, rhs.location)) return locationLess(lhs.location, rhs.location);
        if (!sameLocation(lhs.target_location, rhs.target_location)) {
            return locationLess(lhs.target_location, rhs.target_location);
        }
        if (!sameLocation(lhs.expression_location, rhs.expression_location)) {
            return locationLess(lhs.expression_location, rhs.expression_location);
        }
        if (lhs.kind != rhs.kind) {
            return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
        }
        if (lhs.source_role != rhs.source_role) {
            return static_cast<int>(lhs.source_role) < static_cast<int>(rhs.source_role);
        }
        if (lhs.slice_kind != rhs.slice_kind) {
            return static_cast<int>(lhs.slice_kind) < static_cast<int>(rhs.slice_kind);
        }
        if (lhs.generated_instance_id != rhs.generated_instance_id) {
            return lhs.generated_instance_id < rhs.generated_instance_id;
        }
        return lhs.expression < rhs.expression;
    });
    adjacency.edges.erase(std::unique(adjacency.edges.begin(), adjacency.edges.end(), [](const auto& lhs,
                                                                                          const auto& rhs) {
                              return lhs.from_symbol_id == rhs.from_symbol_id &&
                                     lhs.to_symbol_id == rhs.to_symbol_id &&
                                     sameLocation(lhs.location, rhs.location) &&
                                     sameLocation(lhs.target_location, rhs.target_location) &&
                                     sameLocation(lhs.expression_location, rhs.expression_location) &&
                                     lhs.kind == rhs.kind &&
                                     lhs.source_role == rhs.source_role &&
                                     lhs.slice_kind == rhs.slice_kind &&
                                     lhs.generated_instance_id == rhs.generated_instance_id &&
                                     lhs.expression == rhs.expression;
                          }), adjacency.edges.end());
    for (size_t index = 0; index < adjacency.edges.size(); ++index) {
        const auto& edge = adjacency.edges[index];
        adjacency.edges_by_from_symbol_id[edge.from_symbol_id].push_back(index);
        adjacency.edges_by_to_symbol_id[edge.to_symbol_id].push_back(index);
        if (!edge.target_location.uri.empty() && edge.target_location.range.start_line >= 0) {
            adjacency.root_selections_by_uri[edge.target_location.uri].push_back(
                SnapshotConeRootSelectionFact{.symbol_id = edge.from_symbol_id,
                                               .range = edge.target_location.range,
                                               .slice = edge.sink_slice});
        }
    }
    for (auto& [_, roots] : adjacency.root_selections_by_uri) {
        std::sort(roots.begin(), roots.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameRange(lhs.range, rhs.range)) return rangeStartLess(lhs.range, rhs.range);
            if (lhs.symbol_id != rhs.symbol_id) return lhs.symbol_id < rhs.symbol_id;
            return std::tie(lhs.slice.precision, lhs.slice.msb, lhs.slice.lsb) <
                   std::tie(rhs.slice.precision, rhs.slice.msb, rhs.slice.lsb);
        });
        std::vector<SnapshotConeRootSelectionFact> merged_roots;
        for (const auto& root : roots) {
            if (!merged_roots.empty() && merged_roots.back().symbol_id == root.symbol_id &&
                sameRange(merged_roots.back().range, root.range)) {
                merged_roots.back().slice = mergeRootSlices(merged_roots.back().slice, root.slice);
            }
            else {
                merged_roots.push_back(root);
            }
        }
        roots = std::move(merged_roots);
    }

    std::sort(bindings.connection_bindings.begin(), bindings.connection_bindings.end(), [](const auto& lhs,
                                                                                           const auto& rhs) {
        if (!sameLocation(lhs.location, rhs.location)) return locationLess(lhs.location, rhs.location);
        if (lhs.kind != rhs.kind) return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
        if (lhs.endpoint_stable_id != rhs.endpoint_stable_id) {
            return lhs.endpoint_stable_id < rhs.endpoint_stable_id;
        }
        if (lhs.instance_stable_id != rhs.instance_stable_id) {
            return lhs.instance_stable_id < rhs.instance_stable_id;
        }
        return lhs.source_symbol_ids < rhs.source_symbol_ids;
    });
    bindings.connection_bindings.erase(
        std::unique(bindings.connection_bindings.begin(), bindings.connection_bindings.end(), [](const auto& lhs,
                                                                                                  const auto& rhs) {
            return lhs.instance_stable_id == rhs.instance_stable_id &&
                   lhs.endpoint_stable_id == rhs.endpoint_stable_id &&
                   sameLocation(lhs.location, rhs.location) && lhs.kind == rhs.kind &&
                   lhs.source_symbol_ids == rhs.source_symbol_ids;
        }),
        bindings.connection_bindings.end());
    bindings.connection_bindings_by_uri_range.clear();
    for (size_t index = 0; index < bindings.connection_bindings.size(); ++index) {
        const auto& binding = bindings.connection_bindings[index];
        bindings.connection_bindings_by_uri_range[uriRangeKey(binding.location.uri, binding.location.range)]
            .push_back(index);
    }
}

} // namespace pristine::analysis::semantic
