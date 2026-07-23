#include "AstIndex.h"

#include "CompletionProvider.h"
#include "DebugTrace.h"
#include "DesignGraphIndexBuilder.h"
#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/ASTContext.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/parsing/Token.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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

bool sameLocation(const SemanticLocation& lhs, const SemanticLocation& rhs) {
    return lhs.uri == rhs.uri && lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line && lhs.range.end_character == rhs.range.end_character;
}

bool rangeStartLess(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.start_line) return lhs.start_line < rhs.start_line;
    return lhs.start_character < rhs.start_character;
}

bool rangeStartsAfter(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.end_line) return lhs.start_line > rhs.end_line;
    return lhs.start_character > rhs.end_character;
}

bool sameRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
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

bool positionLess(int left_line, int left_character, int right_line, int right_character) {
    return left_line < right_line ||
           (left_line == right_line && left_character < right_character);
}

bool rangeEndLess(const ParseRange& lhs, const ParseRange& rhs) {
    return positionLess(lhs.end_line, lhs.end_character, rhs.end_line, rhs.end_character);
}

bool rangeEndBeforePosition(const ParseRange& range, int line, int character) {
    return positionLess(range.end_line, range.end_character, line, character) ||
           (range.end_line == line && range.end_character == character);
}

int rangeLengthScore(const ParseRange& range) {
    return (range.end_line - range.start_line) * 100000 + (range.end_character - range.start_character);
}

bool referenceHasTypeDisplay(const SnapshotData& data, const SnapshotIndexedReference& reference) {
    const auto symbol_it = data.symbols_by_id.find(reference.stable_id);
    return symbol_it != data.symbols_by_id.end() && !symbol_it->second.type_display.empty();
}

bool referenceBetterForLookup(const SnapshotData& data,
                              const SnapshotIndexedReference& candidate,
                              const SnapshotIndexedReference& current) {
    if (candidate.location.range.start_line != current.location.range.start_line) {
        return candidate.location.range.start_line > current.location.range.start_line;
    }
    if (candidate.location.range.start_character != current.location.range.start_character) {
        return candidate.location.range.start_character > current.location.range.start_character;
    }
    if (candidate.is_declaration != current.is_declaration) {
        return candidate.is_declaration;
    }
    const auto candidate_has_type = referenceHasTypeDisplay(data, candidate);
    const auto current_has_type = referenceHasTypeDisplay(data, current);
    if (candidate_has_type != current_has_type) {
        return candidate_has_type;
    }
    const auto candidate_width = rangeLengthScore(candidate.location.range);
    const auto current_width = rangeLengthScore(current.location.range);
    if (candidate_width != current_width) {
        return candidate_width < current_width;
    }
    return candidate.stable_id < current.stable_id;
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

bool rangeContainsRange(const ParseRange& outer, const ParseRange& inner) {
    if (inner.start_line < outer.start_line || inner.end_line > outer.end_line) {
        return false;
    }
    if (inner.start_line == outer.start_line && inner.start_character < outer.start_character) {
        return false;
    }
    if (inner.end_line == outer.end_line && inner.end_character > outer.end_character) {
        return false;
    }
    return true;
}

bool isSimpleSystemVerilogIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto is_start = [](unsigned char ch) {
        return std::isalpha(ch) != 0 || ch == '_' || ch == '$';
    };
    const auto is_continue = [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '$';
    };
    if (!is_start(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [&](char ch) {
        return is_continue(static_cast<unsigned char>(ch));
    });
}

bool rangeLessWideFirst(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.start_line) {
        return lhs.start_line < rhs.start_line;
    }
    if (lhs.start_character != rhs.start_character) {
        return lhs.start_character < rhs.start_character;
    }
    if (lhs.end_line != rhs.end_line) {
        return lhs.end_line > rhs.end_line;
    }
    return lhs.end_character > rhs.end_character;
}

bool isTypeDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "TypeAlias" || kind == "Type" || kind == "ClassType" ||
           kind == "EnumType" || kind == "Interface" || kind == "Modport";
}

bool isModuleDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "Instance" || kind == "InstanceBody";
}

bool isPackageMemberDefinitionKind(std::string_view kind) {
    return isTypeDefinitionKind(kind) || kind == "Parameter" || kind == "EnumValue" ||
           kind == "Variable" || kind == "Net";
}

bool isDuplicateSymbolDiagnosticKind(std::string_view kind) {
    return kind == "Net" || kind == "Variable" || kind == "Parameter" || kind == "TypeAlias" ||
           kind == "Type" || kind == "ClassType" || kind == "EnumType" || kind == "EnumValue" ||
           kind == "Field" || kind == "Member" || kind == "Subroutine";
}

bool identityLess(const SemanticSymbolIdentity& lhs, const SemanticSymbolIdentity& rhs) {
    if (lhs.location.uri != rhs.location.uri) {
        return lhs.location.uri < rhs.location.uri;
    }
    if (!sameRange(lhs.location.range, rhs.location.range)) {
        return locationLess(lhs.location, rhs.location);
    }
    if (lhs.kind != rhs.kind) {
        return lhs.kind < rhs.kind;
    }
    return lhs.stable_id < rhs.stable_id;
}

bool documentImportsPackage(const SnapshotData& data,
                            std::string_view document_uri,
                            std::string_view package_name) {
    const auto imports_it = data.package_imports_by_uri.find(std::string(document_uri));
    if (imports_it == data.package_imports_by_uri.end()) {
        return false;
    }
    return std::any_of(imports_it->second.begin(),
                       imports_it->second.end(),
                       [&](const PackageImport& package_import) {
                           return package_import.package_name == package_name;
                       });
}

bool symbolIsDeclaredInsidePackage(const SnapshotData& data,
                                   std::string_view stable_id,
                                   const SemanticSymbolIdentity& identity,
                                   std::string_view package_name) {
    const auto scope_prefix = std::string("|") + std::string(package_name) + "::";
    if (stable_id.find(scope_prefix) != std::string::npos) {
        return true;
    }

    for (const auto& [_, package_symbol] : data.symbols_by_id) {
        const auto& package_identity = package_symbol.identity;
        if (package_identity.kind != "Package" || package_identity.name != package_name ||
            package_identity.location.uri != identity.location.uri) {
            continue;
        }
        if (rangeContainsRange(package_identity.location.range, identity.location.range)) {
            return true;
        }
    }
    return false;
}

std::vector<SemanticLocation> visibleTypeDefinitionLocationsByName(
    const SnapshotData& data,
    std::string_view reference_uri,
    std::string_view name,
    std::optional<std::string_view> qualified_package) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, symbol] : data.symbols_by_id) {
        const auto& identity = symbol.identity;
        if (identity.name != name || !isTypeDefinitionKind(identity.kind) ||
            isModuleDefinitionKind(identity.kind)) {
            continue;
        }
        if (identity.location.uri == reference_uri) {
            locations.push_back(identity.location);
            continue;
        }
        if (qualified_package.has_value()) {
            if (symbolIsDeclaredInsidePackage(data, symbol.identity.stable_id, identity, *qualified_package)) {
                locations.push_back(identity.location);
            }
            continue;
        }
        for (const auto& [__, package_symbol] : data.symbols_by_id) {
            const auto& package_identity = package_symbol.identity;
            if (package_identity.kind != "Package" ||
                package_identity.location.uri != identity.location.uri) {
                continue;
            }
            if (documentImportsPackage(data, reference_uri, package_identity.name)) {
                locations.push_back(identity.location);
            }
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

std::string directionName(slang::ast::ArgumentDirection direction) {
    switch (direction) {
        case slang::ast::ArgumentDirection::In:
            return "input";
        case slang::ast::ArgumentDirection::Out:
            return "output";
        case slang::ast::ArgumentDirection::InOut:
            return "inout";
        case slang::ast::ArgumentDirection::Ref:
            return "ref";
    }
    return {};
}

std::string normalizedTypeDisplay(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    const auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    });
    value.erase(value.begin(), first);
    const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    }).base();
    value.erase(last, value.end());
    for (size_t index = 0; index + 1 < value.size();) {
        if (value[index] == ' ' && value[index + 1] == ' ') {
            value.erase(index, 1);
            continue;
        }
        ++index;
    }
    const auto packed_open = value.find('[');
    if (packed_open != std::string::npos && packed_open > 0 && value[packed_open - 1] != ' ') {
        value.insert(packed_open, " ");
    }
    return value;
}

std::optional<std::string> textForRange(std::string_view text, const ParseRange& range) {
    if (range.start_line != range.end_line || range.start_line < 0 ||
        range.start_character < 0 || range.end_character < range.start_character) {
        return std::nullopt;
    }
    int current_line = 0;
    size_t line_start = 0;
    for (size_t offset = 0; offset <= text.size(); ++offset) {
        if (offset != text.size() && text[offset] != '\n') {
            continue;
        }
        if (current_line == range.start_line) {
            const auto line_end = offset > line_start && text[offset - 1] == '\r' ? offset - 1 : offset;
            const auto line = text.substr(line_start, line_end - line_start);
            if (static_cast<size_t>(range.end_character) > line.size()) {
                return std::nullopt;
            }
            return std::string(line.substr(static_cast<size_t>(range.start_character),
                                           static_cast<size_t>(range.end_character -
                                                               range.start_character)));
        }
        ++current_line;
        line_start = offset + 1;
    }
    return std::nullopt;
}

std::string textForRangeOrEmpty(const SemanticEngineDocument* document, const ParseRange& range) {
    if (document == nullptr) {
        return {};
    }
    if (auto text = textForRange(document->text, range)) {
        return *text;
    }
    return {};
}

std::string expressionText(const SemanticEngineDocument* document,
                           const slang::SourceManager& source_manager,
                           const slang::ast::Expression& expression) {
    if (document == nullptr) {
        return {};
    }
    const auto range = sourceRangeForSourceRange(source_manager, expression.sourceRange);
    if (auto text = textForRange(document->text, range)) {
        return *text;
    }
    return {};
}

const slang::ast::Expression& unwrapImplicitConversions(const slang::ast::Expression& expression) {
    const slang::ast::Expression* current = &expression;
    while (current->kind == slang::ast::ExpressionKind::Conversion) {
        const auto& conversion = current->as<slang::ast::ConversionExpression>();
        if (!conversion.isImplicit()) break;
        current = &conversion.operand();
    }
    return *current;
}

SnapshotConeSliceKind sliceKindForExpression(const slang::ast::Expression& expression) {
    const auto& unwrapped = unwrapImplicitConversions(expression);
    switch (unwrapped.kind) {
    case slang::ast::ExpressionKind::ElementSelect: {
        const auto& select = unwrapped.as<slang::ast::ElementSelectExpression>();
        return select.selector().kind == slang::ast::ExpressionKind::IntegerLiteral
                   ? SnapshotConeSliceKind::ElementSelect
                   : SnapshotConeSliceKind::DynamicSelect;
    }
    case slang::ast::ExpressionKind::RangeSelect: {
        const auto& select = unwrapped.as<slang::ast::RangeSelectExpression>();
        return select.left().kind == slang::ast::ExpressionKind::IntegerLiteral &&
                       select.right().kind == slang::ast::ExpressionKind::IntegerLiteral
                   ? SnapshotConeSliceKind::RangeSelect
                   : SnapshotConeSliceKind::DynamicSelect;
    }
    case slang::ast::ExpressionKind::Concatenation:
        return SnapshotConeSliceKind::Concatenation;
    case slang::ast::ExpressionKind::MemberAccess:
        return SnapshotConeSliceKind::MemberAccess;
    default:
        return SnapshotConeSliceKind::Whole;
    }
}

std::optional<std::int64_t> staticIntegerValue(const slang::ast::Expression& expression) {
    const auto& unwrapped = unwrapImplicitConversions(expression);
    if (unwrapped.kind != slang::ast::ExpressionKind::IntegerLiteral) {
        return std::nullopt;
    }
    return unwrapped.as<slang::ast::IntegerLiteral>().getValue().as<std::int64_t>();
}

SnapshotConeSliceFact sliceFactForExpression(const slang::ast::Expression& expression) {
    const auto& unwrapped = unwrapImplicitConversions(expression);
    switch (unwrapped.kind) {
    case slang::ast::ExpressionKind::ElementSelect: {
        const auto& select = unwrapped.as<slang::ast::ElementSelectExpression>();
        if (const auto index = staticIntegerValue(select.selector())) {
            return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                         .msb = *index,
                                         .lsb = *index};
        }
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Dynamic,
                                     .msb = {},
                                     .lsb = {}};
    }
    case slang::ast::ExpressionKind::RangeSelect: {
        const auto& select = unwrapped.as<slang::ast::RangeSelectExpression>();
        const auto left = staticIntegerValue(select.left());
        const auto right = staticIntegerValue(select.right());
        if (left && right) {
            return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                         .msb = *left,
                                         .lsb = *right};
        }
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Dynamic,
                                     .msb = {},
                                     .lsb = {}};
    }
    case slang::ast::ExpressionKind::BinaryOp:
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Aggregate,
                                     .msb = {},
                                     .lsb = {}};
    case slang::ast::ExpressionKind::Invalid:
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Unresolved,
                                     .msb = {},
                                     .lsb = {}};
    default:
        return {};
    }
}

bool sameSliceFact(const SnapshotConeSliceFact& lhs, const SnapshotConeSliceFact& rhs) {
    return lhs.precision == rhs.precision && lhs.msb == rhs.msb && lhs.lsb == rhs.lsb;
}

std::optional<std::int64_t> staticBitWidth(const slang::ast::Expression& expression) {
    const auto width = unwrapImplicitConversions(expression).type->getBitWidth();
    if (width == 0 || width > static_cast<slang::bitwidth_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(width);
}

SnapshotConeSliceFact concatOperandSinkSlice(const SnapshotConeSliceFact& parent,
                                             std::int64_t offset,
                                             const slang::ast::Expression& operand) {
    if (parent.precision != SnapshotConeSlicePrecision::Exact || !parent.msb || !parent.lsb) {
        return parent;
    }

    const auto width = staticBitWidth(operand);
    const auto parent_width = std::llabs(*parent.msb - *parent.lsb) + 1;
    if (!width || offset < 0 || offset + *width > parent_width) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Aggregate,
                                     .msb = {},
                                     .lsb = {}};
    }

    const auto step = *parent.msb >= *parent.lsb ? std::int64_t{-1} : std::int64_t{1};
    const auto start = *parent.msb + step * offset;
    return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                 .msb = start,
                                 .lsb = start + step * (*width - 1)};
}

std::string sliceFactKey(const SnapshotConeSliceFact& fact) {
    return std::to_string(static_cast<int>(fact.precision)) + ":" +
           (fact.msb ? std::to_string(*fact.msb) : std::string{}) + ":" +
           (fact.lsb ? std::to_string(*fact.lsb) : std::string{});
}

std::optional<SnapshotConeControlSourceSeed> controlSourceForExpression(
    const SemanticEngineDocument* document,
    const slang::SourceManager& source_manager,
    const slang::ast::Expression& expression) {
    const auto range = sourceRangeForSourceRange(source_manager, expression.sourceRange);
    if (range.start_line < 0 || range.end_line < range.start_line) {
        return std::nullopt;
    }
    return SnapshotConeControlSourceSeed{.range = range,
                                         .expression = expressionText(document, source_manager, expression),
                                         .slice_kind = sliceKindForExpression(expression),
                                         .source_slice = sliceFactForExpression(expression),
                                         .source_symbol_ids = {},
                                         .source_symbol_names = {}};
}

bool sameControlSources(const std::vector<SnapshotConeControlSourceSeed>& lhs,
                        const std::vector<SnapshotConeControlSourceSeed>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!sameRange(lhs[index].range, rhs[index].range) ||
            lhs[index].expression != rhs[index].expression ||
            lhs[index].slice_kind != rhs[index].slice_kind ||
            !sameSliceFact(lhs[index].source_slice, rhs[index].source_slice) ||
            lhs[index].source_symbol_ids != rhs[index].source_symbol_ids ||
            lhs[index].source_symbol_names != rhs[index].source_symbol_names ||
            lhs[index].origin != rhs[index].origin || lhs[index].unresolved != rhs[index].unresolved) {
            return false;
        }
    }
    return true;
}

bool sameDataSources(const std::vector<SnapshotConeDataSourceSeed>& lhs,
                     const std::vector<SnapshotConeDataSourceSeed>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!sameRange(lhs[index].range, rhs[index].range) ||
            lhs[index].expression != rhs[index].expression ||
            lhs[index].slice_kind != rhs[index].slice_kind ||
            !sameSliceFact(lhs[index].source_slice, rhs[index].source_slice) ||
            !sameSliceFact(lhs[index].sink_slice, rhs[index].sink_slice) ||
            lhs[index].source_symbol_ids != rhs[index].source_symbol_ids ||
            lhs[index].source_symbol_names != rhs[index].source_symbol_names ||
            lhs[index].unresolved != rhs[index].unresolved) {
            return false;
        }
    }
    return true;
}

void appendAssignmentEdgeSeed(SnapshotData& data, SnapshotAssignmentEdgeSeed seed) {
    const auto duplicate = std::any_of(data.assignment_edge_seeds.begin(),
                                       data.assignment_edge_seeds.end(),
                                       [&](const SnapshotAssignmentEdgeSeed& existing) {
                                           return existing.uri == seed.uri &&
                                                  sameRange(existing.scope_range, seed.scope_range) &&
                                                  sameRange(existing.assignment_range,
                                                            seed.assignment_range) &&
                                                  sameRange(existing.left_range, seed.left_range) &&
                                                  sameRange(existing.right_range, seed.right_range) &&
                                                  existing.left_expression == seed.left_expression &&
                                                  existing.right_expression == seed.right_expression &&
                                                  existing.left_symbol_ids == seed.left_symbol_ids &&
                                                  existing.left_symbol_names == seed.left_symbol_names &&
                                                  sameDataSources(existing.data_sources, seed.data_sources) &&
                                                  sameControlSources(existing.control_sources,
                                                                     seed.control_sources);
                                       });
    if (duplicate) {
        return;
    }
    data.assignment_edge_seeds.push_back(std::move(seed));
}

std::optional<std::string> logicKindForBinary(slang::ast::BinaryOperator op) {
    switch (op) {
        case slang::ast::BinaryOperator::BinaryAnd:
        case slang::ast::BinaryOperator::LogicalAnd:
            return "and";
        case slang::ast::BinaryOperator::BinaryOr:
        case slang::ast::BinaryOperator::LogicalOr:
            return "or";
        case slang::ast::BinaryOperator::BinaryXor:
            return "xor";
        case slang::ast::BinaryOperator::BinaryXnor:
            return "xnor";
        default:
            return std::nullopt;
    }
}

std::optional<std::string> logicKindForUnary(slang::ast::UnaryOperator op) {
    switch (op) {
        case slang::ast::UnaryOperator::BitwiseNot:
        case slang::ast::UnaryOperator::LogicalNot:
            return "not";
        default:
            return std::nullopt;
    }
}

std::string generatedName(std::string_view prefix, int index) {
    return std::string("$") + std::string(prefix) + std::to_string(index);
}

std::string rangeKey(const ParseRange& range) {
    return std::to_string(range.start_line) + ":" + std::to_string(range.start_character) + ":" +
           std::to_string(range.end_line) + ":" + std::to_string(range.end_character);
}

SchematicConnection makeConnection(std::string port_name,
                                   int port_index,
                                   std::string signal,
                                   const slang::SourceManager& source_manager,
                                   const slang::ast::Expression& expression) {
    return SchematicConnection{.port_name = std::move(port_name),
                               .port_index = port_index,
                               .signal = std::move(signal),
                               .range = sourceRangeForSourceRange(source_manager, expression.sourceRange)};
}

struct AstExpressionSchematicContext {
    int cell_index = 0;
    int net_index = 0;
};

std::string materializeAstExpression(ModuleSchematic& schematic,
                                     const SemanticEngineDocument* document,
                                     const slang::SourceManager& source_manager,
                                     const slang::ast::Expression& expression,
                                     AstExpressionSchematicContext& context);

void appendAstLogicCell(ModuleSchematic& schematic,
                        const slang::SourceManager& source_manager,
                        std::string kind,
                        const slang::ast::Expression& source,
                        std::vector<SchematicConnection> connections,
                        AstExpressionSchematicContext& context) {
    const auto name = generatedName(kind, context.cell_index++);
    const auto range = sourceRangeForSourceRange(source_manager, source.sourceRange);
    schematic.cells.push_back(SchematicCell{.id = name,
                                            .name = name,
                                            .type = kind,
                                            .kind = std::move(kind),
                                            .range = range,
                                            .selection_range = range,
                                            .connections = std::move(connections)});
}

bool appendAstLogicExpression(ModuleSchematic& schematic,
                              const SemanticEngineDocument* document,
                              const slang::SourceManager& source_manager,
                              const slang::ast::Expression& expression,
                              std::string output_signal,
                              AstExpressionSchematicContext& context) {
    if (const auto* binary = expression.as_if<slang::ast::BinaryExpression>()) {
        const auto kind = logicKindForBinary(binary->op);
        if (!kind.has_value()) {
            return false;
        }
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal), source_manager, expression));
        connections.push_back(makeConnection("A",
                                             -1,
                                             materializeAstExpression(schematic,
                                                                      document,
                                                                      source_manager,
                                                                      binary->left(),
                                                                      context),
                                             source_manager,
                                             binary->left()));
        connections.push_back(makeConnection("B",
                                             -1,
                                             materializeAstExpression(schematic,
                                                                      document,
                                                                      source_manager,
                                                                      binary->right(),
                                                                      context),
                                             source_manager,
                                             binary->right()));
        appendAstLogicCell(schematic, source_manager, *kind, expression, std::move(connections), context);
        return true;
    }

    if (const auto* unary = expression.as_if<slang::ast::UnaryExpression>()) {
        const auto kind = logicKindForUnary(unary->op);
        if (!kind.has_value()) {
            return false;
        }
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal), source_manager, expression));
        connections.push_back(makeConnection("A",
                                             -1,
                                             materializeAstExpression(schematic,
                                                                      document,
                                                                      source_manager,
                                                                      unary->operand(),
                                                                      context),
                                             source_manager,
                                             unary->operand()));
        appendAstLogicCell(schematic, source_manager, *kind, expression, std::move(connections), context);
        return true;
    }

    if (const auto* conditional = expression.as_if<slang::ast::ConditionalExpression>()) {
        std::vector<SchematicConnection> connections;
        connections.push_back(makeConnection("Y", -1, std::move(output_signal), source_manager, expression));
        if (!conditional->conditions.empty()) {
            const auto& condition = *conditional->conditions.begin();
            connections.push_back(makeConnection("S",
                                                 -1,
                                                 materializeAstExpression(schematic,
                                                                          document,
                                                                          source_manager,
                                                                          *condition.expr,
                                                                          context),
                                                 source_manager,
                                                 *condition.expr));
        }
        connections.push_back(makeConnection("I1",
                                             -1,
                                             materializeAstExpression(schematic,
                                                                      document,
                                                                      source_manager,
                                                                      conditional->left(),
                                                                      context),
                                             source_manager,
                                             conditional->left()));
        connections.push_back(makeConnection("I0",
                                             -1,
                                             materializeAstExpression(schematic,
                                                                      document,
                                                                      source_manager,
                                                                      conditional->right(),
                                                                      context),
                                             source_manager,
                                             conditional->right()));
        appendAstLogicCell(schematic, source_manager, "mux", expression, std::move(connections), context);
        return true;
    }

    return false;
}

std::string materializeAstExpression(ModuleSchematic& schematic,
                                     const SemanticEngineDocument* document,
                                     const slang::SourceManager& source_manager,
                                     const slang::ast::Expression& expression,
                                     AstExpressionSchematicContext& context) {
    if (expression.as_if<slang::ast::BinaryExpression>() != nullptr ||
        expression.as_if<slang::ast::UnaryExpression>() != nullptr ||
        expression.as_if<slang::ast::ConditionalExpression>() != nullptr) {
        const auto signal = generatedName("net", context.net_index++);
        if (appendAstLogicExpression(schematic, document, source_manager, expression, signal, context)) {
            return signal;
        }
    }
    return expressionText(document, source_manager, expression);
}

std::optional<ParseRange> identifierRangeByName(std::string_view text,
                                                const ParseRange& range,
                                                std::string_view name) {
    if (range.start_line != range.end_line || range.start_line < 0 ||
        range.start_character < 0 || range.end_character < range.start_character || name.empty()) {
        return std::nullopt;
    }

    int current_line = 0;
    size_t line_start = 0;
    for (size_t offset = 0; offset <= text.size(); ++offset) {
        if (offset != text.size() && text[offset] != '\n') {
            continue;
        }
        if (current_line == range.start_line) {
            const auto line_end = offset > line_start && text[offset - 1] == '\r' ? offset - 1 : offset;
            const auto line = text.substr(line_start, line_end - line_start);
            if (static_cast<size_t>(range.end_character) > line.size()) {
                return std::nullopt;
            }
            const auto bounded = line.substr(static_cast<size_t>(range.start_character),
                                             static_cast<size_t>(range.end_character -
                                                                 range.start_character));
            const auto found = bounded.find(name);
            if (found == std::string_view::npos) {
                return std::nullopt;
            }
            const auto start = range.start_character + static_cast<int>(found);
            return ParseRange{.start_line = range.start_line,
                              .start_character = start,
                              .end_line = range.start_line,
                              .end_character = start + static_cast<int>(name.size())};
        }
        ++current_line;
        line_start = offset + 1;
    }
    return std::nullopt;
}

std::optional<ParseRange> instanceStatementRange(std::string_view text,
                                                 const ParseRange& instance_selection,
                                                 std::string_view module_name) {
    if (instance_selection.start_line != instance_selection.end_line ||
        instance_selection.start_line < 0 || instance_selection.start_character < 0 ||
        module_name.empty()) {
        return std::nullopt;
    }

    int current_line = 0;
    size_t line_start = 0;
    for (size_t offset = 0; offset <= text.size(); ++offset) {
        if (offset != text.size() && text[offset] != '\n') {
            continue;
        }
        if (current_line == instance_selection.start_line) {
            const auto line_end = offset > line_start && text[offset - 1] == '\r' ? offset - 1 : offset;
            const auto line = text.substr(line_start, line_end - line_start);
            if (static_cast<size_t>(instance_selection.start_character) > line.size()) {
                return std::nullopt;
            }
            const auto before_instance = line.substr(0,
                                                     static_cast<size_t>(
                                                         instance_selection.start_character));
            const auto module_start = before_instance.rfind(module_name);
            if (module_start == std::string_view::npos) {
                return std::nullopt;
            }
            auto statement_end = line.find(';',
                                           static_cast<size_t>(
                                               std::max(instance_selection.end_character,
                                                        instance_selection.start_character)));
            if (statement_end == std::string_view::npos) {
                statement_end = line.size();
            }
            else {
                ++statement_end;
            }
            return ParseRange{.start_line = instance_selection.start_line,
                              .start_character = static_cast<int>(module_start),
                              .end_line = instance_selection.start_line,
                              .end_character = static_cast<int>(statement_end)};
        }
        ++current_line;
        line_start = offset + 1;
    }
    return std::nullopt;
}

std::string symbolKindName(slang::ast::SymbolKind kind) {
    return std::string(slang::ast::toString(kind));
}

std::string locationUriForSourceLocation(const slang::SourceManager& source_manager,
                                         slang::SourceLocation location) {
    if (!location.valid()) {
        return {};
    }
    const auto original = source_manager.getFullyOriginalLoc(location);
    const auto path = source_manager.getFullPath(original.buffer());
    if (!path.empty()) {
        return pathToFileUri(path);
    }
    const auto file_name = source_manager.getFileName(original);
    return file_name.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(file_name));
}

std::optional<SemanticLocation> locationForSourceRange(const slang::SourceManager& source_manager,
                                                       slang::SourceRange range) {
    if (!range.start().valid()) {
        return std::nullopt;
    }
    const auto uri = locationUriForSourceLocation(source_manager, range.start());
    if (uri.empty()) {
        return std::nullopt;
    }
    return SemanticLocation{.uri = uri, .range = sourceRangeForSourceRange(source_manager, range)};
}

std::optional<SemanticLocation> locationForSyntaxToken(const slang::SourceManager& source_manager,
                                                       slang::parsing::Token token) {
    return locationForSourceRange(source_manager, token.range());
}

std::optional<ParseRange> parseRangeForSymbolSyntax(const slang::SourceManager& source_manager,
                                                    const slang::ast::Symbol& symbol) {
    if (const auto* syntax = symbol.getSyntax()) {
        return sourceRangeForSourceRange(source_manager, syntax->sourceRange());
    }
    if (const auto location = locationForSourceRange(source_manager,
                                                    slang::SourceRange(symbol.location,
                                                                       symbol.location +
                                                                           symbol.name.size()))) {
        return location->range;
    }
    return std::nullopt;
}

std::string typeDisplayForSymbol(const slang::ast::Symbol& symbol);

std::string symbolStableId(const slang::SourceManager& source_manager,
                           const slang::ast::Symbol& symbol,
                           const SemanticLocation& location) {
    std::string path = symbol.getLexicalPath();
    if (path.empty()) {
        path = symbol.getHierarchicalPath();
    }
    if (path.empty()) {
        path = std::string(symbol.name);
    }

    return location.uri + "|" + path + "|" + symbolKindName(symbol.kind) + "|" +
           std::to_string(location.range.start_line) + ":" +
           std::to_string(location.range.start_character) + ":" +
           std::to_string(source_manager.getFullyOriginalLoc(symbol.location).offset());
}

std::string instanceStableId(const slang::SourceManager& source_manager,
                             const slang::ast::InstanceSymbol& instance,
                             const SemanticLocation& location) {
    std::string path = instance.getHierarchicalPath();
    if (path.empty()) {
        path = instance.getLexicalPath();
    }
    if (path.empty()) {
        path = std::string(instance.name);
    }
    return location.uri + "|" + path + "|Instance|" +
           std::to_string(location.range.start_line) + ":" +
           std::to_string(location.range.start_character) + ":" +
           std::to_string(source_manager.getFullyOriginalLoc(instance.location).offset());
}

std::optional<SchematicPort> schematicPortForAstPort(const slang::SourceManager& source_manager,
                                                     const slang::ast::Symbol& port_symbol) {
    if (port_symbol.name.empty()) {
        return std::nullopt;
    }

    std::string direction = "inout";
    std::string type_display;
    if (port_symbol.kind == slang::ast::SymbolKind::Port) {
        const auto& port = port_symbol.as<slang::ast::PortSymbol>();
        direction = directionName(port.direction);
        type_display = normalizedTypeDisplay(port.getType().toString());
        if (port.internalSymbol != nullptr) {
            if (auto internal_type_display = typeDisplayForSymbol(*port.internalSymbol);
                !internal_type_display.empty()) {
                type_display = std::move(internal_type_display);
            }
        }
    }
    else if (port_symbol.kind == slang::ast::SymbolKind::MultiPort) {
        const auto& port = port_symbol.as<slang::ast::MultiPortSymbol>();
        direction = directionName(port.direction);
        type_display = normalizedTypeDisplay(port.getType().toString());
        for (const auto* sub_port : port.ports) {
            if (sub_port == nullptr || sub_port->internalSymbol == nullptr) {
                continue;
            }
            if (auto internal_type_display = typeDisplayForSymbol(*sub_port->internalSymbol);
                !internal_type_display.empty()) {
                type_display = std::move(internal_type_display);
                break;
            }
        }
    }
    else if (port_symbol.kind == slang::ast::SymbolKind::InterfacePort) {
        const auto& port = port_symbol.as<slang::ast::InterfacePortSymbol>();
        direction = "interface";
        if (port.interfaceDef != nullptr) {
            type_display = std::string(port.interfaceDef->name);
        }
        if (!port.modport.empty()) {
            if (!type_display.empty()) {
                type_display += ".";
            }
            type_display += std::string(port.modport);
        }
        if (type_display.empty() && port.isGeneric) {
            type_display = "interface";
        }
    }
    else {
        return std::nullopt;
    }

    const auto location = declarationLocationForSymbol(source_manager, port_symbol);
    const auto range = parseRangeForSymbolSyntax(source_manager, port_symbol);
    if (!location.has_value() || !range.has_value()) {
        return std::nullopt;
    }

    return SchematicPort{.name = std::string(port_symbol.name),
                         .direction = std::move(direction),
                         .width_text = std::move(type_display),
                         .range = *range,
                         .selection_range = location->range};
}

std::optional<SchematicPort> schematicPortForAstParameter(
    const slang::SourceManager& source_manager,
    const slang::ast::ParameterSymbolBase& parameter) {
    const auto& symbol = parameter.symbol;
    if (symbol.name.empty()) {
        return std::nullopt;
    }

    const auto location = declarationLocationForSymbol(source_manager, symbol);
    const auto range = parseRangeForSymbolSyntax(source_manager, symbol);
    if (!location.has_value() || !range.has_value()) {
        return std::nullopt;
    }

    std::string type_display;
    if (symbol.kind == slang::ast::SymbolKind::Parameter) {
        type_display = typeDisplayForSymbol(symbol);
    }
    else if (symbol.kind == slang::ast::SymbolKind::TypeParameter) {
        type_display = "type";
    }

    return SchematicPort{.name = std::string(symbol.name),
                         .direction = "parameter",
                         .width_text = std::move(type_display),
                         .range = *range,
                         .selection_range = location->range};
}

SnapshotConeSliceFact declaredSliceForEndpoint(const slang::ast::Symbol& symbol);
void collectResolvedConnectionSourceParts(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::Expression& expression,
    const SnapshotConeSliceFact& endpoint_slice,
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts,
    std::optional<SemanticLocation> source_location_override = std::nullopt);
void collectSyntaxConnectionSourceParts(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::syntax::ExpressionSyntax& syntax,
    const SnapshotConeSliceFact& endpoint_slice,
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts);
void normalizeConnectionSourceParts(std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts);

