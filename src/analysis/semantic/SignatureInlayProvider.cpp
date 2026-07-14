#include "SignatureInlayProvider.h"

#include "pristine/analysis/SourceUtil.h"
#include "CompletionProvider.h"
#include <algorithm>
namespace pristine::analysis::semantic {
namespace {

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

bool rangesOverlapOrTouch(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.end_line < rhs.start_line || rhs.end_line < lhs.start_line) {
        return false;
    }
    if (lhs.end_line == rhs.start_line && lhs.end_character < rhs.start_character) {
        return false;
    }
    if (rhs.end_line == lhs.start_line && rhs.end_character < lhs.start_character) {
        return false;
    }
    return true;
}

bool sameRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line &&
           lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line &&
           lhs.end_character == rhs.end_character;
}

const SchematicPort* portForConnection(const ModuleDefinition& module,
                                       const SchematicConnection& connection) {
    if (!connection.port_name.empty()) {
        const auto found_parameter = std::find_if(module.parameter_details.begin(),
                                                  module.parameter_details.end(),
                                                  [&](const SchematicPort& port) {
                                                      return port.name == connection.port_name;
                                                  });
        if (found_parameter != module.parameter_details.end()) {
            return &*found_parameter;
        }
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
    if (module.port_details.empty() && connection.port_index >= 0 &&
        static_cast<size_t>(connection.port_index) < module.parameter_details.size()) {
        return &module.parameter_details[static_cast<size_t>(connection.port_index)];
    }
    return nullptr;
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

int activeParameterAt(const std::vector<ParseRange>& argument_ranges,
                      size_t parameter_count,
                      int line,
                      int character) {
    if (parameter_count == 0) {
        return 0;
    }
    size_t active = 0;
    for (const auto& argument : argument_ranges) {
        if (comparePosition(line,
                            character,
                            argument.end_line,
                            argument.end_character) <= 0) {
            break;
        }
        ++active;
    }
    return static_cast<int>(std::min(active, parameter_count - 1));
}

ParseRange pointAtRangeStart(const ParseRange& range) {
    return ParseRange{.start_line = range.start_line,
                      .start_character = range.start_character,
                      .end_line = range.start_line,
                      .end_character = range.start_character};
}

bool rangeIsNarrowerAtPosition(const ParseRange& candidate, const ParseRange& current) {
    if (candidate.start_line != current.start_line) {
        return candidate.start_line > current.start_line;
    }
    if (candidate.start_character != current.start_character) {
        return candidate.start_character > current.start_character;
    }
    if (candidate.end_line != current.end_line) {
        return candidate.end_line < current.end_line;
    }
    return candidate.end_character < current.end_character;
}


std::string callSignatureLabel(const CallableInvocationFact& call) {
    std::string label;
    if (!call.kind.empty()) {
        label += call.kind;
        label += " ";
    }
    if (!call.return_type.empty() && call.kind == "function") {
        label += call.return_type;
        label += " ";
    }
    label += call.name;
    label += "(";
    for (size_t index = 0; index < call.parameters.size(); ++index) {
        if (index != 0) {
            label += ", ";
        }
        label += call.parameters[index];
    }
    label += ")";
    return label;
}

} // namespace

SemanticSignatureHelpResult signatureHelpAt(const SignatureInlayContext& context,
                                            int line,
                                            int character) {
    SemanticSignatureHelpResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const MacroInvocationFact* matched_macro = nullptr;
    for (const auto& macro : context.macro_invocations) {
        ++result.scanned_macro_definition_count;
        if (!macro.function_like || !parseRangeContainsPosition(macro.range, line, character)) {
            continue;
        }
        if (matched_macro == nullptr ||
            rangeIsNarrowerAtPosition(macro.range, matched_macro->range)) {
            matched_macro = &macro;
        }
    }
    if (matched_macro != nullptr) {
        result.label = macroSignatureLabel(matched_macro->definition);
        result.parameters = matched_macro->definition.parameters;
        result.active_parameter = activeParameterAt(matched_macro->argument_ranges,
                                                    result.parameters.size(),
                                                    line,
                                                    character);
        return result;
    }

    const CallableInvocationFact* matched_call = nullptr;
    for (const auto& call : context.callable_invocations) {
        ++result.scanned_invocation_count;
        if (!parseRangeContainsPosition(call.range, line, character)) {
            continue;
        }
        if (matched_call == nullptr ||
            rangeIsNarrowerAtPosition(call.range, matched_call->range)) {
            matched_call = &call;
        }
    }
    if (matched_call != nullptr) {
        if (!matched_call->resolved) {
            result.unresolved = true;
            result.messages.push_back("callable target is unresolved in the AST snapshot");
            return result;
        }
        result.label = callSignatureLabel(*matched_call);
        result.parameters = matched_call->parameters;
        result.active_parameter = activeParameterAt(matched_call->argument_ranges,
                                                    result.parameters.size(),
                                                    line,
                                                    character);
        return result;
    }

    for (const auto& instance : context.module_instances) {
        ++result.scanned_invocation_count;
        if (!parseRangeContainsPosition(instance.range, line, character)) {
            continue;
        }
        if (context.modules_by_name == nullptr) {
            result.unresolved = true;
            result.messages.push_back("signature target module index is unavailable");
            return result;
        }
        const auto module_it = context.modules_by_name->find(instance.module_name);
        if (module_it == context.modules_by_name->end()) {
            result.unresolved = true;
            result.messages.push_back("signature target module is not indexed in the AST snapshot");
            return result;
        }

        result.label = moduleSignatureLabel(module_it->second);
        if (module_it->second.port_details.empty()) {
            result.parameters = module_it->second.ports;
        }
        else {
            for (const auto& port : module_it->second.port_details) {
                result.parameters.push_back(portSignatureLabel(port));
            }
        }
        std::vector<ParseRange> connection_ranges;
        connection_ranges.reserve(instance.connections.size());
        for (const auto& connection : instance.connections) {
            connection_ranges.push_back(connection.range);
        }
        result.active_parameter = activeParameterAt(connection_ranges,
                                                    result.parameters.size(), line, character);
        return result;
    }

    result.unresolved = true;
    result.messages.push_back("no AST-backed signature invocation at position");
    return result;
}

SemanticInlayHintResult inlayHints(const SignatureInlayContext& context,
                                   ParseRange range) {
    SemanticInlayHintResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& symbol : context.symbols) {
        if (symbol.identity.location.uri != context.document_uri ||
            !rangesOverlapOrTouch(symbol.identity.location.range, range)) {
            continue;
        }
        if (!symbol.type_display.empty()) {
            result.hints.push_back(SemanticInlayHint{.location = symbol.identity.location,
                                                     .label = ": " + symbol.type_display,
                                                     .kind = "type",
                                                     .tooltip = "Resolved type"});
        }
        if (!symbol.value_display.empty()) {
            const auto& symbol_range = symbol.identity.location.range;
            result.hints.push_back(SemanticInlayHint{
                .location = SemanticLocation{.uri = symbol.identity.location.uri,
                                             .range = ParseRange{
                                                 .start_line = symbol_range.end_line,
                                                 .start_character = symbol_range.end_character,
                                                 .end_line = symbol_range.end_line,
                                                 .end_character = symbol_range.end_character}},
                .label = " = " + symbol.value_display,
                .kind = "parameter",
                .tooltip = "Resolved constant value"});
        }
    }

    for (const auto& instance : context.module_instances) {
        ++result.scanned_invocation_count;
        if (!rangesOverlapOrTouch(instance.selection_range, range)) {
            continue;
        }
        const auto module_it = context.modules_by_name == nullptr
                                   ? std::unordered_map<std::string, ModuleDefinition>::const_iterator{}
                                   : context.modules_by_name->find(instance.module_name);
        const auto module_found = context.modules_by_name != nullptr &&
                                  module_it != context.modules_by_name->end();
        const auto type_display = instance.type_display.empty() ? instance.module_name
                                                                : instance.type_display;
        result.hints.push_back(SemanticInlayHint{
            .location = SemanticLocation{.uri = context.document_uri,
                                         .range = ParseRange{
                                             .start_line = instance.selection_range.end_line,
                                             .start_character = instance.selection_range.end_character,
                                             .end_line = instance.selection_range.end_line,
                                             .end_character = instance.selection_range.end_character}},
            .label = ": " + type_display,
            .kind = "type",
            .tooltip = module_found ? moduleSignatureLabel(module_it->second) : std::string{}});

        if (!module_found) {
            continue;
        }
        for (const auto& connection : instance.connections) {
            if (!rangesOverlapOrTouch(connection.range, range)) {
                continue;
            }
            const auto* port = portForConnection(module_it->second, connection);
            if (port == nullptr || port->name.empty()) {
                continue;
            }
            const ParseRange label_range{.start_line = connection.range.start_line,
                                         .start_character = connection.range.start_character,
                                         .end_line = connection.range.start_line,
                                         .end_character = connection.range.start_character};
            const auto duplicate = std::any_of(result.hints.begin(),
                                               result.hints.end(),
                                               [&](const SemanticInlayHint& hint) {
                                                   return hint.kind == "parameter" &&
                                                          hint.label == "." + port->name &&
                                                          sameRange(hint.location.range, label_range);
                                               });
            if (duplicate) {
                continue;
            }
            result.hints.push_back(SemanticInlayHint{
                .location = SemanticLocation{.uri = context.document_uri,
                                             .range = label_range},
                .label = "." + port->name,
                .kind = "parameter",
                .tooltip = portSignatureLabel(*port)});
        }
    }

    for (const auto& call : context.callable_invocations) {
        ++result.scanned_invocation_count;
        if (!call.resolved || !rangesOverlapOrTouch(call.range, range)) {
            continue;
        }
        for (size_t index = 0;
             index < call.argument_ranges.size() && index < call.parameters.size();
             ++index) {
            if (!rangesOverlapOrTouch(call.argument_ranges[index], range)) {
                continue;
            }
            const auto label = call.parameters[index].empty()
                                   ? std::string{"arg"}
                                   : call.parameters[index] + ":";
            const auto label_range = pointAtRangeStart(call.argument_ranges[index]);
            const auto duplicate = std::any_of(result.hints.begin(),
                                               result.hints.end(),
                                               [&](const SemanticInlayHint& hint) {
                                                   return hint.kind == "parameter" &&
                                                          hint.label == label &&
                                                          sameRange(hint.location.range, label_range);
                                               });
            if (!duplicate) {
                result.hints.push_back(SemanticInlayHint{
                    .location = SemanticLocation{.uri = context.document_uri,
                                                 .range = label_range},
                    .label = label,
                    .kind = "parameter",
                    .tooltip = callSignatureLabel(call)});
            }
        }
    }

    for (const auto& macro : context.macro_invocations) {
        ++result.scanned_macro_definition_count;
        if (!macro.resolved || !rangesOverlapOrTouch(macro.range, range)) {
            continue;
        }
        for (size_t index = 0;
             index < macro.argument_ranges.size() && index < macro.definition.parameters.size();
             ++index) {
            if (!rangesOverlapOrTouch(macro.argument_ranges[index], range)) {
                continue;
            }
            result.hints.push_back(SemanticInlayHint{
                .location = SemanticLocation{.uri = context.document_uri,
                                             .range = pointAtRangeStart(macro.argument_ranges[index])},
                .label = macro.definition.parameters[index] + ":",
                .kind = "parameter",
                .tooltip = macroSignatureLabel(macro.definition)});
        }
    }
    std::sort(result.hints.begin(), result.hints.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

} // namespace pristine::analysis::semantic
