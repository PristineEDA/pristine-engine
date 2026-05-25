#include "pristine/analysis/CompilationService.h"

#include "pristine/text/Utf.h"

#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/text/SourceManager.h"

#include <optional>
#include <span>

namespace pristine::analysis {
namespace {

struct ParsePosition {
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

std::vector<DocumentSymbol> collectMemberSymbols(const slang::SourceManager& source_manager,
                                                 std::string_view text,
                                                 std::span<slang::syntax::MemberSyntax* const> members);

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
                         const slang::syntax::MemberSyntax& member) {
    switch (member.kind) {
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
            result.push_back(makeDocumentSymbol(
                std::string(declaration.name.valueText()), 26,
                toParseRange(source_manager, text, declaration.sourceRange()),
                toParseRange(source_manager, text, declaration.name.range())));
            return;
        }
        case slang::syntax::SyntaxKind::ParameterDeclarationStatement: {
            const auto& declaration = member.as<slang::syntax::ParameterDeclarationStatementSyntax>();
            auto symbols = collectParameterSymbols(source_manager, text, *declaration.parameter);
            result.insert(result.end(), std::make_move_iterator(symbols.begin()),
                          std::make_move_iterator(symbols.end()));
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

    ParseResult result{.syntax_tree = syntax_tree};
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

} // namespace pristine::analysis