bool appendResolvedConnectionSliceFact(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    std::string instance_stable_id,
    std::string endpoint_stable_id,
    std::string endpoint_name,
    int endpoint_index,
    SnapshotConeEdgeKind kind,
    const SnapshotConeSliceFact& declared_slice,
    const slang::ast::Expression& expression,
    std::optional<slang::SourceRange> source_range_override = std::nullopt,
    const slang::syntax::ExpressionSyntax* source_syntax = nullptr) {
    if (instance_stable_id.empty() || (endpoint_stable_id.empty() && endpoint_name.empty())) {
        return false;
    }
    auto location = locationForSourceRange(source_manager, expression.sourceRange);
    if (!location.has_value() && source_range_override.has_value()) {
        location = locationForSourceRange(source_manager, *source_range_override);
    }
    if (!location.has_value()) {
        return false;
    }
    SnapshotResolvedConnectionSliceFact fact{.instance_stable_id = std::move(instance_stable_id),
                                             .endpoint_stable_id = std::move(endpoint_stable_id),
                                             .endpoint_name = std::move(endpoint_name),
                                             .endpoint_index = endpoint_index,
                                             .location = *location,
                                             .kind = kind,
                                             .source_parts = {},
                                             .literal_display = source_syntax == nullptr
                                                                    ? std::string{}
                                                                    : std::string(source_syntax->toString()),
                                             .unresolved = false};
    // The endpoint's declared range is captured from the resolved port or
    // parameter symbol. The expression collector then narrows individual
    // concat/select sources without using SchematicConnection::signal.
    if (!fact.endpoint_stable_id.empty()) {
        data.endpoint_declared_slices_by_id.insert_or_assign(fact.endpoint_stable_id, declared_slice);
    }
    std::optional<SemanticLocation> source_location_override;
    if (source_range_override.has_value()) {
        source_location_override = locationForSourceRange(source_manager, *source_range_override);
    }
    if (source_syntax != nullptr &&
        unwrapImplicitConversions(expression).kind == slang::ast::ExpressionKind::Invalid) {
        collectSyntaxConnectionSourceParts(data,
                                            source_manager,
                                            *source_syntax,
                                            declared_slice,
                                            fact.source_parts);
    }
    else {
        collectResolvedConnectionSourceParts(data,
                                             source_manager,
                                             expression,
                                             declared_slice,
                                             fact.source_parts,
                                             std::move(source_location_override));
    }
    fact.unresolved = std::any_of(fact.source_parts.begin(),
                                  fact.source_parts.end(),
                                  [](const auto& part) { return part.unresolved; });
    data.resolved_connection_slices_by_instance_id[fact.instance_stable_id].push_back(std::move(fact));
    return true;
}

void collectResolvedPortConnectionSlices(SnapshotData& data,
                                         const slang::SourceManager& source_manager,
                                         const slang::ast::InstanceSymbol& instance,
                                         std::string_view instance_stable_id) {
    size_t index = 0;
    for (const auto* connection : instance.getPortConnections()) {
        if (connection == nullptr) {
            ++index;
            continue;
        }
        const auto endpoint = data.ids_by_symbol.find(&connection->port);
        const auto* expression = connection->getExpression();
        if (endpoint != data.ids_by_symbol.end() && expression != nullptr) {
            appendResolvedConnectionSliceFact(data,
                                              source_manager,
                                              std::string(instance_stable_id),
                                              endpoint->second,
                                              std::string(connection->port.name),
                                              static_cast<int>(index),
                                              SnapshotConeEdgeKind::InstancePort,
                                              declaredSliceForEndpoint(connection->port),
                                              *expression);
        }
        ++index;
    }
}

std::optional<std::string> symbolIdForDefinitionParameter(
    const SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::DefinitionSymbol::ParameterDecl& parameter) {
    if (!parameter.hasSyntax) {
        return std::nullopt;
    }

    const auto declaration_range = parameter.isTypeParam ? parameter.typeDecl->sourceRange()
                                                          : parameter.valueDecl->sourceRange();
    const auto location = locationForSourceRange(source_manager, declaration_range);
    if (!location.has_value()) {
        return std::nullopt;
    }
    const auto symbols = data.graph_symbols_by_uri.find(location->uri);
    if (symbols == data.graph_symbols_by_uri.end()) {
        return std::nullopt;
    }
    const auto first = std::lower_bound(
        symbols->second.begin(),
        symbols->second.end(),
        location->range,
        [](const SnapshotUriSymbolRangeFact& candidate, const ParseRange& range) {
            if (candidate.range.start_line != range.start_line) {
                return candidate.range.start_line < range.start_line;
            }
            return candidate.range.start_character < range.start_character;
        });
    for (auto it = first; it != symbols->second.end(); ++it) {
        if (rangeStartsAfter(it->range, location->range)) break;
        if (!rangeContainsRange(location->range, it->range)) continue;
        const auto indexed = data.symbols_by_id.find(it->stable_id);
        if (indexed == data.symbols_by_id.end()) continue;
        if (indexed->second.identity.kind.find("Parameter") == std::string::npos &&
            indexed->second.identity.kind.find("Param") == std::string::npos) {
            continue;
        }
        return it->stable_id;
    }
    return std::nullopt;
}

void collectResolvedParameterOverrideSlices(SnapshotData& data,
                                            const slang::SourceManager& source_manager,
                                            const slang::ast::InstanceSymbol& instance,
                                            std::string_view instance_stable_id,
                                            const slang::syntax::ParameterValueAssignmentSyntax&
                                                parameter_syntax) {
    const auto* parent_scope = instance.getParentScope();
    if (parent_scope == nullptr) {
        return;
    }
    const auto parameters = instance.body.getParameters();
    const auto definition_parameters = instance.getDefinition().parameters;
    data.parameter_override_available_endpoint_count += parameters.size();
    std::unordered_map<std::string_view, std::pair<const slang::ast::ParameterSymbolBase*, size_t>>
        parameters_by_name;
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index] != nullptr) {
            parameters_by_name.try_emplace(parameters[index]->symbol.name, parameters[index], index);
        }
    }

    slang::ast::ASTContext context(*parent_scope,
                                   slang::ast::LookupLocation::after(instance),
                                   slang::ast::ASTFlags::NonProcedural);
    const auto bind_assignment = [&](const slang::syntax::ExpressionSyntax& syntax,
                                     const slang::ast::ParameterSymbolBase& parameter,
                                     size_t endpoint_index) {
        ++data.parameter_override_matched_endpoint_count;
        const auto& expression = slang::ast::Expression::bindRValue(
            parent_scope->getCompilation().getErrorType(),
            syntax,
            {},
            context,
            slang::ast::ASTFlags::AllowDataType);
        std::string endpoint_stable_id;
        if (endpoint_index < definition_parameters.size()) {
            if (const auto definition_id = symbolIdForDefinitionParameter(
                    data, source_manager, definition_parameters[endpoint_index]);
                definition_id.has_value()) {
                endpoint_stable_id = *definition_id;
            }
        }
        if (appendResolvedConnectionSliceFact(data,
                                              source_manager,
                                              std::string(instance_stable_id),
                                              std::move(endpoint_stable_id),
                                              std::string(parameter.symbol.name),
                                              static_cast<int>(endpoint_index),
                                              SnapshotConeEdgeKind::ParameterOverride,
                                              declaredSliceForEndpoint(parameter.symbol),
                                              expression,
                                              syntax.sourceRange(),
                                              &syntax)) {
            ++data.parameter_override_resolved_fact_count;
        }
    };

    bool ordered = true;
    if (!parameter_syntax.parameters.empty()) {
        ordered = parameter_syntax.parameters[0]->kind ==
                  slang::syntax::SyntaxKind::OrderedParamAssignment;
    }
    if (ordered) {
        const auto count = std::min(parameter_syntax.parameters.size(), parameters.size());
        for (size_t parameter_index = 0; parameter_index < count; ++parameter_index) {
            const auto* assignment = parameter_syntax.parameters[parameter_index];
            if (assignment == nullptr || assignment->kind != slang::syntax::SyntaxKind::OrderedParamAssignment) {
                return;
            }
            const auto& expression = assignment->as<slang::syntax::OrderedParamAssignmentSyntax>().expr;
            bind_assignment(*expression, *parameters[parameter_index], parameter_index);
        }
        return;
    }

    for (const auto* assignment : parameter_syntax.parameters) {
        if (assignment == nullptr || assignment->kind != slang::syntax::SyntaxKind::NamedParamAssignment) {
            continue;
        }
        const auto& named = assignment->as<slang::syntax::NamedParamAssignmentSyntax>();
        if (named.expr == nullptr) {
            continue;
        }
        const auto parameter = parameters_by_name.find(named.name.valueText());
        if (parameter == parameters_by_name.end()) {
            continue;
        }
        bind_assignment(*named.expr, *parameter->second.first, parameter->second.second);
    }
}

void collectSyntaxBoundParameterOverrideSlices(SnapshotData& data,
                                                const slang::SourceManager& source_manager) {
    for (const auto& pending : data.parameter_override_syntax_facts) {
        if (pending.syntax == nullptr || pending.uri.empty() || pending.module_name.empty() ||
            pending.instance_name.empty()) {
            continue;
        }
        const auto instances = data.module_instances_by_uri.find(pending.uri);
        if (instances == data.module_instances_by_uri.end()) {
            ++data.parameter_override_syntax_binding_miss_count;
            continue;
        }
        bool matched_instance = false;
        for (const auto& instance : instances->second) {
            if (instance.instance_stable_id.empty() || instance.module_name != pending.module_name ||
                instance.instance_name != pending.instance_name ||
                !(sameRange(instance.selection_range, pending.instance_range) ||
                  rangesOverlapOrTouch(instance.selection_range, pending.instance_range))) {
                continue;
            }
            matched_instance = true;
            const auto ast_instance = data.instance_symbols_by_stable_id.find(instance.instance_stable_id);
            if (ast_instance == data.instance_symbols_by_stable_id.end() || ast_instance->second == nullptr) {
                ++data.parameter_override_syntax_binding_miss_count;
                continue;
            }
            ++data.parameter_override_syntax_binding_count;
            collectResolvedParameterOverrideSlices(data,
                                                   source_manager,
                                                   *ast_instance->second,
                                                   instance.instance_stable_id,
                                                   *pending.syntax);
        }
        if (!matched_instance) {
            ++data.parameter_override_syntax_binding_miss_count;
        }
    }
}

void collectSyntaxBoundEmptyPortConnectionRanges(SnapshotData& data,
                                                  const slang::SourceManager& source_manager) {
    for (const auto& pending : data.port_connection_syntax_facts) {
        if (pending.syntax == nullptr || pending.uri.empty() || pending.module_name.empty() ||
            pending.instance_name.empty()) {
            continue;
        }
        const auto instances = data.module_instances_by_uri.find(pending.uri);
        if (instances == data.module_instances_by_uri.end()) {
            continue;
        }
        auto instance = std::find_if(instances->second.begin(),
                                     instances->second.end(),
                                     [&](const SnapshotModuleInstance& candidate) {
                                         return candidate.module_name == pending.module_name &&
                                                candidate.instance_name == pending.instance_name &&
                                                (sameRange(candidate.selection_range, pending.instance_range) ||
                                                 rangesOverlapOrTouch(candidate.selection_range,
                                                                      pending.instance_range));
                                     });
        if (instance == instances->second.end()) {
            continue;
        }
        for (const auto* connection : pending.syntax->connections) {
            if (connection == nullptr ||
                connection->kind != slang::syntax::SyntaxKind::NamedPortConnection) {
                continue;
            }
            const auto& named = connection->as<slang::syntax::NamedPortConnectionSyntax>();
            if (named.expr != nullptr || named.name.isMissing()) {
                continue;
            }
            const auto location = locationForSourceRange(source_manager, named.sourceRange());
            if (!location.has_value()) {
                continue;
            }
            const auto port_name = std::string(named.name.valueText());
            const auto duplicate = std::any_of(instance->port_connections.begin(),
                                               instance->port_connections.end(),
                                               [&](const SchematicConnection& existing) {
                                                   return existing.port_name == port_name &&
                                                          sameRange(existing.range, location->range);
                                               });
            if (!duplicate) {
                instance->port_connections.push_back(
                    SchematicConnection{.port_name = port_name,
                                        .port_index = -1,
                                        .signal = {},
                                        .range = location->range});
            }
        }
    }
}

class ConnectionSliceIndexer {
public:
    ConnectionSliceIndexer(SnapshotData& data, const slang::SourceManager& source_manager) :
        data_(data), source_manager_(source_manager) {}

    void collectForInstance(const slang::ast::InstanceSymbol& instance,
                            std::string_view instance_stable_id) {
        data_.instance_symbols_by_stable_id.insert_or_assign(std::string(instance_stable_id), &instance);
        collectResolvedPortConnectionSlices(data_, source_manager_, instance, instance_stable_id);
    }

    void finalize() {
        for (auto& [_, facts] : data_.resolved_connection_slices_by_instance_id) {
            std::sort(facts.begin(), facts.end(), [](const auto& lhs, const auto& rhs) {
                return std::tie(lhs.endpoint_stable_id,
                                lhs.endpoint_index,
                                lhs.location.uri,
                                lhs.location.range.start_line,
                                lhs.location.range.start_character,
                                lhs.kind,
                                lhs.unresolved) <
                       std::tie(rhs.endpoint_stable_id,
                                rhs.endpoint_index,
                                rhs.location.uri,
                                rhs.location.range.start_line,
                                rhs.location.range.start_character,
                                rhs.kind,
                                rhs.unresolved);
            });
        }
        data_.parameter_override_syntax_facts.clear();
        data_.port_connection_syntax_facts.clear();
        data_.instance_symbols_by_stable_id.clear();
    }

private:
    SnapshotData& data_;
    const slang::SourceManager& source_manager_;
};

struct ResolvedConnectionSliceVisitor
    : slang::ast::ASTVisitor<ResolvedConnectionSliceVisitor, slang::ast::VisitFlags::AllGood> {
    SnapshotData& data;
    const slang::SourceManager& source_manager;
    ConnectionSliceIndexer& indexer;

    ResolvedConnectionSliceVisitor(SnapshotData& data,
                                   const slang::SourceManager& source_manager,
                                   ConnectionSliceIndexer& indexer) :
        data(data), source_manager(source_manager), indexer(indexer) {}

    void handle(const slang::ast::InstanceSymbol& instance) {
        const auto location = declarationLocationForSymbol(source_manager, instance);
        if (location.has_value()) {
            const auto instance_id = instanceStableId(source_manager, instance, *location);
            indexer.collectForInstance(instance, instance_id);
        }
        this->visitDefault(instance);
    }

    template<typename T>
    void handle(const T& symbol)
        requires std::is_base_of_v<slang::ast::Symbol, T>
    {
        this->visitDefault(symbol);
    }

    template<typename T>
    void handle(const T& expression)
        requires std::is_base_of_v<slang::ast::Expression, T>
    {
        this->visitDefault(expression);
    }
};

void buildResolvedConnectionSliceFacts(SnapshotData& data, const slang::SourceManager& source_manager) {
    data.resolved_connection_slices_by_instance_id.clear();
    data.endpoint_declared_slices_by_id.clear();
    data.parameter_override_syntax_binding_count = 0;
    data.parameter_override_syntax_binding_miss_count = 0;
    data.parameter_override_available_endpoint_count = 0;
    data.parameter_override_matched_endpoint_count = 0;
    data.parameter_override_resolved_fact_count = 0;
    ConnectionSliceIndexer indexer(data, source_manager);
    ResolvedConnectionSliceVisitor visitor(data, source_manager, indexer);
    data.compilation->getRoot().visit(visitor);
    collectSyntaxBoundEmptyPortConnectionRanges(data, source_manager);
    collectSyntaxBoundParameterOverrideSlices(data, source_manager);
    indexer.finalize();
}

std::string typeDisplayForSymbol(const slang::ast::Symbol& symbol) {
    if (const auto* declared_type = symbol.getDeclaredType()) {
        return normalizedTypeDisplay(declared_type->getType().toString());
    }
    if (symbol.kind == slang::ast::SymbolKind::Port) {
        return normalizedTypeDisplay(symbol.as<slang::ast::PortSymbol>().getType().toString());
    }
    if (symbol.kind == slang::ast::SymbolKind::MultiPort) {
        return normalizedTypeDisplay(symbol.as<slang::ast::MultiPortSymbol>().getType().toString());
    }
    if (symbol.kind == slang::ast::SymbolKind::InterfacePort) {
        const auto& port = symbol.as<slang::ast::InterfacePortSymbol>();
        std::string display;
        if (port.interfaceDef != nullptr) {
            display = std::string(port.interfaceDef->name);
        }
        if (!port.modport.empty()) {
            if (!display.empty()) {
                display += ".";
            }
            display += std::string(port.modport);
        }
        if (display.empty() && port.isGeneric) {
            display = "interface";
        }
        return display;
    }
    if (symbol.isType()) {
        if (const auto* type = symbol.as_if<slang::ast::Type>()) {
            return normalizedTypeDisplay(type->toString());
        }
    }
    return {};
}

std::string normalizedConstantDisplay(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    for (size_t index = 0; index + 1 < value.size();) {
        if (std::isspace(static_cast<unsigned char>(value[index])) &&
            std::isspace(static_cast<unsigned char>(value[index + 1]))) {
            value.erase(index, 1);
            continue;
        }
        ++index;
    }
    return value;
}

std::string constantValueDisplay(const slang::ConstantValue& value) {
    if (!value) {
        return {};
    }
    return normalizedConstantDisplay(value.toString());
}

std::string valueDisplayForSymbol(const slang::ast::Symbol& symbol) {
    if (symbol.kind == slang::ast::SymbolKind::Parameter) {
        return constantValueDisplay(symbol.as<slang::ast::ParameterSymbol>().getValue());
    }
    if (symbol.kind == slang::ast::SymbolKind::Specparam) {
        return constantValueDisplay(symbol.as<slang::ast::SpecparamSymbol>().getValue());
    }
    if (symbol.kind == slang::ast::SymbolKind::EnumValue) {
        return constantValueDisplay(symbol.as<slang::ast::EnumValueSymbol>().getValue());
    }
    return {};
}

const slang::ast::Type& unwrapArrayElementType(const slang::ast::Type& type) {
    const auto* current = &type.getCanonicalType();
    while (const auto* element = current->getArrayElementType()) {
        current = &element->getCanonicalType();
    }
    return *current;
}

std::vector<SemanticSymbolIdentity> typedMembersForType(const slang::SourceManager& source_manager,
                                                        const slang::ast::Type& type,
                                                        const SemanticLocation& owner_location,
                                                        std::string_view owner_stable_id) {
    const auto& canonical = unwrapArrayElementType(type);
    std::vector<SemanticSymbolIdentity> members;
    const auto append_member = [&](const slang::ast::Symbol& member, std::string_view kind) {
        if (member.name.empty()) {
            return;
        }
        const auto member_location = declarationLocationForSymbol(source_manager, member)
                                         .value_or(owner_location);
        members.push_back(SemanticSymbolIdentity{
            .stable_id = std::string(owner_stable_id) + "|member|" + std::string(member.name),
            .name = std::string(member.name),
            .kind = std::string(kind),
            .location = member_location});
    };

    if (canonical.kind == slang::ast::SymbolKind::PackedStructType) {
        const auto& struct_type = canonical.as<slang::ast::PackedStructType>();
        for (const auto& member : struct_type.members()) {
            if (member.kind == slang::ast::SymbolKind::Field) {
                append_member(member, "Field");
            }
        }
    }
    else if (canonical.kind == slang::ast::SymbolKind::UnpackedStructType) {
        const auto& struct_type = canonical.as<slang::ast::UnpackedStructType>();
        for (const auto* field : struct_type.fields) {
            if (field != nullptr) {
                append_member(*field, "Field");
            }
        }
    }
    else if (canonical.kind == slang::ast::SymbolKind::ClassType) {
        const auto& class_type = canonical.as<slang::ast::ClassType>();
        for (const auto& property : class_type.properties()) {
            append_member(property, "Field");
        }
        for (const auto& member : class_type.members()) {
            if (member.kind == slang::ast::SymbolKind::Subroutine) {
                append_member(member, "Subroutine");
            }
        }
    }
    return members;
}

std::string typeDisplayForMemberCompletion(const slang::ast::Symbol& symbol) {
    if (const auto* declared_type = symbol.getDeclaredType()) {
        return normalizedTypeDisplay(declared_type->getType().toString());
    }
    return {};
}

std::optional<std::string> completionMemberKind(std::string_view kind) {
    if (kind == "Field" || kind == "Member" || kind == "Net" || kind == "Variable" ||
        kind == "Parameter" || kind == "Subroutine") {
        return std::string(kind);
    }
    if (kind == "MethodPrototype") {
        return "Subroutine";
    }
    if (kind == "TypeParameter") {
        return "Parameter";
    }
    if (kind == "Port" || kind == "MultiPort" || kind == "InterfacePort" ||
        kind == "ModportPort" || kind == "ModportClocking") {
        return "Field";
    }
    return std::nullopt;
}

void appendMemberCompletion(SnapshotData& data,
                            std::string_view uri,
                            std::string_view qualifier,
                            std::string_view owner_stable_id,
                            SemanticSymbolIdentity member,
                            std::string type_display = {}) {
    if (uri.empty() || qualifier.empty() || member.name.empty()) {
        return;
    }
    auto kind = completionMemberKind(member.kind);
    if (!kind.has_value()) {
        return;
    }
    member.kind = std::move(*kind);
    if (member.stable_id.empty()) {
        member.stable_id = std::string(owner_stable_id) + "|member|" + member.name;
    }

    auto& completions = data.member_completions_by_uri[std::string(uri)];
    const auto duplicate = std::any_of(completions.begin(),
                                       completions.end(),
                                       [&](const SnapshotMemberCompletion& existing) {
                                           return existing.qualifier == qualifier &&
                                                  existing.identity.name == member.name &&
                                                  existing.identity.kind == member.kind;
                                       });
    if (duplicate) {
        return;
    }
    completions.push_back(SnapshotMemberCompletion{.qualifier = std::string(qualifier),
                                                   .identity = std::move(member),
                                                   .type_display = std::move(type_display)});
}

void indexNestedTypedMemberCompletions(SnapshotData& data,
                                       const slang::SourceManager& source_manager,
                                       const slang::ast::Type& type,
                                       const SemanticLocation& owner_location,
                                       std::string_view owner_stable_id,
                                       std::string_view qualifier,
                                       int depth) {
    constexpr int kMaxNestedMemberDepth = 8;
    if (depth >= kMaxNestedMemberDepth) {
        return;
    }
    const auto& canonical = unwrapArrayElementType(type);
    const auto append_children = [&](const slang::ast::Symbol& member) {
        const auto* declared_type = member.getDeclaredType();
        if (declared_type == nullptr || member.name.empty()) {
            return;
        }
        const auto nested_qualifier = std::string(qualifier) + "." + std::string(member.name);
        const auto nested_owner_id = std::string(owner_stable_id) + "|member|" +
                                     std::string(member.name);
        auto nested_members = typedMembersForType(source_manager,
                                                  declared_type->getType(),
                                                  owner_location,
                                                  nested_owner_id);
        for (auto& nested : nested_members) {
            appendMemberCompletion(data,
                                   owner_location.uri,
                                   nested_qualifier,
                                   nested_owner_id,
                                   std::move(nested));
        }
        indexNestedTypedMemberCompletions(data,
                                          source_manager,
                                          declared_type->getType(),
                                          owner_location,
                                          nested_owner_id,
                                          nested_qualifier,
                                          depth + 1);
    };

    if (canonical.kind == slang::ast::SymbolKind::PackedStructType) {
        for (const auto& member : canonical.as<slang::ast::PackedStructType>().members()) {
            if (member.kind == slang::ast::SymbolKind::Field) {
                append_children(member);
            }
        }
    }
    else if (canonical.kind == slang::ast::SymbolKind::UnpackedStructType) {
        for (const auto* field : canonical.as<slang::ast::UnpackedStructType>().fields) {
            if (field != nullptr) {
                append_children(*field);
            }
        }
    }
    else if (canonical.kind == slang::ast::SymbolKind::ClassType) {
        for (const auto& property : canonical.as<slang::ast::ClassType>().properties()) {
            append_children(property);
        }
    }
}

void indexTypedMemberCompletions(SnapshotData& data,
                                 const slang::SourceManager& source_manager,
                                 const slang::ast::Symbol& symbol,
                                 const SemanticLocation& owner_location,
                                 std::string_view owner_stable_id) {
    if (symbol.name.empty()) {
        return;
    }
    const auto* declared_type = symbol.getDeclaredType();
    if (declared_type == nullptr) {
        return;
    }

    auto members = typedMembersForType(source_manager, declared_type->getType(), owner_location, owner_stable_id);
    if (members.empty()) {
        return;
    }

    for (auto& member : members) {
        appendMemberCompletion(data, owner_location.uri, symbol.name, owner_stable_id, std::move(member));
    }
    indexNestedTypedMemberCompletions(data,
                                      source_manager,
                                      declared_type->getType(),
                                      owner_location,
                                      owner_stable_id,
                                      symbol.name,
                                      0);
}

std::optional<ParseRange> indexedSymbolRange(const slang::SourceManager& source_manager,
                                             const SnapshotIndexedSymbol& symbol) {
    if (symbol.symbol != nullptr) {
        if (auto range = parseRangeForSymbolSyntax(source_manager, *symbol.symbol)) {
            return range;
        }
    }
    if (!symbol.identity.location.uri.empty()) {
        return symbol.identity.location.range;
    }
    return std::nullopt;
}

std::optional<ParseRange> modportRangeForInterfaceDefinition(const SnapshotData& data,
                                                            const slang::SourceManager& source_manager,
                                                            std::string_view uri,
                                                            const ParseRange& definition_range,
                                                            std::string_view modport_name) {
    if (modport_name.empty()) {
        return std::nullopt;
    }

    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (indexed.identity.name != modport_name || indexed.identity.kind != "Modport" ||
            indexed.identity.location.uri != uri ||
            !rangeContainsRange(definition_range, indexed.identity.location.range)) {
            continue;
        }
        if (auto range = indexedSymbolRange(source_manager, indexed)) {
            return range;
        }
        return indexed.identity.location.range;
    }
    return std::nullopt;
}

void appendIndexedMembersInRange(SnapshotData& data,
                                 const slang::SourceManager& source_manager,
                                 std::string_view target_uri,
                                 std::string_view qualifier,
                                 std::string_view owner_stable_id,
                                 std::string_view definition_uri,
                                 const ParseRange& member_range,
                                 const std::vector<ParseRange>& excluded_ranges = {}) {
    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (indexed.identity.location.uri != definition_uri ||
            indexed.identity.stable_id == owner_stable_id ||
            !rangeContainsRange(member_range, indexed.identity.location.range)) {
            continue;
        }
        const auto excluded = std::any_of(excluded_ranges.begin(),
                                          excluded_ranges.end(),
                                          [&](const ParseRange& excluded_range) {
                                              return rangeContainsRange(excluded_range,
                                                                        indexed.identity.location.range);
                                          });
        if (excluded) {
            continue;
        }
        auto member = indexed.identity;
        if (auto range = indexedSymbolRange(source_manager, indexed)) {
            member.location.range = *range;
        }
        appendMemberCompletion(data,
                               target_uri,
                               qualifier,
                               owner_stable_id,
                               std::move(member),
                               indexed.type_display);
    }
}

void appendScopeMembers(SnapshotData& data,
                        const slang::SourceManager& source_manager,
                        std::string_view target_uri,
                        std::string_view qualifier,
                        std::string_view owner_stable_id,
                        const slang::ast::Scope& scope) {
    for (const auto& member_symbol : scope.members()) {
        const auto location = declarationLocationForSymbol(source_manager, member_symbol);
        if (!location.has_value()) {
            continue;
        }
        const auto stable_id = symbolStableId(source_manager, member_symbol, *location);
        appendMemberCompletion(data,
                               target_uri,
                               qualifier,
                               owner_stable_id,
                               SemanticSymbolIdentity{.stable_id = stable_id,
                                                      .name = std::string(member_symbol.name),
                                                      .kind = symbolKindName(member_symbol.kind),
                                                      .location = *location},
                               typeDisplayForMemberCompletion(member_symbol));
    }
}

void indexInstanceMemberCompletions(SnapshotData& data, const slang::SourceManager& source_manager) {
    for (const auto& [stable_id, indexed] : data.symbols_by_id) {
        if (indexed.symbol == nullptr || indexed.identity.name.empty() ||
            indexed.identity.location.uri.empty()) {
            continue;
        }

        if (indexed.symbol->kind == slang::ast::SymbolKind::Instance) {
            const auto& instance = indexed.symbol->as<slang::ast::InstanceSymbol>();
            const auto& definition = instance.getDefinition();
            const auto definition_location = declarationLocationForSymbol(source_manager, definition);
            if (!definition_location.has_value()) {
                continue;
            }
            if (definition_location.has_value() &&
                instance.name == definition.name &&
                sameLocation(indexed.identity.location, *definition_location)) {
                continue;
            }
            if (definition_location->uri.empty()) {
                continue;
            }
            appendScopeMembers(data,
                               source_manager,
                               indexed.identity.location.uri,
                               indexed.identity.name,
                               stable_id,
                               instance.body);
            continue;
        }

        if (indexed.symbol->kind != slang::ast::SymbolKind::InterfacePort) {
            continue;
        }
        const auto& port = indexed.symbol->as<slang::ast::InterfacePortSymbol>();
        if (port.interfaceDef == nullptr) {
            continue;
        }
        const auto definition_location = declarationLocationForSymbol(source_manager, *port.interfaceDef);
        const auto definition_range = parseRangeForSymbolSyntax(source_manager, *port.interfaceDef);
        if (!definition_location.has_value() || !definition_range.has_value() ||
            definition_location->uri.empty()) {
            continue;
        }

        auto member_range = definition_range;
        if (!port.modport.empty()) {
            member_range = modportRangeForInterfaceDefinition(data,
                                                             source_manager,
                                                             definition_location->uri,
                                                             *definition_range,
                                                             port.modport);
        }
        if (!member_range.has_value()) {
            continue;
        }

        appendIndexedMembersInRange(data,
                                    source_manager,
                                    indexed.identity.location.uri,
                                    indexed.identity.name,
                                    stable_id,
                                    definition_location->uri,
                                    *member_range);
    }
}

int compareSourcePosition(int lhs_line, int lhs_character, int rhs_line, int rhs_character) {
    if (lhs_line != rhs_line) {
        return lhs_line < rhs_line ? -1 : 1;
    }
    if (lhs_character == rhs_character) {
        return 0;
    }
    return lhs_character < rhs_character ? -1 : 1;
}

bool macroIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool macroIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

std::string trimMacroArgument(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string expandMacroInvocation(const MacroDefinition& definition,
                                  const std::vector<std::string>& arguments) {
    if (!definition.function_like) {
        return definition.body;
    }
    if (definition.parameters.size() != arguments.size()) {
        return {};
    }

    std::unordered_map<std::string_view, std::string_view> arguments_by_parameter;
    for (size_t index = 0; index < definition.parameters.size(); ++index) {
        arguments_by_parameter.emplace(definition.parameters[index], arguments[index]);
    }

    std::string expanded;
    expanded.reserve(definition.body.size());
    for (size_t offset = 0; offset < definition.body.size();) {
        if (!macroIdentifierStart(definition.body[offset])) {
            expanded.push_back(definition.body[offset++]);
            continue;
        }
        size_t end = offset + 1;
        while (end < definition.body.size() && macroIdentifierContinue(definition.body[end])) {
            ++end;
        }
        const std::string_view token(definition.body.data() + offset, end - offset);
        if (const auto argument = arguments_by_parameter.find(token);
            argument != arguments_by_parameter.end()) {
            expanded.append(argument->second);
        }
        else {
            expanded.append(token);
        }
        offset = end;
    }
    return expanded;
}

const SnapshotVisibleMacro* visibleMacroDefinitionAt(const SnapshotData& data,
                                                     std::string_view uri,
                                                     std::string_view name,
                                                     const ParseRange& invocation_range) {
    const auto visible_it = data.visible_macros_by_uri.find(std::string(uri));
    if (visible_it == data.visible_macros_by_uri.end()) {
        return nullptr;
    }

    const SnapshotVisibleMacro* result = nullptr;
    for (const auto& macro : visible_it->second) {
        if (macro.definition.name != name ||
            compareSourcePosition(macro.available_after.start_line,
                                  macro.available_after.start_character,
                                  invocation_range.start_line,
                                  invocation_range.start_character) > 0) {
            continue;
        }
        if (macro.unavailable_after.has_value() &&
            compareSourcePosition(macro.unavailable_after->start_line,
                                  macro.unavailable_after->start_character,
                                  invocation_range.start_line,
                                  invocation_range.start_character) <= 0) {
            continue;
        }
        result = &macro;
    }
    return result;
}

std::optional<size_t> matchingMacroCloseParen(std::string_view text, size_t open_paren) {
    int depth = 0;
    for (size_t offset = open_paren; offset < text.size(); ++offset) {
        if (text[offset] == '(') {
            ++depth;
        }
        else if (text[offset] == ')' && --depth == 0) {
            return offset;
        }
    }
    return std::nullopt;
}

std::vector<std::string> splitMacroArguments(std::string_view text) {
    std::vector<std::string> result;
    int depth = 0;
    size_t start = 0;
    for (size_t offset = 0; offset <= text.size(); ++offset) {
        const bool at_end = offset == text.size();
        if (!at_end && text[offset] == '(') {
            ++depth;
        }
        else if (!at_end && text[offset] == ')') {
            --depth;
        }
        if (at_end || (text[offset] == ',' && depth == 0)) {
            result.push_back(trimMacroArgument(std::string(text.substr(start, offset - start))));
            start = offset + 1;
        }
    }
    if (result.size() == 1 && result.front().empty() && text.empty()) {
        result.clear();
    }
    return result;
}

std::string expandNestedMacroText(const SnapshotData& data,
                                  std::string_view uri,
                                  const ParseRange& invocation_range,
                                  std::string_view text,
                                  std::vector<std::string>& expansion_stack,
                                  int depth) {
    if (depth >= 16) {
        return std::string(text);
    }

    std::string result;
    for (size_t offset = 0; offset < text.size();) {
        if (text[offset] != '`' || offset + 1 >= text.size() ||
            !macroIdentifierStart(text[offset + 1])) {
            result.push_back(text[offset++]);
            continue;
        }

        size_t name_end = offset + 2;
        while (name_end < text.size() && macroIdentifierContinue(text[name_end])) {
            ++name_end;
        }
        const auto name = std::string(text.substr(offset + 1, name_end - offset - 1));
        const auto* definition = visibleMacroDefinitionAt(data, uri, name, invocation_range);
        if (definition == nullptr ||
            std::find(expansion_stack.begin(), expansion_stack.end(), name) != expansion_stack.end()) {
            result.append(text.substr(offset, name_end - offset));
            offset = name_end;
            continue;
        }

        size_t invocation_end = name_end;
        std::vector<std::string> arguments;
        if (definition->definition.function_like) {
            size_t open_paren = name_end;
            while (open_paren < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[open_paren])) != 0) {
                ++open_paren;
            }
            if (open_paren >= text.size() || text[open_paren] != '(') {
                result.append(text.substr(offset, name_end - offset));
                offset = name_end;
                continue;
            }
            const auto close_paren = matchingMacroCloseParen(text, open_paren);
            if (!close_paren.has_value()) {
                result.append(text.substr(offset));
                break;
            }
            arguments = splitMacroArguments(text.substr(open_paren + 1,
                                                        *close_paren - open_paren - 1));
            invocation_end = *close_paren + 1;
        }

        auto expansion = expandMacroInvocation(definition->definition, arguments);
        if (definition->definition.function_like && expansion.empty() &&
            definition->definition.parameters.size() != arguments.size()) {
            result.append(text.substr(offset, invocation_end - offset));
            offset = invocation_end;
            continue;
        }
        expansion_stack.push_back(name);
        result += expandNestedMacroText(data,
                                        uri,
                                        invocation_range,
                                        expansion,
                                        expansion_stack,
                                        depth + 1);
        expansion_stack.pop_back();
        offset = invocation_end;
    }
    return result;
}

