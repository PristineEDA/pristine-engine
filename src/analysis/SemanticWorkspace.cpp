#include "pristine/analysis/SemanticWorkspace.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace pristine::analysis {
namespace {

constexpr std::string_view kRootScope = "$root";

bool isBuiltinTypeName(std::string_view name);

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

bool rangesEqual(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

bool diagnosticLess(const SemanticDiagnostic& lhs, const SemanticDiagnostic& rhs) {
    if (lhs.range.start_line != rhs.range.start_line) {
        return lhs.range.start_line < rhs.range.start_line;
    }
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.code != rhs.code) {
        return lhs.code < rhs.code;
    }
    return lhs.message < rhs.message;
}

bool startsWithInsensitive(std::string_view prefix, std::string_view candidate) {
    if (prefix.size() > candidate.size()) {
        return false;
    }
    for (size_t index = 0; index < prefix.size(); ++index) {
        const auto lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
        const auto rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[index])));
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

bool opensScope(int kind) {
    switch (kind) {
        case 2:
        case 3:
        case 4:
        case 5:
        case 10:
        case 11:
        case 12:
            return true;
        default:
            return false;
    }
}

std::string childScopePath(std::string_view parent_path, std::string_view name) {
    return std::string(parent_path) + "::" + std::string(name);
}

std::string parentScopePath(std::string_view scope_path) {
    const auto separator = scope_path.rfind("::");
    if (separator == std::string_view::npos) {
        return {};
    }
    return std::string(scope_path.substr(0, separator));
}

int scopeDepth(std::string_view scope_path) {
    int depth = 0;
    size_t position = 0;
    while ((position = scope_path.find("::", position)) != std::string_view::npos) {
        ++depth;
        position += 2;
    }
    return depth;
}

ParseRange rootRange() {
    return ParseRange{.start_line = 0,
                      .start_character = 0,
                      .end_line = std::numeric_limits<int>::max(),
                      .end_character = std::numeric_limits<int>::max()};
}

std::string makeSymbolId(std::string_view uri,
                         std::string_view scope_path,
                         std::string_view name,
                         const ParseRange& range) {
    return std::string(uri) + "|" + std::string(scope_path) + "|" + std::string(name) + "|" +
           std::to_string(range.start_line) + ":" + std::to_string(range.start_character);
}

void appendSymbols(SemanticDocument& document,
                   std::string_view uri,
                   std::string_view scope_path,
                   const DocumentSymbol& symbol) {
    auto symbol_scope = std::string(scope_path);
    auto symbol_id = makeSymbolId(uri, symbol_scope, symbol.name, symbol.selection_range);
    document.symbols.push_back(SemanticSymbol{.id = std::move(symbol_id),
                                              .name = symbol.name,
                                              .kind = symbol.kind,
                                              .scope_path = symbol_scope,
                                              .location = Location{.uri = std::string(uri),
                                                                   .range = symbol.selection_range},
                                              .selection_range = symbol.selection_range,
                                              .type = std::nullopt,
                                              .direction = {},
                                              .constant_expression = {},
                                              .constant_value = std::nullopt});

    std::string child_scope = symbol_scope;
    if (opensScope(symbol.kind)) {
        child_scope = childScopePath(scope_path, symbol.name);
        document.scopes.push_back(SemanticScope{.path = child_scope,
                                                .parent_path = std::string(scope_path),
                                                .range = symbol.range});
    }

    for (const auto& child : symbol.children) {
        appendSymbols(document, uri, child_scope, child);
    }
}

std::string scopePathAt(const SemanticDocument& document, int line, int character) {
    std::string best_scope = std::string(kRootScope);
    int best_depth = -1;
    for (const auto& scope : document.scopes) {
        if (!containsPosition(scope.range, line, character)) {
            continue;
        }
        const auto depth = scopeDepth(scope.path);
        if (depth > best_depth) {
            best_scope = scope.path;
            best_depth = depth;
        }
    }
    return best_scope;
}

bool symbolLess(const SemanticSymbol& lhs, const SemanticSymbol& rhs) {
    if (lhs.location.uri != rhs.location.uri) {
        return lhs.location.uri < rhs.location.uri;
    }
    if (lhs.location.range.start_line != rhs.location.range.start_line) {
        return lhs.location.range.start_line < rhs.location.range.start_line;
    }
    if (lhs.location.range.start_character != rhs.location.range.start_character) {
        return lhs.location.range.start_character < rhs.location.range.start_character;
    }
    return lhs.id < rhs.id;
}

std::string duplicateSymbolMessage(std::string_view name) {
    return std::string("Duplicate symbol '") + std::string(name) + "' in the same scope.";
}

std::string ambiguousReferenceMessage(std::string_view name, size_t definition_count) {
    return std::string("Symbol '") + std::string(name) + "' has " +
           std::to_string(definition_count) + " possible definitions in scope.";
}

std::string unresolvedPackageMessage(std::string_view name) {
    return std::string("Package '") + std::string(name) + "' could not be resolved.";
}

std::string unresolvedTypeMessage(std::string_view name) {
    return std::string("Type '") + std::string(name) + "' could not be resolved.";
}

bool isDeclarationIdentifier(const SemanticDocument& document, const SemanticReference& reference) {
    return std::any_of(document.symbols.begin(), document.symbols.end(), [&](const SemanticSymbol& symbol) {
        return symbol.name == reference.name && rangesEqual(symbol.selection_range, reference.location.range);
    });
}

bool isPackageSymbol(const SemanticSymbol& symbol) {
    return symbol.kind == 4;
}

bool canHaveUserDefinedTypeReference(const SemanticSymbol& symbol) {
    return symbol.kind == 13 || symbol.kind == 14;
}

bool isAssignableSignalSymbol(const SemanticSymbol& symbol) {
    return symbol.kind == 13;
}

bool isSimpleIdentifierExpression(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(text.front());
    if (std::isalpha(first) == 0 && text.front() != '_') {
        return false;
    }

    for (const char value : text) {
        const auto ch = static_cast<unsigned char>(value);
        if (std::isalnum(ch) == 0 && value != '_' && value != '$') {
            return false;
        }
    }
    return true;
}

std::string widthMismatchMessage(std::string_view left_name,
                                 std::int64_t left_width,
                                 std::string_view right_name,
                                 std::int64_t right_width) {
    return std::string("Width mismatch: assigning ") + std::to_string(right_width) + "-bit '" +
           std::string(right_name) + "' to " + std::to_string(left_width) + "-bit '" +
           std::string(left_name) + "'.";
}

