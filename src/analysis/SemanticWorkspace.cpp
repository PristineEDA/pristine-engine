#include "pristine/analysis/SemanticWorkspace.h"

#include <algorithm>
#include <cctype>
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
                              .scopes = {SemanticScope{.path = std::string(kRootScope),
                                                       .parent_path = {},
                                                       .range = rootRange()}},
                              .symbols = {},
                              .references = {}};

    try {
        for (const auto& symbol : compilation_service_.documentSymbols(text, document_uri)) {
            appendSymbols(document, document_uri, kRootScope, symbol);
        }
    }
    catch (...) {
        document.scopes.resize(1);
        document.symbols.clear();
    }

    for (const auto& identifier : compilation_service_.identifiers(text)) {
        document.references.push_back(SemanticReference{
            .name = identifier.name,
            .scope_path = scopePathAt(document, identifier.range.start_line, identifier.range.start_character),
            .location = Location{.uri = document_uri, .range = identifier.range}});
    }

    std::sort(document.symbols.begin(), document.symbols.end(), symbolLess);
    std::sort(document.references.begin(), document.references.end(), referenceLess);

    documents_.insert_or_assign(document_uri, std::move(document));
    rebuildReverseIncludes();
}

void SemanticWorkspace::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    markDependentsStale(document_uri);
    documents_.erase(document_uri);
    rebuildReverseIncludes();
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

        current_scope = parentScopePath(current_scope);
    }

    return {};
}

std::optional<SemanticSymbol> SemanticWorkspace::resolveReference(const SemanticReference& reference) const {
    const auto definitions = resolveName(reference.name, reference.scope_path, reference.location.uri);
    if (definitions.empty()) {
        return std::nullopt;
    }
    return definitions.front();
}

std::optional<SemanticSymbol> SemanticWorkspace::resolvedSymbolAt(std::string_view uri,
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

std::vector<SemanticSymbol> SemanticWorkspace::definitionsAt(std::string_view uri,
                                                             int line,
                                                             int character) const {
    if (const auto symbol = symbolAt(uri, line, character)) {
        return {*symbol};
    }

    const auto reference = referenceAt(uri, line, character);
    if (!reference.has_value()) {
        return {};
    }
    return resolveName(reference->name, reference->scope_path, reference->location.uri);
}

std::vector<SemanticReference> SemanticWorkspace::referencesAt(std::string_view uri,
                                                               int line,
                                                               int character,
                                                               bool include_declaration) const {
    const auto target = resolvedSymbolAt(uri, line, character);
    if (!target.has_value()) {
        return {};
    }

    std::vector<SemanticReference> result;
    if (include_declaration) {
        result.push_back(SemanticReference{.name = target->name,
                                           .scope_path = target->scope_path,
                                           .location = target->location});
    }

    for (const auto& document_entry : documents_) {
        for (const auto& reference : document_entry.second.references) {
            if (reference.name != target->name) {
                continue;
            }
            if (rangesEqual(reference.location.range, target->selection_range) &&
                reference.location.uri == target->location.uri) {
                continue;
            }
            const auto resolved = resolveReference(reference);
            if (resolved.has_value() && resolved->id == target->id) {
                result.push_back(reference);
            }
        }
    }

    std::sort(result.begin(), result.end(), referenceLess);
    return result;
}

std::vector<SemanticReference> SemanticWorkspace::documentReferencesAt(std::string_view uri,
                                                                       int line,
                                                                       int character,
                                                                       bool include_declaration) const {
    std::vector<SemanticReference> result;
    for (const auto& reference : referencesAt(uri, line, character, include_declaration)) {
        if (reference.location.uri == uri) {
            result.push_back(reference);
        }
    }
    return result;
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