void appendInactiveTokens(SnapshotData& data,
                          const slang::SourceManager& source_manager,
                          slang::BufferID buffer,
                          const slang::syntax::TokenList& tokens) {
    if (tokens.empty() || tokens.front().location().buffer() != buffer) {
        return;
    }
    const auto location = locationForSourceRange(
        source_manager,
        slang::SourceRange(tokens.front().location(), tokens.back().range().end()));
    if (!location.has_value() || location->uri.empty()) {
        return;
    }
    data.inactive_regions_by_uri[location->uri].push_back(
        SnapshotInactiveRegion{.location = *location,
                               .directive_id = location->uri + "|inactive|" +
                                                   std::to_string(location->range.start_line) + ":" +
                                                   std::to_string(location->range.start_character)});
}

void collectInactiveRegions(SnapshotData& data,
                            const slang::SourceManager& source_manager,
                            slang::BufferID buffer,
                            const slang::syntax::SyntaxNode& node);

void collectInactiveTrivia(SnapshotData& data,
                           const slang::SourceManager& source_manager,
                           slang::BufferID buffer,
                           std::span<const slang::parsing::Trivia> trivia_list) {
    for (const auto& trivia : trivia_list) {
        if (trivia.kind == slang::parsing::TriviaKind::Directive && trivia.syntax() != nullptr) {
            collectInactiveRegions(data, source_manager, buffer, *trivia.syntax());
        }
        if (trivia.kind == slang::parsing::TriviaKind::SkippedTokens) {
            for (const auto& skipped : trivia.getSkippedTokens()) {
                collectInactiveTrivia(data, source_manager, buffer, skipped.trivia());
            }
            continue;
        }

        const auto* syntax = trivia.syntax();
        if (syntax == nullptr || syntax->getFirstToken().location().buffer() != buffer) {
            continue;
        }
        if (slang::syntax::ConditionalBranchDirectiveSyntax::isKind(syntax->kind)) {
            appendInactiveTokens(data,
                                 source_manager,
                                 buffer,
                                 syntax->as<slang::syntax::ConditionalBranchDirectiveSyntax>().disabledTokens);
        }
        else if (slang::syntax::UnconditionalBranchDirectiveSyntax::isKind(syntax->kind)) {
            appendInactiveTokens(data,
                                 source_manager,
                                 buffer,
                                 syntax->as<slang::syntax::UnconditionalBranchDirectiveSyntax>().disabledTokens);
        }
    }
}

void collectInactiveRegions(SnapshotData& data,
                            const slang::SourceManager& source_manager,
                            slang::BufferID buffer,
                            const slang::syntax::SyntaxNode& node) {
    for (size_t index = 0; index < node.getChildCount(); ++index) {
        if (const auto* child = node.childNode(index)) {
            collectInactiveRegions(data, source_manager, buffer, *child);
            continue;
        }
        auto* token = const_cast<slang::syntax::SyntaxNode&>(node).childTokenPtr(index);
        if (token != nullptr) {
            collectInactiveTrivia(data, source_manager, buffer, token->trivia());
        }
    }
}

void buildInactiveRegionIndex(SnapshotData& data) {
    data.inactive_regions_by_uri.clear();
    data.inactive_region_count = 0;
    const auto started_at = std::chrono::steady_clock::now();
    if (!data.source_manager) {
        return;
    }
    for (const auto& tree : data.syntax_trees) {
        if (tree == nullptr || tree->getSourceBufferIds().empty()) {
            continue;
        }
        collectInactiveRegions(data,
                               *data.source_manager,
                               tree->getSourceBufferIds().front(),
                               tree->root());
    }
    for (auto& [_, regions] : data.inactive_regions_by_uri) {
        std::sort(regions.begin(), regions.end(), [](const auto& lhs, const auto& rhs) {
            return locationLess(lhs.location, rhs.location);
        });
        std::vector<SnapshotInactiveRegion> merged;
        for (const auto& region : regions) {
            if (!merged.empty() && rangesOverlapOrTouch(merged.back().location.range, region.location.range)) {
                auto& prior = merged.back().location.range;
                if (region.location.range.end_line > prior.end_line ||
                    (region.location.range.end_line == prior.end_line &&
                     region.location.range.end_character > prior.end_character)) {
                    prior.end_line = region.location.range.end_line;
                    prior.end_character = region.location.range.end_character;
                }
                continue;
            }
            merged.push_back(region);
        }
        data.inactive_region_count += merged.size();
        regions = std::move(merged);
    }
    data.inactive_region_build_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started_at)
                                               .count();
}

bool isInactiveMacroRange(const SnapshotData& data, std::string_view uri, const ParseRange& range) {
    const auto regions = data.inactive_regions_by_uri.find(std::string(uri));
    if (regions == data.inactive_regions_by_uri.end()) {
        return false;
    }
    return std::any_of(regions->second.begin(), regions->second.end(), [&](const auto& region) {
        return rangeContainsRange(region.location.range, range);
    });
}

void indexMacroUsage(SnapshotData& data,
                     const slang::SourceManager& source_manager,
                     const slang::syntax::MacroUsageSyntax& usage) {
    const auto selection_location = locationForSourceRange(source_manager, usage.directive.range());
    const auto invocation_location = locationForSourceRange(source_manager, usage.sourceRange());
    if (!selection_location.has_value() || !invocation_location.has_value() ||
        selection_location->uri.empty() || selection_location->uri != invocation_location->uri) {
        return;
    }

    auto name = std::string(usage.directive.valueText());
    if (!name.empty() && name.front() == '`') {
        name.erase(name.begin());
    }
    if (name.empty()) {
        return;
    }

    MacroInvocationFact fact;
    fact.name = std::move(name);
    fact.range = invocation_location->range;
    fact.selection_range = selection_location->range;
    if (isInactiveMacroRange(data, selection_location->uri, fact.range)) {
        return;
    }
    if (usage.args != nullptr) {
        fact.function_like = true;
        for (const auto* argument : usage.args->args) {
            if (argument == nullptr) {
                continue;
            }
            fact.arguments.push_back(trimMacroArgument(argument->toString()));
            if (const auto location = locationForSourceRange(source_manager, argument->sourceRange());
                location.has_value() && location->uri == selection_location->uri) {
                fact.argument_ranges.push_back(location->range);
            }
        }
    }

    if (const auto* definition = visibleMacroDefinitionAt(data,
                                                          selection_location->uri,
                                                          fact.name,
                                                          fact.range)) {
        fact.definition = definition->definition;
        fact.definition_uri = definition->source_uri;
        fact.resolved = !fact.definition.function_like ||
                        fact.definition.parameters.size() == fact.arguments.size();
        if (fact.resolved) {
            fact.expansion_text = expandMacroInvocation(fact.definition, fact.arguments);
            std::vector<std::string> expansion_stack{fact.name};
            fact.expansion_text = expandNestedMacroText(data,
                                                        selection_location->uri,
                                                        fact.range,
                                                        fact.expansion_text,
                                                        expansion_stack,
                                                        0);
        }
    }
    data.macro_invocations_by_uri[selection_location->uri].push_back(std::move(fact));
}

void collectMacroUsages(SnapshotData& data,
                        const slang::SourceManager& source_manager,
                        const slang::syntax::SyntaxNode& node) {
    if (node.kind == slang::syntax::SyntaxKind::MacroUsage) {
        indexMacroUsage(data, source_manager, node.as<slang::syntax::MacroUsageSyntax>());
    }
    for (size_t index = 0; index < node.getChildCount(); ++index) {
        if (const auto* child = node.childNode(index)) {
            collectMacroUsages(data, source_manager, *child);
            continue;
        }
        auto* token = const_cast<slang::syntax::SyntaxNode&>(node).childTokenPtr(index);
        if (token == nullptr) {
            continue;
        }
        for (const auto& trivia : token->trivia()) {
            if (trivia.kind == slang::parsing::TriviaKind::Directive && trivia.syntax() != nullptr) {
                collectMacroUsages(data, source_manager, *trivia.syntax());
            }
            else if (trivia.kind == slang::parsing::TriviaKind::SkippedTokens) {
                for (const auto& skipped : trivia.getSkippedTokens()) {
                    for (const auto& skipped_trivia : skipped.trivia()) {
                        if (skipped_trivia.syntax() != nullptr) {
                            collectMacroUsages(data, source_manager, *skipped_trivia.syntax());
                        }
                    }
                }
            }
        }
    }
}

void buildMacroInvocationIndex(SnapshotData& data) {
    data.macro_invocations_by_uri.clear();
    for (const auto& tree : data.syntax_trees) {
        if (tree != nullptr) {
            collectMacroUsages(data, *data.source_manager, tree->root());
        }
    }
    for (auto& [_, invocations] : data.macro_invocations_by_uri) {
        std::sort(invocations.begin(), invocations.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameRange(lhs.range, rhs.range)) {
                return locationLess(SemanticLocation{.uri = {}, .range = lhs.range},
                                    SemanticLocation{.uri = {}, .range = rhs.range});
            }
            return lhs.name < rhs.name;
        });
        invocations.erase(std::unique(invocations.begin(), invocations.end(), [](const auto& lhs, const auto& rhs) {
                              return lhs.name == rhs.name && sameRange(lhs.range, rhs.range);
                          }),
                          invocations.end());
    }
}

std::string argumentSignatureLabel(const slang::ast::FormalArgumentSymbol& argument) {
    std::string label = directionName(argument.direction);
    const auto type_display = normalizedTypeDisplay(argument.getType().toString());
    if (!type_display.empty()) {
        if (!label.empty()) {
            label += " ";
        }
        label += type_display;
    }
    if (!argument.name.empty()) {
        if (!label.empty()) {
            label += " ";
        }
        label += std::string(argument.name);
    }
    return label;
}

std::optional<CallableInvocationFact> callableInvocationForSubroutine(
    const SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::CallExpression& call,
    const slang::ast::SubroutineSymbol& subroutine) {
    auto call_range = call.sourceRange;
    if (call.thisClass() != nullptr) {
        for (const auto* argument : call.arguments()) {
            if (argument != nullptr &&
                argument->sourceRange.end().buffer() == call_range.end().buffer() &&
                argument->sourceRange.end() > call_range.end()) {
                call_range = slang::SourceRange(call_range.start(), argument->sourceRange.end());
            }
        }
        call_range = slang::SourceRange(call_range.start(), call_range.end() + 1);
    }

    const auto call_location = locationForSourceRange(source_manager, call_range);
    const auto selection_start = call.thisClass() == nullptr
                                     ? call.sourceRange.start()
                                     : call.sourceRange.end() - subroutine.name.size();
    const auto selection_end = selection_start + subroutine.name.size();
    const auto selection_location = locationForSourceRange(
        source_manager,
        slang::SourceRange(selection_start, selection_end));
    if (!call_location.has_value() || !selection_location.has_value() ||
        selection_location->uri != call_location->uri) {
        return std::nullopt;
    }

    CallableInvocationFact result;
    if (const auto id_it = data.ids_by_symbol.find(&subroutine);
        id_it != data.ids_by_symbol.end()) {
        result.target_stable_id = id_it->second;
    }
    if (call.thisClass() != nullptr) {
        result.receiver_type = normalizedTypeDisplay(call.thisClass()->type->toString());
    }
    result.name = std::string(subroutine.name);
    result.kind = std::string(slang::ast::SemanticFacts::getSubroutineKindStr(subroutine.subroutineKind));
    result.return_type = normalizedTypeDisplay(subroutine.getReturnType().toString());
    result.range = call_location->range;
    result.selection_range = selection_location->range;
    for (const auto* argument : subroutine.getArguments()) {
        if (argument == nullptr) {
            continue;
        }
        auto label = argumentSignatureLabel(*argument);
        if (label.empty()) {
            label = std::string(argument->name);
        }
        result.parameters.push_back(std::move(label));
    }
    for (const auto* argument : call.arguments()) {
        if (argument == nullptr) {
            continue;
        }
        if (const auto location = locationForSourceRange(source_manager, argument->sourceRange);
            location.has_value() && location->uri == call_location->uri) {
            result.argument_ranges.push_back(location->range);
        }
    }
    return result;
}

void addSignatureCall(SnapshotData& data,
                      const slang::SourceManager& source_manager,
                      const slang::ast::CallExpression& call) {
    if (call.isSystemCall()) {
        return;
    }
    const auto* subroutine = std::get_if<const slang::ast::SubroutineSymbol*>(&call.subroutine);
    if (subroutine == nullptr || *subroutine == nullptr || (*subroutine)->name.empty()) {
        return;
    }
    auto signature_call = callableInvocationForSubroutine(data, source_manager, call, **subroutine);
    if (!signature_call.has_value()) {
        return;
    }
    const auto call_location = locationForSourceRange(source_manager, call.sourceRange);
    if (!call_location.has_value() || call_location->uri.empty()) {
        return;
    }
    data.selection_ranges_by_uri[call_location->uri].push_back(signature_call->range);
    data.selection_ranges_by_uri[call_location->uri].push_back(signature_call->selection_range);
    data.callable_invocations_by_uri[call_location->uri].push_back(std::move(*signature_call));
}

std::optional<ModuleDefinition> moduleDefinitionForAstBody(const slang::SourceManager& source_manager,
                                                           const slang::ast::InstanceBodySymbol& body) {
    const auto& definition = body.getDefinition();
    const auto location = declarationLocationForSymbol(source_manager, definition);
    const auto range = parseRangeForSymbolSyntax(source_manager, definition);
    if (!location.has_value() || !range.has_value()) {
        return std::nullopt;
    }
    const auto body_range = parseRangeForSymbolSyntax(source_manager, body).value_or(*range);

    ModuleDefinition module;
    module.name = std::string(definition.name);
    module.kind = std::string(definition.getKindString());
    module.range = body_range;
    module.selection_range = location->range;
    for (const auto* port_symbol : body.getPortList()) {
        if (port_symbol == nullptr || port_symbol->name.empty()) {
            continue;
        }
        module.ports.push_back(std::string(port_symbol->name));
        if (auto port = schematicPortForAstPort(source_manager, *port_symbol)) {
            module.port_details.push_back(std::move(*port));
        }
    }
    for (const auto* parameter : body.getParameters()) {
        if (parameter == nullptr || !parameter->isPortParam()) {
            continue;
        }
        if (auto port = schematicPortForAstParameter(source_manager, *parameter)) {
            module.parameter_details.push_back(std::move(*port));
        }
    }
    return module;
}

std::optional<SchematicCell> schematicCellForAstInstance(const slang::SourceManager& source_manager,
                                                         const slang::ast::InstanceSymbol& instance) {
    const auto location = declarationLocationForSymbol(source_manager, instance);
    const auto range = parseRangeForSymbolSyntax(source_manager, instance);
    if (!location.has_value() || !range.has_value()) {
        return std::nullopt;
    }

    SchematicCell cell;
    cell.id = std::string(instance.name);
    cell.name = std::string(instance.name);
    cell.type = std::string(instance.getDefinition().name);
    cell.kind = instance.isInterface() ? "interface" : "module";
    cell.range = *range;
    cell.selection_range = location->range;
    return cell;
}

std::optional<SchematicCell> schematicCellForAstPrimitiveInstance(
    const slang::SourceManager& source_manager,
    const slang::ast::PrimitiveInstanceSymbol& instance,
    const SemanticEngineDocument* document) {
    const auto location = declarationLocationForSymbol(source_manager, instance);
    const auto range = parseRangeForSymbolSyntax(source_manager, instance);
    if (!location.has_value() || !range.has_value()) {
        return std::nullopt;
    }

    SchematicCell cell;
    cell.id = std::string(instance.name);
    cell.name = std::string(instance.name);
    cell.type = std::string(instance.primitiveType.name);
    cell.kind = std::string(instance.primitiveType.name);
    cell.range = *range;
    cell.selection_range = location->range;
    if (document != nullptr) {
        if (const auto statement_range = instanceStatementRange(document->text,
                                                                location->range,
                                                                cell.type)) {
            cell.range = *statement_range;
        }
        else if (const auto type_range = identifierRangeByName(document->text, *range, cell.type)) {
            cell.range = ParseRange{.start_line = type_range->start_line,
                                    .start_character = type_range->start_character,
                                    .end_line = range->end_line,
                                    .end_character = range->end_character};
        }
    }

    const auto connections = instance.getPortConnections();
    for (size_t index = 0; index < connections.size(); ++index) {
        const auto* expression = connections[index];
        if (expression == nullptr) {
            continue;
        }

        ParseRange connection_range = location->range;
        std::string signal;
        if (expression->sourceRange.start().valid()) {
            connection_range = sourceRangeForSourceRange(source_manager, expression->sourceRange);
            if (document != nullptr) {
                if (auto text = textForRange(document->text, connection_range)) {
                    signal = std::move(*text);
                }
            }
        }
        if (signal.empty()) {
            signal = std::string("P") + std::to_string(index);
        }

        std::string port_name = std::string("P") + std::to_string(index);
        if (index < instance.primitiveType.ports.size()) {
            if (!instance.primitiveType.ports[index]->name.empty()) {
                port_name = std::string(instance.primitiveType.ports[index]->name);
            }
            else if (index == 0) {
                port_name = "Y";
            }
            else if (index == 1) {
                port_name = "A";
            }
            else if (index == 2) {
                port_name = "B";
            }
            else {
                port_name = std::string("I") + std::to_string(index - 1);
            }
        }
        else if (index == 0) {
            port_name = "Y";
        }
        else if (index == 1) {
            port_name = "A";
        }
        else if (index == 2) {
            port_name = "B";
        }
        else {
            port_name = std::string("I") + std::to_string(index - 1);
        }

        cell.connections.push_back(SchematicConnection{.port_name = std::move(port_name),
                                                       .port_index = static_cast<int>(index),
                                                       .signal = std::move(signal),
                                                       .range = connection_range});
    }
    return cell;
}

void upsertAstModuleSignature(SnapshotData& data,
                              const slang::SourceManager& source_manager,
                              const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                              const slang::ast::InstanceBodySymbol& body) {
    auto definition = moduleDefinitionForAstBody(source_manager, body);
    if (!definition.has_value()) {
        return;
    }

    ModuleSchematic schematic;
    schematic.name = definition->name;
    schematic.range = definition->range;
    schematic.selection_range = definition->selection_range;
    schematic.ports = definition->port_details;
    for (const auto& member : body.members()) {
        if (member.kind != slang::ast::SymbolKind::Instance) {
            continue;
        }
        if (auto cell = schematicCellForAstInstance(source_manager,
                                                   member.as<slang::ast::InstanceSymbol>())) {
            definition->instances.push_back(ModuleInstantiation{.module_name = cell->type,
                                                                .instance_name = cell->name,
                                                                .range = cell->range,
                                                                .selection_range = cell->selection_range,
                                                                .module_selection_range = cell->range});
            auto existing_cell = std::find_if(schematic.cells.begin(),
                                              schematic.cells.end(),
                                              [&](const SchematicCell& candidate) {
                                                  return candidate.name == cell->name &&
                                                         candidate.kind == cell->kind;
                                              });
            if (existing_cell == schematic.cells.end()) {
                schematic.cells.push_back(std::move(*cell));
            }
            else {
                *existing_cell = std::move(*cell);
            }
        }
    }
    for (const auto& member : body.members()) {
        if (member.kind != slang::ast::SymbolKind::PrimitiveInstance) {
            continue;
        }
        const auto location = declarationLocationForSymbol(source_manager, member);
        const auto document_it = location.has_value() ? documents.find(location->uri) : documents.end();
        const auto* document = document_it == documents.end() ? nullptr : &document_it->second;
        if (auto cell = schematicCellForAstPrimitiveInstance(source_manager,
                                                            member.as<slang::ast::PrimitiveInstanceSymbol>(),
                                                            document)) {
            schematic.cells.push_back(std::move(*cell));
        }
    }

    const auto uri = definition->selection_range.start_line >= 0
                         ? declarationLocationForSymbol(source_manager, body.getDefinition())->uri
                         : std::string{};
    const auto name = definition->name;
    SemanticModuleSignature signature{.definition = *definition,
                                      .schematic = std::move(schematic),
                                      .uri = uri};

    data.modules_by_name[name] = signature.definition;
    if (!uri.empty()) {
        data.module_uris_by_name[name] = uri;
    }
    const auto entry = SnapshotModuleEntry{.uri = uri, .definition = signature.definition};
    const auto entry_it = std::find_if(data.module_entries.begin(),
                                       data.module_entries.end(),
                                       [&](const SnapshotModuleEntry& existing) {
                                           return existing.definition.name == name;
                                       });
    if (entry_it == data.module_entries.end()) {
        data.module_entries.push_back(entry);
    }
    else {
        *entry_it = entry;
    }
    data.ast_module_signatures_by_name[name] = std::move(signature);
}

bool moduleNameReferencedByIndexedInstance(const SnapshotData& data, std::string_view name) {
    return std::any_of(data.module_instances_by_uri.begin(),
                       data.module_instances_by_uri.end(),
                       [&](const auto& entry) {
                           return std::any_of(entry.second.begin(),
                                              entry.second.end(),
                                              [&](const SnapshotModuleInstance& instance) {
                                                  return instance.module_name == name;
                                              });
                       });
}

std::string tokenDirection(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
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
            return normalizedTypeDisplay(declaration.dataType->toString());
        }
        case slang::syntax::SyntaxKind::NetPortHeader: {
            const auto& declaration = header.as<slang::syntax::NetPortHeaderSyntax>();
            return normalizedTypeDisplay(declaration.dataType->toString());
        }
        default:
            return {};
    }
}

std::string portDeclaratorWidthText(const slang::syntax::PortHeaderSyntax& header,
                                    const slang::syntax::DeclaratorSyntax& declarator) {
    auto width_text = portHeaderWidthText(header);
    for (const auto* dimension : declarator.dimensions) {
        if (dimension == nullptr) {
            continue;
        }
        auto dimension_text = normalizedTypeDisplay(dimension->toString());
        if (dimension_text.empty()) {
            continue;
        }
        if (!width_text.empty()) {
            width_text += " ";
        }
        width_text += std::move(dimension_text);
    }
    return width_text;
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
                           const slang::syntax::PortHeaderSyntax& header,
                           const slang::syntax::SeparatedSyntaxList<slang::syntax::DeclaratorSyntax>& declarators) {
    const auto direction = portHeaderDirection(header);
    for (const auto* declarator : declarators) {
        if (declarator == nullptr || declarator->name.valueText().empty()) {
            continue;
        }
        const auto width_text = portDeclaratorWidthText(header, *declarator);
        appendOrUpdatePort(result,
                           SchematicPort{.name = std::string(declarator->name.valueText()),
                                         .direction = direction,
                                         .width_text = width_text,
                                         .range = sourceRangeForSourceRange(source_manager,
                                                                           declarator->sourceRange()),
                                         .selection_range = sourceRangeForSourceRange(source_manager,
                                                                                      declarator->name.range())});
    }
}