std::vector<std::string> identifierNamesInExpression(std::string_view expression) {
    std::vector<std::string> result;
    std::set<std::string> emitted;
    size_t offset = 0;
    while (offset < expression.size()) {
        const auto first = static_cast<unsigned char>(expression[offset]);
        if (std::isalpha(first) == 0 && expression[offset] != '_') {
            ++offset;
            continue;
        }

        const auto start = offset;
        ++offset;
        while (offset < expression.size()) {
            const auto current = static_cast<unsigned char>(expression[offset]);
            if (std::isalnum(current) == 0 && expression[offset] != '_' && expression[offset] != '$') {
                break;
            }
            ++offset;
        }

        std::string name(expression.substr(start, offset - start));
        if (emitted.insert(name).second) {
            result.push_back(std::move(name));
        }
    }
    return result;
}

SemanticConeNode makeConeNode(const SemanticSymbol& symbol) {
    std::optional<std::int64_t> bit_width;
    if (symbol.type.has_value()) {
        bit_width = symbol.type->bit_width;
    }

    return SemanticConeNode{.id = symbol.id,
                            .name = symbol.name,
                            .location = symbol.location,
                            .bit_width = bit_width};
}

std::optional<SemanticReference> typeReferenceForSymbol(const SemanticDocument& document,
                                                        const SemanticSymbol& symbol) {
    if (!symbol.type.has_value() || symbol.type->name.empty() || isBuiltinTypeName(symbol.type->name) ||
        symbol.type->name == "enum" || symbol.type->display_name.find("::") != std::string::npos) {
        return std::nullopt;
    }

    std::optional<SemanticReference> result;
    for (const auto& reference : document.references) {
        if (reference.name != symbol.type->name || reference.location.range.start_line != symbol.selection_range.start_line ||
            reference.location.range.end_line != symbol.selection_range.start_line ||
            reference.location.range.end_character > symbol.selection_range.start_character ||
            isDeclarationIdentifier(document, reference)) {
            continue;
        }
        if (!result.has_value() || reference.location.range.start_character > result->location.range.start_character) {
            result = reference;
        }
    }
    return result;
}

bool referenceLess(const SemanticReference& lhs, const SemanticReference& rhs) {
    if (lhs.location.uri != rhs.location.uri) {
        return lhs.location.uri < rhs.location.uri;
    }
    if (lhs.location.range.start_line != rhs.location.range.start_line) {
        return lhs.location.range.start_line < rhs.location.range.start_line;
    }
    if (lhs.location.range.start_character != rhs.location.range.start_character) {
        return lhs.location.range.start_character < rhs.location.range.start_character;
    }
    return lhs.name < rhs.name;
}

