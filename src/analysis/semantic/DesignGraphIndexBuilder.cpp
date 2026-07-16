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

bool simpleIdentifier(std::string_view value) {
    if (value.empty()) return false;
    const auto start = [](unsigned char ch) { return std::isalpha(ch) != 0 || ch == '_' || ch == '$'; };
    const auto next = [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '$'; };
    return start(static_cast<unsigned char>(value.front())) &&
           std::all_of(value.begin() + 1, value.end(), [&](char ch) {
               return next(static_cast<unsigned char>(ch));
           });
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

std::vector<std::string> sourceIdsForRange(const std::vector<const SnapshotIndexedReference*>& references,
                                           const ParseRange& range) {
    std::vector<std::string> ids;
    for (const auto* reference : references) {
        if (!reference->is_declaration && rangeContainsRange(range, reference->location.range) &&
            !reference->stable_id.empty()) {
            ids.push_back(reference->stable_id);
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

std::optional<std::string> endpointStableId(const SnapshotData& data,
                                            const SnapshotDesignGraphBindingIndex& bindings,
                                            const SemanticModuleSignature& signature,
                                            const SchematicPort& port) {
    const auto assignment_it = data.assignment_edges_by_uri.find(signature.uri);
    if (assignment_it != data.assignment_edges_by_uri.end()) {
        const bool output = port.direction == "output";
        for (const auto& edge : assignment_it->second) {
            const auto& id = output ? edge.from_symbol_id : edge.to_symbol_id;
            const auto symbol = data.symbols_by_id.find(id);
            if (symbol != data.symbols_by_id.end() && symbol->second.identity.name == port.name &&
                symbol->second.identity.location.uri == signature.uri) {
                return id;
            }
        }
    }
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

    std::vector<const SnapshotIndexedReference*> references;
    std::unordered_map<std::string, std::vector<const SnapshotIndexedReference*>> references_by_uri;
    references.reserve(data.references.size());
    for (const auto& reference : data.references) {
        if (!reference.stable_id.empty() && !reference.location.uri.empty()) {
            references.push_back(&reference);
            references_by_uri[reference.location.uri].push_back(&reference);
        }
    }
    std::sort(references.begin(), references.end(), [](const auto* lhs, const auto* rhs) {
        if (lhs->location.uri != rhs->location.uri || !sameRange(lhs->location.range, rhs->location.range)) {
            return locationLess(lhs->location, rhs->location);
        }
        if (lhs->is_declaration != rhs->is_declaration) return lhs->is_declaration;
        return lhs->stable_id < rhs->stable_id;
    });
    for (const auto* reference : references) {
        upsertBinding(bindings.symbol_ids_by_uri_range,
                      uriRangeKey(reference->location.uri, reference->location.range),
                      reference->stable_id);
    }

    for (const auto& [module_name, signature] : data.ast_module_signatures_by_name) {
        if (signature.uri.empty()) continue;
        for (const auto& [stable_id, indexed] : data.symbols_by_id) {
            const auto& identity = indexed.identity;
            if (identity.name.empty() || identity.location.uri != signature.uri ||
                !rangeContainsRange(signature.definition.range, identity.location.range)) continue;
            upsertBinding(bindings.symbol_ids_by_module_scope_name,
                          scopeNameKey(signature.uri, signature.definition.range, identity.name), stable_id);
            if (identity.kind.find("Parameter") != std::string::npos ||
                identity.kind.find("Param") != std::string::npos) {
                upsertBinding(bindings.parameter_symbol_ids_by_module_parameter,
                              moduleMemberKey(module_name, identity.name), stable_id);
            }
        }
        for (const auto& port : signature.definition.port_details) {
            const auto stable_id = endpointStableId(data, bindings, signature, port);
            if (!stable_id.has_value()) continue;
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
                                                     .generated_instance_id = {}});
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
                                                     .generated_instance_id = {}});
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
                                                     .generated_instance_id = instance.instance_stable_id});
        }
    }

    auto& adjacency = data.cone_adjacency_index;
    for (const auto& [_, edges] : data.assignment_edges_by_uri) {
        for (const auto& edge : edges) {
            appendEdge(adjacency,
                       SnapshotConeAdjacencyEdge{.from_symbol_id = edge.from_symbol_id,
                                                  .to_symbol_id = edge.to_symbol_id,
                                                  .location = edge.location,
                                                  .expression_location = edge.expression_location,
                                                  .expression = edge.expression,
                                                  .kind = SnapshotConeEdgeKind::Assignment,
                                                  .generated_instance_id = {}});
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
                static const std::vector<const SnapshotIndexedReference*> kNoReferences;
                const auto references = references_by_uri.find(parent->second.uri);
                auto source_ids = sourceIdsForRange(
                    references == references_by_uri.end() ? kNoReferences : references->second, connection.range);
                if (source_ids.empty() && simpleIdentifier(connection.signal)) {
                    const auto scoped = bindings.symbol_ids_by_module_scope_name.find(
                        scopeNameKey(parent->second.uri, parent->second.definition.range, connection.signal));
                    if (scoped != bindings.symbol_ids_by_module_scope_name.end()) {
                        source_ids.push_back(scoped->second);
                    }
                }
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
                    const auto source_ids = sourceIdsForConnection(connection);
                    const auto location = SemanticLocation{.uri = parent->second.uri, .range = connection.range};
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
                                           .expression_location = location,
                                           .expression = connection.signal.empty() ? child_endpoint->name : connection.signal,
                                           .kind = kind,
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
                                       .expression_location = location,
                                       .expression = connection.signal.empty() ? child_endpoint->name : connection.signal,
                                       .kind = kind,
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
        if (!sameLocation(lhs.expression_location, rhs.expression_location)) {
            return locationLess(lhs.expression_location, rhs.expression_location);
        }
        if (lhs.kind != rhs.kind) {
            return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
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
                                     sameLocation(lhs.expression_location, rhs.expression_location) &&
                                     lhs.kind == rhs.kind &&
                                     lhs.generated_instance_id == rhs.generated_instance_id &&
                                     lhs.expression == rhs.expression;
                          }), adjacency.edges.end());
    for (size_t index = 0; index < adjacency.edges.size(); ++index) {
        const auto& edge = adjacency.edges[index];
        adjacency.edges_by_from_symbol_id[edge.from_symbol_id].push_back(index);
        adjacency.edges_by_to_symbol_id[edge.to_symbol_id].push_back(index);
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