std::vector<SchematicPort> headerSchematicPortsForDefinition(
    const slang::SourceManager& source_manager,
    const slang::syntax::ModuleDeclarationSyntax& declaration) {
    std::vector<SchematicPort> result;
    if (declaration.header == nullptr || declaration.header->ports == nullptr) {
        return result;
    }
    if (declaration.header->ports->kind == slang::syntax::SyntaxKind::AnsiPortList) {
        const auto& ports = declaration.header->ports->as<slang::syntax::AnsiPortListSyntax>();
        for (const auto* port : ports.ports) {
            if (port == nullptr) {
                continue;
            }
            switch (port->kind) {
                case slang::syntax::SyntaxKind::ImplicitAnsiPort: {
                    const auto& ansi_port = port->as<slang::syntax::ImplicitAnsiPortSyntax>();
                    if (ansi_port.declarator == nullptr || ansi_port.header == nullptr ||
                        ansi_port.declarator->name.valueText().empty()) {
                        break;
                    }
                    appendOrUpdatePort(result,
                                       SchematicPort{
                                           .name = std::string(ansi_port.declarator->name.valueText()),
                                           .direction = portHeaderDirection(*ansi_port.header),
                                           .width_text = portDeclaratorWidthText(*ansi_port.header,
                                                                                 *ansi_port.declarator),
                                           .range = sourceRangeForSourceRange(source_manager,
                                                                             ansi_port.sourceRange()),
                                           .selection_range = sourceRangeForSourceRange(
                                               source_manager,
                                               ansi_port.declarator->name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::ExplicitAnsiPort: {
                    const auto& ansi_port = port->as<slang::syntax::ExplicitAnsiPortSyntax>();
                    if (ansi_port.name.valueText().empty()) {
                        break;
                    }
                    appendOrUpdatePort(result,
                                       SchematicPort{.name = std::string(ansi_port.name.valueText()),
                                                     .direction = tokenDirection(ansi_port.direction.valueText()),
                                                     .width_text = {},
                                                     .range = sourceRangeForSourceRange(source_manager,
                                                                                       ansi_port.sourceRange()),
                                                     .selection_range = sourceRangeForSourceRange(
                                                         source_manager,
                                                         ansi_port.name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::PortDeclaration: {
                    const auto& port_declaration = port->as<slang::syntax::PortDeclarationSyntax>();
                    if (port_declaration.header != nullptr) {
                        appendPortDeclarators(result,
                                              source_manager,
                                              *port_declaration.header,
                                              port_declaration.declarators);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    else if (declaration.header->ports->kind == slang::syntax::SyntaxKind::NonAnsiPortList) {
        const auto& ports = declaration.header->ports->as<slang::syntax::NonAnsiPortListSyntax>();
        for (const auto* port : ports.ports) {
            if (port == nullptr) {
                continue;
            }
            switch (port->kind) {
                case slang::syntax::SyntaxKind::ExplicitNonAnsiPort: {
                    const auto& non_ansi_port = port->as<slang::syntax::ExplicitNonAnsiPortSyntax>();
                    if (non_ansi_port.name.valueText().empty()) {
                        break;
                    }
                    appendOrUpdatePort(result,
                                       SchematicPort{.name = std::string(non_ansi_port.name.valueText()),
                                                     .direction = "inout",
                                                     .width_text = {},
                                                     .range = sourceRangeForSourceRange(source_manager,
                                                                                       non_ansi_port.sourceRange()),
                                                     .selection_range = sourceRangeForSourceRange(
                                                         source_manager,
                                                         non_ansi_port.name.range())});
                    break;
                }
                case slang::syntax::SyntaxKind::ImplicitNonAnsiPort: {
                    const auto& non_ansi_port = port->as<slang::syntax::ImplicitNonAnsiPortSyntax>();
                    if (non_ansi_port.expr == nullptr) {
                        break;
                    }
                    auto name = normalizedTypeDisplay(non_ansi_port.expr->toString());
                    if (name.empty()) {
                        break;
                    }
                    appendOrUpdatePort(result,
                                       SchematicPort{.name = std::move(name),
                                                     .direction = "inout",
                                                     .width_text = {},
                                                     .range = sourceRangeForSourceRange(source_manager,
                                                                                       non_ansi_port.sourceRange()),
                                                     .selection_range = sourceRangeForSourceRange(
                                                         source_manager,
                                                         non_ansi_port.expr->sourceRange())});
                    break;
                }
                default:
                    break;
            }
        }
    }
    return result;
}

std::optional<SchematicPort> parameterPortForDefinition(
    const slang::SourceManager& source_manager,
    const slang::ast::DefinitionSymbol::ParameterDecl& parameter) {
    if (!parameter.isPortParam || parameter.name.empty() || !parameter.location.valid()) {
        return std::nullopt;
    }
    ParseRange range = sourceRangeForSourceRange(
        source_manager,
        slang::SourceRange(parameter.location, parameter.location + parameter.name.size()));
    if (parameter.hasSyntax) {
        if (parameter.isTypeParam && parameter.typeDecl != nullptr) {
            range = sourceRangeForSourceRange(source_manager, parameter.typeDecl->sourceRange());
        }
        else if (!parameter.isTypeParam && parameter.valueDecl != nullptr) {
            range = sourceRangeForSourceRange(source_manager, parameter.valueDecl->sourceRange());
        }
    }
    return SchematicPort{.name = std::string(parameter.name),
                         .direction = "parameter",
                         .width_text = parameter.isTypeParam ? "type" : "",
                         .range = range,
                         .selection_range = sourceRangeForSourceRange(
                             source_manager,
                             slang::SourceRange(parameter.location,
                                                parameter.location + parameter.name.size()))};
}

std::optional<SemanticModuleSignature> moduleSignatureSkeletonForAstDefinition(
    const slang::SourceManager& source_manager,
    const slang::ast::DefinitionSymbol& definition) {
    const auto* syntax = definition.getSyntax();
    if (syntax == nullptr ||
        (syntax->kind != slang::syntax::SyntaxKind::ModuleDeclaration &&
         syntax->kind != slang::syntax::SyntaxKind::InterfaceDeclaration)) {
        return std::nullopt;
    }
    const auto& declaration = syntax->as<slang::syntax::ModuleDeclarationSyntax>();
    if (declaration.header == nullptr || declaration.header->name.valueText().empty()) {
        return std::nullopt;
    }
    const auto location = locationForSourceRange(source_manager, declaration.header->name.range());
    const auto range_location = locationForSourceRange(source_manager, declaration.sourceRange());
    if (!location.has_value() || !range_location.has_value()) {
        return std::nullopt;
    }

    ModuleDefinition module;
    module.name = std::string(declaration.header->name.valueText());
    module.kind = std::string(definition.getKindString());
    module.range = range_location->range;
    module.selection_range = location->range;
    module.port_details = headerSchematicPortsForDefinition(source_manager, declaration);
    for (const auto& port : module.port_details) {
        module.ports.push_back(port.name);
    }
    for (const auto& parameter : definition.parameters) {
        if (auto port = parameterPortForDefinition(source_manager, parameter)) {
            module.parameter_details.push_back(std::move(*port));
        }
    }

    ModuleSchematic schematic;
    schematic.name = module.name;
    schematic.range = module.range;
    schematic.selection_range = module.selection_range;
    schematic.ports = module.port_details;
    return SemanticModuleSignature{.definition = std::move(module),
                                   .schematic = std::move(schematic),
                                   .uri = range_location->uri};
}

void upsertAstModuleSignatureSkeleton(SnapshotData& data,
                                      const slang::SourceManager& source_manager,
                                      const slang::ast::DefinitionSymbol& definition) {
    auto signature = moduleSignatureSkeletonForAstDefinition(source_manager, definition);
    if (!signature.has_value()) {
        return;
    }

    const auto name = signature->definition.name;
    data.modules_by_name[name] = signature->definition;
    if (!signature->uri.empty()) {
        data.module_uris_by_name[name] = signature->uri;
    }

    const auto entry = SnapshotModuleEntry{.uri = signature->uri,
                                          .definition = signature->definition};
    const auto entry_it = std::find_if(data.module_entries.begin(),
                                       data.module_entries.end(),
                                       [&](const SnapshotModuleEntry& existing) {
                                           return existing.definition.name == name;
                                       });
    if (entry_it == data.module_entries.end()) {
        data.module_entries.push_back(entry);
    }
    else {
        *entry_it = entry;
    }

    data.ast_module_signatures_by_name[name] = std::move(*signature);
}

void upsertMissingAstModuleSignatureSkeletonsFromDefinitions(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const std::unordered_map<std::string, SemanticEngineDocument>&) {
    if (!data.compilation) {
        return;
    }
    for (const auto* symbol : data.compilation->getDefinitions()) {
        if (symbol == nullptr || symbol->name.empty() ||
            symbol->kind != slang::ast::SymbolKind::Definition) {
            continue;
        }
        const auto& definition = symbol->as<slang::ast::DefinitionSymbol>();
        const auto name = std::string(definition.name);
        if (data.ast_module_signatures_by_name.contains(name) ||
            !moduleNameReferencedByIndexedInstance(data, name)) {
            continue;
        }
        upsertAstModuleSignatureSkeleton(data, source_manager, definition);
    }
}

void insertSymbol(SnapshotData& data,
                  const slang::SourceManager& source_manager,
                  const slang::ast::Symbol& symbol);

void insertReference(SnapshotData& data,
                     std::string stable_id,
                     std::string name,
                     SemanticLocation location,
                     SemanticReferenceRole role);

std::vector<std::string> directSymbolIdsForExpression(SnapshotData& data,
                                                       const slang::SourceManager&,
                                                       const slang::ast::Expression& expression) {
    std::vector<std::string> ids;
    const auto append_symbol = [&](const slang::ast::Symbol& symbol) {
        if (const auto found = data.ids_by_symbol.find(&symbol); found != data.ids_by_symbol.end()) {
            ids.push_back(found->second);
        }
    };
    const auto collect = [&](const auto& self, const slang::ast::Expression& current) -> void {
        switch (current.kind) {
        case slang::ast::ExpressionKind::NamedValue:
        case slang::ast::ExpressionKind::HierarchicalValue:
            append_symbol(current.as<slang::ast::ValueExpressionBase>().symbol);
            return;
        case slang::ast::ExpressionKind::ElementSelect: {
            const auto& select = current.as<slang::ast::ElementSelectExpression>();
            self(self, select.value());
            self(self, select.selector());
            return;
        }
        case slang::ast::ExpressionKind::RangeSelect: {
            const auto& select = current.as<slang::ast::RangeSelectExpression>();
            self(self, select.value());
            self(self, select.left());
            self(self, select.right());
            return;
        }
        case slang::ast::ExpressionKind::MemberAccess: {
            const auto& member = current.as<slang::ast::MemberAccessExpression>();
            append_symbol(member.member);
            self(self, member.value());
            return;
        }
        case slang::ast::ExpressionKind::Concatenation:
            for (const auto* operand : current.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand != nullptr) self(self, *operand);
            }
            return;
        case slang::ast::ExpressionKind::BinaryOp: {
            const auto& binary = current.as<slang::ast::BinaryExpression>();
            self(self, binary.left());
            self(self, binary.right());
            return;
        }
        case slang::ast::ExpressionKind::ConditionalOp: {
            const auto& conditional = current.as<slang::ast::ConditionalExpression>();
            for (const auto& condition : conditional.conditions) self(self, *condition.expr);
            self(self, conditional.left());
            self(self, conditional.right());
            return;
        }
        default:
            current.visitSymbolReferences([&](const slang::ast::Expression&, const slang::ast::Symbol& symbol) {
                append_symbol(symbol);
            });
            return;
        }
    };
    collect(collect, expression);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Cone data edges use the selected value, while dynamic selectors are control
// dependencies. Keep the selector out of this traversal so it cannot become a
// spurious data driver for data[index] or data[base +: width].
std::vector<std::string> dataSymbolIdsForExpression(SnapshotData& data,
                                                     const slang::ast::Expression& expression) {
    std::vector<std::string> ids;
    const auto append_symbol = [&](const slang::ast::Symbol& symbol) {
        if (const auto found = data.ids_by_symbol.find(&symbol); found != data.ids_by_symbol.end()) {
            ids.push_back(found->second);
        }
    };
    const auto collect = [&](const auto& self, const slang::ast::Expression& current) -> void {
        const auto& unwrapped = unwrapImplicitConversions(current);
        switch (unwrapped.kind) {
        case slang::ast::ExpressionKind::NamedValue:
        case slang::ast::ExpressionKind::HierarchicalValue:
            append_symbol(unwrapped.as<slang::ast::ValueExpressionBase>().symbol);
            return;
        case slang::ast::ExpressionKind::ElementSelect:
            self(self, unwrapped.as<slang::ast::ElementSelectExpression>().value());
            return;
        case slang::ast::ExpressionKind::RangeSelect:
            self(self, unwrapped.as<slang::ast::RangeSelectExpression>().value());
            return;
        case slang::ast::ExpressionKind::MemberAccess: {
            const auto& member = unwrapped.as<slang::ast::MemberAccessExpression>();
            append_symbol(member.member);
            self(self, member.value());
            return;
        }
        case slang::ast::ExpressionKind::Concatenation:
            for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand != nullptr) self(self, *operand);
            }
            return;
        case slang::ast::ExpressionKind::BinaryOp: {
            const auto& binary = unwrapped.as<slang::ast::BinaryExpression>();
            self(self, binary.left());
            self(self, binary.right());
            return;
        }
        default:
            unwrapped.visitSymbolReferences([&](const slang::ast::Expression&, const slang::ast::Symbol& symbol) {
                append_symbol(symbol);
            });
            return;
        }
    };
    collect(collect, expression);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

const slang::ast::Type* endpointTypeForSymbol(const slang::ast::Symbol& symbol) {
    if (const auto* declared_type = symbol.getDeclaredType()) {
        return &declared_type->getType();
    }
    if (symbol.kind == slang::ast::SymbolKind::Port) {
        return &symbol.as<slang::ast::PortSymbol>().getType();
    }
    if (symbol.kind == slang::ast::SymbolKind::MultiPort) {
        return &symbol.as<slang::ast::MultiPortSymbol>().getType();
    }
    return nullptr;
}

SnapshotConeSliceFact declaredSliceForEndpoint(const slang::ast::Symbol& symbol) {
    const auto* type = endpointTypeForSymbol(symbol);
    if (type == nullptr) {
        return {};
    }
    if (type->hasFixedRange()) {
        const auto range = type->getFixedRange();
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                     .msb = range.left,
                                     .lsb = range.right};
    }
    return type->getBitWidth() == 0
               ? SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Aggregate,
                                       .msb = {},
                                       .lsb = {}}
               : SnapshotConeSliceFact{};
}

SnapshotConeSliceFact connectionEndpointSlice(const SnapshotConeSliceFact& declared_slice,
                                              const slang::ast::Expression& expression) {
    if (declared_slice.precision != SnapshotConeSlicePrecision::Exact || !declared_slice.msb ||
        !declared_slice.lsb) {
        return declared_slice;
    }
    const auto width = staticBitWidth(expression);
    const auto endpoint_width = std::llabs(*declared_slice.msb - *declared_slice.lsb) + 1;
    if (!width || *width > endpoint_width) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Aggregate,
                                     .msb = {},
                                     .lsb = {}};
    }
    if (*width == endpoint_width) {
        return declared_slice;
    }

    const auto step = *declared_slice.msb >= *declared_slice.lsb ? std::int64_t{1} : std::int64_t{-1};
    const auto lsb = *declared_slice.lsb;
    return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                 .msb = lsb + step * (*width - 1),
                                 .lsb = lsb};
}

bool connectionExpressionIsLiteral(const slang::ast::Expression& expression) {
    const auto& unwrapped = unwrapImplicitConversions(expression);
    return unwrapped.kind == slang::ast::ExpressionKind::IntegerLiteral ||
           unwrapped.kind == slang::ast::ExpressionKind::StringLiteral ||
           unwrapped.kind == slang::ast::ExpressionKind::UnbasedUnsizedIntegerLiteral;
}

void normalizeConnectionSourceParts(std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts) {
    std::sort(parts.begin(), parts.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.source_symbol_id,
                        lhs.source_location.uri,
                        lhs.source_location.range.start_line,
                        lhs.source_location.range.start_character,
                        lhs.source_location.range.end_line,
                        lhs.source_location.range.end_character,
                        lhs.source_role,
                        lhs.control_origin,
                        lhs.slice_kind,
                        lhs.source_slice.precision,
                        lhs.source_slice.msb,
                        lhs.source_slice.lsb,
                        lhs.endpoint_slice.precision,
                        lhs.endpoint_slice.msb,
                        lhs.endpoint_slice.lsb,
                        lhs.unresolved) <
               std::tie(rhs.source_symbol_id,
                        rhs.source_location.uri,
                        rhs.source_location.range.start_line,
                        rhs.source_location.range.start_character,
                        rhs.source_location.range.end_line,
                        rhs.source_location.range.end_character,
                        rhs.source_role,
                        rhs.control_origin,
                        rhs.slice_kind,
                        rhs.source_slice.precision,
                        rhs.source_slice.msb,
                        rhs.source_slice.lsb,
                        rhs.endpoint_slice.precision,
                        rhs.endpoint_slice.msb,
                        rhs.endpoint_slice.lsb,
                        rhs.unresolved);
    });
    parts.erase(std::unique(parts.begin(), parts.end(), [](const auto& lhs, const auto& rhs) {
                    return lhs.source_symbol_id == rhs.source_symbol_id &&
                           sameLocation(lhs.source_location, rhs.source_location) &&
                           lhs.source_role == rhs.source_role &&
                           lhs.control_origin == rhs.control_origin && lhs.slice_kind == rhs.slice_kind &&
                           sameSliceFact(lhs.source_slice, rhs.source_slice) &&
                           sameSliceFact(lhs.endpoint_slice, rhs.endpoint_slice) &&
                           lhs.unresolved == rhs.unresolved;
                }),
                parts.end());
}

void collectResolvedConnectionSourceParts(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::Expression& expression,
    const SnapshotConeSliceFact& endpoint_slice,
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts,
    std::optional<SemanticLocation> source_location_override) {
    const auto append_ids = [&](const slang::ast::Expression& current,
                                SnapshotConeSourceRole source_role,
                                SnapshotConeControlOrigin control_origin,
                                SnapshotConeSliceKind slice_kind,
                                const SnapshotConeSliceFact& source_slice,
                                const SnapshotConeSliceFact& current_endpoint_slice) {
        auto location = locationForSourceRange(source_manager, current.sourceRange);
        if (!location.has_value() && source_location_override.has_value()) {
            location = source_location_override;
        }
        if (!location.has_value()) {
            return;
        }
        auto ids = dataSymbolIdsForExpression(data, current);
        if (ids.empty()) {
            const auto references = data.graph_references_by_uri.find(location->uri);
            if (references != data.graph_references_by_uri.end()) {
                const auto first = std::lower_bound(references->second.begin(),
                                                    references->second.end(),
                                                    location->range,
                                                    [](const SnapshotUriReferenceRangeFact& reference,
                                                       const ParseRange& range) {
                                                        return rangeStartLess(reference.range, range);
                                                    });
                for (auto it = first; it != references->second.end(); ++it) {
                    if (rangeStartsAfter(it->range, location->range)) {
                        break;
                    }
                    if (!it->is_declaration && rangeContainsRange(location->range, it->range) &&
                        !it->stable_id.empty()) {
                        ids.push_back(it->stable_id);
                    }
                }
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            }
        }
        for (const auto& id : ids) {
            parts.push_back(SnapshotGraphConnectionBindingFact::SourcePart{
                .source_symbol_id = id,
                .source_location = *location,
                .source_slice = source_slice,
                .endpoint_slice = current_endpoint_slice,
                .slice_kind = slice_kind,
                .source_role = source_role,
                .control_origin = control_origin,
                .unresolved = false});
        }
        if (ids.empty() && !connectionExpressionIsLiteral(current) &&
            unwrapImplicitConversions(current).kind == slang::ast::ExpressionKind::Invalid) {
            parts.push_back(SnapshotGraphConnectionBindingFact::SourcePart{
                .source_symbol_id = {},
                .source_location = *location,
                .source_slice = source_slice,
                .endpoint_slice = current_endpoint_slice,
                .slice_kind = slice_kind,
                .source_role = source_role,
                .control_origin = control_origin,
                .unresolved = true});
        }
    };

    const auto collect = [&](const auto& self,
                             const slang::ast::Expression& current,
                             const SnapshotConeSliceFact& current_endpoint_slice,
                             std::optional<SnapshotConeSliceKind> inherited_slice_kind) -> void {
        const auto& unwrapped = unwrapImplicitConversions(current);
        if (unwrapped.kind == slang::ast::ExpressionKind::ConditionalOp) {
            const auto& conditional = unwrapped.as<slang::ast::ConditionalExpression>();
            for (const auto& condition : conditional.conditions) {
                append_ids(*condition.expr,
                           SnapshotConeSourceRole::Control,
                           SnapshotConeControlOrigin::TernaryCondition,
                           sliceKindForExpression(*condition.expr),
                           sliceFactForExpression(*condition.expr),
                           current_endpoint_slice);
            }
            self(self, conditional.left(), current_endpoint_slice, inherited_slice_kind);
            self(self, conditional.right(), current_endpoint_slice, inherited_slice_kind);
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::Concatenation) {
            std::int64_t offset = 0;
            for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand == nullptr) {
                    continue;
                }
                self(self,
                     *operand,
                     concatOperandSinkSlice(current_endpoint_slice, offset, *operand),
                     SnapshotConeSliceKind::Concatenation);
                if (const auto width = staticBitWidth(*operand)) {
                    offset += *width;
                }
            }
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::ElementSelect) {
            const auto& select = unwrapped.as<slang::ast::ElementSelectExpression>();
            const auto source_slice = sliceFactForExpression(unwrapped);
            if (!staticIntegerValue(select.selector()).has_value()) {
                append_ids(select.selector(),
                           SnapshotConeSourceRole::Control,
                           SnapshotConeControlOrigin::DynamicSelect,
                           SnapshotConeSliceKind::DynamicSelect,
                           sliceFactForExpression(select.selector()),
                           current_endpoint_slice);
            }
            append_ids(select.value(),
                       SnapshotConeSourceRole::Data,
                       SnapshotConeControlOrigin::None,
                       inherited_slice_kind.value_or(sliceKindForExpression(unwrapped)),
                       source_slice,
                       current_endpoint_slice);
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::RangeSelect) {
            const auto& select = unwrapped.as<slang::ast::RangeSelectExpression>();
            const auto source_slice = sliceFactForExpression(unwrapped);
            if (!staticIntegerValue(select.left()).has_value()) {
                append_ids(select.left(),
                           SnapshotConeSourceRole::Control,
                           SnapshotConeControlOrigin::DynamicSelect,
                           SnapshotConeSliceKind::DynamicSelect,
                           sliceFactForExpression(select.left()),
                           current_endpoint_slice);
            }
            if (!staticIntegerValue(select.right()).has_value()) {
                append_ids(select.right(),
                           SnapshotConeSourceRole::Control,
                           SnapshotConeControlOrigin::DynamicSelect,
                           SnapshotConeSliceKind::DynamicSelect,
                           sliceFactForExpression(select.right()),
                           current_endpoint_slice);
            }
            append_ids(select.value(),
                       SnapshotConeSourceRole::Data,
                       SnapshotConeControlOrigin::None,
                       inherited_slice_kind.value_or(sliceKindForExpression(unwrapped)),
                       source_slice,
                       current_endpoint_slice);
            return;
        }
        append_ids(current,
                   SnapshotConeSourceRole::Data,
                   SnapshotConeControlOrigin::None,
                   inherited_slice_kind.value_or(sliceKindForExpression(unwrapped)),
                   sliceFactForExpression(unwrapped),
                   current_endpoint_slice);
    };

    collect(collect, expression, connectionEndpointSlice(endpoint_slice, expression), std::nullopt);
    normalizeConnectionSourceParts(parts);
}

std::optional<std::int64_t> staticSyntaxIntegerValue(const slang::syntax::ExpressionSyntax& syntax) {
    if (syntax.kind == slang::syntax::SyntaxKind::ParenthesizedExpression) {
        return staticSyntaxIntegerValue(
            *syntax.as<slang::syntax::ParenthesizedExpressionSyntax>().expression);
    }
    if (syntax.kind != slang::syntax::SyntaxKind::IntegerLiteralExpression) {
        return std::nullopt;
    }
    const auto value = syntax.as<slang::syntax::LiteralExpressionSyntax>().literal.valueText();
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

bool syntaxExpressionIsLiteral(const slang::syntax::ExpressionSyntax& syntax) {
    return syntax.kind == slang::syntax::SyntaxKind::IntegerLiteralExpression ||
           syntax.kind == slang::syntax::SyntaxKind::IntegerVectorExpression ||
           syntax.kind == slang::syntax::SyntaxKind::UnbasedUnsizedLiteralExpression ||
           syntax.kind == slang::syntax::SyntaxKind::StringLiteralExpression;
}

SnapshotConeSliceFact nonExactSyntaxSlice(SnapshotConeSlicePrecision precision) {
    return SnapshotConeSliceFact{.precision = precision, .msb = {}, .lsb = {}};
}

SnapshotConeSliceFact sliceFactForSyntaxSelector(const slang::syntax::SelectorSyntax& selector) {
    if (selector.kind == slang::syntax::SyntaxKind::BitSelect) {
        const auto& bit = selector.as<slang::syntax::BitSelectSyntax>();
        if (const auto index = staticSyntaxIntegerValue(*bit.expr)) {
            return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                         .msb = *index,
                                         .lsb = *index};
        }
        return nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic);
    }
    if (!slang::syntax::RangeSelectSyntax::isKind(selector.kind)) {
        return nonExactSyntaxSlice(SnapshotConeSlicePrecision::Unresolved);
    }
    const auto& range = selector.as<slang::syntax::RangeSelectSyntax>();
    const auto left = staticSyntaxIntegerValue(*range.left);
    const auto right = staticSyntaxIntegerValue(*range.right);
    if (!left || !right) {
        return nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic);
    }
    if (selector.kind == slang::syntax::SyntaxKind::AscendingRangeSelect) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                     .msb = *left + *right - 1,
                                     .lsb = *left};
    }
    if (selector.kind == slang::syntax::SyntaxKind::DescendingRangeSelect) {
        return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                     .msb = *left,
                                     .lsb = *left - *right + 1};
    }
    return SnapshotConeSliceFact{.precision = SnapshotConeSlicePrecision::Exact,
                                 .msb = *left,
                                 .lsb = *right};
}

SnapshotConeSliceKind sliceKindForSyntaxSelector(const slang::syntax::SelectorSyntax& selector) {
    const auto slice = sliceFactForSyntaxSelector(selector);
    if (slice.precision == SnapshotConeSlicePrecision::Dynamic ||
        slice.precision == SnapshotConeSlicePrecision::Unresolved) {
        return SnapshotConeSliceKind::DynamicSelect;
    }
    return selector.kind == slang::syntax::SyntaxKind::BitSelect
               ? SnapshotConeSliceKind::ElementSelect
               : SnapshotConeSliceKind::RangeSelect;
}

void collectSyntaxConnectionSourceParts(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::syntax::ExpressionSyntax& syntax,
    const SnapshotConeSliceFact& endpoint_slice,
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart>& parts) {
    const auto append_ids = [&](const slang::syntax::ExpressionSyntax& current,
                                SnapshotConeSourceRole source_role,
                                SnapshotConeControlOrigin control_origin,
                                SnapshotConeSliceKind slice_kind,
                                const SnapshotConeSliceFact& source_slice,
                                const SnapshotConeSliceFact& current_endpoint_slice,
                                std::optional<SemanticLocation> location_override = std::nullopt) {
        auto location = location_override;
        if (!location.has_value()) {
            location = locationForSourceRange(source_manager, current.sourceRange());
        }
        if (!location.has_value()) {
            return;
        }

        std::vector<std::string> ids;
        const auto references = data.graph_references_by_uri.find(location->uri);
        if (references != data.graph_references_by_uri.end()) {
            const auto first = std::lower_bound(references->second.begin(),
                                                references->second.end(),
                                                location->range,
                                                [](const SnapshotUriReferenceRangeFact& reference,
                                                   const ParseRange& range) {
                                                    return rangeStartLess(reference.range, range);
                                                });
            for (auto it = first; it != references->second.end(); ++it) {
                if (rangeStartsAfter(it->range, location->range)) {
                    break;
                }
                if (!it->is_declaration && rangeContainsRange(location->range, it->range) &&
                    !it->stable_id.empty()) {
                    ids.push_back(it->stable_id);
                }
            }
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const auto& id : ids) {
            parts.push_back(SnapshotGraphConnectionBindingFact::SourcePart{
                .source_symbol_id = id,
                .source_location = *location,
                .source_slice = source_slice,
                .endpoint_slice = current_endpoint_slice,
                .slice_kind = slice_kind,
                .source_role = source_role,
                .control_origin = control_origin,
                .unresolved = false});
        }
        if (ids.empty() && !syntaxExpressionIsLiteral(current)) {
            parts.push_back(SnapshotGraphConnectionBindingFact::SourcePart{
                .source_symbol_id = {},
                .source_location = *location,
                .source_slice = source_slice,
                .endpoint_slice = current_endpoint_slice,
                .slice_kind = slice_kind,
                .source_role = source_role,
                .control_origin = control_origin,
                .unresolved = true});
        }
    };

    const auto collect = [&](const auto& self,
                             const slang::syntax::ExpressionSyntax& current,
                             const SnapshotConeSliceFact& current_endpoint_slice,
                             std::optional<SnapshotConeSliceKind> inherited_slice_kind) -> void {
        if (current.kind == slang::syntax::SyntaxKind::ParenthesizedExpression) {
            self(self,
                 *current.as<slang::syntax::ParenthesizedExpressionSyntax>().expression,
                 current_endpoint_slice,
                 inherited_slice_kind);
            return;
        }
        if (current.kind == slang::syntax::SyntaxKind::ConcatenationExpression) {
            for (const auto* operand : current.as<slang::syntax::ConcatenationExpressionSyntax>().expressions) {
                if (operand != nullptr) {
                    self(self,
                         *operand,
                         current_endpoint_slice,
                         SnapshotConeSliceKind::Concatenation);
                }
            }
            return;
        }
        if (current.kind == slang::syntax::SyntaxKind::IdentifierSelectName) {
            const auto& name = current.as<slang::syntax::IdentifierSelectNameSyntax>();
            const slang::syntax::ElementSelectSyntax* final_select = nullptr;
            for (const auto* select : name.selectors) {
                final_select = select;
            }
            if (final_select != nullptr && final_select->selector != nullptr) {
                const auto source_slice = sliceFactForSyntaxSelector(*final_select->selector);
                const auto slice_kind = sliceKindForSyntaxSelector(*final_select->selector);
                if (source_slice.precision == SnapshotConeSlicePrecision::Dynamic) {
                    const auto& selector = *final_select->selector;
                    if (selector.kind == slang::syntax::SyntaxKind::BitSelect) {
                        append_ids(*selector.as<slang::syntax::BitSelectSyntax>().expr,
                                   SnapshotConeSourceRole::Control,
                                   SnapshotConeControlOrigin::DynamicSelect,
                                   SnapshotConeSliceKind::DynamicSelect,
                                   nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                                   current_endpoint_slice);
                    }
                    else if (slang::syntax::RangeSelectSyntax::isKind(selector.kind)) {
                        const auto& range = selector.as<slang::syntax::RangeSelectSyntax>();
                        append_ids(*range.left,
                                   SnapshotConeSourceRole::Control,
                                   SnapshotConeControlOrigin::DynamicSelect,
                                   SnapshotConeSliceKind::DynamicSelect,
                                   nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                                   current_endpoint_slice);
                        append_ids(*range.right,
                                   SnapshotConeSourceRole::Control,
                                   SnapshotConeControlOrigin::DynamicSelect,
                                   SnapshotConeSliceKind::DynamicSelect,
                                   nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                                   current_endpoint_slice);
                    }
                }
                append_ids(current,
                           SnapshotConeSourceRole::Data,
                           SnapshotConeControlOrigin::None,
                           inherited_slice_kind.value_or(slice_kind),
                           source_slice,
                           current_endpoint_slice,
                           locationForSyntaxToken(source_manager, name.identifier));
                return;
            }
        }
        if (current.kind == slang::syntax::SyntaxKind::ElementSelectExpression) {
            const auto& selected = current.as<slang::syntax::ElementSelectExpressionSyntax>();
            if (selected.select->selector == nullptr) {
                append_ids(current,
                           SnapshotConeSourceRole::Data,
                           SnapshotConeControlOrigin::None,
                           SnapshotConeSliceKind::DynamicSelect,
                           nonExactSyntaxSlice(SnapshotConeSlicePrecision::Unresolved),
                           current_endpoint_slice);
                return;
            }
            const auto source_slice = sliceFactForSyntaxSelector(*selected.select->selector);
            const auto slice_kind = sliceKindForSyntaxSelector(*selected.select->selector);
            if (source_slice.precision == SnapshotConeSlicePrecision::Dynamic) {
                const auto& selector = *selected.select->selector;
                if (selector.kind == slang::syntax::SyntaxKind::BitSelect) {
                    append_ids(*selector.as<slang::syntax::BitSelectSyntax>().expr,
                               SnapshotConeSourceRole::Control,
                               SnapshotConeControlOrigin::DynamicSelect,
                               SnapshotConeSliceKind::DynamicSelect,
                               nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                               current_endpoint_slice);
                }
                else if (slang::syntax::RangeSelectSyntax::isKind(selector.kind)) {
                    const auto& range = selector.as<slang::syntax::RangeSelectSyntax>();
                    append_ids(*range.left,
                               SnapshotConeSourceRole::Control,
                               SnapshotConeControlOrigin::DynamicSelect,
                               SnapshotConeSliceKind::DynamicSelect,
                               nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                               current_endpoint_slice);
                    append_ids(*range.right,
                               SnapshotConeSourceRole::Control,
                               SnapshotConeControlOrigin::DynamicSelect,
                               SnapshotConeSliceKind::DynamicSelect,
                               nonExactSyntaxSlice(SnapshotConeSlicePrecision::Dynamic),
                               current_endpoint_slice);
                }
            }
            append_ids(*selected.left,
                       SnapshotConeSourceRole::Data,
                       SnapshotConeControlOrigin::None,
                       inherited_slice_kind.value_or(slice_kind),
                       source_slice,
                       current_endpoint_slice);
            return;
        }
        append_ids(current,
                   SnapshotConeSourceRole::Data,
                   SnapshotConeControlOrigin::None,
                   inherited_slice_kind.value_or(SnapshotConeSliceKind::Whole),
                   SnapshotConeSliceFact{},
                   current_endpoint_slice);
    };

    collect(collect, syntax, endpoint_slice, std::nullopt);
    normalizeConnectionSourceParts(parts);
}

// Generated scopes can expose an AST symbol that has no source-backed stable
// identity. Preserve its AST name in the build input so lowering can resolve it
// through the already-indexed URI/scope view without reading source text.
std::vector<std::string> dataSymbolNamesForExpression(const slang::ast::Expression& expression) {
    std::vector<std::string> names;
    const auto append_symbol = [&](const slang::ast::Symbol& symbol) {
        if (!symbol.name.empty()) {
            names.emplace_back(symbol.name);
        }
    };
    const auto collect = [&](const auto& self, const slang::ast::Expression& current) -> void {
        const auto& unwrapped = unwrapImplicitConversions(current);
        switch (unwrapped.kind) {
        case slang::ast::ExpressionKind::NamedValue:
        case slang::ast::ExpressionKind::HierarchicalValue:
            append_symbol(unwrapped.as<slang::ast::ValueExpressionBase>().symbol);
            return;
        case slang::ast::ExpressionKind::ElementSelect:
            self(self, unwrapped.as<slang::ast::ElementSelectExpression>().value());
            return;
        case slang::ast::ExpressionKind::RangeSelect:
            self(self, unwrapped.as<slang::ast::RangeSelectExpression>().value());
            return;
        case slang::ast::ExpressionKind::MemberAccess: {
            const auto& member = unwrapped.as<slang::ast::MemberAccessExpression>();
            append_symbol(member.member);
            self(self, member.value());
            return;
        }
        case slang::ast::ExpressionKind::Concatenation:
            for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand != nullptr) self(self, *operand);
            }
            return;
        case slang::ast::ExpressionKind::BinaryOp: {
            const auto& binary = unwrapped.as<slang::ast::BinaryExpression>();
            self(self, binary.left());
            self(self, binary.right());
            return;
        }
        default:
            unwrapped.visitSymbolReferences([&](const slang::ast::Expression&, const slang::ast::Symbol& symbol) {
                append_symbol(symbol);
            });
            return;
        }
    };
    collect(collect, expression);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::string> coneSymbolNamesForExpression(const SemanticEngineDocument* document,
                                                       const slang::SourceManager& source_manager,
                                                       const slang::ast::Expression& expression) {
    auto names = dataSymbolNamesForExpression(expression);
    if (!names.empty()) {
        return names;
    }

    // Slang can materialize generated continuous assignments without retaining
    // source-backed ValueSymbol links. This accepts only the exact identifier
    // range already attached to that AST expression; providers never perform
    // source recovery or global name lookup at query time.
    const auto text = expressionText(document, source_manager, expression);
    if (isSimpleSystemVerilogIdentifier(text)) {
        names.push_back(text);
    }
    return names;
}

bool containsConditionalExpression(const slang::ast::Expression& expression) {
    const auto& unwrapped = unwrapImplicitConversions(expression);
    if (unwrapped.kind == slang::ast::ExpressionKind::ConditionalOp) {
        return true;
    }
    if (unwrapped.kind == slang::ast::ExpressionKind::BinaryOp) {
        const auto& binary = unwrapped.as<slang::ast::BinaryExpression>();
        return containsConditionalExpression(binary.left()) || containsConditionalExpression(binary.right());
    }
    if (unwrapped.kind == slang::ast::ExpressionKind::Concatenation) {
        for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
            if (operand != nullptr && containsConditionalExpression(*operand)) {
                return true;
            }
        }
    }
    return false;
}

template<typename DocumentResolver>
void collectConeSourcesForExpression(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::Expression& expression,
    DocumentResolver&& document_for,
    const SnapshotConeSliceFact& sink_slice,
    std::vector<SnapshotConeDataSourceSeed>& data_sources,
    std::vector<SnapshotConeControlSourceSeed>& control_sources) {
    const auto append_control = [&](const slang::ast::Expression& control,
                                    SnapshotConeControlOrigin origin) {
        if (auto source = controlSourceForExpression(document_for(control), source_manager, control)) {
            source->source_symbol_ids = dataSymbolIdsForExpression(data, control);
            source->source_symbol_names = coneSymbolNamesForExpression(document_for(control),
                                                                         source_manager,
                                                                         control);
            source->origin = origin;
            source->unresolved = source->source_symbol_ids.empty() &&
                                 unwrapImplicitConversions(control).kind ==
                                     slang::ast::ExpressionKind::Invalid;
            control_sources.push_back(std::move(*source));
        }
    };

    const auto collect_dynamic_select_controls = [&](const auto& self,
                                                     const slang::ast::Expression& current) -> void {
        const auto& unwrapped = unwrapImplicitConversions(current);
        if (unwrapped.kind == slang::ast::ExpressionKind::ElementSelect) {
            const auto& select = unwrapped.as<slang::ast::ElementSelectExpression>();
            if (!staticIntegerValue(select.selector()).has_value()) {
                append_control(select.selector(), SnapshotConeControlOrigin::DynamicSelect);
            }
            self(self, select.value());
            self(self, select.selector());
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::RangeSelect) {
            const auto& select = unwrapped.as<slang::ast::RangeSelectExpression>();
            if (!staticIntegerValue(select.left()).has_value()) {
                append_control(select.left(), SnapshotConeControlOrigin::DynamicSelect);
            }
            if (!staticIntegerValue(select.right()).has_value()) {
                append_control(select.right(), SnapshotConeControlOrigin::DynamicSelect);
            }
            self(self, select.value());
            self(self, select.left());
            self(self, select.right());
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::ConditionalOp) {
            const auto& conditional = unwrapped.as<slang::ast::ConditionalExpression>();
            for (const auto& condition : conditional.conditions) self(self, *condition.expr);
            self(self, conditional.left());
            self(self, conditional.right());
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::BinaryOp) {
            const auto& binary = unwrapped.as<slang::ast::BinaryExpression>();
            self(self, binary.left());
            self(self, binary.right());
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::Concatenation) {
            for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand != nullptr) self(self, *operand);
            }
        }
    };
    collect_dynamic_select_controls(collect_dynamic_select_controls, expression);

    const auto collect = [&](const auto& self,
                             const slang::ast::Expression& current,
                             std::optional<SnapshotConeSliceKind> inherited_slice,
                             const SnapshotConeSliceFact& inherited_sink_slice) -> void {
        const auto& unwrapped = unwrapImplicitConversions(current);
        if (unwrapped.kind == slang::ast::ExpressionKind::ConditionalOp) {
            const auto& conditional = unwrapped.as<slang::ast::ConditionalExpression>();
            for (const auto& condition : conditional.conditions) {
                append_control(*condition.expr, SnapshotConeControlOrigin::TernaryCondition);
            }
            self(self, conditional.left(), inherited_slice, inherited_sink_slice);
            self(self, conditional.right(), inherited_slice, inherited_sink_slice);
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::BinaryOp && containsConditionalExpression(unwrapped)) {
            const auto& binary = unwrapped.as<slang::ast::BinaryExpression>();
            self(self, binary.left(), inherited_slice, inherited_sink_slice);
            self(self, binary.right(), inherited_slice, inherited_sink_slice);
            return;
        }
        if (unwrapped.kind == slang::ast::ExpressionKind::Concatenation) {
            std::int64_t offset = 0;
            for (const auto* operand : unwrapped.as<slang::ast::ConcatenationExpression>().operands()) {
                if (operand != nullptr) {
                    const auto operand_sink_slice = concatOperandSinkSlice(inherited_sink_slice, offset, *operand);
                    self(self, *operand, SnapshotConeSliceKind::Concatenation, operand_sink_slice);
                    if (const auto width = staticBitWidth(*operand)) {
                        offset += *width;
                    }
                }
            }
            return;
        }

        const auto range = sourceRangeForSourceRange(source_manager, current.sourceRange);
        if (range.start_line < 0 || range.end_line < range.start_line) {
            return;
        }
        auto ids = dataSymbolIdsForExpression(data, current);
        data_sources.push_back(SnapshotConeDataSourceSeed{
            .range = range,
            .expression = expressionText(document_for(current), source_manager, current),
            .slice_kind = inherited_slice.value_or(sliceKindForExpression(current)),
            .source_slice = sliceFactForExpression(current),
            .sink_slice = inherited_sink_slice,
            .source_symbol_ids = std::move(ids),
            .source_symbol_names = coneSymbolNamesForExpression(document_for(current),
                                                                 source_manager,
                                                                 current),
            .unresolved = unwrapped.kind == slang::ast::ExpressionKind::Invalid});
    };
    collect(collect, expression, std::nullopt, sink_slice);
}

void normalizeConeDataSources(std::vector<SnapshotConeDataSourceSeed>& sources) {
    std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
        return std::make_tuple(lhs.range.start_line,
                               lhs.range.start_character,
                               lhs.range.end_line,
                               lhs.range.end_character,
                               lhs.expression,
                               lhs.slice_kind,
                               sliceFactKey(lhs.source_slice),
                               sliceFactKey(lhs.sink_slice),
                               lhs.source_symbol_ids,
                               lhs.source_symbol_names,
                               lhs.unresolved) <
               std::make_tuple(rhs.range.start_line,
                               rhs.range.start_character,
                               rhs.range.end_line,
                               rhs.range.end_character,
                               rhs.expression,
                               rhs.slice_kind,
                               sliceFactKey(rhs.source_slice),
                               sliceFactKey(rhs.sink_slice),
                               rhs.source_symbol_ids,
                               rhs.source_symbol_names,
                               rhs.unresolved);
    });
    sources.erase(std::unique(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
                      return sameRange(lhs.range, rhs.range) && lhs.expression == rhs.expression &&
                             lhs.slice_kind == rhs.slice_kind &&
                             sameSliceFact(lhs.source_slice, rhs.source_slice) &&
                             sameSliceFact(lhs.sink_slice, rhs.sink_slice) &&
                             lhs.source_symbol_ids == rhs.source_symbol_ids &&
                             lhs.source_symbol_names == rhs.source_symbol_names &&
                             lhs.unresolved == rhs.unresolved;
                  }),
                  sources.end());
}

void normalizeConeControlSources(std::vector<SnapshotConeControlSourceSeed>& sources) {
    std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
        return std::make_tuple(lhs.range.start_line,
                               lhs.range.start_character,
                               lhs.range.end_line,
                               lhs.range.end_character,
                               lhs.expression,
                               lhs.slice_kind,
                               sliceFactKey(lhs.source_slice),
                               lhs.origin,
                               lhs.source_symbol_ids,
                               lhs.source_symbol_names,
                               lhs.unresolved) <
               std::make_tuple(rhs.range.start_line,
                               rhs.range.start_character,
                               rhs.range.end_line,
                               rhs.range.end_character,
                               rhs.expression,
                               rhs.slice_kind,
                               sliceFactKey(rhs.source_slice),
                               rhs.origin,
                               rhs.source_symbol_ids,
                               rhs.source_symbol_names,
                               rhs.unresolved);
    });
    sources.erase(std::unique(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
                      return sameRange(lhs.range, rhs.range) && lhs.expression == rhs.expression &&
                             lhs.slice_kind == rhs.slice_kind &&
                             sameSliceFact(lhs.source_slice, rhs.source_slice) && lhs.origin == rhs.origin &&
                             lhs.source_symbol_ids == rhs.source_symbol_ids &&
                             lhs.source_symbol_names == rhs.source_symbol_names &&
                             lhs.unresolved == rhs.unresolved;
                  }),
                  sources.end());
}

