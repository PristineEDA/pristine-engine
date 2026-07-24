#include "DesignGraphIndexBuilder.h"

#include "SnapshotBuilder.h"

#include <algorithm>
#include <map>
#include <set>
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

SnapshotConeSliceFact endpointDeclaredSlice(const SnapshotData& data, std::string_view stable_id) {
    const auto found = data.endpoint_declared_slices_by_id.find(std::string(stable_id));
    return found == data.endpoint_declared_slices_by_id.end() ? SnapshotConeSliceFact{} : found->second;
}

void appendConnectionBinding(SnapshotDesignGraphBindingIndex& index,
                             SnapshotGraphConnectionBindingFact binding) {
    if (binding.instance_stable_id.empty() || binding.endpoint_stable_id.empty() ||
        binding.location.uri.empty() ||
        (binding.source_symbol_ids.empty() && binding.source_parts.empty() && !binding.unresolved)) {
        return;
    }
    for (const auto& part : binding.source_parts) {
        if (!part.source_symbol_id.empty()) {
            binding.source_symbol_ids.push_back(part.source_symbol_id);
        }
    }
    std::sort(binding.source_symbol_ids.begin(), binding.source_symbol_ids.end());
    binding.source_symbol_ids.erase(std::unique(binding.source_symbol_ids.begin(), binding.source_symbol_ids.end()),
                                    binding.source_symbol_ids.end());
    const auto key = uriRangeKey(binding.location.uri, binding.location.range);
    index.connection_bindings_by_uri_range[key].push_back(index.connection_bindings.size());
    index.connection_bindings_by_instance_id[binding.instance_stable_id].push_back(
        index.connection_bindings.size());
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

bool isStaticSchematicSource(const SnapshotGraphConnectionBindingFact::SourcePart& part) {
    return !part.unresolved && !part.source_symbol_id.empty() &&
           (part.source_slice.precision == SnapshotConeSlicePrecision::Whole ||
            part.source_slice.precision == SnapshotConeSlicePrecision::Exact);
}

std::string displayLabelForSchematicSource(const SnapshotData& data,
                                           const SnapshotGraphConnectionBindingFact::SourcePart& part) {
    const auto symbol = data.symbols_by_id.find(part.source_symbol_id);
    if (symbol == data.symbols_by_id.end() || symbol->second.identity.name.empty()) {
        return "<partial>";
    }
    auto label = symbol->second.identity.name;
    if (part.source_slice.precision != SnapshotConeSlicePrecision::Exact || !part.source_slice.msb ||
        !part.source_slice.lsb) {
        return label;
    }
    label += "[" + std::to_string(*part.source_slice.msb);
    if (*part.source_slice.msb != *part.source_slice.lsb) {
        label += ":" + std::to_string(*part.source_slice.lsb);
    }
    return label + "]";
}

std::string displayLabelForSchematicConnection(
    const SnapshotData& data,
    const std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& source_parts,
    bool unresolved,
    std::string_view literal_display) {
    std::vector<std::string> labels;
    bool partial = unresolved;
    for (const auto& part : source_parts) {
        if (!isStaticSchematicSource(part)) {
            partial = true;
            continue;
        }
        labels.push_back(displayLabelForSchematicSource(data, part));
    }
    // A schematic net must represent a proven source-to-endpoint mapping. Do
    // not label a dynamic / aggregate connection with only its exact-looking
    // control or operand fragments.
    if (partial) {
        return "<partial>";
    }
    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    if (labels.empty()) {
        if (partial) return "<partial>";
        return literal_display.empty() ? "<constant>" : std::string(literal_display);
    }
    if (labels.size() == 1) {
        return labels.front();
    }
    std::string label = "{";
    for (size_t index = 0; index < labels.size(); ++index) {
        if (index != 0) label += ",";
        label += labels[index];
    }
    return label + "}";
}

bool hasPartialSchematicSource(const SnapshotSchematicConnectionFact& fact) {
    if (fact.unresolved) return true;
    return std::any_of(fact.source_parts.begin(), fact.source_parts.end(), [](const auto& part) {
        return !isStaticSchematicSource(part);
    });
}

void appendSchematicConnectionFact(
    SnapshotData& data,
    SnapshotDesignGraphBindingIndex& bindings,
    std::string caller_module_name,
    const SnapshotModuleInstance& instance,
    const SnapshotGraphEndpointFact& endpoint,
    const SnapshotResolvedConnectionSliceFact& connection) {
    SnapshotSchematicConnectionFact fact{.caller_module_name = std::move(caller_module_name),
                                         .instance_stable_id = instance.instance_stable_id,
                                         .instance_name = instance.instance_name,
                                         .instance_selection_range = instance.selection_range,
                                         .endpoint_stable_id = endpoint.stable_id,
                                         .endpoint_name = endpoint.name,
                                         .endpoint_index = connection.endpoint_index,
                                         .endpoint_direction = endpoint.direction,
                                         .location = connection.location,
                                         .kind = connection.kind,
                                         .source_parts = connection.source_parts,
                                         .display_label = {},
                                         .unresolved = connection.unresolved};
    fact.display_label =
        displayLabelForSchematicConnection(data, fact.source_parts, fact.unresolved, connection.literal_display);
    ++bindings.schematic_connection_fact_count;
    if (hasPartialSchematicSource(fact)) {
        ++bindings.schematic_partial_connection_fact_count;
    }
    bindings.schematic_connections_by_module[fact.caller_module_name].push_back(std::move(fact));
}

bool schematicFactLess(const SnapshotSchematicConnectionFact& lhs,
                       const SnapshotSchematicConnectionFact& rhs) {
    if (lhs.instance_stable_id != rhs.instance_stable_id) {
        return lhs.instance_stable_id < rhs.instance_stable_id;
    }
    if (lhs.kind != rhs.kind) return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
    if (lhs.endpoint_index != rhs.endpoint_index) return lhs.endpoint_index < rhs.endpoint_index;
    if (lhs.endpoint_stable_id != rhs.endpoint_stable_id) return lhs.endpoint_stable_id < rhs.endpoint_stable_id;
    if (!sameLocation(lhs.location, rhs.location)) return locationLess(lhs.location, rhs.location);
    return lhs.display_label < rhs.display_label;
}

void indexSchematicCellIds(const SnapshotData& data, SnapshotDesignGraphBindingIndex& bindings) {
    bindings.schematic_cell_ids_by_instance_id.clear();
    for (const auto& [_, signature] : data.ast_module_signatures_by_name) {
        if (signature.uri.empty()) {
            continue;
        }
        for (const auto& cell : signature.schematic.cells) {
            const auto instance = bindings.instance_ids_by_uri_range.find(
                uriRangeKey(signature.uri, cell.selection_range));
            if (instance == bindings.instance_ids_by_uri_range.end() || cell.id.empty()) {
                continue;
            }
            const auto existing = bindings.schematic_cell_ids_by_instance_id.find(instance->second);
            if (existing == bindings.schematic_cell_ids_by_instance_id.end() || cell.id < existing->second) {
                bindings.schematic_cell_ids_by_instance_id.insert_or_assign(instance->second, cell.id);
            }
        }
    }
    for (auto& [_, facts] : bindings.schematic_connections_by_module) {
        std::sort(facts.begin(), facts.end(), schematicFactLess);
    }
}

void buildCallerModuleBindings(SnapshotData& data, SnapshotDesignGraphBindingIndex& bindings) {
    bindings.caller_module_names_by_instance_id.clear();
    struct ModuleScope {
        ParseRange range;
        std::string name;
    };
    std::unordered_map<std::string, std::vector<ModuleScope>> scopes_by_uri;
    for (const auto& [name, signature] : data.ast_module_signatures_by_name) {
        if (!signature.uri.empty()) {
            scopes_by_uri[signature.uri].push_back(ModuleScope{.range = signature.definition.range,
                                                                .name = name});
        }
    }
    for (auto& [_, scopes] : scopes_by_uri) {
        std::sort(scopes.begin(), scopes.end(), [](const ModuleScope& lhs, const ModuleScope& rhs) {
            if (!sameRange(lhs.range, rhs.range)) return rangeStartLess(lhs.range, rhs.range);
            return lhs.name < rhs.name;
        });
    }
    const auto range_size = [](const ParseRange& range) {
        return (range.end_line - range.start_line) * 100000 +
               (range.end_character - range.start_character);
    };
    for (const auto& [uri, instances] : data.module_instances_by_uri) {
        const auto scopes = scopes_by_uri.find(uri);
        if (scopes == scopes_by_uri.end()) continue;
        for (const auto& instance : instances) {
            if (instance.instance_stable_id.empty()) continue;
            const ModuleScope* best = nullptr;
            for (const auto& candidate : scopes->second) {
                if (!rangeContainsRange(candidate.range, instance.selection_range)) continue;
                if (best == nullptr || range_size(candidate.range) < range_size(best->range) ||
                    (range_size(candidate.range) == range_size(best->range) && candidate.name < best->name)) {
                    best = &candidate;
                }
            }
            if (best != nullptr) {
                bindings.caller_module_names_by_instance_id.insert_or_assign(instance.instance_stable_id,
                                                                              best->name);
            }
        }
    }
}
void projectSchematicCellPins(SnapshotData& data, SnapshotDesignGraphBindingIndex& bindings) {
    bindings.schematic_cell_pins_by_module.clear();
    bindings.schematic_cell_pin_fact_count = 0;
    bindings.schematic_partial_cell_pin_fact_count = 0;
    for (const auto& fact : data.schematic_cell_pin_facts) {
        if (!fact.caller_module_name.empty() && !fact.cell_id.empty()) {
            bindings.schematic_cell_pins_by_module[fact.caller_module_name].push_back(fact);
        }
    }
    for (auto& [_, facts] : bindings.schematic_cell_pins_by_module) {
        const auto less = [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.cell_id,
                            lhs.cell_kind,
                            lhs.pin_direction,
                            lhs.cell_type,
                            lhs.location.uri,
                            lhs.location.range.start_line,
                            lhs.location.range.start_character,
                            lhs.net_symbol_id,
                            lhs.source_role,
                            lhs.net_slice.precision,
                            lhs.net_slice.msb,
                            lhs.net_slice.lsb,
                            lhs.unresolved,
                            lhs.literal) <
                   std::tie(rhs.cell_id,
                            rhs.cell_kind,
                            rhs.pin_direction,
                            rhs.cell_type,
                            rhs.location.uri,
                            rhs.location.range.start_line,
                            rhs.location.range.start_character,
                            rhs.net_symbol_id,
                            rhs.source_role,
                            rhs.net_slice.precision,
                            rhs.net_slice.msb,
                            rhs.net_slice.lsb,
                            rhs.unresolved,
                            rhs.literal);
        };
        std::sort(facts.begin(), facts.end(), less);
        facts.erase(std::unique(facts.begin(), facts.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.cell_id == rhs.cell_id && lhs.cell_kind == rhs.cell_kind &&
                               lhs.cell_type == rhs.cell_type && lhs.pin_direction == rhs.pin_direction && sameLocation(lhs.location, rhs.location) &&
                               lhs.net_symbol_id == rhs.net_symbol_id && lhs.source_role == rhs.source_role &&
                               lhs.net_slice.precision == rhs.net_slice.precision &&
                               lhs.net_slice.msb == rhs.net_slice.msb && lhs.net_slice.lsb == rhs.net_slice.lsb &&
                               lhs.unresolved == rhs.unresolved && lhs.literal == rhs.literal;
                    }),
                    facts.end());

        std::map<std::pair<std::string, SnapshotSchematicCellPinDirection>, int> pin_ordinals;
        for (auto& fact : facts) {
            if ((fact.cell_kind != SnapshotSchematicCellKind::Assignment &&
                 fact.cell_kind != SnapshotSchematicCellKind::Operator) ||
                fact.pin_direction == SnapshotSchematicCellPinDirection::Output ||
                fact.pin_direction == SnapshotSchematicCellPinDirection::Unknown) {
                continue;
            }
            const auto ordinal = pin_ordinals[{fact.cell_id, fact.pin_direction}]++;
            fact.pin_index = ordinal;
            if (fact.pin_direction == SnapshotSchematicCellPinDirection::Control) {
                fact.pin_name = ordinal == 0 ? "S" : "S" + std::to_string(ordinal);
            }
            else if (fact.cell_type == "mux") {
                fact.pin_name = ordinal == 0 ? "I1" : "I" + std::to_string(ordinal - 1);
            }
            else {
                fact.pin_name = ordinal == 0 ? "A" : "A" + std::to_string(ordinal);
            }
        }
        std::sort(facts.begin(), facts.end(), less);
        for (const auto& fact : facts) {
            ++bindings.schematic_cell_pin_fact_count;
            if (fact.unresolved || fact.literal || fact.net_symbol_id.empty() ||
                (fact.net_slice.precision != SnapshotConeSlicePrecision::Whole &&
                 fact.net_slice.precision != SnapshotConeSlicePrecision::Exact)) {
                ++bindings.schematic_partial_cell_pin_fact_count;
            }
        }
    }
}
bool isStaticPrimitiveCellPin(const SnapshotSchematicCellPinFact& fact) {
    return !fact.unresolved && !fact.literal && !fact.net_symbol_id.empty() &&
           (fact.net_slice.precision == SnapshotConeSlicePrecision::Whole ||
            fact.net_slice.precision == SnapshotConeSlicePrecision::Exact);
}

