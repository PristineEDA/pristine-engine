#include "pristine/analysis/CompilationService.h"

#include "pristine/text/Utf.h"

#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace pristine::analysis {
namespace {

struct ParsePosition {
    int line = 0;
    int character = 0;
};

struct ScanState {
    size_t offset = 0;
    int line = 0;
    int character = 0;
};

int toLspSeverity(slang::DiagnosticSeverity severity) {
    switch (severity) {
        case slang::DiagnosticSeverity::Ignored:
        case slang::DiagnosticSeverity::Note:
            return 3;
        case slang::DiagnosticSeverity::Warning:
            return 2;
        case slang::DiagnosticSeverity::Error:
        case slang::DiagnosticSeverity::Fatal:
            return 1;
    }

    return 1;
}

ParsePosition toParsePosition(const slang::SourceManager& source_manager,
                             std::string_view text,
                             slang::SourceLocation location) {
    const auto fallback_line = static_cast<int>(source_manager.getLineNumber(location)) - 1;
    const auto fallback_character = static_cast<int>(source_manager.getColumnNumber(location)) - 1;

    const auto byte_offset = location.offset();
    const auto byte_column = source_manager.getColumnNumber(location);
    if (byte_column == 0) {
        return ParsePosition{.line = fallback_line, .character = fallback_character};
    }

    const auto byte_index_in_line = byte_column - 1;
    if (byte_offset < byte_index_in_line || byte_offset > text.size()) {
        return ParsePosition{.line = fallback_line, .character = fallback_character};
    }

    const auto line_start_offset = byte_offset - byte_index_in_line;

    return ParsePosition{
        .line = fallback_line,
        .character = static_cast<int>(
            text::utf16UnitsForUtf8Prefix(text.substr(line_start_offset, byte_offset - line_start_offset),
                                          byte_offset - line_start_offset))};
}

ParseRange toParseRange(const slang::SourceManager& source_manager,
                       std::string_view text,
                       slang::SourceRange range) {
    const auto start_position = toParsePosition(source_manager, text, range.start());
    const auto end_position = toParsePosition(source_manager, text, range.end());

    return ParseRange{.start_line = start_position.line,
                      .start_character = start_position.character,
                      .end_line = end_position.line,
                      .end_character = end_position.character};
}

ParseRange toParseRange(const slang::SourceManager& source_manager,
                       std::string_view text,
                       const slang::Diagnostic& diagnostic) {
    slang::SourceLocation start = diagnostic.location;
    slang::SourceLocation end = diagnostic.location;
    if (!diagnostic.ranges.empty()) {
        start = diagnostic.ranges.front().start();
        end = diagnostic.ranges.front().end();
    }

    return toParseRange(source_manager, text, slang::SourceRange{start, end});
}

int toDocumentSymbolKind(slang::syntax::SyntaxKind kind) {
    switch (kind) {
        case slang::syntax::SyntaxKind::PackageDeclaration:
            return 4;
        case slang::syntax::SyntaxKind::ClassDeclaration:
            return 5;
        case slang::syntax::SyntaxKind::EnumType:
            return 10;
        case slang::syntax::SyntaxKind::InterfaceDeclaration:
            return 11;
        case slang::syntax::SyntaxKind::FunctionDeclaration:
        case slang::syntax::SyntaxKind::TaskDeclaration:
            return 12;
        case slang::syntax::SyntaxKind::CheckerDeclaration:
        case slang::syntax::SyntaxKind::ModuleDeclaration:
        case slang::syntax::SyntaxKind::ProgramDeclaration:
            return 2;
        default:
            return 0;
    }
}

DocumentSymbol makeDocumentSymbol(std::string name,
                                  int kind,
                                  ParseRange range,
                                  ParseRange selection_range,
                                  std::vector<DocumentSymbol> children = {}) {
    return DocumentSymbol{.name = std::move(name),
                          .kind = kind,
                          .range = range,
                          .selection_range = selection_range,
                          .children = std::move(children)};
}

std::string trimWhitespace(std::string value) {
    const auto is_space = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };

    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }

    return value;
}