void upsertAstContinuousAssignment(SnapshotData& data,
                                   const slang::SourceManager& source_manager,
                                   const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                   const slang::ast::ContinuousAssignSymbol& assignment) {
    const slang::ast::InstanceBodySymbol* body = nullptr;
    for (const auto* parent_scope = assignment.getParentScope(); parent_scope != nullptr;) {
        const auto& parent_symbol = parent_scope->asSymbol();
        if (parent_symbol.kind == slang::ast::SymbolKind::InstanceBody) {
            body = &parent_symbol.as<slang::ast::InstanceBodySymbol>();
            break;
        }
        parent_scope = parent_symbol.getParentScope();
    }
    if (body == nullptr) {
        return;
    }

    const auto signature_it = data.ast_module_signatures_by_name.find(std::string(body->getDefinition().name));
    if (signature_it == data.ast_module_signatures_by_name.end()) {
        return;
    }

    const auto location = locationForSourceRange(source_manager, assignment.getAssignment().sourceRange);
    const auto document_it = location.has_value() ? documents.find(location->uri) : documents.end();
    const auto* document = document_it == documents.end() ? nullptr : &document_it->second;
    const auto* assignment_expression =
        assignment.getAssignment().as_if<slang::ast::AssignmentExpression>();
    if (assignment_expression == nullptr) {
        return;
    }
    if (location.has_value()) {
        std::vector<SnapshotConeDataSourceSeed> data_sources;
        std::vector<SnapshotConeControlSourceSeed> controls;
        const auto sink_slice = sliceFactForExpression(assignment_expression->left());
        collectConeSourcesForExpression(data,
                                        source_manager,
                                        assignment_expression->right(),
                                        [&](const slang::ast::Expression&) { return document; },
                                        sink_slice,
                                        data_sources,
                                        controls);
        normalizeConeDataSources(data_sources);
        normalizeConeControlSources(controls);
        appendAssignmentEdgeSeed(data,
                                 SnapshotAssignmentEdgeSeed{
            .uri = location->uri,
            .scope_range = signature_it->second.definition.range,
            .assignment_range = location->range,
            .left_range = sourceRangeForSourceRange(source_manager,
                                                    assignment_expression->left().sourceRange),
            .right_range = sourceRangeForSourceRange(source_manager,
                                                     assignment_expression->right().sourceRange),
            .left_expression = expressionText(document, source_manager, assignment_expression->left()),
            .right_expression = expressionText(document, source_manager, assignment_expression->right()),
            .left_symbol_ids = directSymbolIdsForExpression(data, source_manager, assignment_expression->left()),
            .left_symbol_names = coneSymbolNamesForExpression(document,
                                                               source_manager,
                                                               assignment_expression->left()),
            .sink_slice = sink_slice,
            .data_sources = std::move(data_sources),
            .control_sources = std::move(controls)});
    }

    auto& schematic = signature_it->second.schematic;
    AstExpressionSchematicContext context{.cell_index = static_cast<int>(schematic.cells.size())};
    std::vector<SchematicConnection> connections;
    connections.push_back(makeConnection("Y",
                                         -1,
                                         expressionText(document,
                                                        source_manager,
                                                        assignment_expression->left()),
                                         source_manager,
                                         assignment_expression->left()));
    connections.push_back(makeConnection("A",
                                         -1,
                                         materializeAstExpression(schematic,
                                                                  document,
                                                                  source_manager,
                                                                  assignment_expression->right(),
                                                                  context),
                                         source_manager,
                                         assignment_expression->right()));
    appendAstLogicCell(schematic,
                       source_manager,
                       "buf",
                       assignment.getAssignment(),
                       std::move(connections),
                       context);
}

void insertReference(SnapshotData& data,
                     std::string stable_id,
                     std::string name,
                     SemanticLocation location,
                     SemanticReferenceRole role) {
    const auto duplicate = std::find_if(data.references.begin(),
                                        data.references.end(),
                                        [&](const auto& reference) {
                                            return reference.stable_id == stable_id &&
                                                   sameLocation(reference.location, location);
                                        });
    if (duplicate != data.references.end()) {
        if (role == SemanticReferenceRole::Declaration || role == SemanticReferenceRole::Write ||
            (role == SemanticReferenceRole::Type && duplicate->role == SemanticReferenceRole::Read) ||
            (role == SemanticReferenceRole::Instance && duplicate->role == SemanticReferenceRole::Read)) {
            duplicate->role = role;
            duplicate->is_declaration = role == SemanticReferenceRole::Declaration;
        }
        return;
    }

    const auto index = data.references.size();
    data.references.push_back(SnapshotIndexedReference{.stable_id = std::move(stable_id),
                                                       .name = std::move(name),
                                                       .location = std::move(location),
                                                       .is_declaration = role == SemanticReferenceRole::Declaration,
                                                       .role = role});
    data.references_by_symbol[data.references.back().stable_id].push_back(index);
}

void markReferenceRole(SnapshotData& data,
                       std::string_view stable_id,
                       std::string_view uri,
                       const ParseRange& containing_range,
                       SemanticReferenceRole role) {
    for (auto& reference : data.references) {
        if (reference.stable_id == stable_id && reference.location.uri == uri &&
            !reference.is_declaration &&
            rangeContainsRange(containing_range, reference.location.range)) {
            reference.role = role;
        }
    }
}

std::optional<std::string> findUniqueModuleDefinitionSymbolId(const SnapshotData& data,
                                                              std::string_view name) {
    std::optional<std::string> definition_result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && indexed_symbol.identity.kind == "Definition") {
            if (definition_result.has_value()) {
                return std::nullopt;
            }
            definition_result = stable_id;
        }
    }
    if (definition_result.has_value()) {
        return definition_result;
    }

    std::optional<std::string> module_like_result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && isModuleDefinitionKind(indexed_symbol.identity.kind)) {
            if (module_like_result.has_value()) {
                return std::nullopt;
            }
            module_like_result = stable_id;
        }
    }
    return module_like_result;
}

void insertSymbol(SnapshotData& data,
                  const slang::SourceManager& source_manager,
                  const slang::ast::Symbol& symbol) {
    if (symbol.name.empty()) {
        return;
    }

    const auto location = declarationLocationForSymbol(source_manager, symbol);
    if (!location.has_value()) {
        return;
    }

    const auto stable_id = symbolStableId(source_manager, symbol, *location);
    if (data.symbols_by_id.find(stable_id) == data.symbols_by_id.end()) {
        auto type_display = typeDisplayForSymbol(symbol);
        auto value_display = valueDisplayForSymbol(symbol);

        data.symbols_by_id.emplace(
            stable_id,
            SnapshotIndexedSymbol{.identity = SemanticSymbolIdentity{.stable_id = stable_id,
                                                                      .name = std::string(symbol.name),
                                                                      .kind = symbolKindName(symbol.kind),
                                                                      .location = *location},
                                  .symbol = &symbol,
                                  .type_display = std::move(type_display),
                                  .value_display = std::move(value_display)});
    }
    data.ids_by_symbol.emplace(&symbol, stable_id);
    if (symbol.kind == slang::ast::SymbolKind::Port) {
        const auto& port = symbol.as<slang::ast::PortSymbol>();
        if (port.internalSymbol != nullptr) {
            data.ids_by_symbol.emplace(port.internalSymbol, stable_id);
        }
    }
    indexTypedMemberCompletions(data, source_manager, symbol, *location, stable_id);

    auto& completions = data.completions_by_uri[location->uri];
    const auto duplicate = std::any_of(completions.begin(),
                                       completions.end(),
                                       [&](const SemanticCompletionItem& item) {
                                           return item.label == symbol.name;
                                       });
    if (!duplicate) {
        const auto kind_name = symbolKindName(symbol.kind);
        completions.push_back(SemanticCompletionItem{.stable_id = stable_id,
                                                     .label = std::string(symbol.name),
                                                     .detail = completionDetailForSemanticKind(kind_name),
                                                     .documentation = {},
                                                     .insert_text = std::string(symbol.name),
                                                     .kind = completionKindForSemanticKind(kind_name),
                                                     .unresolved = false});
    }
}

int visibilityOriginPriority(SnapshotVisibilityOrigin origin) {
    switch (origin) {
    case SnapshotVisibilityOrigin::Local:
        return 0;
    case SnapshotVisibilityOrigin::ExplicitImport:
        return 1;
    case SnapshotVisibilityOrigin::WildcardImport:
        return 2;
    case SnapshotVisibilityOrigin::PackageExport:
        return 3;
    case SnapshotVisibilityOrigin::Workspace:
        return 4;
    }
    return 5;
}

bool visibilityCandidateLess(const SnapshotVisibilityCandidate& left,
                             const SnapshotVisibilityCandidate& right) {
    const auto left_priority = visibilityOriginPriority(left.origin);
    const auto right_priority = visibilityOriginPriority(right.origin);
    if (left_priority != right_priority) {
        return left_priority < right_priority;
    }
    if (left.identity.location.uri != right.identity.location.uri) {
        return left.identity.location.uri < right.identity.location.uri;
    }
    if (!sameRange(left.identity.location.range, right.identity.location.range)) {
        return locationLess(left.identity.location, right.identity.location);
    }
    if (left.identity.kind != right.identity.kind) {
        return left.identity.kind < right.identity.kind;
    }
    return left.identity.stable_id < right.identity.stable_id;
}

bool workspaceVisibilityCandidateLess(const SnapshotVisibilityCandidate& left,
                                      const SnapshotVisibilityCandidate& right) {
    const auto less_name = std::lexicographical_compare(
        left.identity.name.begin(),
        left.identity.name.end(),
        right.identity.name.begin(),
        right.identity.name.end(),
        [](unsigned char lhs, unsigned char rhs) {
            return std::tolower(lhs) < std::tolower(rhs);
        });
    const auto same_name = left.identity.name.size() == right.identity.name.size() &&
                           !less_name &&
                           !std::lexicographical_compare(
                               right.identity.name.begin(),
                               right.identity.name.end(),
                               left.identity.name.begin(),
                               left.identity.name.end(),
                               [](unsigned char lhs, unsigned char rhs) {
                                   return std::tolower(lhs) < std::tolower(rhs);
                               });
    if (!same_name) {
        return less_name;
    }
    if (left.identity.kind != right.identity.kind) {
        return left.identity.kind < right.identity.kind;
    }
    return left.identity.stable_id < right.identity.stable_id;
}

std::string scopeStableId(std::string_view uri,
                          const ParseRange& range,
                          std::string_view context_kind) {
    return "scope|" + std::string(uri) + "|" + rangeKey(range) + "|" +
           std::string(context_kind);
}

const slang::ast::Symbol* visibleSymbol(const slang::ast::Symbol& symbol,
                                        SnapshotVisibilityOrigin& origin) {
    if (symbol.kind == slang::ast::SymbolKind::TransparentMember) {
        return &symbol.as<slang::ast::TransparentMemberSymbol>().wrapped;
    }
    if (symbol.kind == slang::ast::SymbolKind::ExplicitImport) {
        origin = SnapshotVisibilityOrigin::ExplicitImport;
        return symbol.as<slang::ast::ExplicitImportSymbol>().importedSymbol();
    }
    if (symbol.kind == slang::ast::SymbolKind::WildcardImport || symbol.name.empty()) {
        return nullptr;
    }
    return &symbol;
}

std::optional<SnapshotVisibilityCandidate> visibilityCandidateForSymbol(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const slang::ast::Symbol& source_symbol,
    SnapshotVisibilityOrigin origin) {
    const auto* symbol = visibleSymbol(source_symbol, origin);
    if (symbol == nullptr || symbol->name.empty()) {
        return std::nullopt;
    }
    insertSymbol(data, source_manager, *symbol);
    const auto id_it = data.ids_by_symbol.find(symbol);
    if (id_it == data.ids_by_symbol.end()) {
        return std::nullopt;
    }
    const auto indexed_it = data.symbols_by_id.find(id_it->second);
    if (indexed_it == data.symbols_by_id.end()) {
        return std::nullopt;
    }
    return SnapshotVisibilityCandidate{.identity = indexed_it->second.identity,
                                       .type_display = indexed_it->second.type_display,
                                       .value_display = indexed_it->second.value_display,
                                       .origin = origin};
}

void appendVisibilityCandidate(std::vector<SnapshotVisibilityCandidate>& candidates,
                               SnapshotVisibilityCandidate candidate) {
    const auto duplicate = std::any_of(candidates.begin(),
                                       candidates.end(),
                                       [&](const SnapshotVisibilityCandidate& existing) {
                                           return existing.identity.stable_id ==
                                                  candidate.identity.stable_id;
                                       });
    if (!duplicate) {
        candidates.push_back(std::move(candidate));
    }
}

void collectPackageVisibility(SnapshotData& data,
                              const slang::SourceManager& source_manager,
                              const slang::ast::PackageSymbol& package,
                              std::set<std::string>& visiting,
                              SnapshotPackageVisibility& result);

void appendExportedPackageVisibility(SnapshotData& data,
                                     const slang::SourceManager& source_manager,
                                     const slang::ast::PackageSymbol& exported_package,
                                     std::string_view exported_item,
                                     std::set<std::string>& visiting,
                                     SnapshotPackageVisibility& result) {
    result.exported_packages.push_back(std::string(exported_package.name));

    SnapshotPackageVisibility exported;
    exported.package_name = std::string(exported_package.name);
    if (const auto location = declarationLocationForSymbol(source_manager, exported_package)) {
        exported.uri = location->uri;
    }
    collectPackageVisibility(data, source_manager, exported_package, visiting, exported);
    for (auto candidate : exported.candidates) {
        if (exported_item != "*" && candidate.identity.name != exported_item) {
            continue;
        }
        candidate.origin = SnapshotVisibilityOrigin::PackageExport;
        appendVisibilityCandidate(result.candidates, std::move(candidate));
    }
}

void collectPackageVisibility(SnapshotData& data,
                              const slang::SourceManager& source_manager,
                              const slang::ast::PackageSymbol& package,
                              std::set<std::string>& visiting,
                              SnapshotPackageVisibility& result) {
    if (!visiting.insert(std::string(package.name)).second) {
        return;
    }
    package.checkExplicitExports();

    for (const auto* export_decl : package.exportDecls) {
        if (export_decl == nullptr) {
            continue;
        }
        const auto package_name = export_decl->package.valueText();
        const auto item_name = export_decl->item.valueText();
        if (package_name.empty() || package_name == "*" || item_name.empty()) {
            continue;
        }
        const auto* exported_package = data.compilation->getPackage(package_name);
        if (exported_package == nullptr || exported_package->name.empty()) {
            continue;
        }
        appendExportedPackageVisibility(data,
                                        source_manager,
                                        *exported_package,
                                        item_name,
                                        visiting,
                                        result);
    }

    for (const auto& member : package.members()) {
        if (member.kind == slang::ast::SymbolKind::ExplicitImport) {
            const auto& import = member.as<slang::ast::ExplicitImportSymbol>();
            if (!import.isFromExport && !package.hasExportAll) {
                continue;
            }
            if (const auto candidate = visibilityCandidateForSymbol(
                    data,
                    source_manager,
                    member,
                    SnapshotVisibilityOrigin::PackageExport)) {
                appendVisibilityCandidate(result.candidates, *candidate);
            }
            continue;
        }
        if (member.kind == slang::ast::SymbolKind::WildcardImport) {
            continue;
        }
        if (const auto candidate = visibilityCandidateForSymbol(data,
                                                                 source_manager,
                                                                 member,
                                                                 SnapshotVisibilityOrigin::Local)) {
            appendVisibilityCandidate(result.candidates, *candidate);
        }
    }

    if (const auto* imports = package.getWildcardImportData()) {
        for (const auto* wildcard : imports->wildcardImports) {
            if (wildcard == nullptr || (!wildcard->isFromExport && !package.hasExportAll)) {
                continue;
            }
            const auto* exported_package = wildcard->getPackage();
            if (exported_package == nullptr || exported_package->name.empty()) {
                continue;
            }
            appendExportedPackageVisibility(data,
                                            source_manager,
                                            *exported_package,
                                            "*",
                                            visiting,
                                            result);
        }
    }
    visiting.erase(std::string(package.name));
}

int scopeDepth(const slang::ast::Scope& scope) {
    int depth = 0;
    const auto* parent = scope.asSymbol().getParentScope();
    while (parent != nullptr) {
        ++depth;
        parent = parent->asSymbol().getParentScope();
    }
    return depth;
}

void appendScopeVisibility(SnapshotData& data,
                           const slang::SourceManager& source_manager,
                           const slang::ast::Scope& scope) {
    const auto& scope_symbol = scope.asSymbol();
    const auto scope_location = declarationLocationForSymbol(source_manager, scope_symbol);
    const auto scope_range = parseRangeForSymbolSyntax(source_manager, scope_symbol);
    if (!scope_location.has_value() || !scope_range.has_value() || scope_location->uri.empty()) {
        return;
    }

    SnapshotScopeVisibility result;
    result.uri = scope_location->uri;
    result.range = *scope_range;
    result.lexical_depth = scopeDepth(scope);
    result.context_kind = std::string(slang::ast::toString(scope_symbol.kind));
    result.stable_id = scopeStableId(result.uri, result.range, result.context_kind);
    if (const auto* parent = scope_symbol.getParentScope()) {
        if (const auto parent_location = declarationLocationForSymbol(source_manager, parent->asSymbol());
            parent_location.has_value()) {
            if (const auto parent_range = parseRangeForSymbolSyntax(source_manager, parent->asSymbol());
                parent_range.has_value()) {
                result.parent_stable_id = scopeStableId(
                    parent_location->uri,
                    *parent_range,
                    slang::ast::toString(parent->asSymbol().kind));
            }
        }
    }

    for (const auto& member : scope.members()) {
        if (const auto candidate = visibilityCandidateForSymbol(data,
                                                                 source_manager,
                                                                 member,
                                                                 SnapshotVisibilityOrigin::Local)) {
            appendVisibilityCandidate(result.candidates, *candidate);
        }
    }
    if (const auto* imports = scope.getWildcardImportData()) {
        for (const auto* wildcard : imports->wildcardImports) {
            const auto* package = wildcard == nullptr ? nullptr : wildcard->getPackage();
            if (package == nullptr) {
                continue;
            }
            const auto package_it = data.package_visibility_by_name.find(std::string(package->name));
            if (package_it == data.package_visibility_by_name.end()) {
                continue;
            }
            for (auto candidate : package_it->second.candidates) {
                candidate.origin = SnapshotVisibilityOrigin::WildcardImport;
                appendVisibilityCandidate(result.candidates, std::move(candidate));
            }
        }
    }
    std::sort(result.candidates.begin(), result.candidates.end(), visibilityCandidateLess);
    data.scope_visibility_by_uri[result.uri].push_back(std::move(result));
}

void buildDocumentVisibilityIndexes(SnapshotData& data, const slang::SourceManager& source_manager) {
    data.document_visibility_by_uri.clear();

    std::vector<const slang::ast::Symbol*> imports;
    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (indexed.symbol == nullptr ||
            (indexed.symbol->kind != slang::ast::SymbolKind::ExplicitImport &&
             indexed.symbol->kind != slang::ast::SymbolKind::WildcardImport)) {
            continue;
        }
        imports.push_back(indexed.symbol);
    }

    for (const auto* symbol : imports) {
        const auto location = declarationLocationForSymbol(source_manager, *symbol);
        if (!location.has_value() || location->uri.empty()) {
            continue;
        }
        auto& candidates = data.document_visibility_by_uri[location->uri];
        if (symbol->kind == slang::ast::SymbolKind::ExplicitImport) {
            if (const auto candidate = visibilityCandidateForSymbol(
                    data,
                    source_manager,
                    *symbol,
                    SnapshotVisibilityOrigin::ExplicitImport)) {
                appendVisibilityCandidate(candidates, *candidate);
            }
            continue;
        }

        const auto& wildcard = symbol->as<slang::ast::WildcardImportSymbol>();
        const auto* package = wildcard.getPackage();
        if (package == nullptr) {
            continue;
        }
        const auto package_it = data.package_visibility_by_name.find(std::string(package->name));
        if (package_it == data.package_visibility_by_name.end()) {
            continue;
        }
        for (auto candidate : package_it->second.candidates) {
            candidate.origin = SnapshotVisibilityOrigin::WildcardImport;
            appendVisibilityCandidate(candidates, std::move(candidate));
        }
    }

    for (auto& [_, candidates] : data.document_visibility_by_uri) {
        std::sort(candidates.begin(), candidates.end(), visibilityCandidateLess);
    }
}
bool isWorkspaceVisibilityKind(std::string_view kind) {
    return kind == "Definition" || kind == "Package" || kind == "ClassType" ||
           kind == "EnumType" || kind == "TypeAlias" || kind == "ForwardingTypedef" ||
           kind == "NetType" || kind == "Interface";
}

void buildScopeVisibilityIndexes(SnapshotData& data, const slang::SourceManager& source_manager) {
    data.scope_visibility_by_uri.clear();
    data.package_visibility_by_name.clear();
    data.workspace_visibility.clear();
    data.module_definition_ids_by_name.clear();

    for (const auto* package : data.compilation->getPackages()) {
        if (package == nullptr || package->name.empty()) {
            continue;
        }
        SnapshotPackageVisibility result;
        result.package_name = std::string(package->name);
        const auto location = declarationLocationForSymbol(source_manager, *package);
        if (!location.has_value() || location->uri.empty()) {
            continue;
        }
        result.uri = location->uri;
        std::set<std::string> visiting;
        collectPackageVisibility(data, source_manager, *package, visiting, result);
        std::sort(result.candidates.begin(), result.candidates.end(), visibilityCandidateLess);
        std::sort(result.exported_packages.begin(), result.exported_packages.end());
        result.exported_packages.erase(std::unique(result.exported_packages.begin(),
                                                   result.exported_packages.end()),
                                       result.exported_packages.end());
        data.package_visibility_by_name[result.package_name] = std::move(result);
    }

    std::set<const slang::ast::Scope*> scopes;
    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (indexed.symbol == nullptr) {
            continue;
        }
        if (const auto* scope = indexed.symbol->as_if<slang::ast::Scope>()) {
            scopes.insert(scope);
        }
        if (const auto* parent = indexed.symbol->getParentScope()) {
            scopes.insert(parent);
        }
    }
    for (const auto* scope : scopes) {
        appendScopeVisibility(data, source_manager, *scope);
    }
    buildDocumentVisibilityIndexes(data, source_manager);

    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (!isWorkspaceVisibilityKind(indexed.identity.kind)) {
            continue;
        }
        appendVisibilityCandidate(data.workspace_visibility,
                                  SnapshotVisibilityCandidate{.identity = indexed.identity,
                                                              .type_display = indexed.type_display,
                                                              .value_display = indexed.value_display,
                                                              .origin = SnapshotVisibilityOrigin::Workspace});
    }
    std::sort(data.workspace_visibility.begin(),
              data.workspace_visibility.end(),
              workspaceVisibilityCandidateLess);
    for (const auto& candidate : data.workspace_visibility) {
        if (candidate.identity.kind == "Definition") {
            data.module_definition_ids_by_name.try_emplace(candidate.identity.name,
                                                            candidate.identity.stable_id);
        }
    }
    for (auto& [_, scope_views] : data.scope_visibility_by_uri) {
        std::sort(scope_views.begin(), scope_views.end(), [](const auto& left, const auto& right) {
            if (left.lexical_depth != right.lexical_depth) {
                return left.lexical_depth > right.lexical_depth;
            }
            if (!sameRange(left.range, right.range)) {
                return rangeLessWideFirst(right.range, left.range);
            }
            return left.stable_id < right.stable_id;
        });
    }
}

void indexSymbolReferences(SnapshotData& data,
                           const slang::SourceManager& source_manager,
                           const slang::ast::Expression& expression) {
    expression.visitSymbolReferences([&](const slang::ast::Expression& reference_expression,
                                          const slang::ast::Symbol& symbol) {
        const auto id_it = data.ids_by_symbol.find(&symbol);
        if (id_it == data.ids_by_symbol.end()) return;

        auto location = locationForSourceRange(source_manager, reference_expression.sourceRange);
        if (!location.has_value()) {
            return;
        }

        insertReference(data,
                        id_it->second,
                        std::string(symbol.name),
                        *location,
                        SemanticReferenceRole::Read);
    });
}

void indexModuleInstanceBinding(SnapshotData& data,
                                const slang::SourceManager& source_manager,
                                const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                const slang::ast::InstanceSymbol& instance) {
    const auto instance_location = declarationLocationForSymbol(source_manager, instance);
    if (!instance_location.has_value()) {
        return;
    }

    const auto& definition = instance.getDefinition();
    insertSymbol(data, source_manager, definition);
    const auto definition_location = declarationLocationForSymbol(source_manager, definition);
    if (!definition_location.has_value()) {
        return;
    }
    const auto definition_id = symbolStableId(source_manager, definition, *definition_location);
    const auto instance_id = instanceStableId(source_manager, instance, *instance_location);
    const auto document_it = documents.find(instance_location->uri);
    const auto* document = document_it == documents.end() ? nullptr : &document_it->second;
    auto cell = schematicCellForAstInstance(source_manager, instance);
    if (!cell.has_value()) {
        return;
    }
    if (instance.name == definition.name && sameLocation(*instance_location, *definition_location)) {
        return;
    }

    auto instance_range = cell->range;
    auto module_selection_range = cell->range;
    auto type_display = cell->type;
    if (document != nullptr) {
        if (auto module_range = identifierRangeByName(document->text, cell->range, cell->type)) {
            module_selection_range = *module_range;
        }
    }

    auto& instances = data.module_instances_by_uri[instance_location->uri];
    for (auto& module_instance : instances) {
        if (!module_instance.instance_stable_id.empty() &&
            module_instance.instance_stable_id != instance_id) {
            continue;
        }
        if (module_instance.instance_name == instance.name && module_instance.uri == instance_location->uri &&
            (rangeContainsRange(module_instance.range, instance_location->range) ||
             rangesOverlapOrTouch(module_instance.selection_range, instance_location->range))) {
            module_instance.module_name = std::string(definition.name);
            module_instance.instance_stable_id = instance_id;
            module_instance.type_display = type_display;
            module_instance.target_stable_id = definition_id;
            return;
        }
    }

    instances.push_back(SnapshotModuleInstance{.module_name = std::string(definition.name),
                                               .instance_name = std::string(instance.name),
                                               .instance_stable_id = instance_id,
                                               .type_display = std::move(type_display),
                                               .target_stable_id = definition_id,
                                               .uri = instance_location->uri,
                                               .range = instance_range,
                                               .selection_range = instance_location->range,
                                               .module_selection_range = module_selection_range,
                                               .port_connections = {},
                                               .parameter_connections = {}});
    data.selection_ranges_by_uri[instance_location->uri].push_back(instance_range);
    data.selection_ranges_by_uri[instance_location->uri].push_back(instance_location->range);
    data.selection_ranges_by_uri[instance_location->uri].push_back(module_selection_range);
}

void upsertModuleInstanceCandidate(SnapshotData& data,
                                   std::string module_name,
                                   std::string instance_name,
                                   SemanticLocation instance_location,
                                   ParseRange range,
                                   ParseRange module_selection_range) {
    auto& instances = data.module_instances_by_uri[instance_location.uri];
    const auto duplicate = std::find_if(instances.begin(),
                                        instances.end(),
                                        [&](const SnapshotModuleInstance& existing) {
                                            return existing.instance_name == instance_name &&
                                                   sameLocation(SemanticLocation{.uri = existing.uri,
                                                                                 .range = existing.selection_range},
                                                                instance_location);
                                        });
    if (duplicate != instances.end()) {
        if (duplicate->target_stable_id.empty()) {
            duplicate->module_name = std::move(module_name);
            duplicate->type_display = duplicate->module_name;
            duplicate->range = range;
            duplicate->module_selection_range = module_selection_range;
        }
        return;
    }

    const auto type_display = module_name;
    instances.push_back(SnapshotModuleInstance{.module_name = std::move(module_name),
                                               .instance_name = std::move(instance_name),
                                               .instance_stable_id = {},
                                               .type_display = type_display,
                                               .target_stable_id = {},
                                               .uri = instance_location.uri,
                                               .range = range,
                                               .selection_range = instance_location.range,
                                               .module_selection_range = module_selection_range,
                                               .port_connections = {},
                                               .parameter_connections = {}});
    data.selection_ranges_by_uri[instance_location.uri].push_back(range);
    data.selection_ranges_by_uri[instance_location.uri].push_back(instance_location.range);
    data.selection_ranges_by_uri[instance_location.uri].push_back(module_selection_range);
}

void upsertModuleDeclarationCandidate(SnapshotData& data,
                                      const slang::SourceManager& source_manager,
                                      const slang::syntax::ModuleDeclarationSyntax& declaration) {
    if (declaration.header == nullptr) {
        return;
    }
    const auto name_location = locationForSyntaxToken(source_manager, declaration.header->name);
    const auto declaration_location = locationForSourceRange(source_manager, declaration.sourceRange());
    if (!name_location.has_value() || !declaration_location.has_value()) {
        return;
    }
    const auto name = std::string(declaration.header->name.valueText());
    if (name.empty()) {
        return;
    }

    ModuleDefinition candidate;
    candidate.name = name;
    candidate.kind = declaration.kind == slang::syntax::SyntaxKind::InterfaceDeclaration ? "interface"
                                                                                         : "module";
    candidate.range = declaration_location->range;
    candidate.selection_range = name_location->range;
    data.modules_by_name.try_emplace(name, candidate);
    data.module_uris_by_name.try_emplace(name, declaration_location->uri);
    const auto entry_it = std::find_if(data.module_entries.begin(),
                                       data.module_entries.end(),
                                       [&](const SnapshotModuleEntry& entry) {
                                           return entry.definition.name == name;
                                       });
    if (entry_it == data.module_entries.end()) {
        data.module_entries.push_back(SnapshotModuleEntry{.uri = declaration_location->uri,
                                                          .definition = std::move(candidate)});
    }
    data.selection_ranges_by_uri[declaration_location->uri].push_back(declaration_location->range);
    data.selection_ranges_by_uri[declaration_location->uri].push_back(name_location->range);
}

void collectSyntaxContinuousAssignmentsForDefinition(
    SnapshotData& data,
    const slang::SourceManager& source_manager,
    const std::unordered_map<std::string, SemanticEngineDocument>& documents,
    const slang::syntax::ModuleDeclarationSyntax& declaration) {
    const auto declaration_location = locationForSourceRange(source_manager, declaration.sourceRange());
    if (!declaration_location.has_value()) {
        return;
    }

    const auto document_it = documents.find(declaration_location->uri);
    const auto* document = document_it == documents.end() ? nullptr : &document_it->second;
    if (document == nullptr) {
        return;
    }

    auto visitor = slang::syntax::makeSyntaxVisitor(
        [&](auto& self, const slang::syntax::ContinuousAssignSyntax& node) {
            for (const auto* expression : node.assignments) {
                if (expression == nullptr ||
                    expression->kind != slang::syntax::SyntaxKind::AssignmentExpression) {
                    continue;
                }
                const auto& assignment =
                    expression->as<slang::syntax::BinaryExpressionSyntax>();
                const auto assignment_location =
                    locationForSourceRange(source_manager, expression->sourceRange());
                if (!assignment_location.has_value()) {
                    continue;
                }
                const auto left_range =
                    sourceRangeForSourceRange(source_manager, assignment.left->sourceRange());
                const auto right_range =
                    sourceRangeForSourceRange(source_manager, assignment.right->sourceRange());
                auto left_expression = textForRangeOrEmpty(document, left_range);
                auto right_expression = textForRangeOrEmpty(document, right_range);
                if (!isSimpleSystemVerilogIdentifier(left_expression) ||
                    !isSimpleSystemVerilogIdentifier(right_expression)) {
                    continue;
                }
                appendAssignmentEdgeSeed(data,
                                         SnapshotAssignmentEdgeSeed{
                                             .uri = assignment_location->uri,
                                             .scope_range = declaration_location->range,
                                             .assignment_range = assignment_location->range,
                                             .left_range = left_range,
                                             .right_range = right_range,
                                             .left_expression = left_expression,
                                             .right_expression = right_expression,
                                             .left_symbol_ids = {},
                                             .left_symbol_names = {left_expression},
                                             .sink_slice = {},
                                             .data_sources = {SnapshotConeDataSourceSeed{
                                                 .range = right_range,
                                                 .expression = right_expression,
                                                 .slice_kind = SnapshotConeSliceKind::Whole,
                                                 .source_slice = {},
                                                 .sink_slice = {},
                                                 .source_symbol_ids = {},
                                                 .source_symbol_names = {right_expression},
                                                 .unresolved = false}},
                                             .control_sources = {}});
            }
            self.visitDefault(node);
        });
    declaration.visit(visitor);
}

void collectSyntaxModuleCandidates(SnapshotData& data,
                                   const slang::SourceManager& source_manager,
                                   const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    for (const auto& tree : data.syntax_trees) {
        if (!tree) {
            continue;
        }
        auto visitor = slang::syntax::makeSyntaxVisitor(
            [&](auto& self, const slang::syntax::ModuleDeclarationSyntax& node) {
                upsertModuleDeclarationCandidate(data, source_manager, node);
                collectSyntaxContinuousAssignmentsForDefinition(data,
                                                                source_manager,
                                                                documents,
                                                                node);
                self.visitDefault(node);
            },
            [&](auto& self, const slang::syntax::HierarchyInstantiationSyntax& node) {
                const auto module_location = locationForSyntaxToken(source_manager, node.type);
                if (!module_location.has_value()) {
                    self.visitDefault(node);
                    return;
                }
                const auto module_name = std::string(node.type.valueText());
                if (module_name.empty()) {
                    self.visitDefault(node);
                    return;
                }
                for (const auto* instance : node.instances) {
                    if (instance == nullptr || instance->decl == nullptr) {
                        continue;
                    }
                    const auto instance_location = locationForSyntaxToken(source_manager,
                                                                          instance->decl->name);
                    if (!instance_location.has_value()) {
                        continue;
                    }
                    if (node.parameters != nullptr) {
                        data.parameter_override_syntax_facts.push_back(
                            SnapshotParameterOverrideSyntaxFact{
                                .syntax = node.parameters,
                                .uri = instance_location->uri,
                                .module_name = module_name,
                                .instance_name = std::string(instance->decl->name.valueText()),
                                .instance_range = instance_location->range});
                    }
                    data.port_connection_syntax_facts.push_back(
                        SnapshotPortConnectionSyntaxFact{
                            .syntax = instance,
                            .uri = instance_location->uri,
                            .module_name = module_name,
                            .instance_name = std::string(instance->decl->name.valueText()),
                            .instance_range = instance_location->range});
                    const auto statement_location = locationForSourceRange(source_manager,
                                                                           node.sourceRange());
                    const auto instance_range = statement_location.has_value()
                                                    ? statement_location->range
                                                    : sourceRangeForSourceRange(source_manager,
                                                                                instance->sourceRange());
                    upsertModuleInstanceCandidate(data,
                                                  module_name,
                                                  std::string(instance->decl->name.valueText()),
                                                  *instance_location,
                                                  instance_range,
                                                  module_location->range);
                }
                self.visitDefault(node);
            });
        tree->root().visit(visitor);
    }
}