void appendPrimitiveCellConeEdges(const SnapshotDesignGraphBindingIndex& bindings,
                                  SnapshotConeAdjacencyIndex& adjacency) {
    const auto append = [&](const SnapshotSchematicCellPinFact& sink,
                            const SnapshotSchematicCellPinFact& source,
                            SnapshotConeSourceRole role,
                            SnapshotConeControlOrigin origin) {
        appendEdge(adjacency,
                   SnapshotConeAdjacencyEdge{.from_symbol_id = sink.net_symbol_id,
                                              .to_symbol_id = source.net_symbol_id,
                                              .location = SemanticLocation{.uri = sink.location.uri,
                                                                           .range = sink.cell_selection_range},
                                              .target_location = sink.location,
                                              .expression_location = source.location,
                                              .expression = source.display_label.empty()
                                                                ? source.pin_name
                                                                : source.display_label,
                                              .kind = SnapshotConeEdgeKind::PrimitiveCell,
                                              .source_role = role,
                                              .slice_kind = SnapshotConeSliceKind::Whole,
                                              .control_origin = origin,
                                              .source_slice = source.net_slice,
                                              .sink_slice = sink.net_slice,
                                              .generated_instance_id = {}});
    };
    const auto appendUnresolved = [&](const SnapshotSchematicCellPinFact& sink,
                                      const SnapshotSchematicCellPinFact& source,
                                      SnapshotConeSourceRole role,
                                      SnapshotConeControlOrigin origin) {
        if (sink.net_symbol_id.empty()) {
            return;
        }
        adjacency.unresolved_sources_by_from_symbol_id[sink.net_symbol_id].push_back(
            SnapshotConeUnresolvedSourceFact{.from_symbol_id = sink.net_symbol_id,
                                             .location = SemanticLocation{.uri = sink.location.uri,
                                                                          .range = sink.cell_selection_range},
                                             .expression_location = source.location,
                                             .expression = "primitive pin " + source.pin_name,
                                             .kind = SnapshotConeEdgeKind::PrimitiveCell,
                                             .source_role = role,
                                             .control_origin = origin});
    };

    for (const auto& [_, pins] : bindings.schematic_cell_pins_by_module) {
        for (const auto& sink : pins) {
            if (sink.cell_kind != SnapshotSchematicCellKind::Primitive ||
                sink.pin_direction != SnapshotSchematicCellPinDirection::Output ||
                !isStaticPrimitiveCellPin(sink)) {
                continue;
            }
            for (const auto& source : pins) {
                if (source.cell_kind != SnapshotSchematicCellKind::Primitive ||
                    source.cell_id != sink.cell_id || source.pin_index == sink.pin_index) {
                    continue;
                }
                const auto role = source.pin_direction == SnapshotSchematicCellPinDirection::Control
                                      ? SnapshotConeSourceRole::Control
                                      : SnapshotConeSourceRole::Data;
                const auto origin = role == SnapshotConeSourceRole::Control
                                        ? SnapshotConeControlOrigin::PrimitiveControl
                                        : SnapshotConeControlOrigin::None;
                if (source.pin_direction != SnapshotSchematicCellPinDirection::Input &&
                    source.pin_direction != SnapshotSchematicCellPinDirection::Control) {
                    continue;
                }
                if (isStaticPrimitiveCellPin(source)) {
                    append(sink, source, role, origin);
                }
                else {
                    appendUnresolved(sink, source, role, origin);
                }
            }
        }

        for (size_t left_index = 0; left_index < pins.size(); ++left_index) {
            const auto& left = pins[left_index];
            if (left.cell_kind != SnapshotSchematicCellKind::Primitive ||
                left.pin_direction != SnapshotSchematicCellPinDirection::Inout ||
                !isStaticPrimitiveCellPin(left)) {
                continue;
            }
            for (size_t right_index = left_index + 1; right_index < pins.size(); ++right_index) {
                const auto& right = pins[right_index];
                if (right.cell_kind != SnapshotSchematicCellKind::Primitive ||
                    right.cell_id != left.cell_id ||
                    right.pin_direction != SnapshotSchematicCellPinDirection::Inout ||
                    !isStaticPrimitiveCellPin(right)) {
                    continue;
                }
                append(left, right, SnapshotConeSourceRole::Data, SnapshotConeControlOrigin::None);
                append(right, left, SnapshotConeSourceRole::Data, SnapshotConeControlOrigin::None);
            }
            for (const auto& control : pins) {
                if (control.cell_kind != SnapshotSchematicCellKind::Primitive ||
                    control.cell_id != left.cell_id ||
                    control.pin_direction != SnapshotSchematicCellPinDirection::Control) {
                    continue;
                }
                if (isStaticPrimitiveCellPin(control)) {
                    append(left,
                           control,
                           SnapshotConeSourceRole::Control,
                           SnapshotConeControlOrigin::PrimitiveControl);
                }
                else {
                    appendUnresolved(left,
                                     control,
                                     SnapshotConeSourceRole::Control,
                                     SnapshotConeControlOrigin::PrimitiveControl);
                }
            }
        }
    }
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
                                                     .modport_stable_id = modport_id,
                                                     .declared_slice = endpointDeclaredSlice(data, *stable_id)});
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
                                                     .modport_stable_id = {},
                                                     .declared_slice = endpointDeclaredSlice(data,
                                                                                              stable_id->second)});
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
                                                     .modport_stable_id = {},
                                                     .declared_slice = {}});
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

    buildCallerModuleBindings(data, bindings);
    projectSchematicCellPins(data, bindings);
    appendPrimitiveCellConeEdges(bindings, adjacency);
    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            const auto caller = bindings.caller_module_names_by_instance_id.find(instance.instance_stable_id);
            const auto child = data.ast_module_signatures_by_name.find(instance.module_name);
            if (instance.instance_stable_id.empty() ||
                caller == bindings.caller_module_names_by_instance_id.end() ||
                child == data.ast_module_signatures_by_name.end()) continue;
            const auto parent = data.ast_module_signatures_by_name.find(caller->second);
            if (parent == data.ast_module_signatures_by_name.end() || parent->second.uri.empty()) continue;
            const auto resolved_connections =
                data.resolved_connection_slices_by_instance_id.find(instance.instance_stable_id);
            if (resolved_connections == data.resolved_connection_slices_by_instance_id.end()) {
                continue;
            }
            for (const auto& connection : resolved_connections->second) {
                auto child_endpoint = bindings.endpoints_by_stable_id.find(connection.endpoint_stable_id);
                if (child_endpoint == bindings.endpoints_by_stable_id.end()) {
                    continue;
                }
                const auto& endpoint = child_endpoint->second;
                const auto location = connection.location;
                appendSchematicConnectionFact(data, bindings, caller->second, instance, endpoint, connection);
                if (connection.kind == SnapshotConeEdgeKind::InstancePort &&
                    endpoint.kind == SnapshotGraphEndpointKind::InterfacePort) {
                    const auto port_binding = data.interface_modport_binding_index.ports_by_stable_id.find(
                        endpoint.stable_id);
                    if (port_binding == data.interface_modport_binding_index.ports_by_stable_id.end() ||
                        !port_binding->second.resolved || port_binding->second.modport_stable_id.empty() ||
                        port_binding->second.connected_modport_stable_id.empty()) {
                        continue;
                    }
                    const auto child_members = data.interface_modport_binding_index.members_by_modport_stable_id.find(
                        port_binding->second.modport_stable_id);
                    const auto parent_members = data.interface_modport_binding_index.members_by_modport_stable_id.find(
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
                                       SnapshotConeAdjacencyEdge{.from_symbol_id = std::move(from),
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

                bindings.connection_reference_candidate_count += connection.source_parts.size();
                appendConnectionBinding(bindings,
                                        SnapshotGraphConnectionBindingFact{
                                            .instance_stable_id = connection.instance_stable_id,
                                            .endpoint_stable_id = endpoint.stable_id,
                                            .location = connection.location,
                                            .kind = connection.kind,
                                            .source_symbol_ids = {},
                                            .source_parts = connection.source_parts,
                                            .unresolved = connection.unresolved});
                const auto append_unresolved = [&]() {
                    if (!connection.unresolved) {
                        return;
                    }
                    adjacency.unresolved_sources_by_from_symbol_id[endpoint.stable_id].push_back(
                        SnapshotConeUnresolvedSourceFact{.from_symbol_id = endpoint.stable_id,
                                                         .location = location,
                                                         .expression_location = location,
                                                         .expression = "connection",
                                                         .kind = connection.kind,
                                                         .source_role = SnapshotConeSourceRole::Data,
                                                         .control_origin = SnapshotConeControlOrigin::None});
                };
                append_unresolved();

                for (const auto& part : connection.source_parts) {
                    if (part.unresolved || part.source_symbol_id.empty()) {
                        continue;
                    }
                    const auto append_data_edge = [&](bool child_to_parent) {
                        const auto from = child_to_parent ? endpoint.stable_id : part.source_symbol_id;
                        const auto to = child_to_parent ? part.source_symbol_id : endpoint.stable_id;
                        const auto source_slice = child_to_parent ? part.source_slice : part.endpoint_slice;
                        const auto sink_slice = child_to_parent ? part.endpoint_slice : part.source_slice;
                        const auto target_location = child_to_parent ? endpoint.location : part.source_location;
                        const auto source_symbol = data.symbols_by_id.find(part.source_symbol_id);
                        const auto expression = source_symbol == data.symbols_by_id.end()
                                                    ? endpoint.name
                                                    : source_symbol->second.identity.name;
                        appendEdge(adjacency,
                                   SnapshotConeAdjacencyEdge{.from_symbol_id = from,
                                                              .to_symbol_id = to,
                                                              .location = location,
                                                              .target_location = target_location,
                                                              .expression_location = part.source_location,
                                                              .expression = expression,
                                                              .kind = connection.kind,
                                                              .source_role = part.source_role,
                                                              .slice_kind = part.slice_kind,
                                                              .control_origin = part.control_origin,
                                                              .source_slice = source_slice,
                                                              .sink_slice = sink_slice,
                                                              .generated_instance_id = instance.instance_stable_id});
                    };
                    if (connection.kind == SnapshotConeEdgeKind::ParameterOverride ||
                        endpoint.direction == SnapshotGraphPortDirection::Input) {
                        append_data_edge(true);
                    }
                    else if (endpoint.direction == SnapshotGraphPortDirection::Output) {
                        append_data_edge(false);
                    }
                    else if (endpoint.direction == SnapshotGraphPortDirection::Inout ||
                             endpoint.direction == SnapshotGraphPortDirection::Ref) {
                        append_data_edge(true);
                        append_data_edge(false);
                    }
                }
            }
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
        if (lhs.control_origin != rhs.control_origin) {
            return static_cast<int>(lhs.control_origin) < static_cast<int>(rhs.control_origin);
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
                                     lhs.control_origin == rhs.control_origin &&
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

    indexSchematicCellIds(data, bindings);

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
    bindings.connection_bindings_by_instance_id.clear();
    for (size_t index = 0; index < bindings.connection_bindings.size(); ++index) {
        const auto& binding = bindings.connection_bindings[index];
        bindings.connection_bindings_by_uri_range[uriRangeKey(binding.location.uri, binding.location.range)]
            .push_back(index);
        bindings.connection_bindings_by_instance_id[binding.instance_stable_id].push_back(index);
    }
}

} // namespace pristine::analysis::semantic
