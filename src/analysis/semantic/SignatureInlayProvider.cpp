#include "SignatureInlayProvider.h"

#include "CompletionProvider.h"
#include "pristine/analysis/SourceUtil.h"
#include "pristine/text/Utf.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

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

std::optional<size_t> openParenBeforePosition(std::string_view text,
                                              size_t search_start,
                                              size_t search_end) {
    if (search_start >= text.size() || search_start >= search_end) {
        return std::nullopt;
    }
    const auto bounded_end = std::min(search_end, text.size());
    for (size_t offset = search_start; offset < bounded_end; ++offset) {
        if (text[offset] == '(') {
            return offset;
        }
    }
    return std::nullopt;
}

std::optional<ParseRange> pointRangeAtUtf8Offset(std::string_view text, size_t offset) {
    if (offset > text.size()) {
        return std::nullopt;
    }

    int line = 0;
    size_t line_start = 0;
    size_t current = 0;
    while (current < offset) {
        const char value = text[current];
        if (value == '\n') {
            ++line;
            ++current;
            line_start = current;
            continue;
        }
        if (value == '\r') {
            ++line;
            ++current;
            if (current < offset && text[current] == '\n') {
                ++current;
            }
            line_start = current;
            continue;
        }
        try {
            const auto decoded = text::decodeNextCodePoint(text, current);
            if (current + decoded.byte_length > offset) {
                return std::nullopt;
            }
            current += decoded.byte_length;
        }
        catch (const std::runtime_error&) {
            return std::nullopt;
        }
    }

    try {
        const auto character = static_cast<int>(
            text::utf16UnitsForUtf8Prefix(text.substr(line_start, offset - line_start),
                                          offset - line_start));
        return ParseRange{.start_line = line,
                          .start_character = character,
                          .end_line = line,
                          .end_character = character};
    }
    catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<ParseRange> argumentStartRanges(std::string_view text,
                                            const SignatureInlayCall& call) {
    std::vector<ParseRange> ranges;
    if (call.parameters.empty()) {
        return ranges;
    }

    const auto search_start = utf8OffsetAtUtf16Position(text,
                                                        call.selection_range.end_line,
                                                        call.selection_range.end_character);
    const auto search_end = utf8OffsetAtUtf16Position(text,
                                                      call.range.end_line,
                                                      call.range.end_character);
    if (!search_start.has_value() || !search_end.has_value()) {
        return ranges;
    }
    const auto open_paren = openParenBeforePosition(text, *search_start, *search_end);
    if (!open_paren.has_value()) {
        return ranges;
    }

    size_t argument_start = *open_paren + 1;
    int depth = 0;
    for (size_t offset = *open_paren + 1; offset < *search_end && offset < text.size(); ++offset) {
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
        if (value != ',' || depth != 0) {
            continue;
        }
        while (argument_start < offset &&
               std::isspace(static_cast<unsigned char>(text[argument_start])) != 0) {
            ++argument_start;
        }
        if (auto range = pointRangeAtUtf8Offset(text, argument_start)) {
            ranges.push_back(*range);
        }
        argument_start = offset + 1;
    }

    while (argument_start < *search_end &&
           std::isspace(static_cast<unsigned char>(text[argument_start])) != 0) {
        ++argument_start;
    }
    if (argument_start < *search_end) {
        if (auto range = pointRangeAtUtf8Offset(text, argument_start)) {
            ranges.push_back(*range);
        }
    }

    if (ranges.size() > call.parameters.size()) {
        ranges.resize(call.parameters.size());
    }
    return ranges;
}

std::optional<size_t> macroInvocationOpenParen(std::string_view text,
                                               const MacroDefinition& macro,
                                               size_t position_offset) {
    if (!macro.function_like || macro.name.empty()) {
        return std::nullopt;
    }
    const auto bounded_position = std::min(position_offset, text.size());
    if (bounded_position == 0) {
        return std::nullopt;
    }

    const auto invocation = std::string("`") + macro.name;
    auto search_end = bounded_position;
    while (search_end > 0) {
        const auto found = text.rfind(invocation, search_end - 1);
        if (found == std::string_view::npos) {
            break;
        }
        const auto name_end = found + invocation.size();
        if (name_end < text.size() && (std::isalnum(static_cast<unsigned char>(text[name_end])) ||
                                       text[name_end] == '_')) {
            search_end = found;
            continue;
        }
        auto open_paren = name_end;
        while (open_paren < text.size() &&
               std::isspace(static_cast<unsigned char>(text[open_paren]))) {
            ++open_paren;
        }
        if (open_paren < text.size() && text[open_paren] == '(' &&
            open_paren < bounded_position) {
            return open_paren;
        }
        search_end = found;
    }
    return std::nullopt;
}

std::string callSignatureLabel(const SignatureInlayCall& call) {
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
    if (context.document_text == nullptr) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }

    const auto position_offset = utf8OffsetAtUtf16Position(*context.document_text, line, character);
    if (!position_offset.has_value()) {
        result.unresolved = true;
        result.messages.push_back("signature help position could not be mapped to a source offset");
        return result;
    }

    for (const auto& macro : context.macros) {
        const auto open_paren = macroInvocationOpenParen(*context.document_text,
                                                         macro,
                                                         *position_offset);
        if (!open_paren.has_value()) {
            continue;
        }
        result.label = macroSignatureLabel(macro);
        result.parameters = macro.parameters;
        const auto parameter_count = result.parameters.size();
        result.active_parameter = parameter_count == 0
                                      ? 0
                                      : std::min(activeParameterAt(*context.document_text,
                                                                   *open_paren,
                                                                   *position_offset),
                                                 static_cast<int>(parameter_count) - 1);
        return result;
    }

    for (const auto& call : context.calls) {
        if (!parseRangeContainsPosition(call.range, line, character)) {
            continue;
        }
        const auto search_start = utf8OffsetAtUtf16Position(*context.document_text,
                                                            call.selection_range.end_line,
                                                            call.selection_range.end_character);
        const auto search_end = utf8OffsetAtUtf16Position(*context.document_text,
                                                          call.range.end_line,
                                                          call.range.end_character);
        if (!search_start.has_value() || !search_end.has_value()) {
            continue;
        }
        const auto open_paren = openParenBeforePosition(*context.document_text,
                                                        *search_start,
                                                        std::min(*position_offset, *search_end));
        if (!open_paren.has_value()) {
            continue;
        }
        result.label = callSignatureLabel(call);
        result.parameters = call.parameters;
        const auto parameter_count = result.parameters.size();
        result.active_parameter = parameter_count == 0
                                      ? 0
                                      : std::min(activeParameterAt(*context.document_text,
                                                                   *open_paren,
                                                                   *position_offset),
                                                 static_cast<int>(parameter_count) - 1);
        return result;
    }

    for (const auto& instance : context.module_instances) {
        if (!parseRangeContainsPosition(instance.range, line, character)) {
            continue;
        }
        const auto search_start = utf8OffsetAtUtf16Position(*context.document_text,
                                                            instance.selection_range.end_line,
                                                            instance.selection_range.end_character);
        const auto search_end = utf8OffsetAtUtf16Position(*context.document_text,
                                                          instance.range.end_line,
                                                          instance.range.end_character);
        if (!search_start.has_value() || !search_end.has_value()) {
            continue;
        }
        const auto open_paren = openParenBeforePosition(*context.document_text,
                                                        *search_start,
                                                        std::min(*position_offset, *search_end));
        if (!open_paren.has_value()) {
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
        const auto parameter_count = result.parameters.size();
        result.active_parameter = parameter_count == 0
                                      ? 0
                                      : std::min(activeParameterAt(*context.document_text,
                                                                   *open_paren,
                                                                   *position_offset),
                                                 static_cast<int>(parameter_count) - 1);
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
        if (symbol.identity.location.uri != context.document_uri || symbol.type_display.empty() ||
            !rangesOverlapOrTouch(symbol.identity.location.range, range)) {
            continue;
        }
        result.hints.push_back(SemanticInlayHint{.location = symbol.identity.location,
                                                 .label = ": " + symbol.type_display,
                                                 .kind = "type",
                                                 .tooltip = "Resolved type"});
    }

    for (const auto& instance : context.module_instances) {
        if (!rangesOverlapOrTouch(instance.selection_range, range)) {
            continue;
        }
        const auto module_it = context.modules_by_name == nullptr
                                   ? std::unordered_map<std::string, ModuleDefinition>::const_iterator{}
                                   : context.modules_by_name->find(instance.module_name);
        const auto module_found = context.modules_by_name != nullptr &&
                                  module_it != context.modules_by_name->end();
        result.hints.push_back(SemanticInlayHint{
            .location = SemanticLocation{.uri = context.document_uri,
                                         .range = ParseRange{
                                             .start_line = instance.selection_range.end_line,
                                             .start_character = instance.selection_range.end_character,
                                             .end_line = instance.selection_range.end_line,
                                             .end_character = instance.selection_range.end_character}},
            .label = ": " + instance.module_name,
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

    if (context.document_text != nullptr) {
        for (const auto& call : context.calls) {
            if (!rangesOverlapOrTouch(call.range, range)) {
                continue;
            }
            const auto argument_ranges = argumentStartRanges(*context.document_text, call);
            for (size_t index = 0; index < argument_ranges.size() && index < call.parameters.size(); ++index) {
                if (!rangesOverlapOrTouch(argument_ranges[index], range)) {
                    continue;
                }
                const auto label = call.parameters[index].empty()
                                       ? std::string{"arg"}
                                       : call.parameters[index] + ":";
                const auto duplicate = std::any_of(result.hints.begin(),
                                                   result.hints.end(),
                                                   [&](const SemanticInlayHint& hint) {
                                                       return hint.kind == "parameter" &&
                                                              hint.label == label &&
                                                              sameRange(hint.location.range,
                                                                        argument_ranges[index]);
                                                   });
                if (duplicate) {
                    continue;
                }
                result.hints.push_back(SemanticInlayHint{
                    .location = SemanticLocation{.uri = context.document_uri,
                                                 .range = argument_ranges[index]},
                    .label = label,
                    .kind = "parameter",
                    .tooltip = callSignatureLabel(call)});
            }
        }
    }
    std::sort(result.hints.begin(), result.hints.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

} // namespace pristine::analysis::semantic