std::vector<ModuleInstantiation> instancesForModule(const SnapshotData& data,
                                                    const ModuleDefinition& definition,
                                                    std::string_view uri) {
    std::vector<ModuleInstantiation> result;
    const auto instances_it = data.module_instances_by_uri.find(std::string(uri));
    if (instances_it == data.module_instances_by_uri.end()) {
        return result;
    }
    for (const auto& instance : instances_it->second) {
        if (!rangeContainsRange(definition.range, instance.selection_range)) {
            continue;
        }
        const auto duplicate = std::any_of(result.begin(),
                                           result.end(),
                                           [&](const ModuleInstantiation& existing) {
                                               return rangeKey(existing.selection_range) ==
                                                      rangeKey(instance.selection_range);
                                           });
        if (duplicate) {
            continue;
        }
        result.push_back(ModuleInstantiation{.module_name = instance.module_name,
                                             .instance_name = instance.instance_name,
                                             .range = instance.range,
                                             .selection_range = instance.selection_range,
                                             .module_selection_range = instance.module_selection_range});
    }
    return result;
}

std::string schematicKindForInstance(const SnapshotData& data, std::string_view module_name) {
    const auto module_it = data.modules_by_name.find(std::string(module_name));
    if (module_it == data.modules_by_name.end()) {
        return "module";
    }
    auto kind = module_it->second.kind;
    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return kind.find("interface") != std::string::npos ? "interface" : "module";
}

void sortSchematicCells(ModuleSchematic& schematic) {
    std::sort(schematic.cells.begin(), schematic.cells.end(), [](const SchematicCell& lhs, const SchematicCell& rhs) {
        const auto lhs_key = rangeKey(lhs.selection_range);
        const auto rhs_key = rangeKey(rhs.selection_range);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        if (lhs.type != rhs.type) {
            return lhs.type < rhs.type;
        }
        return lhs.kind < rhs.kind;
    });
}

void appendMissingSchematicCellsForInstances(const SnapshotData& data, SemanticModuleSignature& signature) {
    if (signature.uri.empty()) {
        return;
    }
    const auto instances_it = data.module_instances_by_uri.find(signature.uri);
    if (instances_it == data.module_instances_by_uri.end()) {
        return;
    }

    bool inserted = false;
    for (const auto& instance : instances_it->second) {
        if (!rangeContainsRange(signature.definition.range, instance.selection_range)) {
            continue;
        }
        const auto duplicate = std::any_of(signature.schematic.cells.begin(),
                                           signature.schematic.cells.end(),
                                           [&](const SchematicCell& cell) {
                                               return cell.name == instance.instance_name &&
                                                      cell.type == instance.module_name &&
                                                      rangeKey(cell.selection_range) ==
                                                          rangeKey(instance.selection_range);
                                           });
        if (duplicate) {
            continue;
        }

        signature.schematic.cells.push_back(SchematicCell{.id = instance.instance_name,
                                                          .name = instance.instance_name,
                                                          .type = instance.module_name,
                                                          .kind = schematicKindForInstance(data,
                                                                                           instance.module_name),
                                                          .range = instance.range,
                                                          .selection_range = instance.selection_range,
                                                          .connections = instance.port_connections});
        inserted = true;
    }
    if (inserted) {
        sortSchematicCells(signature.schematic);
    }
}

void updateModuleInstanceTargets(SnapshotData& data) {
    for (auto& [_, instances] : data.module_instances_by_uri) {
        for (auto& module_instance : instances) {
            if (!module_instance.target_stable_id.empty()) {
                continue;
            }
            const auto target_id = findUniqueModuleDefinitionSymbolId(data, module_instance.module_name);
            if (target_id.has_value()) {
                module_instance.target_stable_id = *target_id;
            }
        }
    }
}

void addModuleInstantiationReferences(SnapshotData& data,
                                       const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    for (const auto& [document_uri, instances] : data.module_instances_by_uri) {
        if (!documents.contains(document_uri)) {
            continue;
        }
        for (const auto& instance : instances) {
            const auto target_id = !instance.target_stable_id.empty()
                                       ? std::optional<std::string>{instance.target_stable_id}
                                       : findUniqueModuleDefinitionSymbolId(data, instance.module_name);
            if (!target_id.has_value()) {
                continue;
            }
            insertReference(data,
                            *target_id,
                            instance.module_name,
                            SemanticLocation{.uri = document_uri, .range = instance.module_selection_range},
                            SemanticReferenceRole::Instance);
        }
    }
}

void sortModuleInstances(SnapshotData& data) {
    for (auto& [_, instances] : data.module_instances_by_uri) {
        std::sort(instances.begin(), instances.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.uri != rhs.uri) {
                return lhs.uri < rhs.uri;
            }
            if (lhs.instance_stable_id != rhs.instance_stable_id) {
                if (lhs.instance_stable_id.empty() != rhs.instance_stable_id.empty()) {
                    return !lhs.instance_stable_id.empty();
                }
                return lhs.instance_stable_id < rhs.instance_stable_id;
            }
            const auto lhs_key = rangeKey(lhs.selection_range);
            const auto rhs_key = rangeKey(rhs.selection_range);
            if (lhs_key != rhs_key) {
                return lhs_key < rhs_key;
            }
            if (lhs.target_stable_id.empty() != rhs.target_stable_id.empty()) {
                return !lhs.target_stable_id.empty();
            }
            if (lhs.instance_name != rhs.instance_name) {
                return lhs.instance_name < rhs.instance_name;
            }
            if (lhs.module_name != rhs.module_name) {
                return lhs.module_name < rhs.module_name;
            }
            return locationLess(SemanticLocation{.uri = lhs.uri, .range = lhs.range},
                                SemanticLocation{.uri = rhs.uri, .range = rhs.range});
        });
        instances.erase(std::unique(instances.begin(),
                                    instances.end(),
                                    [](const auto& lhs, const auto& rhs) {
                                        if (!lhs.instance_stable_id.empty() &&
                                            !rhs.instance_stable_id.empty()) {
                                            return lhs.instance_stable_id == rhs.instance_stable_id;
                                        }
                                        return lhs.uri == rhs.uri &&
                                               rangeKey(lhs.selection_range) == rangeKey(rhs.selection_range);
                                     }),
                        instances.end());
    }
}

void attachInstancesToModuleDefinitions(SnapshotData& data) {
    for (auto& [name, signature] : data.ast_module_signatures_by_name) {
        signature.definition.instances = instancesForModule(data, signature.definition, signature.uri);
        appendMissingSchematicCellsForInstances(data, signature);
        data.modules_by_name[name] = signature.definition;
        const auto entry_it = std::find_if(data.module_entries.begin(),
                                           data.module_entries.end(),
                                           [&](const SnapshotModuleEntry& entry) {
                                               return entry.definition.name == name;
                                           });
        if (entry_it != data.module_entries.end()) {
            entry_it->definition = signature.definition;
        }
    }

    for (auto& [name, definition] : data.modules_by_name) {
        if (data.ast_module_signatures_by_name.contains(name)) {
            continue;
        }
        const auto uri_it = data.module_uris_by_name.find(name);
        if (uri_it == data.module_uris_by_name.end()) {
            continue;
        }
        definition.instances = instancesForModule(data, definition, uri_it->second);
        const auto entry_it = std::find_if(data.module_entries.begin(),
                                           data.module_entries.end(),
                                           [&](const SnapshotModuleEntry& entry) {
                                               return entry.definition.name == name;
                                           });
        if (entry_it != data.module_entries.end()) {
            entry_it->definition = definition;
        }
    }
}

void buildModuleCallEdgeIndex(SnapshotData& data) {
    auto& index = data.module_call_edge_index;
    index = {};

    for (const auto& entry : data.module_entries) {
        const auto definition_id = data.module_definition_ids_by_name.find(entry.definition.name);
        if (definition_id == data.module_definition_ids_by_name.end()) {
            continue;
        }
        const auto [item_it, inserted] = index.items_by_id.emplace(
            definition_id->second,
            SnapshotModuleCallHierarchyItem{.id = definition_id->second,
                                            .name = entry.definition.name,
                                            .kind = entry.definition.kind,
                                            .uri = entry.uri,
                                            .range = entry.definition.range,
                                            .selection_range = entry.definition.selection_range});
        if (inserted) {
            index.items_by_uri[entry.uri].push_back(
                SnapshotModuleCallHierarchyRange{.range = item_it->second.range, .item_id = item_it->first});
        }
    }

    for (const auto& [uri, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            if (instance.target_stable_id.empty() ||
                !index.items_by_id.contains(instance.target_stable_id)) {
                continue;
            }

            const SnapshotModuleCallHierarchyItem* caller = nullptr;
            for (const auto& [_, candidate] : index.items_by_id) {
                if (candidate.uri != uri ||
                    !rangeContainsRange(candidate.range, instance.selection_range)) {
                    continue;
                }
                if (caller == nullptr || rangeLengthScore(candidate.range) < rangeLengthScore(caller->range) ||
                    (rangeLengthScore(candidate.range) == rangeLengthScore(caller->range) &&
                     candidate.id < caller->id)) {
                    caller = &candidate;
                }
            }
            if (caller == nullptr) {
                continue;
            }

            const auto instance_id = !instance.instance_stable_id.empty()
                                         ? instance.instance_stable_id
                                         : caller->id + "|instance|" + uri + "|" +
                                               rangeKey(instance.selection_range) + "|" +
                                               instance.instance_name;
            index.edges.push_back(SnapshotModuleCallEdge{.caller_item_id = caller->id,
                                                         .callee_item_id = instance.target_stable_id,
                                                         .instance_id = instance_id,
                                                         .uri = uri,
                                                         .range = instance.range,
                                                         .selection_range = instance.module_selection_range});
            index.items_by_uri[uri].push_back(SnapshotModuleCallHierarchyRange{
                .range = instance.selection_range, .item_id = instance.target_stable_id});
            index.items_by_uri[uri].push_back(SnapshotModuleCallHierarchyRange{
                .range = instance.module_selection_range, .item_id = instance.target_stable_id});
        }
    }

    std::sort(index.edges.begin(), index.edges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.caller_item_id != rhs.caller_item_id) {
            return lhs.caller_item_id < rhs.caller_item_id;
        }
        if (lhs.callee_item_id != rhs.callee_item_id) {
            return lhs.callee_item_id < rhs.callee_item_id;
        }
        if (lhs.uri != rhs.uri) {
            return lhs.uri < rhs.uri;
        }
        if (rangeKey(lhs.selection_range) != rangeKey(rhs.selection_range)) {
            return rangeKey(lhs.selection_range) < rangeKey(rhs.selection_range);
        }
        return lhs.instance_id < rhs.instance_id;
    });
    index.edges.erase(std::unique(index.edges.begin(),
                                  index.edges.end(),
                                  [](const auto& lhs, const auto& rhs) {
                                      return lhs.caller_item_id == rhs.caller_item_id &&
                                             lhs.callee_item_id == rhs.callee_item_id &&
                                             lhs.instance_id == rhs.instance_id;
                                  }),
                      index.edges.end());

    for (size_t edge_index = 0; edge_index < index.edges.size(); ++edge_index) {
        const auto& edge = index.edges[edge_index];
        index.edges_by_caller_item_id[edge.caller_item_id].push_back(edge_index);
        index.edges_by_callee_item_id[edge.callee_item_id].push_back(edge_index);
    }
    for (auto& [_, ranges] : index.items_by_uri) {
        std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameRange(lhs.range, rhs.range)) {
                return rangeKey(lhs.range) < rangeKey(rhs.range);
            }
            return lhs.item_id < rhs.item_id;
        });
        ranges.erase(std::unique(ranges.begin(),
                                 ranges.end(),
                                 [](const auto& lhs, const auto& rhs) {
                                     return lhs.item_id == rhs.item_id && sameRange(lhs.range, rhs.range);
                                 }),
                     ranges.end());
    }
}

std::string signaturePortStableId(std::string_view uri,
                                  std::string_view module_name,
                                  const SchematicPort& port) {
    return std::string(uri) + "|" + std::string(module_name) + "|Port|" + port.name + "|" +
           std::to_string(port.selection_range.start_line) + ":" +
           std::to_string(port.selection_range.start_character);
}

bool hasIndexedSymbolAtLocation(const SnapshotData& data,
                                std::string_view uri,
                                std::string_view name,
                                const ParseRange& range) {
    return std::any_of(data.symbols_by_id.begin(),
                       data.symbols_by_id.end(),
                       [&](const auto& entry) {
                           const auto& identity = entry.second.identity;
                           return identity.location.uri == uri && identity.name == name &&
                                  sameRange(identity.location.range, range);
                       });
}

void addMissingSignaturePortSymbols(SnapshotData& data) {
    for (const auto& [module_name, signature] : data.ast_module_signatures_by_name) {
        if (signature.uri.empty()) {
            continue;
        }
        for (const auto& port : signature.definition.port_details) {
            if (port.name.empty() ||
                hasIndexedSymbolAtLocation(data, signature.uri, port.name, port.selection_range)) {
                continue;
            }
            const auto stable_id = signaturePortStableId(signature.uri, module_name, port);
            data.symbols_by_id.emplace(
                stable_id,
                SnapshotIndexedSymbol{.identity = SemanticSymbolIdentity{
                                          .stable_id = stable_id,
                                          .name = port.name,
                                          .kind = "Port",
                                          .location = SemanticLocation{.uri = signature.uri,
                                                                       .range = port.selection_range}},
                                      .symbol = nullptr,
                                      .type_display = port.width_text,
                                      .value_display = {}});
        }
    }
}

void addDeclarationReferences(SnapshotData& data) {
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        insertReference(data,
                        stable_id,
                        indexed_symbol.identity.name,
                        indexed_symbol.identity.location,
                        SemanticReferenceRole::Declaration);
    }
}

struct SemanticIndexVisitor
    : slang::ast::ASTVisitor<SemanticIndexVisitor, slang::ast::VisitFlags::AllGood> {
    struct ActiveModuleScope {
        std::string uri;
        ParseRange range;
    };

    SnapshotData& data;
    const slang::SourceManager& source_manager;
    const std::unordered_map<std::string, SemanticEngineDocument>& documents;
    std::vector<ActiveModuleScope> module_scopes;
    std::vector<SnapshotConeControlSourceSeed> control_sources;

    SemanticIndexVisitor(SnapshotData& data,
                         const slang::SourceManager& source_manager,
                         const std::unordered_map<std::string, SemanticEngineDocument>& documents) :
        data(data),
        source_manager(source_manager),
        documents(documents) {}

    bool isClassOwnedSubroutine(const slang::ast::Symbol& symbol) const {
        if (symbol.kind != slang::ast::SymbolKind::Subroutine &&
            symbol.kind != slang::ast::SymbolKind::MethodPrototype) {
            return false;
        }
        const auto* parent_scope = symbol.getParentScope();
        return parent_scope != nullptr &&
               parent_scope->asSymbol().kind == slang::ast::SymbolKind::ClassType;
    }

    const SemanticEngineDocument* documentFor(const slang::ast::Expression& expression) const {
        const auto location = locationForSourceRange(source_manager, expression.sourceRange);
        if (!location.has_value()) return nullptr;
        const auto document = documents.find(location->uri);
        return document == documents.end() ? nullptr : &document->second;
    }

    void appendControlSource(const slang::ast::Expression& expression,
                             SnapshotConeControlOrigin origin) {
        if (auto source = controlSourceForExpression(documentFor(expression),
                                                     source_manager,
                                                     expression)) {
            source->source_symbol_ids = directSymbolIdsForExpression(data, source_manager, expression);
            source->source_symbol_names = coneSymbolNamesForExpression(documentFor(expression),
                                                                         source_manager,
                                                                         expression);
            source->origin = origin;
            source->unresolved = source->source_symbol_ids.empty() &&
                                 unwrapImplicitConversions(expression).kind ==
                                     slang::ast::ExpressionKind::Invalid;
            const auto duplicate = std::any_of(control_sources.begin(), control_sources.end(), [&](const auto& value) {
                return sameRange(value.range, source->range) && value.expression == source->expression &&
                       value.slice_kind == source->slice_kind && value.origin == source->origin &&
                       value.source_symbol_ids == source->source_symbol_ids &&
                       value.source_symbol_names == source->source_symbol_names &&
                       value.unresolved == source->unresolved;
            });
            if (!duplicate) control_sources.push_back(*source);
        }
    }

    void appendAssignmentSeed(const slang::ast::AssignmentExpression& expression) {
        if (module_scopes.empty()) return;
        const auto location = locationForSourceRange(source_manager, expression.sourceRange);
        if (!location.has_value()) return;
        const auto& scope = module_scopes.back();
        if (scope.uri != location->uri) return;
        auto controls = control_sources;
        std::vector<SnapshotConeDataSourceSeed> data_sources;
        const auto sink_slice = sliceFactForExpression(expression.left());
        collectConeSourcesForExpression(data,
                                        source_manager,
                                        expression.right(),
                                        [&](const slang::ast::Expression& source) {
                                            return documentFor(source);
                                        },
                                        sink_slice,
                                        data_sources,
                                        controls);
        normalizeConeDataSources(data_sources);
        normalizeConeControlSources(controls);
        appendAssignmentEdgeSeed(data,
                                 SnapshotAssignmentEdgeSeed{
                                     .uri = location->uri,
                                     .scope_range = scope.range,
                                     .assignment_range = location->range,
                                     .left_range = sourceRangeForSourceRange(source_manager,
                                                                            expression.left().sourceRange),
                                     .right_range = sourceRangeForSourceRange(source_manager,
                                                                             expression.right().sourceRange),
                                     .left_expression = expressionText(documentFor(expression),
                                                                       source_manager,
                                                                       expression.left()),
                                     .right_expression = expressionText(documentFor(expression),
                                                                        source_manager,
                                                                        expression.right()),
                                     .left_symbol_ids = directSymbolIdsForExpression(data,
                                                                                     source_manager,
                                                                                     expression.left()),
                                     .left_symbol_names = coneSymbolNamesForExpression(documentFor(expression),
                                                                                       source_manager,
                                                                                       expression.left()),
                                     .sink_slice = sink_slice,
                                     .data_sources = std::move(data_sources),
                                     .control_sources = std::move(controls)});
    }

    void handle(const slang::ast::GenerateBlockArraySymbol& symbol) {
        insertSymbol(data, source_manager, symbol);
        for (const auto* entry : symbol.entries) {
            if (entry != nullptr) {
                entry->visit(*this);
            }
        }
    }

    void handle(const slang::ast::InstanceBodySymbol& symbol) {
        insertSymbol(data, source_manager, symbol);
        upsertAstModuleSignature(data, source_manager, documents, symbol);
        const auto signature = data.ast_module_signatures_by_name.find(std::string(symbol.getDefinition().name));
        const bool has_scope = signature != data.ast_module_signatures_by_name.end() && !signature->second.uri.empty();
        if (has_scope) {
            module_scopes.push_back(ActiveModuleScope{.uri = signature->second.uri,
                                                      .range = signature->second.definition.range});
        }
        this->visitDefault(symbol);
        if (has_scope) module_scopes.pop_back();
    }

    void handle(const slang::ast::ConditionalStatement& statement) {
        const auto previous_size = control_sources.size();
        for (const auto& condition : statement.conditions) {
            appendControlSource(*condition.expr, SnapshotConeControlOrigin::ConditionalStatement);
        }
        this->visitDefault(statement);
        control_sources.resize(previous_size);
    }

    void handle(const slang::ast::CaseStatement& statement) {
        const auto previous_size = control_sources.size();
        appendControlSource(statement.expr, SnapshotConeControlOrigin::CaseStatement);
        for (const auto& item : statement.items) {
            for (const auto* expression : item.expressions) {
                if (expression != nullptr) {
                    appendControlSource(*expression, SnapshotConeControlOrigin::CaseStatement);
                }
            }
        }
        this->visitDefault(statement);
        control_sources.resize(previous_size);
    }

    template<typename T>
    void handle(const T& symbol)
        requires std::is_base_of_v<slang::ast::Symbol, T>
    {
        insertSymbol(data, source_manager, symbol);
        if constexpr (std::is_same_v<T, slang::ast::InstanceSymbol>) {
            indexModuleInstanceBinding(data, source_manager, documents, symbol);
        }
        if constexpr (std::is_same_v<T, slang::ast::ContinuousAssignSymbol>) {
            upsertAstContinuousAssignment(data, source_manager, documents, symbol);
        }
        if (isClassOwnedSubroutine(symbol)) {
            return;
        }
        this->visitDefault(symbol);
    }

    template<typename T>
    void handle(const T& expression)
        requires std::is_base_of_v<slang::ast::Expression, T>
    {
        indexSymbolReferences(data, source_manager, expression);
        if constexpr (std::is_same_v<T, slang::ast::AssignmentExpression>) {
            expression.left().visitSymbolReferences(
            [&](const slang::ast::Expression& reference_expression,
                const slang::ast::Symbol& symbol) {
                const auto id_it = data.ids_by_symbol.find(&symbol);
                    const auto location = locationForSourceRange(source_manager,
                                                                 reference_expression.sourceRange);
                    if (id_it != data.ids_by_symbol.end() && location.has_value()) {
                        insertReference(data,
                                        id_it->second,
                                        std::string(symbol.name),
                                        *location,
                                        SemanticReferenceRole::Write);
                    }
                });
        }
        if constexpr (std::is_same_v<T, slang::ast::CallExpression>) {
            addSignatureCall(data, source_manager, expression);
        }
        this->visitDefault(expression);
    }

    void handle(const slang::ast::AssignmentExpression& expression) {
        indexSymbolReferences(data, source_manager, expression);
        expression.left().visitSymbolReferences(
            [&](const slang::ast::Expression& reference_expression, const slang::ast::Symbol& symbol) {
                const auto id_it = data.ids_by_symbol.find(&symbol);
                const auto location = locationForSourceRange(source_manager, reference_expression.sourceRange);
                if (id_it != data.ids_by_symbol.end() && location.has_value()) {
                    insertReference(data,
                                    id_it->second,
                                    std::string(symbol.name),
                                    *location,
                                    SemanticReferenceRole::Write);
                }
            });
        appendAssignmentSeed(expression);
        this->visitDefault(expression);
    }
};

std::optional<std::string> symbolIdAtReferenceRangeStart(const SnapshotData& data,
                                                         std::string_view uri,
                                                         const ParseRange& range) {
    const auto occurrences = data.reference_occurrences_by_uri.find(std::string(uri));
    if (occurrences == data.reference_occurrences_by_uri.end()) return std::nullopt;
    const auto& indexes = occurrences->second.reference_indexes;
    const auto upper = std::upper_bound(indexes.begin(),
                                        indexes.end(),
                                        std::pair{range.start_line, range.start_character},
                                        [&](const auto& position, size_t index) {
                                            const auto& candidate = data.references[index].location.range;
                                            return positionLess(position.first,
                                                                position.second,
                                                                candidate.start_line,
                                                                candidate.start_character);
                                        });
    std::optional<std::string> best_id;
    for (auto it = upper; it != indexes.begin();) {
        --it;
        const auto& reference = data.references[*it];
        if (containsPosition(reference.location.range, range.start_line, range.start_character)) {
            best_id = reference.stable_id;
            break;
        }
        const auto prefix_index = static_cast<size_t>(std::distance(indexes.begin(), it));
        if (prefix_index == 0 ||
            rangeEndBeforePosition(occurrences->second.prefix_max_end_ranges[prefix_index - 1],
                                   range.start_line,
                                   range.start_character)) {
            break;
        }
    }
    return best_id;
}

std::vector<const SnapshotIndexedReference*> referencesWithinRange(const SnapshotData& data,
                                                                    std::string_view uri,
                                                                    const ParseRange& range) {
    std::vector<const SnapshotIndexedReference*> result;
    const auto occurrences = data.reference_occurrences_by_uri.find(std::string(uri));
    if (occurrences == data.reference_occurrences_by_uri.end()) return result;
    const auto& indexes = occurrences->second.reference_indexes;
    const auto first = std::lower_bound(indexes.begin(),
                                        indexes.end(),
                                        range,
                                        [&](size_t index, const ParseRange& candidate) {
                                            const auto& current = data.references[index].location.range;
                                            return positionLess(current.start_line,
                                                                current.start_character,
                                                                candidate.start_line,
                                                                candidate.start_character);
                                        });
    for (auto it = first; it != indexes.end(); ++it) {
        const auto& reference = data.references[*it];
        if (positionLess(range.end_line,
                         range.end_character,
                         reference.location.range.start_line,
                         reference.location.range.start_character)) {
            break;
        }
        if (!reference.is_declaration && rangeContainsRange(range, reference.location.range)) {
            result.push_back(&reference);
        }
    }
    return result;
}

void buildAssignmentEdges(SnapshotData& data) {
    data.assignment_edges_by_uri.clear();
    data.unresolved_cone_sources.clear();
    std::set<std::string> emitted_edges;
    std::set<std::string> emitted_unresolved_sources;
    const auto resolve_scoped_ast_name = [&](std::string_view uri,
                                             const ParseRange& scope_range,
                                             std::string_view name) -> std::optional<std::string> {
        if (!isSimpleSystemVerilogIdentifier(name)) return std::nullopt;
        const auto symbols = data.graph_symbols_by_uri.find(std::string(uri));
        if (symbols == data.graph_symbols_by_uri.end()) return std::nullopt;
        const auto first = std::lower_bound(symbols->second.begin(),
                                            symbols->second.end(),
                                            scope_range,
                                            [](const SnapshotUriSymbolRangeFact& symbol, const ParseRange& range) {
                                                return rangeStartLess(symbol.range, range);
                                            });
        for (auto it = first; it != symbols->second.end(); ++it) {
            if (rangeStartsAfter(it->range, scope_range)) {
                break;
            }
            const auto indexed = data.symbols_by_id.find(it->stable_id);
            if (indexed != data.symbols_by_id.end() && indexed->second.identity.name == name &&
                rangeContainsRange(scope_range, it->range)) {
                return it->stable_id;
            }
        }
        return std::nullopt;
    };

    for (const auto& seed : data.assignment_edge_seeds) {
        auto left_id = seed.left_symbol_ids.empty()
                           ? symbolIdAtReferenceRangeStart(data, seed.uri, seed.left_range)
                           : std::optional<std::string>{seed.left_symbol_ids.front()};
        if (!left_id.has_value()) {
            for (const auto& name : seed.left_symbol_names) {
                left_id = resolve_scoped_ast_name(seed.uri, seed.scope_range, name);
                if (left_id.has_value()) break;
            }
        }
        if (!left_id.has_value()) {
            const auto left_references = referencesWithinRange(data, seed.uri, seed.left_range);
            const auto write = std::find_if(left_references.begin(), left_references.end(), [](const auto* reference) {
                return reference->role == SemanticReferenceRole::Write && !reference->stable_id.empty();
            });
            if (write != left_references.end()) {
                left_id = (*write)->stable_id;
            }
        }
        if (!left_id.has_value()) {
            const auto symbols = data.graph_symbols_by_uri.find(seed.uri);
            if (symbols != data.graph_symbols_by_uri.end()) {
                const auto first = std::lower_bound(symbols->second.begin(),
                                                    symbols->second.end(),
                                                    seed.left_range,
                                                    [](const SnapshotUriSymbolRangeFact& symbol,
                                                       const ParseRange& range) {
                                                        return rangeStartLess(symbol.range, range);
                                                    });
                for (auto it = first; it != symbols->second.end(); ++it) {
                    if (rangeStartsAfter(it->range, seed.left_range)) break;
                    if (!rangeContainsRange(seed.scope_range, it->range) ||
                        (!rangeContainsRange(seed.left_range, it->range) &&
                         !rangeContainsRange(it->range, seed.left_range))) {
                        continue;
                    }
                    left_id = it->stable_id;
                    break;
                }
            }
        }
        if (!left_id.has_value()) {
            continue;
        }
        markReferenceRole(data,
                          *left_id,
                          seed.uri,
                          seed.left_range,
                          SemanticReferenceRole::Write);

        const auto append_sources = [&](const ParseRange& source_range,
                                        std::string_view expression,
                                        SnapshotConeEdgeKind kind,
                                        SnapshotConeSourceRole role,
                                        SnapshotConeSliceKind slice_kind,
                                        SnapshotConeControlOrigin control_origin,
                                        const SnapshotConeSliceFact& source_slice,
                                        const SnapshotConeSliceFact& sink_slice,
                                        const std::vector<std::string>& direct_symbol_ids,
                                        const std::vector<std::string>& direct_symbol_names) {
            std::set<std::string> emitted_ids;
            const auto append_source = [&](std::string_view source_id, SemanticLocation source_location) {
                if (source_id.empty() || source_id == *left_id || !emitted_ids.insert(std::string(source_id)).second) {
                    return;
                }
                const auto edge_key = seed.uri + "\n" + *left_id + "\n" + std::string(source_id) + "\n" +
                                      std::to_string(seed.assignment_range.start_line) + ":" +
                                      std::to_string(seed.assignment_range.start_character) + "\n" +
                                      std::to_string(static_cast<int>(kind)) + "\n" +
                                      std::to_string(static_cast<int>(role)) + "\n" +
                                      std::to_string(static_cast<int>(control_origin)) + "\n" +
                                      std::to_string(source_location.range.start_line) + ":" +
                                      std::to_string(source_location.range.start_character);
                if (!emitted_edges.insert(edge_key).second) return;
                data.assignment_edges_by_uri[seed.uri].push_back(SnapshotAssignmentEdge{
                    .from_symbol_id = *left_id,
                    .to_symbol_id = std::string(source_id),
                    .location = SemanticLocation{.uri = seed.uri, .range = seed.assignment_range},
                    .target_location = SemanticLocation{.uri = seed.uri, .range = seed.left_range},
                    .expression_location = std::move(source_location),
                    .expression = std::string(expression),
                    .kind = kind,
                    .source_role = role,
                    .slice_kind = slice_kind,
                    .control_origin = control_origin,
                    .source_slice = source_slice,
                    .sink_slice = sink_slice});
            };
            if (!direct_symbol_ids.empty()) {
                for (const auto& source_id : direct_symbol_ids) {
                    append_source(source_id, SemanticLocation{.uri = seed.uri, .range = source_range});
                }
            }
            else if (!direct_symbol_names.empty()) {
                for (const auto& name : direct_symbol_names) {
                    if (const auto source_id = resolve_scoped_ast_name(seed.uri, seed.scope_range, name)) {
                        append_source(*source_id, SemanticLocation{.uri = seed.uri, .range = source_range});
                    }
                }
            }
            else {
                for (const auto* reference : referencesWithinRange(data, seed.uri, source_range)) {
                    append_source(reference->stable_id, reference->location);
                }
            }
        };

        const auto append_unresolved = [&](const ParseRange& source_range,
                                           std::string_view expression,
                                           SnapshotConeSourceRole role,
                                           SnapshotConeControlOrigin control_origin) {
            const auto key = *left_id + "\n" + std::to_string(source_range.start_line) + ":" +
                             std::to_string(source_range.start_character) + "\n" +
                             std::to_string(static_cast<int>(role)) + "\n" +
                             std::to_string(static_cast<int>(control_origin));
            if (!emitted_unresolved_sources.insert(key).second) return;
            data.unresolved_cone_sources.push_back(SnapshotConeUnresolvedSourceFact{
                .from_symbol_id = *left_id,
                .location = SemanticLocation{.uri = seed.uri, .range = seed.assignment_range},
                .expression_location = SemanticLocation{.uri = seed.uri, .range = source_range},
                .expression = std::string(expression),
                .source_role = role,
                .control_origin = control_origin});
        };

        for (const auto& source : seed.data_sources) {
            append_sources(source.range,
                           source.expression,
                           SnapshotConeEdgeKind::Assignment,
                           SnapshotConeSourceRole::Data,
                           source.slice_kind,
                           SnapshotConeControlOrigin::None,
                           source.source_slice,
                           source.sink_slice,
                           source.source_symbol_ids,
                           source.source_symbol_names);
            if (source.source_symbol_ids.empty()) {
                if (source.unresolved) {
                    append_unresolved(source.range,
                                      source.expression,
                                      SnapshotConeSourceRole::Data,
                                      SnapshotConeControlOrigin::None);
                }
            }
        }
        for (const auto& control : seed.control_sources) {
            append_sources(control.range,
                           control.expression,
                           SnapshotConeEdgeKind::ControlDependency,
                           SnapshotConeSourceRole::Control,
                           control.slice_kind,
                           control.origin,
                           control.source_slice,
                           seed.sink_slice,
                           control.source_symbol_ids,
                           control.source_symbol_names);
            if (control.source_symbol_ids.empty()) {
                if (control.unresolved) {
                    append_unresolved(control.range,
                                      control.expression,
                                      SnapshotConeSourceRole::Control,
                                      control.origin);
                }
            }
        }
    }

    for (auto& [_, edges] : data.assignment_edges_by_uri) {
        std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.from_symbol_id != rhs.from_symbol_id) {
                return lhs.from_symbol_id < rhs.from_symbol_id;
            }
            if (lhs.to_symbol_id != rhs.to_symbol_id) {
                return lhs.to_symbol_id < rhs.to_symbol_id;
            }
            return locationLess(lhs.location, rhs.location);
        });
    }
    std::sort(data.unresolved_cone_sources.begin(),
              data.unresolved_cone_sources.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.from_symbol_id,
                                  lhs.expression_location.uri,
                                  lhs.expression_location.range.start_line,
                                  lhs.expression_location.range.start_character,
                                  lhs.source_role,
                                  lhs.control_origin) <
                         std::tie(rhs.from_symbol_id,
                                  rhs.expression_location.uri,
                                  rhs.expression_location.range.start_line,
                                  rhs.expression_location.range.start_character,
                                  rhs.source_role,
                                  rhs.control_origin);
              });
}

class SchematicCellFactIndexer
    : public slang::ast::ASTVisitor<SchematicCellFactIndexer, slang::ast::VisitFlags::AllGood> {
public:
    SchematicCellFactIndexer(SnapshotData& data, const slang::SourceManager& source_manager) :
        data_(data), source_manager_(source_manager) {
        buildPrimitiveCellBindings();
        buildDisplayLabels();
    }

    void handle(const slang::ast::PrimitiveInstanceSymbol& instance) {
        collect(instance);
        this->visitDefault(instance);
    }

    template<typename T>
    void handle(const T& symbol)
        requires std::is_base_of_v<slang::ast::Symbol, T>
    {
        this->visitDefault(symbol);
    }

    template<typename T>
    void handle(const T& expression)
        requires std::is_base_of_v<slang::ast::Expression, T>
    {
        this->visitDefault(expression);
    }

    void finalize() {
        auto& facts = data_.schematic_cell_pin_facts;
        std::sort(facts.begin(), facts.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.caller_module_name,
                            lhs.cell_id,
                            lhs.pin_index,
                            lhs.pin_direction,
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
                   std::tie(rhs.caller_module_name,
                            rhs.cell_id,
                            rhs.pin_index,
                            rhs.pin_direction,
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
        });
        facts.erase(std::unique(facts.begin(), facts.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.caller_module_name == rhs.caller_module_name &&
                               lhs.cell_id == rhs.cell_id && lhs.pin_index == rhs.pin_index &&
                               lhs.pin_direction == rhs.pin_direction &&
                               sameLocation(lhs.location, rhs.location) &&
                               lhs.net_symbol_id == rhs.net_symbol_id &&
                               lhs.source_role == rhs.source_role &&
                               sameSliceFact(lhs.net_slice, rhs.net_slice) &&
                               lhs.unresolved == rhs.unresolved && lhs.literal == rhs.literal;
                    }),
                    facts.end());
    }