std::string symbolKindLabel(int kind) {
    switch (kind) {
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

bool containsPosition(const ParseRange& range, int line, int character) {
    if (line < range.start_line || line > range.end_line) {
        return false;
    }

    if (line == range.start_line && character < range.start_character) {
        return false;
    }

    if (line == range.end_line && character >= range.end_character) {
        return false;
    }

    return true;
}

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

void advanceOne(std::string_view text, ScanState& state) {
    if (state.offset >= text.size()) {
        return;
    }

    if (text[state.offset] == '\r') {
        ++state.offset;
        if (state.offset < text.size() && text[state.offset] == '\n') {
            ++state.offset;
        }
        ++state.line;
        state.character = 0;
        return;
    }

    if (text[state.offset] == '\n') {
        ++state.offset;
        ++state.line;
        state.character = 0;
        return;
    }

    const auto decoded = text::decodeNextCodePoint(text, state.offset);
    state.offset += decoded.byte_length;
    state.character += static_cast<int>(text::utf16CodeUnitWidth(decoded.value));
}

void advanceAscii(std::string_view text, ScanState& state) {
    if (state.offset >= text.size()) {
        return;
    }
    ++state.offset;
    ++state.character;
}

bool startsWith(std::string_view text, size_t offset, std::string_view value) {
    return offset + value.size() <= text.size() && text.substr(offset, value.size()) == value;
}

void skipLineComment(std::string_view text, ScanState& state) {
    advanceAscii(text, state);
    advanceAscii(text, state);
    while (state.offset < text.size() && text[state.offset] != '\n' && text[state.offset] != '\r') {
        advanceOne(text, state);
    }
}

void skipBlockComment(std::string_view text, ScanState& state) {
    advanceAscii(text, state);
    advanceAscii(text, state);
    while (state.offset < text.size()) {
        if (startsWith(text, state.offset, "*/")) {
            advanceAscii(text, state);
            advanceAscii(text, state);
            return;
        }
        advanceOne(text, state);
    }
}

void skipStringLiteral(std::string_view text, ScanState& state) {
    advanceAscii(text, state);
    bool escaped = false;
    while (state.offset < text.size()) {
        const char value = text[state.offset];
        if (!escaped && value == '"') {
            advanceAscii(text, state);
            return;
        }

        const bool begins_escape = !escaped && value == '\\';
        advanceOne(text, state);
        escaped = begins_escape;
    }
}

std::vector<Identifier> collectIdentifiers(std::string_view text) {
    std::vector<Identifier> result;
    ScanState state{};
    while (state.offset < text.size()) {
        if (startsWith(text, state.offset, "//")) {
            skipLineComment(text, state);
            continue;
        }
        if (startsWith(text, state.offset, "/*")) {
            skipBlockComment(text, state);
            continue;
        }
        if (text[state.offset] == '"') {
            skipStringLiteral(text, state);
            continue;
        }

        const char value = text[state.offset];
        if (!isIdentifierStart(value)) {
            advanceOne(text, state);
            continue;
        }

        const auto start_line = state.line;
        const auto start_character = state.character;
        std::string name;
        while (state.offset < text.size() && isIdentifierContinue(text[state.offset])) {
            name.push_back(text[state.offset]);
            advanceAscii(text, state);
        }

        result.push_back(Identifier{
            .name = std::move(name),
            .range = ParseRange{.start_line = start_line,
                                .start_character = start_character,
                                .end_line = state.line,
                                .end_character = state.character}});
    }

    return result;
}

bool isHorizontalWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\f' || value == '\v';
}

void skipHorizontalWhitespace(std::string_view text, ScanState& state) {
    while (state.offset < text.size() && isHorizontalWhitespace(text[state.offset])) {
        advanceAscii(text, state);
    }
}

std::optional<IncludeDirective> tryCollectIncludeDirective(std::string_view text,
                                                           const ScanState& state) {
    if (state.offset >= text.size() || text[state.offset] != '`') {
        return std::nullopt;
    }

    ScanState cursor = state;
    advanceAscii(text, cursor);
    constexpr std::string_view include_keyword = "include";
    if (!startsWith(text, cursor.offset, include_keyword)) {
        return std::nullopt;
    }

    for (size_t index = 0; index < include_keyword.size(); ++index) {
        advanceAscii(text, cursor);
    }

    skipHorizontalWhitespace(text, cursor);
    if (cursor.offset >= text.size()) {
        return std::nullopt;
    }

    const char delimiter = text[cursor.offset];
    const char terminator = delimiter == '<' ? '>' : delimiter;
    if (delimiter != '"' && delimiter != '<') {
        return std::nullopt;
    }

    advanceAscii(text, cursor);
    const auto target_start_offset = cursor.offset;
    const auto target_start_line = cursor.line;
    const auto target_start_character = cursor.character;

    bool escaped = false;
    while (cursor.offset < text.size()) {
        const char value = text[cursor.offset];
        if (!escaped && value == terminator) {
            const auto target_end_offset = cursor.offset;
            const auto target_end_line = cursor.line;
            const auto target_end_character = cursor.character;
            advanceAscii(text, cursor);

            if (target_end_offset == target_start_offset) {
                return std::nullopt;
            }

            return IncludeDirective{
                .target = std::string(text.substr(target_start_offset,
                                                  target_end_offset - target_start_offset)),
                .range = ParseRange{.start_line = target_start_line,
                                    .start_character = target_start_character,
                                    .end_line = target_end_line,
                                    .end_character = target_end_character}};
        }

        const bool begins_escape = delimiter == '"' && !escaped && value == '\\';
        if (value == '\n' || value == '\r') {
            return std::nullopt;
        }
        advanceOne(text, cursor);
        escaped = begins_escape;
    }

    return std::nullopt;
}

std::vector<IncludeDirective> collectIncludeDirectives(std::string_view text) {
    std::vector<IncludeDirective> result;
    ScanState state{};
    while (state.offset < text.size()) {
        if (startsWith(text, state.offset, "//")) {
            skipLineComment(text, state);
            continue;
        }
        if (startsWith(text, state.offset, "/*")) {
            skipBlockComment(text, state);
            continue;
        }
        if (text[state.offset] == '"') {
            skipStringLiteral(text, state);
            continue;
        }

        if (const auto directive = tryCollectIncludeDirective(text, state)) {
            result.push_back(*directive);
        }

        advanceOne(text, state);
    }

    return result;
}

std::optional<size_t> byteOffsetAtPosition(std::string_view text, int line, int character) {
    if (line < 0 || character < 0) {
        return std::nullopt;
    }

    ScanState state{};
    while (state.offset <= text.size()) {
        if (state.line == line && state.character == character) {
            return state.offset;
        }
        if (state.offset >= text.size()) {
            break;
        }

        const auto previous_line = state.line;
        const auto previous_character = state.character;
        advanceOne(text, state);
        if (previous_line == line && previous_character < character &&
            (state.line > line || state.character > character)) {
            return std::nullopt;
        }
        if (state.line > line) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

const DocumentSymbol* findHoverSymbol(const DocumentSymbol& symbol, int line, int character) {
    if (!containsPosition(symbol.range, line, character) &&
        !containsPosition(symbol.selection_range, line, character)) {
        return nullptr;
    }

    for (const auto& child : symbol.children) {
        if (const auto* match = findHoverSymbol(child, line, character)) {
            return match;
        }
    }

    if (containsPosition(symbol.selection_range, line, character)) {
        return &symbol;
    }

    return nullptr;
}

const DocumentSymbol* findHoverSymbol(const std::vector<DocumentSymbol>& symbols,
                                      int line,
                                      int character) {
    for (const auto& symbol : symbols) {
        if (const auto* match = findHoverSymbol(symbol, line, character)) {
            return match;
        }
    }

    return nullptr;
}

std::string makeHoverContents(const DocumentSymbol& symbol) {
    return "**" + symbolKindLabel(symbol.kind) + "** `" + symbol.name + "`";
}

std::vector<DocumentSymbol> collectMemberSymbols(const slang::SourceManager& source_manager,
                                                 std::string_view text,
                                                 std::span<slang::syntax::MemberSyntax* const> members);

void collectModuleInstantiations(std::vector<ModuleInstantiation>& result,
                                 const slang::SourceManager& source_manager,
                                 std::string_view text,
                                 std::span<slang::syntax::MemberSyntax* const> members);

void appendNodeModuleInstantiations(std::vector<ModuleInstantiation>& result,
                                    const slang::SourceManager& source_manager,
                                    std::string_view text,
                                    const slang::syntax::SyntaxNode& node);

std::optional<DocumentSymbol> toDocumentSymbol(const slang::SourceManager& source_manager,
                                               std::string_view text,
                                               const slang::syntax::MemberSyntax& member);

template<typename TDeclaratorRange>
std::vector<DocumentSymbol> collectDeclaratorSymbols(const slang::SourceManager& source_manager,
                                                     std::string_view text,
                                                     const TDeclaratorRange& declarators,
                                                     int kind) {
    std::vector<DocumentSymbol> result;
    for (const auto* declarator : declarators) {
        result.push_back(makeDocumentSymbol(
            std::string(declarator->name.valueText()), kind,
            toParseRange(source_manager, text, declarator->sourceRange()),
            toParseRange(source_manager, text, declarator->name.range())));
    }
    return result;
}

template<typename TDeclaratorRange>
std::vector<DocumentSymbol> collectTypeAssignmentSymbols(
    const slang::SourceManager& source_manager,
    std::string_view text,
    const TDeclaratorRange& declarators) {
    std::vector<DocumentSymbol> result;
    for (const auto* declarator : declarators) {
        result.push_back(makeDocumentSymbol(
            std::string(declarator->name.valueText()), 26,
            toParseRange(source_manager, text, declarator->sourceRange()),
            toParseRange(source_manager, text, declarator->name.range())));
    }
    return result;
}

std::vector<DocumentSymbol> collectParameterSymbols(const slang::SourceManager& source_manager,
                                                    std::string_view text,
                                                    const slang::syntax::ParameterDeclarationBaseSyntax& parameter) {
    switch (parameter.kind) {
        case slang::syntax::SyntaxKind::ParameterDeclaration: {
            const auto& declaration = parameter.as<slang::syntax::ParameterDeclarationSyntax>();
            return collectDeclaratorSymbols(source_manager, text, declaration.declarators, 14);
        }
        case slang::syntax::SyntaxKind::TypeParameterDeclaration: {
            const auto& declaration = parameter.as<slang::syntax::TypeParameterDeclarationSyntax>();
            return collectTypeAssignmentSymbols(source_manager, text, declaration.declarators);
        }
        default:
            return {};
    }
}

std::vector<DocumentSymbol> collectHeaderParameterSymbols(
    const slang::SourceManager& source_manager,
    std::string_view text,
    const slang::syntax::ModuleHeaderSyntax& header) {
    std::vector<DocumentSymbol> result;
    if (!header.parameters) {
        return result;
    }

    for (const auto* declaration : header.parameters->declarations) {
        auto symbols = collectParameterSymbols(source_manager, text, *declaration);
        result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                      std::make_move_iterator(symbols.end()));
    }

    return result;
}

void appendMemberSymbols(std::vector<DocumentSymbol>& result,
                         const slang::SourceManager& source_manager,
                         std::string_view text,
                         const slang::syntax::MemberSyntax& member);

void appendNodeSymbols(std::vector<DocumentSymbol>& result,
                       const slang::SourceManager& source_manager,
                       std::string_view text,
                       const slang::syntax::SyntaxNode& node) {
    if (slang::syntax::MemberSyntax::isKind(node.kind)) {
        appendMemberSymbols(result, source_manager, text,
                            static_cast<const slang::syntax::MemberSyntax&>(node));
    }
}

std::vector<DocumentSymbol> collectHeaderPortSymbols(const slang::SourceManager& source_manager,
                                                     std::string_view text,
                                                     const slang::syntax::ModuleHeaderSyntax& header) {
    std::vector<DocumentSymbol> result;
    if (!header.ports || header.ports->kind != slang::syntax::SyntaxKind::AnsiPortList) {
        return result;
    }

    const auto& ports = header.ports->as<slang::syntax::AnsiPortListSyntax>();
    for (const auto* port : ports.ports) {
        appendMemberSymbols(result, source_manager, text, *port);
    }

    return result;
}

std::vector<std::string> collectHeaderPortNames(const slang::SourceManager& source_manager,
                                                std::string_view text,
                                                const slang::syntax::ModuleHeaderSyntax& header) {
    std::vector<std::string> result;
    for (const auto& symbol : collectHeaderPortSymbols(source_manager, text, header)) {
        result.push_back(symbol.name);
    }
    return result;
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string tokenDirection(std::string_view value) {
    const auto normalized = toLowerAscii(std::string(value));
    if (normalized == "input") {
        return "input";
    }
    if (normalized == "output") {
        return "output";
    }
    if (normalized == "inout") {
        return "inout";
    }
    return "inout";
}

std::string portHeaderDirection(const slang::syntax::PortHeaderSyntax& header) {
    switch (header.kind) {
        case slang::syntax::SyntaxKind::VariablePortHeader: {
            const auto& declaration = header.as<slang::syntax::VariablePortHeaderSyntax>();
            return tokenDirection(declaration.direction.valueText());
        }
        case slang::syntax::SyntaxKind::NetPortHeader: {
            const auto& declaration = header.as<slang::syntax::NetPortHeaderSyntax>();
            return tokenDirection(declaration.direction.valueText());
        }
        default:
            return "inout";
    }
}

std::string portHeaderWidthText(const slang::syntax::PortHeaderSyntax& header) {
    switch (header.kind) {
        case slang::syntax::SyntaxKind::VariablePortHeader: {
            const auto& declaration = header.as<slang::syntax::VariablePortHeaderSyntax>();
            return trimWhitespace(declaration.dataType->toString());
        }
        case slang::syntax::SyntaxKind::NetPortHeader: {
            const auto& declaration = header.as<slang::syntax::NetPortHeaderSyntax>();
            return trimWhitespace(declaration.dataType->toString());
        }
        default:
            return {};
    }
}

void appendOrUpdatePort(std::vector<SchematicPort>& result, SchematicPort port) {
    const auto existing = std::find_if(result.begin(), result.end(), [&](const SchematicPort& candidate) {
        return candidate.name == port.name;
    });
    if (existing == result.end()) {
        result.push_back(std::move(port));
        return;
    }

    if (existing->direction == "inout" && port.direction != "inout") {
        existing->direction = std::move(port.direction);
    }
    if (existing->width_text.empty() && !port.width_text.empty()) {
        existing->width_text = std::move(port.width_text);
    }
    existing->range = port.range;
    existing->selection_range = port.selection_range;
}

void appendPortDeclarators(std::vector<SchematicPort>& result,
                           const slang::SourceManager& source_manager,
                           std::string_view text,
                           const slang::syntax::PortHeaderSyntax& header,
                           const slang::syntax::SeparatedSyntaxList<slang::syntax::DeclaratorSyntax>& declarators) {
    const auto direction = portHeaderDirection(header);
    const auto width_text = portHeaderWidthText(header);
    for (const auto* declarator : declarators) {
        appendOrUpdatePort(result, SchematicPort{.name = std::string(declarator->name.valueText()),
                                                 .direction = direction,
                                                 .width_text = width_text,
                                                 .range = toParseRange(source_manager, text, declarator->sourceRange()),
                                                 .selection_range = toParseRange(source_manager, text,
                                                                                declarator->name.range())});
    }
}

std::vector<SchematicPort> collectHeaderSchematicPorts(const slang::SourceManager& source_manager,
                                                       std::string_view text,
                                                       const slang::syntax::ModuleHeaderSyntax& header) {
    std::vector<SchematicPort> result;
    if (!header.ports) {
        return result;
    }

    if (header.ports->kind == slang::syntax::SyntaxKind::AnsiPortList) {
        const auto& ports = header.ports->as<slang::syntax::AnsiPortListSyntax>();
        for (const auto* port : ports.ports) {
            switch (port->kind) {
                case slang::syntax::SyntaxKind::ImplicitAnsiPort: {
                    const auto& declaration = port->as<slang::syntax::ImplicitAnsiPortSyntax>();
                    appendOrUpdatePort(result, SchematicPort{
                                                   .name = std::string(declaration.declarator->name.valueText()),
                                                   .direction = portHeaderDirection(*declaration.header),
                                                   .width_text = portHeaderWidthText(*declaration.header),
                                                   .range = toParseRange(source_manager, text,
                                                                         declaration.sourceRange()),
                                                   .selection_range = toParseRange(source_manager, text,
                                                                                  declaration.declarator->name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::ExplicitAnsiPort: {
                    const auto& declaration = port->as<slang::syntax::ExplicitAnsiPortSyntax>();
                    appendOrUpdatePort(result, SchematicPort{.name = std::string(declaration.name.valueText()),
                                                             .direction = tokenDirection(declaration.direction.valueText()),
                                                             .width_text = {},
                                                             .range = toParseRange(source_manager, text,
                                                                                   declaration.sourceRange()),
                                                             .selection_range = toParseRange(source_manager, text,
                                                                                            declaration.name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::PortDeclaration: {
                    const auto& declaration = port->as<slang::syntax::PortDeclarationSyntax>();
                    appendPortDeclarators(result, source_manager, text, *declaration.header,
                                          declaration.declarators);
                    break;
                }
                default:
                    break;
            }
        }
    }
    else if (header.ports->kind == slang::syntax::SyntaxKind::NonAnsiPortList) {
        const auto& ports = header.ports->as<slang::syntax::NonAnsiPortListSyntax>();
        for (const auto* port : ports.ports) {
            switch (port->kind) {
                case slang::syntax::SyntaxKind::ExplicitNonAnsiPort: {
                    const auto& declaration = port->as<slang::syntax::ExplicitNonAnsiPortSyntax>();
                    appendOrUpdatePort(result, SchematicPort{.name = std::string(declaration.name.valueText()),
                                                             .direction = "inout",
                                                             .width_text = {},
                                                             .range = toParseRange(source_manager, text,
                                                                                   declaration.sourceRange()),
                                                             .selection_range = toParseRange(source_manager, text,
                                                                                            declaration.name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::ImplicitNonAnsiPort: {
                    const auto& declaration = port->as<slang::syntax::ImplicitNonAnsiPortSyntax>();
                    const auto name = trimWhitespace(declaration.expr->toString());
                    if (!name.empty()) {
                        appendOrUpdatePort(result, SchematicPort{.name = name,
                                                                 .direction = "inout",
                                                                 .width_text = {},
                                                                 .range = toParseRange(source_manager, text,
                                                                                       declaration.sourceRange()),
                                                                 .selection_range = toParseRange(source_manager, text,
                                                                                                declaration.expr->sourceRange())});
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    return result;
}

void collectMemberPortDeclarations(std::vector<SchematicPort>& result,
                                   const slang::SourceManager& source_manager,
                                   std::string_view text,
                                   std::span<slang::syntax::MemberSyntax* const> members) {
    for (const auto* member : members) {
        if (member->kind != slang::syntax::SyntaxKind::PortDeclaration) {
            continue;
        }
        const auto& declaration = member->as<slang::syntax::PortDeclarationSyntax>();
        appendPortDeclarators(result, source_manager, text, *declaration.header, declaration.declarators);
    }
}

std::string expressionText(const slang::syntax::SyntaxNode& node) {
    return trimWhitespace(node.toString());
}

std::string expressionSignal(const slang::syntax::SyntaxNode& node) {
    return expressionText(node);
}

SchematicConnection makeConnection(std::string port_name,
                                   int port_index,
                                   std::string signal,
                                   ParseRange range) {
    return SchematicConnection{.port_name = std::move(port_name),
                               .port_index = port_index,
                               .signal = std::move(signal),
                               .range = range};
}

template<typename TConnectionRange>
std::vector<SchematicConnection> collectSchematicConnections(
    const slang::SourceManager& source_manager,
    std::string_view text,
    const TConnectionRange& connections) {
    std::vector<SchematicConnection> result;
    int index = 0;
    for (const auto* connection : connections) {
        switch (connection->kind) {
            case slang::syntax::SyntaxKind::NamedPortConnection: {
                const auto& named = connection->as<slang::syntax::NamedPortConnectionSyntax>();
                const auto signal = named.expr ? expressionSignal(*named.expr)
                                               : std::string(named.name.valueText());
                result.push_back(makeConnection(std::string(named.name.valueText()), -1, signal,
                                                toParseRange(source_manager, text,
                                                             named.expr ? named.expr->sourceRange()
                                                                        : named.sourceRange())));
                break;
            }
            case slang::syntax::SyntaxKind::OrderedPortConnection: {
                const auto& ordered = connection->as<slang::syntax::OrderedPortConnectionSyntax>();
                result.push_back(makeConnection({}, index, expressionSignal(*ordered.expr),
                                                toParseRange(source_manager, text,
                                                             ordered.expr->sourceRange())));
                break;
            }
            default:
                break;
        }
        ++index;
    }
    return result;
}

std::string generatedName(std::string_view prefix, int index) {
    return std::string("$") + std::string(prefix) + std::to_string(index);
}

std::string primitivePortName(int index) {
    if (index == 0) {
        return "Y";
    }
    if (index == 1) {
        return "A";
    }
    if (index == 2) {
        return "B";
    }
    return std::string("I") + std::to_string(index - 1);
}

std::optional<std::string> logicKindForBinary(slang::syntax::SyntaxKind kind) {
    switch (kind) {
        case slang::syntax::SyntaxKind::BinaryAndExpression:
        case slang::syntax::SyntaxKind::LogicalAndExpression:
            return "and";
        case slang::syntax::SyntaxKind::BinaryOrExpression:
        case slang::syntax::SyntaxKind::LogicalOrExpression:
            return "or";
        case slang::syntax::SyntaxKind::BinaryXorExpression:
            return "xor";
        case slang::syntax::SyntaxKind::BinaryXnorExpression:
            return "xnor";
        default:
            return std::nullopt;
    }
}

std::optional<std::string> logicKindForUnary(slang::syntax::SyntaxKind kind) {
    switch (kind) {
        case slang::syntax::SyntaxKind::UnaryBitwiseNotExpression:
        case slang::syntax::SyntaxKind::UnaryLogicalNotExpression:
            return "not";
        default:
            return std::nullopt;
    }
}

const slang::syntax::ExpressionSyntax& unwrapExpression(const slang::syntax::ExpressionSyntax& expression) {
    if (expression.kind == slang::syntax::SyntaxKind::ParenthesizedExpression) {
        return *expression.as<slang::syntax::ParenthesizedExpressionSyntax>().expression;
    }
    return expression;
}

struct ExpressionSchematicContext {
    int cell_index = 0;
    int net_index = 0;
};

bool appendLogicExpression(ModuleSchematic& module,
                           const slang::SourceManager& source_manager,
                           std::string_view text,
                           const slang::syntax::ExpressionSyntax& expression,
                           std::string output_signal,
                           ExpressionSchematicContext& context);

std::string materializeExpression(ModuleSchematic& module,
                                  const slang::SourceManager& source_manager,
                                  std::string_view text,
                                  const slang::syntax::ExpressionSyntax& expression,
                                  ExpressionSchematicContext& context) {
    const auto& inner = unwrapExpression(expression);
    if (logicKindForBinary(inner.kind).has_value() ||
        logicKindForUnary(inner.kind).has_value() ||
        inner.kind == slang::syntax::SyntaxKind::ConditionalExpression) {
        const auto signal = generatedName("net", context.net_index++);
        if (appendLogicExpression(module, source_manager, text, inner, signal, context)) {
            return signal;
        }
    }

    return expressionSignal(expression);
}

void appendLogicCell(ModuleSchematic& module,
                     const slang::SourceManager& source_manager,
                     std::string_view text,
                     std::string kind,
                     const slang::syntax::SyntaxNode& source,
                     std::vector<SchematicConnection> connections,
                     ExpressionSchematicContext& context) {
    const auto name = generatedName(kind, context.cell_index++);
    module.cells.push_back(SchematicCell{.id = name,
                                         .name = name,
                                         .type = kind,
                                         .kind = std::move(kind),
                                         .range = toParseRange(source_manager, text, source.sourceRange()),
                                         .selection_range = toParseRange(source_manager, text, source.sourceRange()),
                                         .connections = std::move(connections)});
}

bool appendLogicExpression(ModuleSchematic& module,
                           const slang::SourceManager& source_manager,
                           std::string_view text,
                           const slang::syntax::ExpressionSyntax& expression,
                           std::string output_signal,
                           ExpressionSchematicContext& context) {
    const auto& inner = unwrapExpression(expression);
    if (const auto kind = logicKindForBinary(inner.kind)) {
        const auto& binary = inner.as<slang::syntax::BinaryExpressionSyntax>();
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal),
                                             toParseRange(source_manager, text, inner.sourceRange())));
        connections.push_back(makeConnection("A", -1,
                                             materializeExpression(module, source_manager, text,
                                                                   *binary.left, context),
                                             toParseRange(source_manager, text, binary.left->sourceRange())));
        connections.push_back(makeConnection("B", -1,
                                             materializeExpression(module, source_manager, text,
                                                                   *binary.right, context),
                                             toParseRange(source_manager, text, binary.right->sourceRange())));
        appendLogicCell(module, source_manager, text, *kind, inner, std::move(connections), context);
        return true;
    }

    if (const auto kind = logicKindForUnary(inner.kind)) {
        const auto& unary = inner.as<slang::syntax::PrefixUnaryExpressionSyntax>();
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal),
                                             toParseRange(source_manager, text, inner.sourceRange())));
        connections.push_back(makeConnection("A", -1,
                                             materializeExpression(module, source_manager, text,
                                                                   *unary.operand, context),
                                             toParseRange(source_manager, text, unary.operand->sourceRange())));
        appendLogicCell(module, source_manager, text, *kind, inner, std::move(connections), context);
        return true;
    }

    if (inner.kind == slang::syntax::SyntaxKind::ConditionalExpression) {
        const auto& conditional = inner.as<slang::syntax::ConditionalExpressionSyntax>();
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal),
                                             toParseRange(source_manager, text, inner.sourceRange())));
        if (!conditional.predicate->conditions.empty()) {
            const auto* predicate = *conditional.predicate->conditions.begin();
            connections.push_back(makeConnection("S", -1,
                                                 materializeExpression(module, source_manager, text,
                                                                       *predicate->expr, context),
                                                 toParseRange(source_manager, text,
                                                              predicate->expr->sourceRange())));
        }
        connections.push_back(makeConnection("I1", -1,
                                             materializeExpression(module, source_manager, text,
                                                                   *conditional.left, context),
                                             toParseRange(source_manager, text,
                                                          conditional.left->sourceRange())));
        connections.push_back(makeConnection("I0", -1,
                                             materializeExpression(module, source_manager, text,
                                                                   *conditional.right, context),
                                             toParseRange(source_manager, text,
                                                          conditional.right->sourceRange())));
        appendLogicCell(module, source_manager, text, "mux", inner, std::move(connections), context);
        return true;
    }

    return false;
}

void appendBufferExpression(ModuleSchematic& module,
                            const slang::SourceManager& source_manager,
                            std::string_view text,
                            const slang::syntax::ExpressionSyntax& expression,
                            std::string output_signal,
                            ExpressionSchematicContext& context) {
    std::vector<SchematicConnection> connections;
    connections.push_back(makeConnection("Y", -1, std::move(output_signal),
                                         toParseRange(source_manager, text, expression.sourceRange())));
    connections.push_back(makeConnection("A", -1,
                                         materializeExpression(module, source_manager, text, expression, context),
                                         toParseRange(source_manager, text, expression.sourceRange())));
    appendLogicCell(module, source_manager, text, "buf", expression, std::move(connections), context);
}

void appendSchematicModuleInstantiation(ModuleSchematic& module,
                                        const slang::SourceManager& source_manager,
                                        std::string_view text,
                                        const slang::syntax::HierarchyInstantiationSyntax& declaration) {
    const auto module_name = std::string(declaration.type.valueText());
    if (module_name.empty()) {
        return;
    }

    int generated_index = 0;
    for (const auto* instance : declaration.instances) {
        if (!instance || !instance->decl) {
            continue;
        }

        auto instance_name = std::string(instance->decl->name.valueText());
        if (instance_name.empty()) {
            instance_name = generatedName("inst", generated_index++);
        }

        module.cells.push_back(SchematicCell{
            .id = instance_name,
            .name = instance_name,
            .type = module_name,
            .kind = "module",
            .range = toParseRange(source_manager, text, instance->sourceRange()),
            .selection_range = toParseRange(source_manager, text, instance->decl->name.range()),
            .connections = collectSchematicConnections(source_manager, text, instance->connections)});
    }
}

void appendSchematicPrimitiveInstantiation(ModuleSchematic& module,
                                           const slang::SourceManager& source_manager,
                                           std::string_view text,
                                           const slang::syntax::PrimitiveInstantiationSyntax& declaration) {
    const auto primitive_type = toLowerAscii(std::string(declaration.type.valueText()));
    if (primitive_type.empty()) {
        return;
    }

    int generated_index = 0;
    for (const auto* instance : declaration.instances) {
        if (!instance) {
            continue;
        }

        auto instance_name = instance->decl ? std::string(instance->decl->name.valueText()) : std::string{};
        if (instance_name.empty()) {
            instance_name = generatedName(primitive_type, generated_index++);
        }

        auto connections = collectSchematicConnections(source_manager, text, instance->connections);
        for (auto& connection : connections) {
            if (connection.port_name.empty() && connection.port_index >= 0) {
                connection.port_name = primitivePortName(connection.port_index);
            }
        }

        module.cells.push_back(SchematicCell{.id = instance_name,
                                             .name = instance_name,
                                             .type = primitive_type,
                                             .kind = primitive_type,
                                             .range = toParseRange(source_manager, text,
                                                                   instance->sourceRange()),
                                             .selection_range = instance->decl
                                                                    ? toParseRange(source_manager, text,
                                                                                   instance->decl->name.range())
                                                                    : toParseRange(source_manager, text,
                                                                                   instance->sourceRange()),
                                             .connections = std::move(connections)});
    }
}

void appendSchematicContinuousAssign(ModuleSchematic& module,
                                     const slang::SourceManager& source_manager,
                                     std::string_view text,
                                     const slang::syntax::ContinuousAssignSyntax& declaration) {
    ExpressionSchematicContext context;
    for (const auto* assignment : declaration.assignments) {
        if (!assignment || assignment->kind != slang::syntax::SyntaxKind::AssignmentExpression) {
            continue;
        }

        const auto& binary = assignment->as<slang::syntax::BinaryExpressionSyntax>();
        auto output_signal = expressionSignal(*binary.left);
        if (output_signal.empty()) {
            continue;
        }

        if (!appendLogicExpression(module, source_manager, text, *binary.right, output_signal, context)) {
            appendBufferExpression(module, source_manager, text, *binary.right, std::move(output_signal), context);
        }
    }
}

void appendSchematicCellsFromNode(ModuleSchematic& module,
                                  const slang::SourceManager& source_manager,
                                  std::string_view text,
                                  const slang::syntax::SyntaxNode& node);

void appendSchematicCellsFromMember(ModuleSchematic& module,
                                    const slang::SourceManager& source_manager,
                                    std::string_view text,
                                    const slang::syntax::MemberSyntax& member) {
    switch (member.kind) {
        case slang::syntax::SyntaxKind::HierarchyInstantiation:
            appendSchematicModuleInstantiation(module, source_manager, text,
                                               member.as<slang::syntax::HierarchyInstantiationSyntax>());
            return;
        case slang::syntax::SyntaxKind::PrimitiveInstantiation:
            appendSchematicPrimitiveInstantiation(module, source_manager, text,
                                                  member.as<slang::syntax::PrimitiveInstantiationSyntax>());
            return;
        case slang::syntax::SyntaxKind::ContinuousAssign:
            appendSchematicContinuousAssign(module, source_manager, text,
                                            member.as<slang::syntax::ContinuousAssignSyntax>());
            return;
        case slang::syntax::SyntaxKind::GenerateRegion: {
            const auto& declaration = member.as<slang::syntax::GenerateRegionSyntax>();
            for (const auto* child : declaration.members) {
                appendSchematicCellsFromMember(module, source_manager, text, *child);
            }
            return;
        }
        case slang::syntax::SyntaxKind::GenerateBlock: {
            const auto& declaration = member.as<slang::syntax::GenerateBlockSyntax>();
            for (const auto* child : declaration.members) {
                appendSchematicCellsFromMember(module, source_manager, text, *child);
            }
            return;
        }
        case slang::syntax::SyntaxKind::IfGenerate: {
            const auto& declaration = member.as<slang::syntax::IfGenerateSyntax>();
            appendSchematicCellsFromMember(module, source_manager, text, *declaration.block);
            if (declaration.elseClause) {
                appendSchematicCellsFromNode(module, source_manager, text, *declaration.elseClause->clause);
            }
            return;
        }
        case slang::syntax::SyntaxKind::LoopGenerate: {
            const auto& declaration = member.as<slang::syntax::LoopGenerateSyntax>();
            appendSchematicCellsFromMember(module, source_manager, text, *declaration.block);
            return;
        }
        default:
            return;
    }
}

void appendSchematicCellsFromNode(ModuleSchematic& module,
                                  const slang::SourceManager& source_manager,
                                  std::string_view text,
                                  const slang::syntax::SyntaxNode& node) {
    if (slang::syntax::MemberSyntax::isKind(node.kind)) {
        appendSchematicCellsFromMember(module, source_manager, text,
                                       static_cast<const slang::syntax::MemberSyntax&>(node));
    }
}

std::optional<ModuleSchematic> toModuleSchematic(const slang::SourceManager& source_manager,
                                                 std::string_view text,
                                                 const slang::syntax::MemberSyntax& member) {
    if (member.kind != slang::syntax::SyntaxKind::ModuleDeclaration) {
        return std::nullopt;
    }

    const auto& declaration = member.as<slang::syntax::ModuleDeclarationSyntax>();
    ModuleSchematic schematic{.name = std::string(declaration.header->name.valueText()),
                              .range = toParseRange(source_manager, text, declaration.sourceRange()),
                              .selection_range = toParseRange(source_manager, text,
                                                              declaration.header->name.range()),
                              .ports = collectHeaderSchematicPorts(source_manager, text,
                                                                    *declaration.header),
                              .cells = {}};
    collectMemberPortDeclarations(schematic.ports, source_manager, text, declaration.members);
    for (const auto* child : declaration.members) {
        appendSchematicCellsFromMember(schematic, source_manager, text, *child);
    }

    return schematic;
}

std::vector<DocumentSymbol> collectModportSymbols(const slang::SourceManager& source_manager,
                                                  std::string_view text,
                                                  const slang::syntax::AnsiPortListSyntax& ports) {
    std::vector<DocumentSymbol> result;
    for (const auto* port : ports.ports) {
        switch (port->kind) {
            case slang::syntax::SyntaxKind::ModportSimplePortList: {
                const auto& list = port->as<slang::syntax::ModportSimplePortListSyntax>();
                for (const auto* simple_port : list.ports) {
                    switch (simple_port->kind) {
                        case slang::syntax::SyntaxKind::ModportNamedPort: {
                            const auto& named = simple_port->as<slang::syntax::ModportNamedPortSyntax>();
                            result.push_back(makeDocumentSymbol(
                                std::string(named.name.valueText()), 13,
                                toParseRange(source_manager, text, named.sourceRange()),
                                toParseRange(source_manager, text, named.name.range())));
                            break;
                        }
                        case slang::syntax::SyntaxKind::ModportExplicitPort: {
                            const auto& named = simple_port->as<slang::syntax::ModportExplicitPortSyntax>();
                            result.push_back(makeDocumentSymbol(
                                std::string(named.name.valueText()), 13,
                                toParseRange(source_manager, text, named.sourceRange()),
                                toParseRange(source_manager, text, named.name.range())));
                            break;
                        }
                        default:
                            break;
                    }
                }
                break;
            }
            case slang::syntax::SyntaxKind::ModportSubroutinePortList: {
                const auto& list = port->as<slang::syntax::ModportSubroutinePortListSyntax>();
                for (const auto* subroutine_port : list.ports) {
                    if (subroutine_port->kind == slang::syntax::SyntaxKind::ModportSubroutinePort) {
                        const auto& subroutine =
                            subroutine_port->as<slang::syntax::ModportSubroutinePortSyntax>();
                        result.push_back(makeDocumentSymbol(
                            trimWhitespace(subroutine.prototype->name->toString()), 12,
                            toParseRange(source_manager, text, subroutine.sourceRange()),
                            toParseRange(source_manager, text, subroutine.prototype->name->sourceRange())));
                    }
                }
                break;
            }
            case slang::syntax::SyntaxKind::ModportClockingPort: {
                const auto& clocking = port->as<slang::syntax::ModportClockingPortSyntax>();
                result.push_back(makeDocumentSymbol(
                    std::string(clocking.name.valueText()), 13,
                    toParseRange(source_manager, text, clocking.sourceRange()),
                    toParseRange(source_manager, text, clocking.name.range())));
                break;
            }
            default:
                break;
        }
    }

    return result;
}

std::vector<DocumentSymbol> collectEnumMemberSymbols(const slang::SourceManager& source_manager,
                                                     std::string_view text,
                                                     const slang::syntax::EnumTypeSyntax& enum_type) {
    std::vector<DocumentSymbol> result;
    for (const auto* member : enum_type.members) {
        result.push_back(makeDocumentSymbol(
            std::string(member->name.valueText()), 22,
            toParseRange(source_manager, text, member->sourceRange()),
            toParseRange(source_manager, text, member->name.range())));
    }
    return result;
}

void appendMemberSymbols(std::vector<DocumentSymbol>& result,
                         const slang::SourceManager& source_manager,
                         std::string_view text,
                         const slang::syntax::MemberSyntax& member) {
    switch (member.kind) {
        case slang::syntax::SyntaxKind::ImplicitAnsiPort: {
            const auto& declaration = member.as<slang::syntax::ImplicitAnsiPortSyntax>();
            result.push_back(makeDocumentSymbol(
                std::string(declaration.declarator->name.valueText()), 13,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.declarator->name.range())));
            return;
        }
        case slang::syntax::SyntaxKind::ExplicitAnsiPort: {
            const auto& declaration = member.as<slang::syntax::ExplicitAnsiPortSyntax>();
            result.push_back(makeDocumentSymbol(
                std::string(declaration.name.valueText()), 13,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.name.range())));
            return;
        }
        case slang::syntax::SyntaxKind::DataDeclaration: {
            const auto& declaration = member.as<slang::syntax::DataDeclarationSyntax>();
            auto symbols = collectDeclaratorSymbols(source_manager, text, declaration.declarators, 13);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::CheckerDataDeclaration: {
            const auto& declaration = member.as<slang::syntax::CheckerDataDeclarationSyntax>();
            auto symbols = collectDeclaratorSymbols(source_manager, text, declaration.data->declarators, 13);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::NetDeclaration: {
            const auto& declaration = member.as<slang::syntax::NetDeclarationSyntax>();
            auto symbols = collectDeclaratorSymbols(source_manager, text, declaration.declarators, 13);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::UserDefinedNetDeclaration: {
            const auto& declaration = member.as<slang::syntax::UserDefinedNetDeclarationSyntax>();
            auto symbols = collectDeclaratorSymbols(source_manager, text, declaration.declarators, 13);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::TypedefDeclaration: {
            const auto& declaration = member.as<slang::syntax::TypedefDeclarationSyntax>();
            int kind = 26;
            std::vector<DocumentSymbol> children;
            if (declaration.type->kind == slang::syntax::SyntaxKind::EnumType) {
                kind = toDocumentSymbolKind(declaration.type->kind);
                children = collectEnumMemberSymbols(source_manager, text,
                                                    declaration.type->as<slang::syntax::EnumTypeSyntax>());
            }

            result.push_back(makeDocumentSymbol(
                std::string(declaration.name.valueText()), kind,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.name.range()), std::move(children)));
            return;
        }
        case slang::syntax::SyntaxKind::ClassPropertyDeclaration: {
            const auto& declaration = member.as<slang::syntax::ClassPropertyDeclarationSyntax>();
            appendMemberSymbols(result, source_manager, text, *declaration.declaration);
            return;
        }
        case slang::syntax::SyntaxKind::ClassMethodDeclaration: {
            const auto& declaration = member.as<slang::syntax::ClassMethodDeclarationSyntax>();
            appendMemberSymbols(result, source_manager, text, *declaration.declaration);
            return;
        }
        case slang::syntax::SyntaxKind::ClassMethodPrototype: {
            const auto& declaration = member.as<slang::syntax::ClassMethodPrototypeSyntax>();
            result.push_back(makeDocumentSymbol(
                trimWhitespace(declaration.prototype->name->toString()), 12,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.prototype->name->sourceRange())));
            return;
        }
        case slang::syntax::SyntaxKind::ClassDeclaration: {
            const auto& declaration = member.as<slang::syntax::ClassDeclarationSyntax>();
            result.push_back(makeDocumentSymbol(
                std::string(declaration.name.valueText()), toDocumentSymbolKind(member.kind),
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.name.range()),
                collectMemberSymbols(source_manager, text, declaration.items)));
            return;
        }
        case slang::syntax::SyntaxKind::GenerateRegion: {
            const auto& declaration = member.as<slang::syntax::GenerateRegionSyntax>();
            auto symbols = collectMemberSymbols(source_manager, text, declaration.members);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::GenerateBlock: {
            const auto& declaration = member.as<slang::syntax::GenerateBlockSyntax>();
            auto children = collectMemberSymbols(source_manager, text, declaration.members);
            if (declaration.beginName) {
                result.push_back(makeDocumentSymbol(
                    std::string(declaration.beginName->name.valueText()), 3,
                    toParseRange(source_manager, text, declaration.sourceRange()),
                    toParseRange(source_manager, text, declaration.beginName->name.range()),
                    std::move(children)));
            }
            else {
                result.insert(result.end(), std::make_move_iterator(children.begin()),
                              std::make_move_iterator(children.end()));
            }
            return;
        }
        case slang::syntax::SyntaxKind::IfGenerate: {
            const auto& declaration = member.as<slang::syntax::IfGenerateSyntax>();
            appendMemberSymbols(result, source_manager, text, *declaration.block);
            if (declaration.elseClause) {
                appendNodeSymbols(result, source_manager, text, *declaration.elseClause->clause);
            }
            return;
        }
        case slang::syntax::SyntaxKind::LoopGenerate: {
            const auto& declaration = member.as<slang::syntax::LoopGenerateSyntax>();
            appendMemberSymbols(result, source_manager, text, *declaration.block);
            return;
        }
        case slang::syntax::SyntaxKind::ParameterDeclarationStatement: {
            const auto& declaration = member.as<slang::syntax::ParameterDeclarationStatementSyntax>();
            auto symbols = collectParameterSymbols(source_manager, text, *declaration.parameter);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::HierarchyInstantiation: {
            const auto& declaration = member.as<slang::syntax::HierarchyInstantiationSyntax>();
            for (const auto* instance : declaration.instances) {
                if (!instance->decl) {
                    continue;
                }

                result.push_back(makeDocumentSymbol(
                    std::string(instance->decl->name.valueText()), 19,
                    toParseRange(source_manager, text, instance->sourceRange()),
                    toParseRange(source_manager, text, instance->decl->name.range())));
            }
            return;
        }
        case slang::syntax::SyntaxKind::PortDeclaration: {
            const auto& declaration = member.as<slang::syntax::PortDeclarationSyntax>();
            auto symbols = collectDeclaratorSymbols(source_manager, text, declaration.declarators, 13);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
            return;
        }
        case slang::syntax::SyntaxKind::GenvarDeclaration: {
            const auto& declaration = member.as<slang::syntax::GenvarDeclarationSyntax>();
            for (const auto* identifier : declaration.identifiers) {
                result.push_back(makeDocumentSymbol(
                    std::string(identifier->identifier.valueText()), 13,
                    toParseRange(source_manager, text, identifier->sourceRange()),
                    toParseRange(source_manager, text, identifier->identifier.range())));
            }
            return;
        }
        case slang::syntax::SyntaxKind::FunctionDeclaration:
        case slang::syntax::SyntaxKind::TaskDeclaration: {
            const auto& declaration = member.as<slang::syntax::FunctionDeclarationSyntax>();
            result.push_back(makeDocumentSymbol(
                trimWhitespace(declaration.prototype->name->toString()),
                toDocumentSymbolKind(member.kind),
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.prototype->name->sourceRange())));
            return;
        }
        case slang::syntax::SyntaxKind::CovergroupDeclaration: {
            const auto& declaration = member.as<slang::syntax::CovergroupDeclarationSyntax>();
            result.push_back(makeDocumentSymbol(
                std::string(declaration.name.valueText()), 5,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.name.range()),
                collectMemberSymbols(source_manager, text, declaration.members)));
            return;
        }
        case slang::syntax::SyntaxKind::ModportDeclaration: {
            const auto& declaration = member.as<slang::syntax::ModportDeclarationSyntax>();
            for (const auto* item : declaration.items) {
                result.push_back(makeDocumentSymbol(
                    std::string(item->name.valueText()), 11,
                    toParseRange(source_manager, text, item->sourceRange()),
                    toParseRange(source_manager, text, item->name.range()),
                    collectModportSymbols(source_manager, text, *item->ports)));
            }
            return;
        }
        default:
            if (auto symbol = toDocumentSymbol(source_manager, text, member)) {
                result.push_back(std::move(*symbol));
            }
            return;
    }
}

std::vector<DocumentSymbol> collectMemberSymbols(const slang::SourceManager& source_manager,
                                                 std::string_view text,
                                                 std::span<slang::syntax::MemberSyntax* const> members) {
    std::vector<DocumentSymbol> result;
    for (const auto* member : members) {
        appendMemberSymbols(result, source_manager, text, *member);
    }
    return result;
}

void appendModuleInstantiation(std::vector<ModuleInstantiation>& result,
                               const slang::SourceManager& source_manager,
                               std::string_view text,
                               const slang::syntax::HierarchyInstantiationSyntax& declaration) {
    const auto module_name = std::string(declaration.type.valueText());
    if (module_name.empty()) {
        return;
    }

    for (const auto* instance : declaration.instances) {
        if (!instance || !instance->decl) {
            continue;
        }

        result.push_back(ModuleInstantiation{
            .module_name = module_name,
            .instance_name = std::string(instance->decl->name.valueText()),
            .range = toParseRange(source_manager, text, instance->sourceRange()),
            .selection_range = toParseRange(source_manager, text, instance->decl->name.range()),
            .module_selection_range = toParseRange(source_manager, text, declaration.type.range())});
    }
}

void appendModuleInstantiationsFromMember(std::vector<ModuleInstantiation>& result,
                                          const slang::SourceManager& source_manager,
                                          std::string_view text,
                                          const slang::syntax::MemberSyntax& member) {
    switch (member.kind) {
        case slang::syntax::SyntaxKind::HierarchyInstantiation:
            appendModuleInstantiation(result, source_manager, text,
                                      member.as<slang::syntax::HierarchyInstantiationSyntax>());
            return;
        case slang::syntax::SyntaxKind::GenerateRegion: {
            const auto& declaration = member.as<slang::syntax::GenerateRegionSyntax>();
            collectModuleInstantiations(result, source_manager, text, declaration.members);
            return;
        }
        case slang::syntax::SyntaxKind::GenerateBlock: {
            const auto& declaration = member.as<slang::syntax::GenerateBlockSyntax>();
            collectModuleInstantiations(result, source_manager, text, declaration.members);
            return;
        }
        case slang::syntax::SyntaxKind::IfGenerate: {
            const auto& declaration = member.as<slang::syntax::IfGenerateSyntax>();
            appendModuleInstantiationsFromMember(result, source_manager, text, *declaration.block);
            if (declaration.elseClause) {
                appendNodeModuleInstantiations(result, source_manager, text, *declaration.elseClause->clause);
            }
            return;
        }
        case slang::syntax::SyntaxKind::LoopGenerate: {
            const auto& declaration = member.as<slang::syntax::LoopGenerateSyntax>();
            appendModuleInstantiationsFromMember(result, source_manager, text, *declaration.block);
            return;
        }
        default:
            return;
    }
}

void appendNodeModuleInstantiations(std::vector<ModuleInstantiation>& result,
                                    const slang::SourceManager& source_manager,
                                    std::string_view text,
                                    const slang::syntax::SyntaxNode& node) {
    if (slang::syntax::MemberSyntax::isKind(node.kind)) {
        appendModuleInstantiationsFromMember(result, source_manager, text,
                                            static_cast<const slang::syntax::MemberSyntax&>(node));
    }
}

void collectModuleInstantiations(std::vector<ModuleInstantiation>& result,
                                 const slang::SourceManager& source_manager,
                                 std::string_view text,
                                 std::span<slang::syntax::MemberSyntax* const> members) {
    for (const auto* member : members) {
        appendModuleInstantiationsFromMember(result, source_manager, text, *member);
    }
}

std::optional<ModuleDefinition> toModuleDefinition(const slang::SourceManager& source_manager,
                                                   std::string_view text,
                                                   const slang::syntax::MemberSyntax& member) {
    if (member.kind != slang::syntax::SyntaxKind::ModuleDeclaration) {
        return std::nullopt;
    }

    const auto& declaration = member.as<slang::syntax::ModuleDeclarationSyntax>();
    std::vector<ModuleInstantiation> instances;
    collectModuleInstantiations(instances, source_manager, text, declaration.members);

    return ModuleDefinition{.name = std::string(declaration.header->name.valueText()),
                            .range = toParseRange(source_manager, text, declaration.sourceRange()),
                            .selection_range = toParseRange(source_manager, text, declaration.header->name.range()),
                            .ports = collectHeaderPortNames(source_manager, text, *declaration.header),
                            .instances = std::move(instances)};
}

std::optional<DocumentSymbol> toDocumentSymbol(const slang::SourceManager& source_manager,
                                               std::string_view text,
                                               const slang::syntax::MemberSyntax& member) {
    switch (member.kind) {
        case slang::syntax::SyntaxKind::ModuleDeclaration:
        case slang::syntax::SyntaxKind::InterfaceDeclaration:
        case slang::syntax::SyntaxKind::PackageDeclaration:
        case slang::syntax::SyntaxKind::ProgramDeclaration: {
            const auto& declaration = member.as<slang::syntax::ModuleDeclarationSyntax>();
            auto children = collectHeaderParameterSymbols(source_manager, text, *declaration.header);
            auto port_children = collectHeaderPortSymbols(source_manager, text, *declaration.header);
            children.insert(children.end(), std::make_move_iterator(port_children.begin()),
                            std::make_move_iterator(port_children.end()));
            auto member_children = collectMemberSymbols(source_manager, text, declaration.members);
            children.insert(children.end(), std::make_move_iterator(member_children.begin()),
                            std::make_move_iterator(member_children.end()));
            return makeDocumentSymbol(std::string(declaration.header->name.valueText()),
                                      toDocumentSymbolKind(member.kind),
                                      toParseRange(source_manager, text, declaration.sourceRange()),
                                      toParseRange(source_manager, text, declaration.header->name.range()),
                                      std::move(children));
        }
        case slang::syntax::SyntaxKind::CheckerDeclaration: {
            const auto& declaration = member.as<slang::syntax::CheckerDeclarationSyntax>();
            return makeDocumentSymbol(std::string(declaration.name.valueText()),
                                      toDocumentSymbolKind(member.kind),
                                      toParseRange(source_manager, text, declaration.sourceRange()),
                                      toParseRange(source_manager, text, declaration.name.range()),
                                      collectMemberSymbols(source_manager, text, declaration.members));
        }
        default:
            return std::nullopt;
    }
}

} // namespace

ParseResult CompilationService::parse(std::string_view text, std::string_view uri) const {
    slang::SourceManager source_manager;
    auto syntax_tree = slang::syntax::SyntaxTree::fromText(text, source_manager, "source", uri);
    slang::DiagnosticEngine diagnostic_engine(source_manager);

    ParseResult result{};
    result.syntax_tree = syntax_tree;
    for (const auto& diagnostic : syntax_tree->diagnostics()) {
        const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
        result.diagnostics.push_back(
            ParseDiagnostic{.code = std::string(slang::toString(diagnostic.code)),
                            .message = diagnostic_engine.formatMessage(diagnostic),
                            .range = toParseRange(source_manager, text, diagnostic),
                            .severity = toLspSeverity(severity),
                            .is_error = diagnostic.isError()});
        result.has_errors = result.has_errors || diagnostic.isError();
    }

    return result;
}

std::vector<DocumentSymbol> CompilationService::documentSymbols(std::string_view text,
                                                                std::string_view uri) const {
    slang::SourceManager source_manager;
    auto syntax_tree = slang::syntax::SyntaxTree::fromFileInMemory(text, source_manager, "source", uri);
    if (!syntax_tree || syntax_tree->root().kind != slang::syntax::SyntaxKind::CompilationUnit) {
        return {};
    }

    const auto& compilation_unit = syntax_tree->root().as<slang::syntax::CompilationUnitSyntax>();

    return collectMemberSymbols(source_manager, text, compilation_unit.members);
}

std::vector<ModuleDefinition> CompilationService::moduleDefinitions(std::string_view text,
                                                                    std::string_view uri) const {
    slang::SourceManager source_manager;
    auto syntax_tree = slang::syntax::SyntaxTree::fromFileInMemory(text, source_manager, "source", uri);
    if (!syntax_tree || syntax_tree->root().kind != slang::syntax::SyntaxKind::CompilationUnit) {
        return {};
    }

    const auto& compilation_unit = syntax_tree->root().as<slang::syntax::CompilationUnitSyntax>();
    std::vector<ModuleDefinition> result;
    for (const auto* member : compilation_unit.members) {
        if (auto definition = toModuleDefinition(source_manager, text, *member)) {
            result.push_back(std::move(*definition));
        }
    }

    return result;
}

std::vector<ModuleSchematic> CompilationService::moduleSchematics(std::string_view text,
                                                                  std::string_view uri) const {
    slang::SourceManager source_manager;
    auto syntax_tree = slang::syntax::SyntaxTree::fromFileInMemory(text, source_manager, "source", uri);
    if (!syntax_tree || syntax_tree->root().kind != slang::syntax::SyntaxKind::CompilationUnit) {
        return {};
    }

    const auto& compilation_unit = syntax_tree->root().as<slang::syntax::CompilationUnitSyntax>();
    std::vector<ModuleSchematic> result;
    for (const auto* member : compilation_unit.members) {
        if (auto schematic = toModuleSchematic(source_manager, text, *member)) {
            result.push_back(std::move(*schematic));
        }
    }

    return result;
}

std::optional<HoverResult> CompilationService::hover(std::string_view text,
                                                     std::string_view uri,
                                                     int line,
                                                     int character) const {
    const auto symbols = documentSymbols(text, uri);
    const auto* symbol = findHoverSymbol(symbols, line, character);
    if (!symbol) {
        return std::nullopt;
    }

    return HoverResult{.contents = makeHoverContents(*symbol), .range = symbol->selection_range};
}

std::vector<Identifier> CompilationService::identifiers(std::string_view text) const {
    return collectIdentifiers(text);
}

std::optional<Identifier> CompilationService::identifierAt(std::string_view text,
                                                           int line,
                                                           int character) const {
    for (const auto& identifier : collectIdentifiers(text)) {
        if (containsPosition(identifier.range, line, character)) {
            return identifier;
        }
    }

    return std::nullopt;
}

std::string CompilationService::completionPrefix(std::string_view text,
                                                 int line,
                                                 int character) const {
    const auto offset = byteOffsetAtPosition(text, line, character);
    if (!offset.has_value()) {
        return {};
    }

    size_t start = *offset;
    while (start > 0 && isIdentifierContinue(text[start - 1])) {
        --start;
    }

    if (start == *offset) {
        return {};
    }

    return std::string(text.substr(start, *offset - start));
}

std::vector<IncludeDirective> CompilationService::includeDirectives(std::string_view text) const {
    return collectIncludeDirectives(text);
}

} // namespace pristine::analysis