bool isTypeDefinitionSymbol(int symbol_kind) {
    switch (symbol_kind) {
        case 2:
        case 5:
        case 10:
        case 11:
        case 26:
            return true;
        default:
            return false;
    }
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

bool isBuiltinTypeName(std::string_view name) {
    return name == "bit" || name == "logic" || name == "reg" || name == "wire" || name == "tri" ||
           name == "byte" || name == "shortint" || name == "int" || name == "integer" ||
           name == "longint" || name == "time" || name == "genvar";
}

std::optional<std::int64_t> scalarBuiltinWidth(std::string_view name) {
    if (name == "bit" || name == "logic" || name == "reg" || name == "wire" || name == "tri") {
        return 1;
    }
    if (name == "byte") {
        return 8;
    }
    if (name == "shortint") {
        return 16;
    }
    if (name == "int" || name == "integer" || name == "genvar") {
        return 32;
    }
    if (name == "longint" || name == "time") {
        return 64;
    }
    return std::nullopt;
}

SemanticTypeKind semanticTypeKindFor(const SemanticSymbolMetadata& metadata, int symbol_kind) {
    switch (symbol_kind) {
        case 2:
            return SemanticTypeKind::Module;
        case 5:
            return SemanticTypeKind::Class;
        case 10:
        case 22:
            return SemanticTypeKind::Enum;
        case 11:
            return SemanticTypeKind::Interface;
        case 19:
            return SemanticTypeKind::Module;
        case 26:
            return SemanticTypeKind::Alias;
        default:
            break;
    }

    if (metadata.type_name == "enum") {
        return SemanticTypeKind::Enum;
    }
    if (isBuiltinTypeName(metadata.type_name)) {
        return SemanticTypeKind::Builtin;
    }
    if (!metadata.type_name.empty()) {
        return SemanticTypeKind::Alias;
    }
    return SemanticTypeKind::Unknown;
}

bool metadataMatchesSymbol(const SemanticSymbolMetadata& metadata, const SemanticSymbol& symbol) {
    return metadata.name == symbol.name && metadata.kind == symbol.kind &&
           rangesEqual(metadata.selection_range, symbol.selection_range);
}

std::optional<SemanticSymbolMetadata> metadataForSymbol(const std::vector<SemanticSymbolMetadata>& metadata,
                                                        const SemanticSymbol& symbol) {
    for (const auto& candidate : metadata) {
        if (metadataMatchesSymbol(candidate, symbol)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string removeUnderscores(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        if (ch != '_') {
            result.push_back(ch);
        }
    }
    return result;
}

std::optional<std::int64_t> parseIntegerDigits(std::string_view digits, int base) {
    if (digits.empty()) {
        return std::nullopt;
    }

    std::int64_t result = 0;
    for (const char raw : digits) {
        const auto ch = static_cast<unsigned char>(raw);
        int value = -1;
        if (std::isdigit(ch) != 0) {
            value = raw - '0';
        }
        else if (std::isalpha(ch) != 0) {
            value = static_cast<char>(std::tolower(ch)) - 'a' + 10;
        }
        if (value < 0 || value >= base) {
            return std::nullopt;
        }
        if (result > (std::numeric_limits<std::int64_t>::max() - value) / base) {
            return std::nullopt;
        }
        result = result * base + value;
    }
    return result;
}

std::optional<std::int64_t> parseIntegerLiteral(std::string_view literal) {
    auto normalized = removeUnderscores(literal);
    if (normalized.empty()) {
        return std::nullopt;
    }

    const auto apostrophe = normalized.find('\'');
    if (apostrophe == std::string::npos) {
        return parseIntegerDigits(normalized, 10);
    }

    auto digits = std::string_view(normalized).substr(apostrophe + 1);
    if (!digits.empty() && (digits.front() == 's' || digits.front() == 'S')) {
        digits.remove_prefix(1);
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    int base = 10;
    const auto base_char = static_cast<char>(std::tolower(static_cast<unsigned char>(digits.front())));
    switch (base_char) {
        case 'b':
            base = 2;
            break;
        case 'o':
            base = 8;
            break;
        case 'd':
            base = 10;
            break;
        case 'h':
            base = 16;
            break;
        default:
            return std::nullopt;
    }
    digits.remove_prefix(1);
    return parseIntegerDigits(digits, base);
}

class ConstantExpressionParser {
public:
    using Resolver = std::function<std::optional<std::int64_t>(std::string_view)>;

    ConstantExpressionParser(std::string_view text, Resolver resolver) : text_(text), resolver_(std::move(resolver)) {}

    [[nodiscard]] std::optional<std::int64_t> parse() {
        auto value = parseShift();
        skipWhitespace();
        if (position_ != text_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skipWhitespace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(std::string_view token) {
        skipWhitespace();
        if (text_.substr(position_, token.size()) != token) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    std::optional<std::int64_t> parseShift() {
        auto lhs = parseAdditive();
        while (lhs.has_value()) {
            if (consume("<<")) {
                const auto rhs = parseAdditive();
                if (!rhs.has_value() || *rhs < 0 || *rhs >= 63) {
                    return std::nullopt;
                }
                *lhs <<= *rhs;
            }
            else if (consume(">>")) {
                const auto rhs = parseAdditive();
                if (!rhs.has_value() || *rhs < 0 || *rhs >= 63) {
                    return std::nullopt;
                }
                *lhs >>= *rhs;
            }
            else {
                break;
            }
        }
        return lhs;
    }

    std::optional<std::int64_t> parseAdditive() {
        auto lhs = parseMultiplicative();
        while (lhs.has_value()) {
            if (consume("+")) {
                const auto rhs = parseMultiplicative();
                if (!rhs.has_value()) {
                    return std::nullopt;
                }
                *lhs += *rhs;
            }
            else if (consume("-")) {
                const auto rhs = parseMultiplicative();
                if (!rhs.has_value()) {
                    return std::nullopt;
                }
                *lhs -= *rhs;
            }
            else {
                break;
            }
        }
        return lhs;
    }

    std::optional<std::int64_t> parseMultiplicative() {
        auto lhs = parseUnary();
        while (lhs.has_value()) {
            if (consume("*")) {
                const auto rhs = parseUnary();
                if (!rhs.has_value()) {
                    return std::nullopt;
                }
                *lhs *= *rhs;
            }
            else if (consume("/")) {
                const auto rhs = parseUnary();
                if (!rhs.has_value() || *rhs == 0) {
                    return std::nullopt;
                }
                *lhs /= *rhs;
            }
            else if (consume("%")) {
                const auto rhs = parseUnary();
                if (!rhs.has_value() || *rhs == 0) {
                    return std::nullopt;
                }
                *lhs %= *rhs;
            }
            else {
                break;
            }
        }
        return lhs;
    }

    std::optional<std::int64_t> parseUnary() {
        if (consume("+")) {
            return parseUnary();
        }
        if (consume("-")) {
            const auto value = parseUnary();
            if (!value.has_value()) {
                return std::nullopt;
            }
            return -*value;
        }
        return parsePrimary();
    }

    std::optional<std::int64_t> parsePrimary() {
        skipWhitespace();
        if (consume("(")) {
            auto value = parseShift();
            if (!consume(")")) {
                return std::nullopt;
            }
            return value;
        }

        if (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
            const auto start = position_;
            while (position_ < text_.size()) {
                const auto ch = static_cast<unsigned char>(text_[position_]);
                if (std::isalnum(ch) == 0 && text_[position_] != '_' && text_[position_] != '\'') {
                    break;
                }
                ++position_;
            }
            return parseIntegerLiteral(text_.substr(start, position_ - start));
        }

        if (position_ < text_.size() && (std::isalpha(static_cast<unsigned char>(text_[position_])) != 0 ||
                                        text_[position_] == '_' || text_[position_] == '$')) {
            const auto start = position_;
            while (position_ < text_.size()) {
                const auto ch = static_cast<unsigned char>(text_[position_]);
                if (std::isalnum(ch) == 0 && text_[position_] != '_' && text_[position_] != '$') {
                    break;
                }
                ++position_;
            }
            return resolver_(text_.substr(start, position_ - start));
        }

        return std::nullopt;
    }

    std::string_view text_;
    size_t position_ = 0;
    Resolver resolver_;
};

std::optional<std::int64_t> checkedDimensionWidth(std::int64_t left, std::int64_t right) {
    const auto distance = left > right ? left - right : right - left;
    if (distance == std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    return distance + 1;
}

std::string packageScopePath(std::string_view package_name) {
    return childScopePath(kRootScope, package_name);
}

} // namespace

void SemanticWorkspace::clear() {
    semantic_engine_.clear();
    documents_.clear();
    reverse_includes_.clear();
}

void SemanticWorkspace::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    semantic_engine_.setWorkspaceRoot(workspace_root_uri_);
}

void SemanticWorkspace::configureSemanticEngine(SemanticEngineConfig config) {
    semantic_engine_.configure(std::move(config));
}

void SemanticWorkspace::updateDocument(std::string_view uri,
                                       std::string_view text,
                                       SemanticDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    semantic_engine_.updateDocument(document_uri,
                                    text,
                                    SemanticEngineDocumentState{.version = state.version,
                                                                .is_open = state.is_open,
                                                                .dirty = state.dirty});
    if (state.invalidate_dependents) {
        markDependentsStale(document_uri);
    }

    std::vector<IncludeDirective> includes;
    try {
        includes = compilation_service_.includeDirectives(text);
    }
    catch (...) {
        includes.clear();
    }
    std::set<std::string> included_uris;
    for (const auto& include : includes) {
        for (const auto& included_uri : resolveIncludeUris(document_uri, include.target)) {
            included_uris.insert(included_uri);
        }
    }

    SemanticDocument document{.uri = document_uri,
                              .version = state.version,
                              .is_open = state.is_open,
                              .dirty = state.dirty,
                              .stale = false,
                              .includes = std::move(includes),
                              .included_uris = std::vector<std::string>(included_uris.begin(),
                                                                        included_uris.end()),
                              .imports = {},
                              .assignments = {},
                              .scopes = {SemanticScope{.path = std::string(kRootScope),
                                                       .parent_path = {},
                                                       .range = rootRange()}},
                              .symbols = {},
                              .references = {}};

    try {
        for (const auto& symbol : compilation_service_.documentSymbols(text, document_uri)) {
            appendSymbols(document, document_uri, kRootScope, symbol);
        }

        const auto metadata = compilation_service_.semanticSymbolMetadata(text, document_uri);
        for (auto& symbol : document.symbols) {
            const auto symbol_metadata = metadataForSymbol(metadata, symbol);
            if (!symbol_metadata.has_value()) {
                continue;
            }

            symbol.direction = symbol_metadata->direction;
            symbol.constant_expression = symbol_metadata->value_expression;
            if (!symbol_metadata->type_name.empty() || !symbol_metadata->type_display_name.empty() ||
                !symbol_metadata->alias_target.empty() || !symbol_metadata->enum_members.empty()) {
                symbol.type = SemanticType{.kind = semanticTypeKindFor(*symbol_metadata, symbol.kind),
                                           .name = symbol_metadata->type_name,
                                           .display_name = symbol_metadata->type_display_name,
                                           .alias_target = symbol_metadata->alias_target,
                                           .bit_width = std::nullopt,
                                           .declaration = std::nullopt,
                                           .enum_members = symbol_metadata->enum_members};
            }
        }
    }
    catch (...) {
        document.scopes.resize(1);
        document.symbols.clear();
    }

    try {
        for (const auto& import : compilation_service_.packageImports(text)) {
            document.imports.push_back(SemanticImport{
                .package_name = import.package_name,
                .item_name = import.item_name,
                .scope_path = scopePathAt(document, import.range.start_line, import.range.start_character),
                .package_range = import.package_range,
                .range = import.range});
        }
    }
    catch (...) {
        document.imports.clear();
    }

    try {
        for (const auto& assignment : compilation_service_.continuousAssignments(text, document_uri)) {
            document.assignments.push_back(SemanticAssignment{
                .left_expression = assignment.left_expression,
                .right_expression = assignment.right_expression,
                .scope_path = scopePathAt(document, assignment.range.start_line, assignment.range.start_character),
                .location = Location{.uri = document_uri, .range = assignment.range},
                .left_range = assignment.left_range,
                .right_range = assignment.right_range});
        }
    }
    catch (...) {
        document.assignments.clear();
    }

    try {
        for (const auto& identifier : compilation_service_.identifiers(text)) {
            document.references.push_back(SemanticReference{
                .name = identifier.name,
                .scope_path = scopePathAt(document, identifier.range.start_line, identifier.range.start_character),
                .location = Location{.uri = document_uri, .range = identifier.range},
                .target_symbol_id = std::nullopt,
                .is_declaration = false});
        }
    }
    catch (...) {
        document.references.clear();
    }

    std::sort(document.symbols.begin(), document.symbols.end(), symbolLess);
    std::sort(document.references.begin(), document.references.end(), referenceLess);

    documents_.insert_or_assign(document_uri, std::move(document));
    rebuildReverseIncludes();
    rebuildReferenceBindings();
    rebuildSemanticMetadata();
}

void SemanticWorkspace::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    semantic_engine_.removeDocument(document_uri);
    markDependentsStale(document_uri);
    documents_.erase(document_uri);
    rebuildReverseIncludes();
    rebuildReferenceBindings();
    rebuildSemanticMetadata();
}

const SemanticDocument* SemanticWorkspace::document(std::string_view uri) const {
    const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (document_it == documents_.end()) {
        return nullptr;
    }
    return &document_it->second;
}

std::vector<std::string> SemanticWorkspace::includedUris(std::string_view uri) const {
    const auto* source = document(uri);
    if (!source) {
        return {};
    }
    return source->included_uris;
}

std::vector<std::string> SemanticWorkspace::includingUris(std::string_view uri) const {
    const auto graph_it = reverse_includes_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (graph_it == reverse_includes_.end()) {
        return {};
    }
    return graph_it->second;
}

std::vector<std::string> SemanticWorkspace::staleDocumentUris() const {
    std::vector<std::string> result;
    for (const auto& document_entry : documents_) {
        if (document_entry.second.stale) {
            result.push_back(document_entry.first);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

SemanticLookupResult SemanticWorkspace::lookupAt(std::string_view uri, int line, int character) const {
    return semantic_engine_.lookupAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineDefinitionsAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    return semantic_engine_.definitionsAt(uri, line, character);
}

SemanticReferenceResult SemanticWorkspace::engineReferencesAt(std::string_view uri,
                                                              int line,
                                                              int character,
                                                              bool include_declaration) const {
    return semantic_engine_.referencesAt(uri, line, character, include_declaration);
}

SemanticPrepareRenameResult SemanticWorkspace::enginePrepareRenameAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    return semantic_engine_.prepareRenameAt(uri, line, character);
}

SemanticRenameResult SemanticWorkspace::engineRenameAt(std::string_view uri,
                                                      int line,
                                                      int character,
                                                      std::string_view new_name) const {
    return semantic_engine_.renameAt(uri, line, character, new_name);
}

SemanticCompletionResult SemanticWorkspace::engineCompletionsAt(std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix) const {
    return semantic_engine_.completionsAt(uri, line, character, prefix);
}

SemanticCompletionItem SemanticWorkspace::engineResolveCompletion(std::string_view stable_id,
                                                                  std::string_view label) const {
    return semantic_engine_.resolveCompletion(stable_id, label);
}

SemanticSignatureHelpResult SemanticWorkspace::engineSignatureHelpAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    return semantic_engine_.signatureHelpAt(uri, line, character);
}

SemanticInlayHintResult SemanticWorkspace::engineInlayHints(std::string_view uri,
                                                           ParseRange range) const {
    return semantic_engine_.inlayHints(uri, range);
}

SemanticTokenResult SemanticWorkspace::engineSemanticTokens(std::string_view uri) const {
    return semantic_engine_.semanticTokens(uri);
}

SemanticSelectionRangeResult SemanticWorkspace::engineSelectionRangesAt(std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    return semantic_engine_.selectionRangesAt(uri, line, character);
}

std::optional<SemanticSymbol> SemanticWorkspace::symbolById(std::string_view symbol_id) const {
    for (const auto& document_entry : documents_) {
        for (const auto& symbol : document_entry.second.symbols) {
            if (symbol.id == symbol_id) {
                return symbol;
            }
        }
    }
    return std::nullopt;
}

std::optional<SemanticSymbol> SemanticWorkspace::symbolAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto* source = document(uri);
    if (!source) {
        return std::nullopt;
    }

    std::optional<SemanticSymbol> result;
    for (const auto& symbol : source->symbols) {
        if (!containsPosition(symbol.selection_range, line, character)) {
            continue;
        }
        if (!result.has_value() || symbol.selection_range.start_line > result->selection_range.start_line ||
            (symbol.selection_range.start_line == result->selection_range.start_line &&
             symbol.selection_range.start_character > result->selection_range.start_character)) {
            result = symbol;
        }
    }
    return result;
}

std::optional<SemanticReference> SemanticWorkspace::referenceAt(std::string_view uri,
                                                                int line,
                                                                int character) const {
    const auto* source = document(uri);
    if (!source) {
        return std::nullopt;
    }

    for (const auto& reference : source->references) {
        if (containsPosition(reference.location.range, line, character)) {
            return reference;
        }
    }
    return std::nullopt;
}

std::vector<SemanticSymbol> SemanticWorkspace::resolveName(std::string_view name,
                                                           std::string_view scope_path,
                                                           std::string_view preferred_uri) const {
    const auto preferred_document = document(preferred_uri);
    std::string current_scope(scope_path);
    while (!current_scope.empty()) {
        std::vector<SemanticSymbol> result;
        for (const auto& document_entry : documents_) {
            for (const auto& symbol : document_entry.second.symbols) {
                if (symbol.name == name && symbol.scope_path == current_scope) {
                    result.push_back(symbol);
                }
            }
        }

        if (!result.empty()) {
            std::stable_sort(result.begin(), result.end(), [&](const SemanticSymbol& lhs,
                                                               const SemanticSymbol& rhs) {
                if ((lhs.location.uri == preferred_uri) != (rhs.location.uri == preferred_uri)) {
                    return lhs.location.uri == preferred_uri;
                }
                return symbolLess(lhs, rhs);
            });
            return result;
        }

        if (preferred_document) {
            for (const auto& import : preferred_document->imports) {
                if (import.scope_path != current_scope ||
                    (import.item_name.has_value() && *import.item_name != name)) {
                    continue;
                }

                const auto imported_scope = packageScopePath(import.package_name);
                for (const auto& document_entry : documents_) {
                    for (const auto& symbol : document_entry.second.symbols) {
                        if (symbol.name == name && symbol.scope_path == imported_scope) {
                            result.push_back(symbol);
                        }
                    }
                }
            }

            if (!result.empty()) {
                std::stable_sort(result.begin(), result.end(), symbolLess);
                return result;
            }
        }

        current_scope = parentScopePath(current_scope);
    }

    return {};
}

std::optional<SemanticSymbol> SemanticWorkspace::resolveReference(const SemanticReference& reference) const {
    if (reference.target_symbol_id.has_value()) {
        return symbolById(*reference.target_symbol_id);
    }

    const auto definitions = resolveName(reference.name, reference.scope_path, reference.location.uri);
    if (definitions.empty()) {
        return std::nullopt;
    }
    return definitions.front();
}

std::optional<SemanticSymbol> SemanticWorkspace::findResolvedSymbolAt(std::string_view uri,
                                                                      int line,
                                                                      int character) const {
    if (const auto symbol = symbolAt(uri, line, character)) {
        return symbol;
    }
    const auto reference = referenceAt(uri, line, character);
    if (!reference.has_value()) {
        return std::nullopt;
    }
    return resolveReference(*reference);
}

std::optional<SemanticSymbol> SemanticWorkspace::findSymbolById(std::string_view symbol_id) const {
    return symbolById(symbol_id);
}

std::vector<SemanticSymbol> SemanticWorkspace::findDefinitionsAt(std::string_view uri,
                                                                 int line,
                                                                 int character) const {
    if (const auto symbol = symbolAt(uri, line, character)) {
        return {*symbol};
    }

    const auto reference = referenceAt(uri, line, character);
    if (!reference.has_value()) {
        return {};
    }
    if (const auto resolved = resolveReference(*reference)) {
        return {*resolved};
    }
    return resolveName(reference->name, reference->scope_path, reference->location.uri);
}

std::vector<SemanticSymbol> SemanticWorkspace::findTypeDefinitionsAt(std::string_view uri,
                                                                     int line,
                                                                     int character) const {
    std::vector<SemanticSymbol> result;
    for (const auto& symbol : findDefinitionsAt(uri, line, character)) {
        if (isTypeDefinitionSymbol(symbol.kind)) {
            result.push_back(symbol);
        }
    }
    return result;
}

std::vector<SemanticReference> SemanticWorkspace::findReferencesAt(std::string_view uri,
                                                                   int line,
                                                                   int character,
                                                                   bool include_declaration) const {
    const auto target = findResolvedSymbolAt(uri, line, character);
    if (!target.has_value()) {
        return {};
    }

    std::vector<SemanticReference> result;
    if (include_declaration) {
        result.push_back(SemanticReference{.name = target->name,
                                           .scope_path = target->scope_path,
                                           .location = target->location,
                                           .target_symbol_id = target->id,
                                           .is_declaration = true});
    }

    for (const auto& document_entry : documents_) {
        for (const auto& reference : document_entry.second.references) {
            if (!reference.target_symbol_id.has_value() || *reference.target_symbol_id != target->id ||
                reference.is_declaration) {
                continue;
            }
            result.push_back(reference);
        }
    }

    std::sort(result.begin(), result.end(), referenceLess);
    return result;
}

std::vector<SemanticReference> SemanticWorkspace::findDocumentReferencesAt(std::string_view uri,
                                                                           int line,
                                                                           int character,
                                                                           bool include_declaration) const {
    std::vector<SemanticReference> result;
    for (const auto& reference : findReferencesAt(uri, line, character, include_declaration)) {
        if (reference.location.uri == uri) {
            result.push_back(reference);
        }
    }
    return result;
}

std::optional<SemanticSymbol> SemanticWorkspace::resolvedSymbolAt(std::string_view uri,
                                                                  int line,
                                                                  int character) const {
    return findResolvedSymbolAt(uri, line, character);
}

std::vector<SemanticSymbol> SemanticWorkspace::definitionsAt(std::string_view uri,
                                                             int line,
                                                             int character) const {
    return findDefinitionsAt(uri, line, character);
}

std::vector<SemanticReference> SemanticWorkspace::referencesAt(std::string_view uri,
                                                               int line,
                                                               int character,
                                                               bool include_declaration) const {
    return findReferencesAt(uri, line, character, include_declaration);
}

std::vector<SemanticReference> SemanticWorkspace::documentReferencesAt(std::string_view uri,
                                                                       int line,
                                                                       int character,
                                                                       bool include_declaration) const {
    return findDocumentReferencesAt(uri, line, character, include_declaration);
}

std::vector<SemanticSymbol> SemanticWorkspace::visibleSymbolsAt(std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix) const {
    const auto* source = document(uri);
    if (!source) {
        return {};
    }

    std::set<std::string> emitted_names;
    std::vector<SemanticSymbol> result;
    std::string current_scope = scopePathAt(*source, line, character);
    while (!current_scope.empty()) {
        std::vector<SemanticSymbol> scope_symbols;
        for (const auto& document_entry : documents_) {
            for (const auto& symbol : document_entry.second.symbols) {
                if (symbol.scope_path != current_scope || !startsWithInsensitive(prefix, symbol.name) ||
                    emitted_names.contains(symbol.name)) {
                    continue;
                }
                emitted_names.insert(symbol.name);
                scope_symbols.push_back(symbol);
            }
        }
        std::sort(scope_symbols.begin(), scope_symbols.end(), symbolLess);
        result.insert(result.end(), scope_symbols.begin(), scope_symbols.end());
        current_scope = parentScopePath(current_scope);
    }
    return result;
}

std::vector<SemanticSymbol> SemanticWorkspace::packageMembersAt(std::string_view uri,
                                                                int line,
                                                                int character,
                                                                std::string_view package_name,
                                                                std::string_view prefix) const {
    const auto* source = document(uri);
    if (!source) {
        return {};
    }

    const auto current_scope = scopePathAt(*source, line, character);
    std::vector<std::string> package_scopes;
    for (const auto& package : resolveName(package_name, current_scope, uri)) {
        if (package.kind == 4) {
            package_scopes.push_back(childScopePath(package.scope_path, package.name));
        }
    }

    if (package_scopes.empty()) {
        package_scopes.push_back(childScopePath(kRootScope, package_name));
    }

    std::sort(package_scopes.begin(), package_scopes.end());
    package_scopes.erase(std::unique(package_scopes.begin(), package_scopes.end()), package_scopes.end());

    std::set<std::string> emitted_names;
    std::vector<SemanticSymbol> result;
    for (const auto& package_scope : package_scopes) {
        std::vector<SemanticSymbol> scope_symbols;
        for (const auto& document_entry : documents_) {
            for (const auto& symbol : document_entry.second.symbols) {
                if (symbol.scope_path != package_scope || !startsWithInsensitive(prefix, symbol.name) ||
                    emitted_names.contains(symbol.name)) {
                    continue;
                }
                emitted_names.insert(symbol.name);
                scope_symbols.push_back(symbol);
            }
        }
        std::sort(scope_symbols.begin(), scope_symbols.end(), symbolLess);
        result.insert(result.end(), scope_symbols.begin(), scope_symbols.end());
    }
    return result;
}

std::vector<SemanticDiagnostic> SemanticWorkspace::diagnosticsFor(std::string_view uri) const {
    const auto* source = document(uri);
    if (!source) {
        return {};
    }

    std::map<std::pair<std::string, std::string>, std::vector<SemanticSymbol>> symbols_by_scope_name;
    for (const auto& document_entry : documents_) {
        for (const auto& symbol : document_entry.second.symbols) {
            symbols_by_scope_name[{symbol.scope_path, symbol.name}].push_back(symbol);
        }
    }

    std::vector<SemanticDiagnostic> result;
    for (auto& [_, symbols] : symbols_by_scope_name) {
        if (symbols.size() < 2) {
            continue;
        }
        std::sort(symbols.begin(), symbols.end(), symbolLess);
        for (size_t index = 1; index < symbols.size(); ++index) {
            const auto& duplicate = symbols[index];
            if (duplicate.location.uri != source->uri) {
                continue;
            }
            result.push_back(SemanticDiagnostic{.code = "duplicateSymbol",
                                                .message = duplicateSymbolMessage(duplicate.name),
                                                .range = duplicate.selection_range,
                                                .severity = 1});
        }
    }

    for (const auto& import : source->imports) {
        const auto definitions = resolveName(import.package_name, import.scope_path, source->uri);
        const auto package_it = std::find_if(definitions.begin(), definitions.end(), isPackageSymbol);
        if (package_it != definitions.end()) {
            continue;
        }

        bool has_workspace_package = false;
        for (const auto& document_entry : documents_) {
            has_workspace_package = std::any_of(document_entry.second.symbols.begin(),
                                                document_entry.second.symbols.end(),
                                                [&](const SemanticSymbol& symbol) {
                                                    return isPackageSymbol(symbol) &&
                                                           symbol.name == import.package_name;
                                                });
            if (has_workspace_package) {
                break;
            }
        }
        if (has_workspace_package) {
            continue;
        }

        result.push_back(SemanticDiagnostic{.code = "unresolvedPackage",
                                            .message = unresolvedPackageMessage(import.package_name),
                                            .range = import.package_range,
                                            .severity = 1});
    }

    std::set<std::pair<int, int>> reported_type_ranges;
    for (const auto& symbol : source->symbols) {
        if (!canHaveUserDefinedTypeReference(symbol) || !symbol.type.has_value() ||
            symbol.type->declaration.has_value()) {
            continue;
        }

        const auto type_reference = typeReferenceForSymbol(*source, symbol);
        if (!type_reference.has_value()) {
            continue;
        }

        const auto definitions = resolveName(type_reference->name,
                                             type_reference->scope_path,
                                             type_reference->location.uri);
        if (std::any_of(definitions.begin(), definitions.end(), [](const SemanticSymbol& definition) {
                return isTypeDefinitionSymbol(definition.kind);
            })) {
            continue;
        }

        const auto range_key = std::pair(type_reference->location.range.start_line,
                                         type_reference->location.range.start_character);
        if (!reported_type_ranges.insert(range_key).second) {
            continue;
        }

        result.push_back(SemanticDiagnostic{.code = "unresolvedType",
                                            .message = unresolvedTypeMessage(type_reference->name),
                                            .range = type_reference->location.range,
                                            .severity = 1});
    }

    const auto resolve_assignment_symbol = [&](std::string_view expression,
                                               std::string_view scope_path) -> std::optional<SemanticSymbol> {
        if (!isSimpleIdentifierExpression(expression)) {
            return std::nullopt;
        }

        auto definitions = resolveName(expression, scope_path, source->uri);
        definitions.erase(std::remove_if(definitions.begin(), definitions.end(), [](const SemanticSymbol& symbol) {
                              return !isAssignableSignalSymbol(symbol) || !symbol.type.has_value() ||
                                     !symbol.type->bit_width.has_value();
                          }),
                          definitions.end());
        std::sort(definitions.begin(), definitions.end(), symbolLess);
        definitions.erase(std::unique(definitions.begin(), definitions.end(), [](const SemanticSymbol& lhs,
                                                                                 const SemanticSymbol& rhs) {
                              return lhs.id == rhs.id;
                          }),
                          definitions.end());
        if (definitions.size() != 1) {
            return std::nullopt;
        }
        return definitions.front();
    };

    std::set<std::pair<int, int>> reported_width_ranges;
    for (const auto& assignment : source->assignments) {
        const auto left_symbol = resolve_assignment_symbol(assignment.left_expression, assignment.scope_path);
        const auto right_symbol = resolve_assignment_symbol(assignment.right_expression, assignment.scope_path);
        if (!left_symbol.has_value() || !right_symbol.has_value()) {
            continue;
        }

        const auto left_width = left_symbol->type->bit_width;
        const auto right_width = right_symbol->type->bit_width;
        if (!left_width.has_value() || !right_width.has_value() || *left_width <= 0 || *right_width <= 0 ||
            *left_width == *right_width) {
            continue;
        }

        const auto range_key = std::pair(assignment.right_range.start_line,
                                         assignment.right_range.start_character);
        if (!reported_width_ranges.insert(range_key).second) {
            continue;
        }

        result.push_back(SemanticDiagnostic{.code = "widthMismatch",
                                            .message = widthMismatchMessage(assignment.left_expression,
                                                                            *left_width,
                                                                            assignment.right_expression,
                                                                            *right_width),
                                            .range = assignment.right_range,
                                            .severity = 2});
    }

    for (const auto& reference : source->references) {
        if (reference.is_declaration || isDeclarationIdentifier(*source, reference)) {
            continue;
        }

        auto definitions = resolveName(reference.name, reference.scope_path, reference.location.uri);
        if (definitions.size() < 2) {
            continue;
        }

        std::sort(definitions.begin(), definitions.end(), symbolLess);
        definitions.erase(std::unique(definitions.begin(), definitions.end(), [](const SemanticSymbol& lhs,
                                                                                 const SemanticSymbol& rhs) {
                              return lhs.id == rhs.id;
                          }),
                          definitions.end());
        if (definitions.size() < 2) {
            continue;
        }

        result.push_back(SemanticDiagnostic{.code = "ambiguousReference",
                                            .message = ambiguousReferenceMessage(reference.name,
                                                                                definitions.size()),
                                            .range = reference.location.range,
                                            .severity = 2});
    }

    std::set<std::tuple<std::string, int, int, std::string>> emitted_diagnostics;
    for (const auto& diagnostic : result) {
        emitted_diagnostics.emplace(diagnostic.code,
                                    diagnostic.range.start_line,
                                    diagnostic.range.start_character,
                                    diagnostic.message);
    }
    for (const auto& diagnostic : semantic_engine_.diagnosticsFor(source->uri)) {
        if (!result.empty() && diagnostic.code.starts_with("slang:")) {
            continue;
        }
        auto key = std::tuple(diagnostic.code,
                              diagnostic.range.start_line,
                              diagnostic.range.start_character,
                              diagnostic.message);
        if (!emitted_diagnostics.insert(std::move(key)).second) {
            continue;
        }
        result.push_back(SemanticDiagnostic{.code = diagnostic.code,
                                            .message = diagnostic.message,
                                            .range = diagnostic.range,
                                            .severity = diagnostic.severity});
    }

    std::sort(result.begin(), result.end(), diagnosticLess);
    return result;
}

SemanticConeTrace SemanticWorkspace::backwardConeAt(std::string_view uri,
                                                    int line,
                                                    int character) const {
    SemanticConeTrace trace{};
    const auto* source = document(uri);
    if (!source) {
        trace.messages.push_back("Document is not indexed in the semantic workspace.");
        return trace;
    }

    const auto root = findResolvedSymbolAt(uri, line, character);
    if (!root.has_value() || !isAssignableSignalSymbol(*root)) {
        trace.messages.push_back("No signal symbol was found at the requested position.");
        return trace;
    }
    if (root->location.uri != source->uri) {
        trace.messages.push_back("Selected signal resolves outside the current document.");
        return trace;
    }

    std::map<std::string, SemanticConeNode> emitted_nodes;
    const auto append_node = [&](const SemanticSymbol& symbol) {
        auto [it, inserted] = emitted_nodes.try_emplace(symbol.id, makeConeNode(symbol));
        if (inserted) {
            trace.nodes.push_back(it->second);
        }
    };

    const auto resolve_local_signal = [&](std::string_view name,
                                          std::string_view scope_path) -> std::optional<SemanticSymbol> {
        if (!isSimpleIdentifierExpression(name)) {
            return std::nullopt;
        }

        auto definitions = resolveName(name, scope_path, source->uri);
        definitions.erase(std::remove_if(definitions.begin(), definitions.end(), [&](const SemanticSymbol& symbol) {
                              return !isAssignableSignalSymbol(symbol) || symbol.location.uri != source->uri;
                          }),
                          definitions.end());
        std::sort(definitions.begin(), definitions.end(), symbolLess);
        definitions.erase(std::unique(definitions.begin(), definitions.end(), [](const SemanticSymbol& lhs,
                                                                                 const SemanticSymbol& rhs) {
                              return lhs.id == rhs.id;
                          }),
                          definitions.end());
        if (definitions.size() != 1) {
            return std::nullopt;
        }
        return definitions.front();
    };

    trace.root_symbol_id = root->id;
    append_node(*root);

    std::vector<std::string> pending{root->id};
    std::set<std::string> visited;
    std::set<std::string> emitted_edges;
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto current_id = pending[index];
        if (!visited.insert(current_id).second) {
            continue;
        }

        for (const auto& assignment : source->assignments) {
            const auto left_symbol = resolve_local_signal(assignment.left_expression,
                                                          assignment.scope_path);
            if (!left_symbol.has_value() || left_symbol->id != current_id) {
                continue;
            }

            for (const auto& input_name : identifierNamesInExpression(assignment.right_expression)) {
                const auto input_symbol = resolve_local_signal(input_name, assignment.scope_path);
                if (!input_symbol.has_value()) {
                    continue;
                }

                append_node(*input_symbol);
                const auto edge_key = current_id + "\n" + input_symbol->id + "\n" +
                                      std::to_string(assignment.location.range.start_line) + ":" +
                                      std::to_string(assignment.location.range.start_character);
                if (emitted_edges.insert(edge_key).second) {
                    trace.edges.push_back(SemanticConeEdge{.from_symbol_id = current_id,
                                                           .to_symbol_id = input_symbol->id,
                                                           .location = assignment.location,
                                                           .expression = assignment.right_expression});
                }

                if (!visited.contains(input_symbol->id)) {
                    pending.push_back(input_symbol->id);
                }
            }
        }
    }

    return trace;
}

std::vector<std::string> SemanticWorkspace::resolveIncludeUris(std::string_view including_uri,
                                                               std::string_view target) const {
    std::set<std::string> result;
    const auto target_text = toForwardSlashes(target);
    if (target_text.empty()) {
        return {};
    }

    if (isFileUri(target_text) || (!target_text.empty() && target_text.front() == '/') ||
        isWindowsAbsolutePath(target_text)) {
        result.insert(joinFileUri({}, target_text));
    }
    else {
        result.insert(joinFileUri(uriDirectory(including_uri), target_text));
        if (!workspace_root_uri_.empty()) {
            result.insert(joinFileUri(workspace_root_uri_, target_text));
        }
    }

    return std::vector<std::string>(result.begin(), result.end());
}

void SemanticWorkspace::rebuildSemanticMetadata() {
    const auto evaluate_expression = [&](std::string_view expression,
                                         std::string_view scope_path,
                                         std::string_view uri) -> std::optional<std::int64_t> {
        ConstantExpressionParser parser(expression, [&](std::string_view name) -> std::optional<std::int64_t> {
            for (const auto& definition : resolveName(name, scope_path, uri)) {
                if (definition.constant_value.has_value()) {
                    return definition.constant_value;
                }
            }
            return std::nullopt;
        });
        return parser.parse();
    };

    const auto evaluate_width = [&](const SemanticType& type,
                                    std::string_view scope_path,
                                    std::string_view uri) -> std::optional<std::int64_t> {
        const auto type_text = type.alias_target.empty() ? std::string_view(type.display_name)
                                                         : std::string_view(type.alias_target);
        std::optional<std::int64_t> total_width;
        size_t search_position = 0;
        while (search_position < type_text.size()) {
            const auto open = type_text.find('[', search_position);
            if (open == std::string_view::npos) {
                break;
            }
            const auto close = type_text.find(']', open + 1);
            if (close == std::string_view::npos) {
                break;
            }

            const auto body = type_text.substr(open + 1, close - open - 1);
            const auto colon = body.find(':');
            std::optional<std::int64_t> dimension_width;
            if (colon == std::string_view::npos) {
                dimension_width = evaluate_expression(body, scope_path, uri);
            }
            else {
                const auto left = evaluate_expression(body.substr(0, colon), scope_path, uri);
                const auto right = evaluate_expression(body.substr(colon + 1), scope_path, uri);
                if (left.has_value() && right.has_value()) {
                    dimension_width = checkedDimensionWidth(*left, *right);
                }
            }

            if (!dimension_width.has_value() || *dimension_width <= 0) {
                return std::nullopt;
            }
            total_width = total_width.has_value() ? *total_width * *dimension_width : *dimension_width;
            search_position = close + 1;
        }

        if (total_width.has_value()) {
            return total_width;
        }
        return scalarBuiltinWidth(type.name);
    };

    for (int iteration = 0; iteration < 8; ++iteration) {
        bool changed = false;
        for (auto& document_entry : documents_) {
            for (auto& symbol : document_entry.second.symbols) {
                if (!symbol.constant_expression.empty()) {
                    const auto value = evaluate_expression(symbol.constant_expression,
                                                           symbol.scope_path,
                                                           symbol.location.uri);
                    if (symbol.constant_value != value) {
                        symbol.constant_value = value;
                        changed = true;
                    }
                }

                if (!symbol.type.has_value()) {
                    continue;
                }

                const auto width = evaluate_width(*symbol.type, symbol.scope_path, symbol.location.uri);
                if (symbol.type->bit_width != width) {
                    symbol.type->bit_width = width;
                    changed = true;
                }
            }
        }

        for (auto& document_entry : documents_) {
            for (auto& symbol : document_entry.second.symbols) {
                if (!symbol.type.has_value() || (symbol.type->kind != SemanticTypeKind::Alias &&
                                                symbol.type->kind != SemanticTypeKind::Enum)) {
                    continue;
                }

                for (const auto& definition : resolveName(symbol.type->name, symbol.scope_path,
                                                          symbol.location.uri)) {
                    if (definition.id == symbol.id || !definition.type.has_value() ||
                        (definition.kind != 10 && definition.kind != 26)) {
                        continue;
                    }

                    if (!symbol.type->declaration.has_value()) {
                        symbol.type->declaration = definition.location;
                        changed = true;
                    }
                    if (symbol.type->alias_target.empty() && !definition.type->alias_target.empty()) {
                        symbol.type->alias_target = definition.type->alias_target;
                        changed = true;
                    }
                    if (!symbol.type->bit_width.has_value() && definition.type->bit_width.has_value()) {
                        symbol.type->bit_width = definition.type->bit_width;
                        changed = true;
                    }
                    if (symbol.type->enum_members.empty() && !definition.type->enum_members.empty()) {
                        symbol.type->enum_members = definition.type->enum_members;
                        changed = true;
                    }
                    break;
                }
            }
        }

        if (!changed) {
            break;
        }
    }
}

std::optional<HoverResult> SemanticWorkspace::hoverAt(std::string_view uri,
                                                      int line,
                                                      int character) const {
    std::optional<SemanticSymbol> symbol = symbolAt(uri, line, character);
    std::optional<ParseRange> range;
    if (symbol.has_value()) {
        range = symbol->selection_range;
    }
    else {
        const auto reference = referenceAt(uri, line, character);
        if (!reference.has_value()) {
            return std::nullopt;
        }
        symbol = resolveReference(*reference);
        if (!symbol.has_value()) {
            return std::nullopt;
        }
        range = reference->location.range;
    }

    std::string label = symbol->name;
    if (symbol->type.has_value()) {
        const auto& type = *symbol->type;
        if ((symbol->kind == 13 || symbol->kind == 14 || symbol->kind == 19 || symbol->kind == 22) &&
            !type.display_name.empty()) {
            label += ": ";
            label += type.display_name;
        }
        else if ((symbol->kind == 10 || symbol->kind == 26) && !type.alias_target.empty()) {
            label += " = ";
            label += type.alias_target;
        }
    }
    if (symbol->kind == 14 && symbol->constant_value.has_value()) {
        label += " = ";
        label += std::to_string(*symbol->constant_value);
    }

    std::string contents = "**" + symbolKindLabel(symbol->kind) + "** `" + label + "`";
    if (!symbol->direction.empty()) {
        contents += "\n\nDirection: `" + symbol->direction + "`";
    }
    if (symbol->type.has_value() && symbol->type->bit_width.has_value()) {
        contents += "\n\nWidth: `" + std::to_string(*symbol->type->bit_width) + " bit";
        if (*symbol->type->bit_width != 1) {
            contents += "s";
        }
        contents += "`";
    }
    if (symbol->type.has_value() && !symbol->type->alias_target.empty() && symbol->kind != 10 &&
        symbol->kind != 26) {
        contents += "\n\nAlias: `" + symbol->type->alias_target + "`";
    }
    if (symbol->kind != 14 && symbol->constant_value.has_value()) {
        contents += "\n\nValue: `" + std::to_string(*symbol->constant_value) + "`";
    }
    if (symbol->type.has_value() && !symbol->type->enum_members.empty()) {
        contents += "\n\nEnum members: `";
        for (size_t index = 0; index < symbol->type->enum_members.size(); ++index) {
            if (index != 0) {
                contents += ", ";
            }
            contents += symbol->type->enum_members[index];
        }
        contents += "`";
    }

    return HoverResult{.contents = std::move(contents), .range = *range};
}

void SemanticWorkspace::rebuildReverseIncludes() {
    reverse_includes_.clear();
    for (const auto& document_entry : documents_) {
        for (const auto& included_uri : document_entry.second.included_uris) {
            reverse_includes_[included_uri].push_back(document_entry.first);
        }
    }

    for (auto& graph_entry : reverse_includes_) {
        auto& including_uris = graph_entry.second;
        std::sort(including_uris.begin(), including_uris.end());
        including_uris.erase(std::unique(including_uris.begin(), including_uris.end()), including_uris.end());
    }
}

void SemanticWorkspace::rebuildReferenceBindings() {
    for (auto& document_entry : documents_) {
        for (auto& reference : document_entry.second.references) {
            reference.target_symbol_id.reset();
            reference.is_declaration = false;

            const auto definitions = resolveName(reference.name, reference.scope_path, reference.location.uri);
            if (definitions.empty()) {
                continue;
            }

            const auto& definition = definitions.front();
            reference.target_symbol_id = definition.id;
            reference.is_declaration = definition.location.uri == reference.location.uri &&
                                       rangesEqual(definition.selection_range, reference.location.range);
        }
    }
}

void SemanticWorkspace::markDependentsStale(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::set<std::string> visited;
    std::vector<std::string> pending = includingUris(document_uri);
    while (!pending.empty()) {
        auto current_uri = std::move(pending.back());
        pending.pop_back();
        if (current_uri == document_uri || !visited.insert(current_uri).second) {
            continue;
        }

        if (auto document_it = documents_.find(current_uri); document_it != documents_.end()) {
            document_it->second.stale = true;
        }

        for (const auto& parent_uri : includingUris(current_uri)) {
            pending.push_back(parent_uri);
        }
    }
}

} // namespace pristine::analysis