private:
    struct PrimitiveCellBinding {
        std::string module_name;
        const SchematicCell* cell = nullptr;
    };

    static std::string locationKey(std::string_view uri, const ParseRange& range) {
        return std::string(uri) + "\x1f" + std::to_string(range.start_line) + ":" +
               std::to_string(range.start_character) + ":" + std::to_string(range.end_line) + ":" +
               std::to_string(range.end_character);
    }

    static size_t rangeSize(const ParseRange& range) {
        return static_cast<size_t>(range.end_line - range.start_line) * 100000U +
               static_cast<size_t>(range.end_character - range.start_character);
    }

    void insertPrimitiveCellBinding(std::string_view uri,
                                    const std::string& module_name,
                                    const SchematicCell& cell,
                                    const ParseRange& range) {
        if (uri.empty()) {
            return;
        }
        const auto key = locationKey(uri, range);
        const auto existing = primitive_cells_by_location_.find(key);
        if (existing == primitive_cells_by_location_.end() ||
            rangeSize(cell.range) < rangeSize(existing->second.cell->range) ||
            (rangeSize(cell.range) == rangeSize(existing->second.cell->range) &&
             std::tie(module_name, cell.id) <
                 std::tie(existing->second.module_name, existing->second.cell->id))) {
            primitive_cells_by_location_.insert_or_assign(
                key, PrimitiveCellBinding{.module_name = module_name, .cell = &cell});
        }
    }

    void buildPrimitiveCellBindings() {
        for (const auto& [module_name, signature] : data_.ast_module_signatures_by_name) {
            if (signature.uri.empty()) {
                continue;
            }
            for (const auto& cell : signature.schematic.cells) {
                if (cell.id.empty() || cell.id.front() == '$' || cell.kind == "module" ||
                    cell.kind == "interface") {
                    continue;
                }
                insertPrimitiveCellBinding(signature.uri, module_name, cell, cell.selection_range);
                insertPrimitiveCellBinding(signature.uri, module_name, cell, cell.range);
            }
        }
    }

    void buildDisplayLabels() {
        for (const auto& [_, symbols] : data_.graph_symbols_by_uri) {
            for (const auto& symbol : symbols) {
                const auto indexed = data_.symbols_by_id.find(symbol.stable_id);
                if (indexed == data_.symbols_by_id.end() || indexed->second.identity.name.empty()) {
                    continue;
                }
                const auto existing = display_labels_by_stable_id_.find(symbol.stable_id);
                if (existing == display_labels_by_stable_id_.end() ||
                    indexed->second.identity.name < existing->second) {
                    display_labels_by_stable_id_.insert_or_assign(symbol.stable_id,
                                                                  indexed->second.identity.name);
                }
            }
        }
    }

    static SnapshotSchematicCellPinDirection pinDirection(
        const slang::ast::PrimitiveInstanceSymbol& instance,
        size_t index,
        size_t connection_count) {
        const auto kind = instance.primitiveType.primitiveKind;
        const auto primitive_name = instance.primitiveType.name;
        if ((primitive_name == "bufif0" || primitive_name == "bufif1" ||
             primitive_name == "notif0" || primitive_name == "notif1" ||
             primitive_name == "nmos" || primitive_name == "pmos" ||
             primitive_name == "rnmos" || primitive_name == "rpmos") && index == 2) {
            return SnapshotSchematicCellPinDirection::Control;
        }
        if ((primitive_name == "cmos" || primitive_name == "rcmos") &&
            (index == 2 || index == 3)) {
            return SnapshotSchematicCellPinDirection::Control;
        }
        if ((primitive_name == "tranif0" || primitive_name == "tranif1" ||
             primitive_name == "rtranif0" || primitive_name == "rtranif1") && index == 2) {
            return SnapshotSchematicCellPinDirection::Control;
        }

        if (kind == slang::ast::PrimitiveSymbol::NInput) {
            return index == 0 ? SnapshotSchematicCellPinDirection::Output :
                                SnapshotSchematicCellPinDirection::Input;
        }
        if (kind == slang::ast::PrimitiveSymbol::NOutput) {
            return index + 1 == connection_count ? SnapshotSchematicCellPinDirection::Input :
                                                   SnapshotSchematicCellPinDirection::Output;
        }
        if (kind == slang::ast::PrimitiveSymbol::BiDiSwitch) {
            return SnapshotSchematicCellPinDirection::Inout;
        }
        if (index >= instance.primitiveType.ports.size()) {
            return SnapshotSchematicCellPinDirection::Unknown;
        }
        switch (instance.primitiveType.ports[index]->direction) {
            case slang::ast::PrimitivePortDirection::In:
                return SnapshotSchematicCellPinDirection::Input;
            case slang::ast::PrimitivePortDirection::Out:
            case slang::ast::PrimitivePortDirection::OutReg:
                return SnapshotSchematicCellPinDirection::Output;
            case slang::ast::PrimitivePortDirection::InOut:
                return SnapshotSchematicCellPinDirection::Inout;
        }
        return SnapshotSchematicCellPinDirection::Unknown;
    }

    std::optional<std::pair<std::string, const SchematicCell*>> cellFor(
        const SemanticLocation& location) const {
        const auto binding = primitive_cells_by_location_.find(locationKey(location.uri, location.range));
        if (binding == primitive_cells_by_location_.end() || binding->second.cell == nullptr) {
            return std::nullopt;
        }
        return std::pair<std::string, const SchematicCell*>{binding->second.module_name,
                                                             binding->second.cell};
    }

    std::string displayLabel(std::string_view stable_id) const {
        const auto label = display_labels_by_stable_id_.find(std::string(stable_id));
        return label == display_labels_by_stable_id_.end() ? std::string{} : label->second;
    }

    void collect(const slang::ast::PrimitiveInstanceSymbol& instance) {
        const auto location = declarationLocationForSymbol(source_manager_, instance);
        if (!location.has_value()) {
            return;
        }
        const auto cell = cellFor(*location);
        if (!cell.has_value()) {
            return;
        }
        const auto connections = instance.getPortConnections();
        for (size_t index = 0; index < connections.size(); ++index) {
            const auto* expression = connections[index];
            if (expression == nullptr) {
                continue;
            }
            std::vector<SnapshotGraphConnectionBindingFact::SourcePart> parts;
            collectResolvedConnectionSourceParts(data_,
                                                 source_manager_,
                                                 *expression,
                                                 SnapshotConeSliceFact{},
                                                 parts,
                                                 *location);
            normalizeConnectionSourceParts(parts);
            std::string pin_name;
            if (index < instance.primitiveType.ports.size() &&
                !instance.primitiveType.ports[index]->name.empty()) {
                pin_name = std::string(instance.primitiveType.ports[index]->name);
            }
            else {
                pin_name = "P" + std::to_string(index);
            }
            const auto direction = pinDirection(instance, index, connections.size());
            const auto literal = parts.empty() && connectionExpressionIsLiteral(*expression);
            if (parts.empty()) {
                data_.schematic_cell_pin_facts.push_back(
                    SnapshotSchematicCellPinFact{.caller_module_name = cell->first,
                                                 .cell_id = cell->second->id,
                                                 .cell_selection_range = cell->second->selection_range,
                                                 .location = *location,
                                                 .cell_kind = SnapshotSchematicCellKind::Primitive,
                                                 .pin_name = std::move(pin_name),
                                                 .pin_index = static_cast<int>(index),
                                                 .pin_direction = direction,
                                                 .net_symbol_id = {},
                                                 .display_label = {},
                                                 .net_slice = {},
                                                 .source_role = SnapshotConeSourceRole::Data,
                                                 .unresolved = !literal,
                                                 .literal = literal});
                continue;
            }
            for (const auto& part : parts) {
                data_.schematic_cell_pin_facts.push_back(
                    SnapshotSchematicCellPinFact{.caller_module_name = cell->first,
                                                 .cell_id = cell->second->id,
                                                 .cell_selection_range = cell->second->selection_range,
                                                 .location = part.source_location,
                                                 .cell_kind = SnapshotSchematicCellKind::Primitive,
                                                 .pin_name = pin_name,
                                                 .pin_index = static_cast<int>(index),
                                                 .pin_direction = direction,
                                                 .net_symbol_id = part.source_symbol_id,
                                                 .display_label = displayLabel(part.source_symbol_id),
                                                 .net_slice = part.source_slice,
                                                 .source_role = part.source_role,
                                                 .unresolved = part.unresolved,
                                                 .literal = false});
            }
        }
    }

    SnapshotData& data_;
    std::unordered_map<std::string, PrimitiveCellBinding> primitive_cells_by_location_;
    std::unordered_map<std::string, std::string> display_labels_by_stable_id_;
    const slang::SourceManager& source_manager_;
};

void buildSchematicCellPinFacts(SnapshotData& data, const slang::SourceManager& source_manager) {
    data.schematic_cell_pin_facts.clear();
    if (!data.compilation) {
        return;
    }
    SchematicCellFactIndexer indexer(data, source_manager);
    data.compilation->getRoot().visit(indexer);
    indexer.finalize();
}

std::optional<SemanticLocation> locationForDeclaredTypeReference(
    const slang::SourceManager& source_manager,
    const slang::syntax::DataTypeSyntax& type_syntax) {
    const auto type_range = sourceRangeForSourceRange(source_manager, type_syntax.sourceRange());
    if (type_range.start_line < 0 || type_range.end_line < type_range.start_line) {
        return std::nullopt;
    }
    const auto original = source_manager.getFullyOriginalLoc(type_syntax.sourceRange().start());
    const auto type_uri = locationUriForSourceLocation(source_manager, original);
    if (type_uri.empty()) {
        return std::nullopt;
    }
    return SemanticLocation{.uri = type_uri, .range = type_range};
}

void appendTypeReference(SnapshotData& data,
                         SemanticLocation reference_location,
                         std::string type_name,
                         std::vector<SemanticLocation> definitions) {
    std::sort(definitions.begin(), definitions.end(), locationLess);
    definitions.erase(std::unique(definitions.begin(), definitions.end(), sameLocation),
                      definitions.end());
    auto& references = data.type_references_by_uri[reference_location.uri];
    const auto duplicate = std::any_of(references.begin(),
                                       references.end(),
                                       [&](const SnapshotTypeReference& reference) {
                                           return sameLocation(reference.reference, reference_location);
                                       });
    if (duplicate) {
        return;
    }

    references.push_back(SnapshotTypeReference{.reference = std::move(reference_location),
                                               .type_name = std::move(type_name),
                                               .definitions = std::move(definitions)});
}

std::string interfaceModportKey(std::string_view interface_definition_stable_id,
                                std::string_view modport_name) {
    return std::string(interface_definition_stable_id) + "\x1f" + std::string(modport_name);
}

SnapshotGraphPortDirection graphDirectionFor(slang::ast::ArgumentDirection direction) {
    switch (direction) {
        case slang::ast::ArgumentDirection::In:
            return SnapshotGraphPortDirection::Input;
        case slang::ast::ArgumentDirection::Out:
            return SnapshotGraphPortDirection::Output;
        case slang::ast::ArgumentDirection::InOut:
            return SnapshotGraphPortDirection::Inout;
        case slang::ast::ArgumentDirection::Ref:
            return SnapshotGraphPortDirection::Ref;
    }
    return SnapshotGraphPortDirection::Unknown;
}

struct InterfacePortSyntaxLocations {
    SemanticLocation interface_type_location;
    SemanticLocation modport_location;
    std::string modport_name;
};

std::string interfacePortSyntaxKey(const SemanticLocation& port_location) {
    return port_location.uri + "\x1f" + rangeKey(port_location.range);
}

std::unordered_map<std::string, InterfacePortSyntaxLocations> collectInterfacePortSyntaxLocations(
    const SnapshotData& data,
    const slang::SourceManager& source_manager) {
    std::unordered_map<std::string, InterfacePortSyntaxLocations> locations;
    for (const auto& tree : data.syntax_trees) {
        if (tree == nullptr) {
            continue;
        }
        auto visitor = slang::syntax::makeSyntaxVisitor(
            [&](auto& self, const slang::syntax::ImplicitAnsiPortSyntax& node) {
                if (node.header == nullptr || node.declarator == nullptr ||
                    node.header->kind != slang::syntax::SyntaxKind::InterfacePortHeader) {
                    self.visitDefault(node);
                    return;
                }
                const auto& header = node.header->as<slang::syntax::InterfacePortHeaderSyntax>();
                const auto port_location = locationForSyntaxToken(source_manager, node.declarator->name);
                const auto interface_location = locationForSyntaxToken(source_manager, header.nameOrKeyword);
                if (!port_location.has_value() || !interface_location.has_value()) {
                    self.visitDefault(node);
                    return;
                }
                InterfacePortSyntaxLocations fact{.interface_type_location = *interface_location,
                                                   .modport_location = {},
                                                   .modport_name = {}};
                if (header.modport != nullptr) {
                    fact.modport_location =
                        locationForSyntaxToken(source_manager, header.modport->member).value_or(SemanticLocation{});
                    fact.modport_name = std::string(header.modport->member.valueText());
                }
                locations.insert_or_assign(interfacePortSyntaxKey(*port_location), std::move(fact));
                self.visitDefault(node);
            });
        tree->root().visit(visitor);
    }
    return locations;
}

void buildInterfaceModportBindingIndex(SnapshotData& data,
                                       const slang::SourceManager& source_manager) {
    data.interface_modport_binding_index = {};
    auto& index = data.interface_modport_binding_index;
    const auto syntax_locations = collectInterfacePortSyntaxLocations(data, source_manager);

    std::vector<const SnapshotIndexedSymbol*> symbols;
    symbols.reserve(data.symbols_by_id.size());
    for (const auto& [_, indexed] : data.symbols_by_id) {
        if (indexed.symbol != nullptr) {
            symbols.push_back(&indexed);
        }
    }
    std::sort(symbols.begin(), symbols.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->identity.stable_id < rhs->identity.stable_id;
    });

    for (const auto* indexed : symbols) {
        if (indexed->symbol->kind != slang::ast::SymbolKind::Modport) {
            continue;
        }
        const auto& modport = indexed->symbol->as<slang::ast::ModportSymbol>();
        const auto* parent_scope = modport.getParentScope();
        if (parent_scope == nullptr || parent_scope->asSymbol().kind != slang::ast::SymbolKind::InstanceBody) {
            continue;
        }
        const auto& body = parent_scope->asSymbol().as<slang::ast::InstanceBodySymbol>();
        if (body.parentInstance == nullptr) {
            continue;
        }
        const auto definition_id = data.ids_by_symbol.find(&body.parentInstance->getDefinition());
        if (definition_id == data.ids_by_symbol.end()) {
            continue;
        }
        const auto fact = SnapshotInterfaceModportDefinitionFact{
            .stable_id = indexed->identity.stable_id,
            .interface_definition_stable_id = definition_id->second,
            .name = indexed->identity.name,
            .location = indexed->identity.location};
        const auto name_key = interfaceModportKey(fact.interface_definition_stable_id, fact.name);
        const auto existing = index.modport_ids_by_interface_definition_name.find(name_key);
        if (existing == index.modport_ids_by_interface_definition_name.end() || fact.stable_id < existing->second) {
            index.modport_ids_by_interface_definition_name.insert_or_assign(name_key, fact.stable_id);
        }
        index.modports_by_stable_id.insert_or_assign(fact.stable_id, fact);

        auto& members = index.members_by_modport_stable_id[fact.stable_id];
        for (const auto& symbol : modport.members()) {
            if (symbol.kind != slang::ast::SymbolKind::ModportPort) {
                continue;
            }
            const auto member_id = data.ids_by_symbol.find(&symbol);
            if (member_id == data.ids_by_symbol.end()) {
                continue;
            }
            const auto& member = symbol.as<slang::ast::ModportPortSymbol>();
            std::string internal_symbol_id;
            if (member.internalSymbol != nullptr) {
                if (const auto internal = data.ids_by_symbol.find(member.internalSymbol);
                    internal != data.ids_by_symbol.end()) {
                    internal_symbol_id = internal->second;
                }
            }
            members.push_back(SnapshotInterfaceModportMemberFact{
                .stable_id = member_id->second,
                .interface_definition_stable_id = fact.interface_definition_stable_id,
                .modport_stable_id = fact.stable_id,
                .name = std::string(member.name),
                .direction = graphDirectionFor(member.direction),
                .internal_symbol_stable_id = std::move(internal_symbol_id),
                .location = declarationLocationForSymbol(source_manager, member).value_or(indexed->identity.location)});
        }
    }

    for (auto& [_, members] : index.members_by_modport_stable_id) {
        std::sort(members.begin(), members.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameLocation(lhs.location, rhs.location)) {
                return locationLess(lhs.location, rhs.location);
            }
            return lhs.stable_id < rhs.stable_id;
        });
        members.erase(std::unique(members.begin(), members.end(), [](const auto& lhs, const auto& rhs) {
                          return lhs.stable_id == rhs.stable_id;
                      }),
                      members.end());
        index.member_count += members.size();
    }

    for (const auto* indexed : symbols) {
        if (indexed->symbol->kind != slang::ast::SymbolKind::InterfacePort) {
            continue;
        }
        const auto& port = indexed->symbol->as<slang::ast::InterfacePortSymbol>();
        auto fact = SnapshotInterfacePortBindingFact{.port_stable_id = indexed->identity.stable_id,
                                                     .interface_definition_stable_id = {},
                                                     .modport_stable_id = {},
                                                     .connected_interface_instance_stable_id = {},
                                                     .connected_modport_stable_id = {},
                                                     .interface_type_location = {},
                                                     .modport_location = {},
                                                     .connection_location = {},
                                                     .interface_definition_location = {},
                                                     .modport_definition_location = {},
                                                     .resolved = false};
        if (port.interfaceDef != nullptr) {
            if (const auto definition = data.ids_by_symbol.find(port.interfaceDef);
                definition != data.ids_by_symbol.end()) {
                fact.interface_definition_stable_id = definition->second;
                if (const auto definition_symbol = data.symbols_by_id.find(definition->second);
                    definition_symbol != data.symbols_by_id.end()) {
                    fact.interface_definition_location = definition_symbol->second.identity.location;
                }
            }
        }
        std::string modport_name(port.modport);
        if (const auto locations = syntax_locations.find(interfacePortSyntaxKey(indexed->identity.location));
            locations != syntax_locations.end()) {
            fact.interface_type_location = locations->second.interface_type_location;
            fact.modport_location = locations->second.modport_location;
            if (!locations->second.modport_name.empty()) {
                modport_name = locations->second.modport_name;
            }
        }
        if (!fact.interface_definition_stable_id.empty() && !modport_name.empty()) {
            const auto modport = index.modport_ids_by_interface_definition_name.find(
                interfaceModportKey(fact.interface_definition_stable_id, modport_name));
            if (modport != index.modport_ids_by_interface_definition_name.end()) {
                fact.modport_stable_id = modport->second;
                fact.modport_definition_location = index.modports_by_stable_id.at(modport->second).location;
            }
        }
        const auto [connection, expression] = port.getConnectionAndExpr();
        if (connection.first != nullptr) {
            if (const auto connected = data.ids_by_symbol.find(connection.first); connected != data.ids_by_symbol.end()) {
                fact.connected_interface_instance_stable_id = connected->second;
            }
        }
        if (connection.second != nullptr) {
            if (const auto connected = data.ids_by_symbol.find(connection.second); connected != data.ids_by_symbol.end()) {
                fact.connected_modport_stable_id = connected->second;
            }
        }
        if (expression != nullptr) {
            fact.connection_location =
                locationForSourceRange(source_manager, expression->sourceRange).value_or(SemanticLocation{});
        }
        fact.resolved = !fact.interface_definition_stable_id.empty() &&
                        (modport_name.empty() || !fact.modport_stable_id.empty());
        if (fact.resolved) {
            ++index.resolved_port_binding_count;
        }
        index.ports_by_stable_id.insert_or_assign(fact.port_stable_id, std::move(fact));
    }
}

bool addInterfacePortTypeReferences(SnapshotData& data,
                                    const slang::ast::InterfacePortSymbol& port) {
    const auto port_id = data.ids_by_symbol.find(&port);
    if (port_id == data.ids_by_symbol.end()) {
        return false;
    }
    const auto binding = data.interface_modport_binding_index.ports_by_stable_id.find(port_id->second);
    if (binding == data.interface_modport_binding_index.ports_by_stable_id.end() || !binding->second.resolved) {
        return false;
    }
    const auto& fact = binding->second;
    bool inserted = false;
    if (!fact.interface_type_location.uri.empty() && !fact.interface_definition_location.uri.empty()) {
        appendTypeReference(data,
                            fact.interface_type_location,
                            std::string(port.interfaceDef == nullptr ? "interface" : port.interfaceDef->name),
                            {fact.interface_definition_location});
        inserted = true;
    }
    if (!port.modport.empty() && !fact.modport_location.uri.empty() &&
        !fact.modport_definition_location.uri.empty()) {
        appendTypeReference(data,
                            fact.modport_location,
                            std::string(port.modport),
                            {fact.modport_definition_location});
        inserted = true;
    }
    return inserted;
}

void addTypeReferenceForSymbol(SnapshotData& data,
                               const slang::SourceManager& source_manager,
                               const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                               const slang::ast::Symbol& symbol) {
    if (symbol.kind == slang::ast::SymbolKind::InterfacePort &&
        addInterfacePortTypeReferences(data, symbol.as<slang::ast::InterfacePortSymbol>())) {
        return;
    }

    const auto* declared_type = symbol.getDeclaredType();
    if (declared_type == nullptr || declared_type->getTypeSyntax() == nullptr) {
        return;
    }

    auto reference_location = locationForDeclaredTypeReference(source_manager,
                                                              *declared_type->getTypeSyntax());
    if (!reference_location.has_value()) {
        return;
    }

    const auto& type = declared_type->getType();
    const auto document_it = documents.find(reference_location->uri);
    if (document_it != documents.end() && !type.name.empty()) {
        if (auto narrowed_range = identifierRangeByName(document_it->second.text,
                                                        reference_location->range,
                                                        type.name)) {
            reference_location->range = *narrowed_range;
        }
    }

    std::string type_name = type.toString();
    if (document_it != documents.end()) {
        if (auto text = textForRange(document_it->second.text, reference_location->range)) {
            type_name = std::move(*text);
        }
    }

    std::vector<SemanticLocation> definitions;
    if (const auto type_location = declarationLocationForSymbol(source_manager, type)) {
        definitions.push_back(*type_location);
    }
    for (const auto& [_, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.location.uri == reference_location->uri &&
            sameLocation(indexed_symbol.identity.location, *reference_location)) {
            definitions.push_back(indexed_symbol.identity.location);
        }
    }
    if (definitions.empty()) {
        const auto delimiter = type_name.rfind("::");
        const auto qualified_package = delimiter == std::string::npos
                                           ? std::optional<std::string_view>{}
                                           : std::optional<std::string_view>{
                                                 std::string_view(type_name).substr(0, delimiter)};
        const auto lookup_name = delimiter == std::string::npos
                                     ? std::string_view(type_name)
                                     : std::string_view(type_name).substr(delimiter + 2);
        auto named_definitions = visibleTypeDefinitionLocationsByName(data,
                                                                      reference_location->uri,
                                                                      lookup_name,
                                                                      qualified_package);
        definitions.insert(definitions.end(), named_definitions.begin(), named_definitions.end());
    }
    appendTypeReference(data, *reference_location, std::move(type_name), std::move(definitions));
}

void buildTypeReferences(SnapshotData& data,
                         const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    data.type_references_by_uri.clear();
    if (!data.source_manager) {
        return;
    }
    for (const auto& [_, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.symbol == nullptr) {
            continue;
        }
        addTypeReferenceForSymbol(data, *data.source_manager, documents, *indexed_symbol.symbol);
    }
    for (auto& [_, references] : data.type_references_by_uri) {
        std::sort(references.begin(), references.end(), [](const auto& lhs, const auto& rhs) {
            return locationLess(lhs.reference, rhs.reference);
        });
        for (const auto& reference : references) {
            std::optional<std::string> target_id;
            for (const auto& definition : reference.definitions) {
                for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
                    if (sameLocation(indexed_symbol.identity.location, definition) &&
                        (!target_id.has_value() || stable_id < *target_id)) {
                        target_id = stable_id;
                    }
                }
            }
            if (target_id.has_value()) {
                insertReference(data,
                                *target_id,
                                reference.type_name,
                                reference.reference,
                                SemanticReferenceRole::Type);
            }
        }
    }
}

void buildSameRangeReferenceAliasIndex(SnapshotData& data) {
    data.reference_aliases_by_id.clear();
    std::unordered_map<std::string, std::vector<const SnapshotIndexedReference*>> declarations_by_key;
    for (const auto& reference : data.references) {
        if (!reference.is_declaration) {
            continue;
        }
        const auto key = reference.location.uri + "\n" + rangeKey(reference.location.range) + "\n" +
                         reference.name;
        declarations_by_key[key].push_back(&reference);
    }

    for (const auto& [_, declarations] : declarations_by_key) {
        if (declarations.size() < 2) {
            continue;
        }
        std::vector<std::string> aliases;
        aliases.reserve(declarations.size());
        for (const auto* declaration : declarations) {
            aliases.push_back(declaration->stable_id);
        }
        std::sort(aliases.begin(), aliases.end());
        aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
        for (const auto& alias : aliases) {
            data.reference_aliases_by_id[alias] = aliases;
        }
    }
}

void sortSnapshotIndexes(SnapshotData& data) {
    data.reference_occurrences_by_uri.clear();
    data.graph_references_by_uri.clear();
    for (size_t index = 0; index < data.references.size(); ++index) {
        const auto& reference = data.references[index];
        data.reference_occurrences_by_uri[reference.location.uri].reference_indexes.push_back(index);
        data.graph_references_by_uri[reference.location.uri].push_back(
            SnapshotUriReferenceRangeFact{.stable_id = reference.stable_id,
                                          .range = reference.location.range,
                                          .is_declaration = reference.is_declaration});
    }
    for (auto& [_, occurrence_index] : data.reference_occurrences_by_uri) {
        auto& indexes = occurrence_index.reference_indexes;
        std::sort(indexes.begin(), indexes.end(), [&](size_t lhs, size_t rhs) {
            const auto& left = data.references[lhs];
            const auto& right = data.references[rhs];
            if (!sameRange(left.location.range, right.location.range)) {
                return locationLess(left.location, right.location);
            }
            if (left.is_declaration != right.is_declaration) {
                return left.is_declaration;
            }
            return left.stable_id < right.stable_id;
        });
        occurrence_index.prefix_max_end_ranges.clear();
        occurrence_index.prefix_max_end_ranges.reserve(indexes.size());
        for (const auto index : indexes) {
            const auto& range = data.references[index].location.range;
            if (occurrence_index.prefix_max_end_ranges.empty() ||
                rangeEndLess(occurrence_index.prefix_max_end_ranges.back(), range)) {
                occurrence_index.prefix_max_end_ranges.push_back(range);
            }
            else {
                occurrence_index.prefix_max_end_ranges.push_back(
                    occurrence_index.prefix_max_end_ranges.back());
            }
        }
    }
    for (auto& [_, references] : data.graph_references_by_uri) {
        std::sort(references.begin(), references.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameRange(lhs.range, rhs.range)) return rangeStartLess(lhs.range, rhs.range);
            if (lhs.is_declaration != rhs.is_declaration) return lhs.is_declaration;
            return lhs.stable_id < rhs.stable_id;
        });
    }
    data.graph_symbols_by_uri.clear();
    for (const auto& [stable_id, indexed] : data.symbols_by_id) {
        if (!indexed.identity.location.uri.empty()) {
            data.graph_symbols_by_uri[indexed.identity.location.uri].push_back(
                SnapshotUriSymbolRangeFact{.stable_id = stable_id, .range = indexed.identity.location.range});
        }
    }
    for (auto& [_, symbols] : data.graph_symbols_by_uri) {
        std::sort(symbols.begin(), symbols.end(), [](const auto& lhs, const auto& rhs) {
            if (!sameRange(lhs.range, rhs.range)) return rangeStartLess(lhs.range, rhs.range);
            return lhs.stable_id < rhs.stable_id;
        });
    }
    for (auto& [_, indexes] : data.references_by_symbol) {
        std::sort(indexes.begin(), indexes.end(), [&](size_t lhs, size_t rhs) {
            return locationLess(data.references[lhs].location, data.references[rhs].location);
        });
    }
    for (auto& [_, completions] : data.completions_by_uri) {
        std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhs_priority = completionPriorityForDetail(lhs.detail);
            const auto rhs_priority = completionPriorityForDetail(rhs.detail);
            if (lhs_priority != rhs_priority) {
                return lhs_priority < rhs_priority;
            }
            return lhs.label < rhs.label;
        });
    }
    for (auto& [_, completions] : data.member_completions_by_uri) {
        std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.qualifier != rhs.qualifier) {
                return lhs.qualifier < rhs.qualifier;
            }
            if (lhs.identity.name != rhs.identity.name) {
                return lhs.identity.name < rhs.identity.name;
            }
            if (lhs.identity.kind != rhs.identity.kind) {
                return lhs.identity.kind < rhs.identity.kind;
            }
            return lhs.identity.stable_id < rhs.identity.stable_id;
        });
    }
    data.member_completions_by_qualifier_by_uri.clear();
    data.member_completions_by_stable_id.clear();
    for (const auto& [uri, completions] : data.member_completions_by_uri) {
        auto& by_qualifier = data.member_completions_by_qualifier_by_uri[uri];
        for (const auto& completion : completions) {
            by_qualifier[completion.qualifier].push_back(completion);
            data.member_completions_by_stable_id.try_emplace(completion.identity.stable_id,
                                                             completion);
        }
    }
    for (auto& [_, ranges] : data.selection_ranges_by_uri) {
        std::sort(ranges.begin(), ranges.end(), rangeLessWideFirst);
        ranges.erase(std::unique(ranges.begin(),
                                 ranges.end(),
                                 [](const ParseRange& lhs, const ParseRange& rhs) {
                                     return lhs.start_line == rhs.start_line &&
                                            lhs.start_character == rhs.start_character &&
                                            lhs.end_line == rhs.end_line &&
                                            lhs.end_character == rhs.end_character;
                                 }),
                     ranges.end());
    }

    data.selection_range_indexes_by_uri.clear();
    for (const auto& [uri, ranges] : data.selection_ranges_by_uri) {
        auto& index = data.selection_range_indexes_by_uri[uri];
        index.ranges = ranges;
        std::sort(index.ranges.begin(), index.ranges.end(), [](const ParseRange& left, const ParseRange& right) {
            if (left.start_line != right.start_line) {
                return left.start_line < right.start_line;
            }
            if (left.start_character != right.start_character) {
                return left.start_character < right.start_character;
            }
            if (left.end_line != right.end_line) {
                return left.end_line < right.end_line;
            }
            return left.end_character < right.end_character;
        });
        index.prefix_max_end_ranges.reserve(index.ranges.size());
        for (const auto& range : index.ranges) {
            if (index.prefix_max_end_ranges.empty() ||
                rangeEndLess(index.prefix_max_end_ranges.back(), range)) {
                index.prefix_max_end_ranges.push_back(range);
            }
            else {
                index.prefix_max_end_ranges.push_back(index.prefix_max_end_ranges.back());
            }
        }
    }
    for (auto& [_, calls] : data.callable_invocations_by_uri) {
        std::sort(calls.begin(), calls.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.range.start_line != rhs.range.start_line) {
                return lhs.range.start_line < rhs.range.start_line;
            }
            if (lhs.range.start_character != rhs.range.start_character) {
                return lhs.range.start_character < rhs.range.start_character;
            }
            if (lhs.range.end_line != rhs.range.end_line) {
                return lhs.range.end_line < rhs.range.end_line;
            }
            if (lhs.range.end_character != rhs.range.end_character) {
                return lhs.range.end_character < rhs.range.end_character;
            }
            return lhs.name < rhs.name;
        });
    }
    data.inlay_symbols_by_uri.clear();
    for (const auto& [_, indexed_symbol] : data.symbols_by_id) {
        const auto& identity = indexed_symbol.identity;
        if (identity.location.uri.empty()) {
            continue;
        }
        data.inlay_symbols_by_uri[identity.location.uri].push_back(
            SignatureInlaySymbol{.identity = identity,
                                 .type_display = indexed_symbol.type_display,
                                 .value_display = indexed_symbol.value_display});
    }
    for (auto& [_, symbols] : data.inlay_symbols_by_uri) {
        std::sort(symbols.begin(), symbols.end(), [](const auto& left, const auto& right) {
            if (!sameRange(left.identity.location.range, right.identity.location.range)) {
                return locationLess(left.identity.location, right.identity.location);
            }
            if (left.identity.kind != right.identity.kind) {
                return left.identity.kind < right.identity.kind;
            }
            return left.identity.stable_id < right.identity.stable_id;
        });
        symbols.erase(std::unique(symbols.begin(),
                                  symbols.end(),
                                  [](const auto& left, const auto& right) {
                                      return left.identity.stable_id == right.identity.stable_id;
                                  }),
                      symbols.end());
    }
}

