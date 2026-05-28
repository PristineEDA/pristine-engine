#include "pristine/analysis/SemanticWorkspace.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace pristine::analysis {
namespace {

constexpr std::string_view kRootScope = "$root";

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
                                              .selection_range = symbol.selection_range});

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

bool isFileUri(std::string_view value) {
    return value.starts_with("file://");
}

bool isWindowsAbsolutePath(std::string_view value) {
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':' &&
           (value[2] == '/' || value[2] == '\\');
}

std::string toForwardSlashes(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool isDriveSegment(std::string_view value) {
    return value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':';
}

std::string normalizeFileUri(std::string_view uri) {
    if (!isFileUri(uri)) {
        return toForwardSlashes(uri);
    }

    constexpr std::string_view prefix = "file://";
    const auto path = toForwardSlashes(uri.substr(prefix.size()));
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> segments;

    size_t position = 0;
    while (position <= path.size()) {
        const auto separator = path.find('/', position);
        const auto segment = path.substr(position, separator == std::string::npos ? std::string::npos
                                                                                  : separator - position);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (!segments.empty() && !isDriveSegment(segments.back())) {
                    segments.pop_back();
                }
                else if (!absolute) {
                    segments.push_back(segment);
                }
            }
            else {
                segments.push_back(segment);
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        position = separator + 1;
    }

    std::string normalized_path = absolute ? "/" : "";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized_path.push_back('/');
        }
        normalized_path += segments[index];
    }
    return std::string(prefix) + normalized_path;
}

std::string withoutTrailingSlash(std::string value) {
    constexpr std::string_view root_uri = "file:///";
    while (value.size() > root_uri.size() && value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

std::string uriDirectory(std::string_view uri) {
    auto normalized = withoutTrailingSlash(normalizeFileUri(uri));
    constexpr std::string_view prefix = "file://";
    const auto separator = normalized.rfind('/');
    if (separator == std::string::npos || separator <= prefix.size()) {
        return normalized;
    }
    return normalized.substr(0, separator);
}

std::string joinFileUri(std::string_view base_uri, std::string_view target) {
    if (target.empty()) {
        return {};
    }
    if (isFileUri(target)) {
        return withoutTrailingSlash(normalizeFileUri(target));
    }

    const auto normalized_target = toForwardSlashes(target);
    if (!normalized_target.empty() && normalized_target.front() == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized_target));
    }
    if (isWindowsAbsolutePath(normalized_target)) {
        return withoutTrailingSlash(normalizeFileUri(std::string("file:///") + normalized_target));
    }

    auto base = withoutTrailingSlash(normalizeFileUri(base_uri));
    return withoutTrailingSlash(normalizeFileUri(base + "/" + normalized_target));
}

} // namespace

void SemanticWorkspace::clear() {
    documents_.clear();
    reverse_includes_.clear();
}

void SemanticWorkspace::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
}

void SemanticWorkspace::updateDocument(std::string_view uri,
                                       std::string_view text,
                                       SemanticDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (state.invalidate_dependents) {
        markDependentsStale(document_uri);
    }

    auto includes = compilation_service_.includeDirectives(text);
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

    for (const auto& import : compilation_service_.packageImports(text)) {
        document.imports.push_back(SemanticImport{
            .package_name = import.package_name,
            .item_name = import.item_name,
            .scope_path = scopePathAt(document, import.range.start_line, import.range.start_character),
            .range = import.range});
    }

    for (const auto& identifier : compilation_service_.identifiers(text)) {
        document.references.push_back(SemanticReference{
            .name = identifier.name,
            .scope_path = scopePathAt(document, identifier.range.start_line, identifier.range.start_character),
            .location = Location{.uri = document_uri, .range = identifier.range},
            .target_symbol_id = std::nullopt,
            .is_declaration = false});
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