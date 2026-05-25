#include "pristine/analysis/CompilationService.h"

#include "pristine/text/Utf.h"

#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/text/SourceManager.h"

#include <optional>

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
        case slang::syntax::SyntaxKind::CheckerDeclaration:
        case slang::syntax::SyntaxKind::ModuleDeclaration:
        case slang::syntax::SyntaxKind::ProgramDeclaration:
            return 2;
        default:
            return 0;
    }
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
            return DocumentSymbol{.name = std::string(declaration.header->name.valueText()),
                                  .kind = toDocumentSymbolKind(member.kind),
                                  .range = toParseRange(source_manager, text, declaration.sourceRange()),
                                  .selection_range =
                                      toParseRange(source_manager, text, declaration.header->name.range())};
        }
        case slang::syntax::SyntaxKind::CheckerDeclaration: {
            const auto& declaration = member.as<slang::syntax::CheckerDeclarationSyntax>();
            return DocumentSymbol{.name = std::string(declaration.name.valueText()),
                                  .kind = toDocumentSymbolKind(member.kind),
                                  .range = toParseRange(source_manager, text, declaration.sourceRange()),
                                  .selection_range =
                                      toParseRange(source_manager, text, declaration.name.range())};
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

    std::vector<DocumentSymbol> result;
    for (const auto* member : compilation_unit.members) {
        if (auto symbol = toDocumentSymbol(source_manager, text, *member)) {
            result.push_back(std::move(*symbol));
        }
    }

    return result;
}

} // namespace pristine::analysis