void buildNavigationIndexes(SnapshotData& data) {
    data.navigation_targets_by_id.clear();
    data.navigation_occurrences_by_uri.clear();
    data.navigation_occurrences_by_symbol.clear();
    data.implementation_edge_index = {};

    for (const auto& [stable_id, indexed] : data.symbols_by_id) {
        SnapshotNavigationTargetFact target;
        target.identity = indexed.identity;
        target.type_display = indexed.type_display;
        target.value_display = indexed.value_display;
        target.rename_eligible = !target.identity.name.empty() &&
                                 !target.identity.location.uri.empty() &&
                                 !sameRange(target.identity.location.range, ParseRange{});

        // This is the only point that asks slang for a declared type. The
        // provider later consumes the copied source location, never an AST pointer.
        if (indexed.symbol != nullptr) {
            if (const auto* declared_type = indexed.symbol->getDeclaredType()) {
                const auto& type = declared_type->getType();
                if (const auto type_location = declarationLocationForSymbol(*data.source_manager, type)) {
                    target.type_definition_locations.push_back(*type_location);
                }
            }
        }
        std::sort(target.type_definition_locations.begin(),
                  target.type_definition_locations.end(),
                  locationLess);
        target.type_definition_locations.erase(
            std::unique(target.type_definition_locations.begin(),
                        target.type_definition_locations.end(),
                        sameLocation),
            target.type_definition_locations.end());
        data.navigation_targets_by_id.emplace(stable_id, std::move(target));
    }

    for (const auto& reference : data.references) {
        const auto target_it = data.navigation_targets_by_id.find(reference.stable_id);
        const auto has_type_display = target_it != data.navigation_targets_by_id.end() &&
                                      !target_it->second.type_display.empty();
        auto occurrence = SnapshotNavigationOccurrence{.stable_id = reference.stable_id,
                                                        .location = reference.location,
                                                        .is_declaration = reference.is_declaration,
                                                        .role = reference.role,
                                                        .has_type_display = has_type_display};
        data.navigation_occurrences_by_uri[reference.location.uri].occurrences.push_back(occurrence);
        data.navigation_occurrences_by_symbol[reference.stable_id].push_back(std::move(occurrence));
    }

    const auto occurrence_less = [](const SnapshotNavigationOccurrence& left,
                                    const SnapshotNavigationOccurrence& right) {
        if (!sameRange(left.location.range, right.location.range)) {
            return locationLess(left.location, right.location);
        }
        if (left.is_declaration != right.is_declaration) {
            return left.is_declaration;
        }
        if (left.has_type_display != right.has_type_display) {
            return left.has_type_display;
        }
        return left.stable_id < right.stable_id;
    };
    for (auto& [_, index] : data.navigation_occurrences_by_uri) {
        std::sort(index.occurrences.begin(), index.occurrences.end(), occurrence_less);
        index.prefix_max_end_ranges.reserve(index.occurrences.size());
        for (const auto& occurrence : index.occurrences) {
            const auto& range = occurrence.location.range;
            if (index.prefix_max_end_ranges.empty() ||
                rangeEndLess(index.prefix_max_end_ranges.back(), range)) {
                index.prefix_max_end_ranges.push_back(range);
            }
            else {
                index.prefix_max_end_ranges.push_back(index.prefix_max_end_ranges.back());
            }
        }
    }
    for (auto& [_, occurrences] : data.navigation_occurrences_by_symbol) {
        std::sort(occurrences.begin(), occurrences.end(), occurrence_less);
    }

    const auto append_edge = [&](std::string target_id,
                                 std::string implementation_id,
                                 SemanticLocation location,
                                 std::string kind) {
        if (target_id.empty() || implementation_id.empty() || location.uri.empty()) {
            return;
        }
        auto& edges = data.implementation_edge_index.edges;
        const auto duplicate = std::any_of(edges.begin(), edges.end(), [&](const auto& edge) {
            return edge.target_stable_id == target_id &&
                   edge.implementation_stable_id == implementation_id &&
                   edge.kind == kind && sameLocation(edge.location, location);
        });
        if (!duplicate) {
            edges.push_back(SnapshotImplementationEdge{.target_stable_id = std::move(target_id),
                                                       .implementation_stable_id = std::move(implementation_id),
                                                       .location = std::move(location),
                                                       .kind = std::move(kind)});
        }
    };

    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            auto target_id = instance.target_stable_id;
            if (target_id.empty()) {
                if (const auto definition = data.module_definition_ids_by_name.find(instance.module_name);
                    definition != data.module_definition_ids_by_name.end()) {
                    target_id = definition->second;
                }
            }
            append_edge(std::move(target_id),
                        instance.instance_stable_id,
                        SemanticLocation{.uri = instance.uri, .range = instance.module_selection_range},
                        "moduleInstance");
        }
    }

    for (const auto& [stable_id, indexed] : data.symbols_by_id) {
        if (indexed.symbol == nullptr) {
            continue;
        }
        if (indexed.symbol->kind == slang::ast::SymbolKind::ClassType) {
            const auto& derived = indexed.symbol->as<slang::ast::ClassType>();
            if (const auto* base_type = derived.getBaseClass(); base_type != nullptr && base_type->isClass()) {
                const auto& base = base_type->getCanonicalType().as<slang::ast::ClassType>();
                if (const auto base_id = data.ids_by_symbol.find(&base); base_id != data.ids_by_symbol.end()) {
                    append_edge(base_id->second,
                                stable_id,
                                indexed.identity.location,
                                "classDerived");
                }
            }
        }
        else if (indexed.symbol->kind == slang::ast::SymbolKind::Subroutine) {
            const auto& subroutine = indexed.symbol->as<slang::ast::SubroutineSymbol>();
            if (const auto* base = subroutine.getOverride(); base != nullptr) {
                if (const auto base_id = data.ids_by_symbol.find(base); base_id != data.ids_by_symbol.end()) {
                    append_edge(base_id->second,
                                stable_id,
                                indexed.identity.location,
                                "callableOverride");
                }
            }
        }
    }

    auto& edges = data.implementation_edge_index.edges;
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        if (left.target_stable_id != right.target_stable_id) {
            return left.target_stable_id < right.target_stable_id;
        }
        if (!sameLocation(left.location, right.location)) {
            return locationLess(left.location, right.location);
        }
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        return left.implementation_stable_id < right.implementation_stable_id;
    });
    for (size_t index = 0; index < edges.size(); ++index) {
        data.implementation_edge_index.edges_by_target_stable_id[edges[index].target_stable_id].push_back(index);
    }
}

void buildProviderLookupIndexes(SnapshotData& data) {
    data.completion_resolve_by_id.clear();
    data.diagnostic_lookup_index = {};

    std::unordered_map<std::string, std::vector<std::string>> package_names_by_uri;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        const auto& identity = indexed_symbol.identity;
        data.completion_resolve_by_id.emplace(
            stable_id,
            SnapshotCompletionResolveFact{.kind = SnapshotCompletionResolveKind::Symbol,
                                          .identity = identity,
                                          .type_display = indexed_symbol.type_display,
                                          .value_display = indexed_symbol.value_display,
                                          .module_uri = {},
                                          .module = std::nullopt,
                                          .port = std::nullopt,
                                          .macro = std::nullopt});
        if (identity.kind == "Package") {
            data.diagnostic_lookup_index.package_definition_ids_by_name[identity.name].push_back(stable_id);
            package_names_by_uri[identity.location.uri].push_back(identity.name);
        }
    }

    for (auto& [_, names] : package_names_by_uri) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }

    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        const auto& identity = indexed_symbol.identity;
        if (isTypeDefinitionKind(identity.kind) && !isModuleDefinitionKind(identity.kind)) {
            data.diagnostic_lookup_index.type_definition_locations_by_name[identity.name].push_back(
                identity.location);
        }
        if (isPackageMemberDefinitionKind(identity.kind)) {
            if (const auto packages = package_names_by_uri.find(identity.location.uri);
                packages != package_names_by_uri.end()) {
                auto& names = data.diagnostic_lookup_index.package_names_by_member[identity.name];
                names.insert(names.end(), packages->second.begin(), packages->second.end());
                for (const auto& package_name : packages->second) {
                    ++data.diagnostic_lookup_index.package_member_definition_counts[
                        package_name + "\x1f" + identity.name];
                }
            }
        }
        if (isDuplicateSymbolDiagnosticKind(identity.kind) && !identity.name.empty() &&
            !identity.location.uri.empty()) {
            data.diagnostic_lookup_index.duplicate_symbols_by_uri[identity.location.uri].push_back(identity);
        }
    }

    for (const auto& [stable_id, member] : data.member_completions_by_stable_id) {
        auto type_display = member.type_display;
        if (type_display.empty()) {
            if (const auto symbol = data.symbols_by_id.find(stable_id);
                symbol != data.symbols_by_id.end()) {
                type_display = symbol->second.type_display;
            }
        }
        data.completion_resolve_by_id.insert_or_assign(
            stable_id,
            SnapshotCompletionResolveFact{.kind = SnapshotCompletionResolveKind::Member,
                                          .identity = member.identity,
                                          .type_display = std::move(type_display),
                                          .value_display = {},
                                          .module_uri = {},
                                          .module = std::nullopt,
                                          .port = std::nullopt,
                                          .macro = std::nullopt});
    }

    for (const auto& [module_name, module] : data.modules_by_name) {
        const auto module_id_it = data.module_definition_ids_by_name.find(module_name);
        const auto module_id = module_id_it == data.module_definition_ids_by_name.end()
                                   ? "module|" + module_name
                                   : module_id_it->second;
        auto identity = SemanticSymbolIdentity{.stable_id = module_id,
                                               .name = module.name,
                                               .kind = "Definition",
                                               .location = SemanticLocation{
                                                   .uri = data.module_uris_by_name.contains(module_name)
                                                              ? data.module_uris_by_name.at(module_name)
                                                              : std::string{},
                                                   .range = module.selection_range}};
        if (const auto indexed = data.symbols_by_id.find(module_id); indexed != data.symbols_by_id.end()) {
            identity = indexed->second.identity;
        }
        const auto module_uri = data.module_uris_by_name.contains(module_name)
                                    ? data.module_uris_by_name.at(module_name)
                                    : std::string{};
        data.completion_resolve_by_id.insert_or_assign(
            module_id,
            SnapshotCompletionResolveFact{.kind = SnapshotCompletionResolveKind::Module,
                                          .identity = identity,
                                          .type_display = {},
                                          .value_display = {},
                                          .module_uri = module_uri,
                                          .module = module,
                                          .port = std::nullopt,
                                          .macro = std::nullopt});

        const auto append_port = [&](const SchematicPort& port) {
            data.completion_resolve_by_id.insert_or_assign(
                portCompletionResolveId(module_id, port),
                SnapshotCompletionResolveFact{.kind = SnapshotCompletionResolveKind::Port,
                                              .identity = identity,
                                              .type_display = {},
                                              .value_display = {},
                                              .module_uri = module_uri,
                                              .module = module,
                                              .port = port,
                                              .macro = std::nullopt});
        };
        if (module.port_details.empty()) {
            for (const auto& port_name : module.ports) {
                append_port(SchematicPort{.name = port_name,
                                          .direction = {},
                                          .width_text = {},
                                          .range = module.selection_range,
                                          .selection_range = module.selection_range});
            }
        }
        else {
            for (const auto& port : module.port_details) {
                append_port(port);
            }
        }
    }

    for (const auto& [_, visible_macros] : data.visible_macros_by_uri) {
        for (const auto& visible_macro : visible_macros) {
            const auto resolve_id = macroCompletionResolveId(visible_macro);
            SemanticSymbolIdentity identity;
            identity.stable_id = resolve_id;
            identity.name = visible_macro.definition.name;
            identity.kind = "Macro";
            identity.location = SemanticLocation{.uri = visible_macro.source_uri,
                                                 .range = visible_macro.definition.selection_range};
            data.completion_resolve_by_id.insert_or_assign(
                resolve_id,
                SnapshotCompletionResolveFact{.kind = SnapshotCompletionResolveKind::Macro,
                                              .identity = std::move(identity),
                                              .type_display = {},
                                              .value_display = {},
                                              .module_uri = {},
                                              .module = std::nullopt,
                                              .port = std::nullopt,
                                              .macro = visible_macro.definition});
        }
    }

    for (auto& [_, ids] : data.diagnostic_lookup_index.package_definition_ids_by_name) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    for (auto& [_, locations] : data.diagnostic_lookup_index.type_definition_locations_by_name) {
        std::sort(locations.begin(), locations.end(), locationLess);
        locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    }
    for (auto& [_, names] : data.diagnostic_lookup_index.package_names_by_member) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
    for (auto& [_, symbols] : data.diagnostic_lookup_index.duplicate_symbols_by_uri) {
        std::sort(symbols.begin(), symbols.end(), identityLess);
        symbols.erase(std::unique(symbols.begin(), symbols.end(), [](const auto& lhs, const auto& rhs) {
                          return lhs.stable_id == rhs.stable_id;
                      }),
                      symbols.end());
    }
}

bool fuzzyMatch(std::string_view query, std::string_view candidate) {
    if (query.empty()) {
        return true;
    }

    auto query_it = query.begin();
    for (auto candidate_it = candidate.begin();
         query_it != query.end() && candidate_it != candidate.end(); ++candidate_it) {
        const auto query_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*query_it)));
        const auto candidate_char = static_cast<char>(std::tolower(static_cast<unsigned char>(*candidate_it)));
        if (query_char == candidate_char) {
            ++query_it;
        }
    }

    return query_it == query.end();
}

int lspSymbolKindForSemanticKind(std::string_view kind) {
    if (kind == "Package" || kind == "Namespace") {
        return 4;
    }
    if (kind == "ClassType") {
        return 5;
    }
    if (kind == "EnumType") {
        return 10;
    }
    if (kind == "Interface" || kind == "Modport") {
        return 11;
    }
    if (kind == "Subroutine" || kind == "SubroutinePort") {
        return 12;
    }
    if (kind == "Definition") {
        return 2;
    }
    if (kind == "TypeAlias" || kind == "Type") {
        return 26;
    }
    if (kind == "Parameter") {
        return 14;
    }
    if (kind == "EnumValue") {
        return 22;
    }
    if (kind == "Net" || kind == "Variable" || kind == "Field" || kind == "Member") {
        return 13;
    }
    return 0;
}

} // namespace

std::optional<SemanticLocation> declarationLocationForSymbol(
    const slang::SourceManager& source_manager,
    const slang::ast::Symbol& symbol) {
    if (!symbol.location.valid() || symbol.name.empty()) {
        return std::nullopt;
    }
    const auto original = source_manager.getFullyOriginalLoc(symbol.location);
    const auto uri = locationUriForSourceLocation(source_manager, original);
    if (uri.empty()) {
        return std::nullopt;
    }
    const auto name_length = static_cast<int>(symbol.name.size());
    auto range = sourceRangeForSourceRange(source_manager,
                                           slang::SourceRange(original, original + name_length));
    return SemanticLocation{.uri = uri, .range = range};
}

void buildAstIndexes(SnapshotData& data,
                     const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    if (!data.compilation || !data.source_manager) {
        return;
    }

    const auto& root = data.compilation->getRoot();
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.collectSyntaxModuleCandidates");
        collectSyntaxModuleCandidates(data, *data.source_manager, documents);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.visitRoot");
        SemanticIndexVisitor visitor(data, *data.source_manager, documents);
        root.visit(visitor);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildInactiveRegionIndex");
        buildInactiveRegionIndex(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildMacroInvocationIndex");
        buildMacroInvocationIndex(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.insertDefinitions");
        for (const auto* definition : data.compilation->getDefinitions()) {
            if (definition != nullptr) {
                insertSymbol(data, *data.source_manager, *definition);
            }
        }
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.upsertDefinitionSkeletons");
        upsertMissingAstModuleSignatureSkeletonsFromDefinitions(data, *data.source_manager, documents);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.addMissingSignaturePortSymbols");
        addMissingSignaturePortSymbols(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildScopeVisibilityIndexes");
        const auto visibility_start = std::chrono::steady_clock::now();
        buildScopeVisibilityIndexes(data, *data.source_manager);
        data.scope_visibility_build_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now() - visibility_start)
                                                  .count();
        data.package_visibility_count = data.package_visibility_by_name.size();
        data.scope_visibility_count = 0;
        for (const auto& [_, scopes] : data.scope_visibility_by_uri) {
            data.scope_visibility_count += scopes.size();
        }
        data.member_visibility_count = 0;
        for (const auto& [_, members] : data.member_completions_by_uri) {
            data.member_visibility_count += members.size();
        }
        data.callable_visibility_count = 0;
        for (const auto& [_, calls] : data.callable_invocations_by_uri) {
            data.callable_visibility_count += calls.size();
        }
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.indexInstanceMemberCompletions");
        indexInstanceMemberCompletions(data, *data.source_manager);
        data.member_visibility_count = 0;
        for (const auto& [_, members] : data.member_completions_by_uri) {
            data.member_visibility_count += members.size();
        }
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.updateModuleInstanceTargets");
        updateModuleInstanceTargets(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.sortModuleInstances");
        sortModuleInstances(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.attachInstancesToModuleDefinitions");
        attachInstancesToModuleDefinitions(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.addDeclarationReferences");
        addDeclarationReferences(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.addModuleInstantiationReferences");
        addModuleInstantiationReferences(data, documents);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildModuleCallEdgeIndex");
        buildModuleCallEdgeIndex(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.sortReferenceOccurrenceIndexes");
        sortSnapshotIndexes(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildAssignmentEdges");
        buildAssignmentEdges(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildSchematicCellPinFacts");
        buildSchematicCellPinFacts(data, *data.source_manager);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildInterfaceModportBindingIndex");
        buildInterfaceModportBindingIndex(data, *data.source_manager);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildTypeReferences");
        buildTypeReferences(data, documents);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildSameRangeReferenceAliasIndex");
        buildSameRangeReferenceAliasIndex(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.sortSnapshotIndexes");
        sortSnapshotIndexes(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildResolvedConnectionSliceFacts");
        buildResolvedConnectionSliceFacts(data, *data.source_manager);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildDesignGraphIndexes");
        buildDesignGraphIndexes(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildNavigationIndexes");
        buildNavigationIndexes(data);
    }
    {
        PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("astIndex.buildProviderLookupIndexes");
        buildProviderLookupIndexes(data);
    }
}

std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
                                                  std::string_view name) {
    return findUniqueModuleDefinitionSymbolId(data, name);
}

std::optional<std::string> findSymbolIdByNameAndKind(const SnapshotData& data,
                                                     std::string_view name,
                                                     std::string_view kind) {
    std::optional<std::string> result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && indexed_symbol.identity.kind == kind) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = stable_id;
        }
    }
    return result;
}

std::vector<SemanticLocation> typeDefinitionLocationsByName(const SnapshotData& data,
                                                            std::string_view name) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, symbol] : data.symbols_by_id) {
        if (symbol.identity.name == name && isTypeDefinitionKind(symbol.identity.kind) &&
            !isModuleDefinitionKind(symbol.identity.kind)) {
            locations.push_back(symbol.identity.location);
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

std::vector<SemanticLocation> typeDefinitionLocationsAt(const AstIndexView& view,
                                                        std::string_view uri,
                                                        int line,
                                                        int character) {
    const auto references_it = view.type_references_by_uri.find(std::string(uri));
    if (references_it == view.type_references_by_uri.end()) {
        return {};
    }
    for (const auto& reference : references_it->second) {
        if (containsPosition(reference.reference.range, line, character)) {
            return reference.definitions;
        }
    }
    return {};
}

std::optional<ReferenceOccurrenceLookup> referenceOccurrenceAtLocation(const SnapshotData& data,
                                                                         std::string_view uri,
                                                                         int line,
                                                                         int character) {
    const auto occurrences_it = data.reference_occurrences_by_uri.find(std::string(uri));
    if (occurrences_it == data.reference_occurrences_by_uri.end()) {
        return std::nullopt;
    }

    const auto& occurrence_index = occurrences_it->second;
    const auto& indexes = occurrence_index.reference_indexes;
    const auto upper = std::upper_bound(indexes.begin(),
                                        indexes.end(),
                                        std::pair{line, character},
                                        [&](const auto& position, size_t index) {
                                            const auto& range = data.references[index].location.range;
                                            return positionLess(position.first,
                                                                position.second,
                                                                range.start_line,
                                                                range.start_character);
                                        });
    if (upper == indexes.begin()) {
        return std::nullopt;
    }

    const SnapshotIndexedReference* best_reference = nullptr;
    size_t scanned = 0;
    for (auto it = upper; it != indexes.begin();) {
        --it;
        ++scanned;
        const auto& reference = data.references[*it];
        if (containsPosition(reference.location.range, line, character) &&
            (best_reference == nullptr || referenceBetterForLookup(data, reference, *best_reference))) {
            best_reference = &reference;
        }

        const auto prefix_index = static_cast<size_t>(std::distance(indexes.begin(), it));
        if (prefix_index == 0 ||
            rangeEndBeforePosition(occurrence_index.prefix_max_end_ranges[prefix_index - 1],
                                   line,
                                   character)) {
            break;
        }
    }

    if (best_reference == nullptr) {
        return std::nullopt;
    }
    return ReferenceOccurrenceLookup{.stable_id = best_reference->stable_id,
                                     .location = best_reference->location,
                                     .role = best_reference->role,
                                     .scanned_occurrence_count = scanned};
}

std::optional<SnapshotNavigationOccurrence> navigationOccurrenceAtLocation(
    const SnapshotNavigationOccurrenceIndex& index,
    int line,
    int character,
    size_t& scanned_occurrences) {
    scanned_occurrences = 0;
    const auto upper = std::upper_bound(index.occurrences.begin(),
                                        index.occurrences.end(),
                                        std::pair{line, character},
                                        [&](const auto& position,
                                            const SnapshotNavigationOccurrence& occurrence) {
                                            return positionLess(position.first,
                                                                position.second,
                                                                occurrence.location.range.start_line,
                                                                occurrence.location.range.start_character);
                                        });
    if (upper == index.occurrences.begin()) {
        return std::nullopt;
    }

    const SnapshotNavigationOccurrence* best = nullptr;
    for (auto it = upper; it != index.occurrences.begin();) {
        --it;
        ++scanned_occurrences;
        if (containsPosition(it->location.range, line, character)) {
            if (best == nullptr || it->location.range.start_line > best->location.range.start_line ||
                (it->location.range.start_line == best->location.range.start_line &&
                 it->location.range.start_character > best->location.range.start_character) ||
                (it->location.range.start_line == best->location.range.start_line &&
                 it->location.range.start_character == best->location.range.start_character &&
                 it->is_declaration != best->is_declaration && it->is_declaration) ||
                (it->location.range.start_line == best->location.range.start_line &&
                 it->location.range.start_character == best->location.range.start_character &&
                 it->is_declaration == best->is_declaration &&
                 it->has_type_display != best->has_type_display && it->has_type_display) ||
                (it->location.range.start_line == best->location.range.start_line &&
                 it->location.range.start_character == best->location.range.start_character &&
                 it->is_declaration == best->is_declaration &&
                 it->has_type_display == best->has_type_display && it->stable_id < best->stable_id)) {
                best = &*it;
            }
        }
        const auto prefix_index = static_cast<size_t>(std::distance(index.occurrences.begin(), it));
        if (prefix_index == 0 ||
            rangeEndBeforePosition(index.prefix_max_end_ranges[prefix_index - 1], line, character)) {
            break;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<SnapshotNavigationOccurrence>(*best);
}

std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                              std::string_view uri,
                                              int line,
                                              int character) {
    const auto occurrence = referenceOccurrenceAtLocation(data, uri, line, character);
    return occurrence.has_value() ? std::optional<std::string>{occurrence->stable_id} : std::nullopt;
}

std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                 std::string_view stable_id,
                                                 bool include_declaration,
                                                 size_t max_locations,
                                                 bool& truncated) {
    std::vector<SemanticLocation> locations;
    const auto aliases_it = data.reference_aliases_by_id.find(std::string(stable_id));
    const std::vector<std::string> single_id{std::string(stable_id)};
    const auto& ids = aliases_it == data.reference_aliases_by_id.end() ? single_id
                                                                       : aliases_it->second;
    for (const auto& id : ids) {
        const auto references_it = data.references_by_symbol.find(id);
        if (references_it == data.references_by_symbol.end()) {
            continue;
        }
        for (const auto index : references_it->second) {
            const auto& reference = data.references[index];
            if (!include_declaration && reference.is_declaration) {
                continue;
            }
            locations.push_back(reference.location);
            if (max_locations > 0 && locations.size() >= max_locations) {
                truncated = true;
                break;
            }
        }
        if (truncated) {
            break;
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

SemanticReferenceRole referenceRoleAtLocation(const SnapshotData& data,
                                              std::string_view stable_id,
                                              const SemanticLocation& location) {
    const auto aliases_it = data.reference_aliases_by_id.find(std::string(stable_id));
    const std::vector<std::string> single_id{std::string(stable_id)};
    const auto& ids = aliases_it == data.reference_aliases_by_id.end() ? single_id
                                                                       : aliases_it->second;
    for (const auto& id : ids) {
        const auto references_it = data.references_by_symbol.find(id);
        if (references_it == data.references_by_symbol.end()) {
            continue;
        }
        for (const auto index : references_it->second) {
            const auto& reference = data.references[index];
            if (sameLocation(reference.location, location)) {
                return reference.role;
            }
        }
    }
    return SemanticReferenceRole::Read;
}

std::optional<MacroInvocationFact> macroInvocationAt(const AstIndexView& view,
                                                     std::string_view uri,
                                                     int line,
                                                     int character) {
    const auto invocations_it = view.macro_invocations_by_uri.find(std::string(uri));
    if (invocations_it == view.macro_invocations_by_uri.end()) {
        return std::nullopt;
    }
    const MacroInvocationFact* best = nullptr;
    for (const auto& invocation : invocations_it->second) {
        if (!containsPosition(invocation.range, line, character) &&
            !containsPosition(invocation.selection_range, line, character)) {
            continue;
        }
        if (best == nullptr || invocation.range.start_line > best->range.start_line ||
            (invocation.range.start_line == best->range.start_line &&
             invocation.range.start_character > best->range.start_character) ||
            (invocation.range.start_line == best->range.start_line &&
             invocation.range.start_character == best->range.start_character &&
             locationLess(SemanticLocation{.uri = {}, .range = invocation.range},
                          SemanticLocation{.uri = {}, .range = best->range}))) {
            best = &invocation;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<MacroInvocationFact>(*best);
}

std::vector<ParseRange> inactiveRegionsForUri(const AstIndexView& view, std::string_view uri) {
    const auto it = view.inactive_regions_by_uri.find(std::string(uri));
    if (it == view.inactive_regions_by_uri.end()) {
        return {};
    }
    std::vector<ParseRange> regions;
    regions.reserve(it->second.size());
    for (const auto& region : it->second) {
        regions.push_back(region.location.range);
    }
    return regions;
}

AstIndexView buildAstIndexView(const SnapshotData* data, std::uint64_t generation) {
    AstIndexView view;
    view.generation = generation;
    view.snapshot_available = data != nullptr;
    if (data == nullptr) {
        return view;
    }

    view.modules_by_name = data->modules_by_name;
    view.module_uris_by_name = data->module_uris_by_name;
    view.module_signatures_by_name = data->ast_module_signatures_by_name;
    for (const auto& [name, signature] : view.module_signatures_by_name) {
        view.modules_by_name[name] = signature.definition;
        if (!signature.uri.empty()) {
            view.module_uris_by_name[name] = signature.uri;
        }
    }
    view.design_graph_binding_index = data->design_graph_binding_index;
    view.cone_adjacency_index = data->cone_adjacency_index;
    view.interface_modport_binding_index = data->interface_modport_binding_index;
    view.assignment_edges_by_uri = data->assignment_edges_by_uri;
    view.type_references_by_uri = data->type_references_by_uri;
    view.module_call_edge_index = data->module_call_edge_index;
    view.member_completions_by_uri = data->member_completions_by_uri;
    view.member_completions_by_qualifier_by_uri = data->member_completions_by_qualifier_by_uri;
    view.member_completions_by_stable_id = data->member_completions_by_stable_id;
    view.scope_visibility_by_uri = data->scope_visibility_by_uri;
    view.document_visibility_by_uri = data->document_visibility_by_uri;
    view.package_visibility_by_name = data->package_visibility_by_name;
    view.workspace_visibility = data->workspace_visibility;
    view.module_definition_ids_by_name = data->module_definition_ids_by_name;
    view.visible_macros_by_uri = data->visible_macros_by_uri;
    view.completion_resolve_by_id = data->completion_resolve_by_id;
    view.diagnostic_lookup_index = data->diagnostic_lookup_index;
    view.inactive_regions_by_uri = data->inactive_regions_by_uri;
    view.scope_visibility_count = data->scope_visibility_count;
    view.package_visibility_count = data->package_visibility_count;
    view.member_visibility_count = data->member_visibility_count;
    view.callable_visibility_count = data->callable_visibility_count;
    view.scope_visibility_build_micros = data->scope_visibility_build_micros;
    view.inactive_region_count = data->inactive_region_count;
    view.inactive_region_build_micros = data->inactive_region_build_micros;
    view.include_directives_by_uri = data->include_directives_by_uri;
    view.macros_by_uri = data->macros_by_uri;
    view.package_imports_by_uri = data->package_imports_by_uri;
    view.module_instances_by_uri = data->module_instances_by_uri;
    view.callable_invocations_by_uri = data->callable_invocations_by_uri;
    view.macro_invocations_by_uri = data->macro_invocations_by_uri;
    view.inlay_symbols_by_uri = data->inlay_symbols_by_uri;
    view.navigation_occurrences_by_uri = data->navigation_occurrences_by_uri;
    view.navigation_occurrences_by_symbol = data->navigation_occurrences_by_symbol;
    view.navigation_targets_by_id = data->navigation_targets_by_id;
    view.implementation_edge_index = data->implementation_edge_index;
    view.selection_range_indexes_by_uri = data->selection_range_indexes_by_uri;
    view.design_graph_module_entries.reserve(view.module_signatures_by_name.size() +
                                             data->module_entries.size());
    std::set<std::string> emitted_graph_entries;
    for (const auto& [name, signature] : view.module_signatures_by_name) {
        DesignGraphModuleEntry graph_entry;
        graph_entry.uri = signature.uri;
        graph_entry.definition = signature.definition;
        view.design_graph_module_entries.push_back(std::move(graph_entry));
        emitted_graph_entries.insert(name);
    }
    for (const auto& entry : data->module_entries) {
        if (!emitted_graph_entries.insert(entry.definition.name).second) {
            continue;
        }
        DesignGraphModuleEntry graph_entry;
        graph_entry.uri = entry.uri;
        graph_entry.definition = entry.definition;
        view.design_graph_module_entries.push_back(std::move(graph_entry));
    }
    for (const auto& [uri, instances] : data->module_instances_by_uri) {
        auto& view_instances = view.signature_module_instances_by_uri[uri];
        view_instances.reserve(instances.size());
        for (const auto& instance : instances) {
            const auto duplicate = std::any_of(view_instances.begin(),
                                               view_instances.end(),
                                               [&](const SignatureInlayModuleInstance& existing) {
                                                   return existing.module_name == instance.module_name &&
                                                          ((!existing.instance_name.empty() &&
                                                            existing.instance_name == instance.instance_name) ||
                                                           rangesOverlapOrTouch(existing.range, instance.range) ||
                                                           rangesOverlapOrTouch(existing.selection_range,
                                                                                instance.selection_range));
                                               });
            if (duplicate) {
                continue;
            }
            SignatureInlayModuleInstance view_instance;
            view_instance.module_name = instance.module_name;
            view_instance.instance_name = instance.instance_name;
            view_instance.type_display = instance.type_display.empty() ? instance.module_name
                                                                       : instance.type_display;
            view_instance.range = instance.range;
            view_instance.selection_range = instance.selection_range;
            for (const auto& [_, signature] : view.module_signatures_by_name) {
                if (signature.uri != uri) {
                    continue;
                }
                const auto cell_it = std::find_if(signature.schematic.cells.begin(),
                                                  signature.schematic.cells.end(),
                                                  [&](const SchematicCell& cell) {
                                                      return cell.name == instance.instance_name &&
                                                             cell.type == instance.module_name &&
                                                              rangesOverlapOrTouch(cell.selection_range,
                                                                                  instance.selection_range);
                                                  });
                if (cell_it != signature.schematic.cells.end()) {
                    view_instance.connections = cell_it->connections;
                    break;
                }
            }
            if (view_instance.connections.empty()) {
                view_instance.connections = instance.port_connections;
            }
            view_instance.connections.insert(view_instance.connections.end(),
                                             instance.parameter_connections.begin(),
                                             instance.parameter_connections.end());
            std::sort(view_instance.connections.begin(),
                      view_instance.connections.end(),
                      [](const SchematicConnection& lhs, const SchematicConnection& rhs) {
                          if (lhs.range.start_line != rhs.range.start_line) {
                              return lhs.range.start_line < rhs.range.start_line;
                          }
                          if (lhs.range.start_character != rhs.range.start_character) {
                              return lhs.range.start_character < rhs.range.start_character;
                          }
                          if (lhs.range.end_line != rhs.range.end_line) {
                              return lhs.range.end_line < rhs.range.end_line;
                          }
                          if (lhs.range.end_character != rhs.range.end_character) {
                              return lhs.range.end_character < rhs.range.end_character;
                          }
                          return lhs.port_name < rhs.port_name;
                      });
            view_instances.push_back(std::move(view_instance));
        }
    }

    view.symbols.reserve(data->symbols_by_id.size());
    view.diagnostic_symbols_by_id.reserve(data->symbols_by_id.size());
    view.design_graph_symbols_by_id.reserve(data->symbols_by_id.size());
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        view.symbols.push_back(AstIndexSymbol{.stable_id = stable_id,
                                              .identity = indexed_symbol.identity});
        view.diagnostic_symbols_by_id.emplace(stable_id,
                                              DiagnosticSymbol{.identity = indexed_symbol.identity,
                                                               .type_display = indexed_symbol.type_display});
        view.design_graph_symbols_by_id.emplace(stable_id,
                                                DesignGraphSymbol{.identity = indexed_symbol.identity});
    }

    view.diagnostic_references.reserve(data->references.size());
    for (const auto& reference : data->references) {
        view.diagnostic_references.push_back(DiagnosticReference{.stable_id = reference.stable_id,
                                                                 .location = reference.location});
    }
    return view;
}

AstIndexContext workspaceSymbolContext(const AstIndexView& view) {
    AstIndexContext context;
    context.generation = view.generation;
    context.snapshot_available = view.snapshot_available;
    context.symbols = view.symbols;
    return context;
}

SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                               std::string_view query,
                                               size_t limit) {
    SemanticWorkspaceSymbolResult result;
    result.generation = context.generation;
    if (!context.snapshot_available) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::set<std::string> emitted_ids;
    for (const auto& symbol : context.symbols) {
        const auto& identity = symbol.identity;
        if (identity.name.empty() || identity.location.uri.empty() ||
            !fuzzyMatch(query, identity.name) || !emitted_ids.insert(symbol.stable_id).second) {
            continue;
        }
        result.symbols.push_back(SemanticWorkspaceSymbol{.name = identity.name,
                                                         .kind = lspSymbolKindForSemanticKind(identity.kind),
                                                         .location = identity.location,
                                                         .selection_range = identity.location.range,
                                                         .stable_id = symbol.stable_id});
    }

    std::sort(result.symbols.begin(), result.symbols.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return locationLess(lhs.location, rhs.location);
    });

    if (limit > 0 && result.symbols.size() > limit) {
        result.symbols.resize(limit);
        result.truncated = true;
        result.messages.push_back("workspace/symbol results were truncated at " + std::to_string(limit) +
                                  " entries");
    }
    return result;
}

} // namespace pristine::analysis::semantic
