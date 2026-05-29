#include "pristine/analysis/SemanticEngine.h"

#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <utility>

namespace pristine::analysis {
namespace {

int toLspSeverity(slang::DiagnosticSeverity severity) {
    switch (severity) {
        case slang::DiagnosticSeverity::Fatal:
        case slang::DiagnosticSeverity::Error:
            return 1;
        case slang::DiagnosticSeverity::Warning:
            return 2;
        case slang::DiagnosticSeverity::Note:
            return 3;
        case slang::DiagnosticSeverity::Ignored:
            return 4;
    }
    return 1;
}

std::string diagnosticUri(const slang::SourceManager& source_manager,
                          const slang::Diagnostic& diagnostic) {
    if (!diagnostic.location.valid()) {
        return {};
    }

    const auto location = source_manager.getFullyOriginalLoc(diagnostic.location);
    const auto path = source_manager.getFullPath(location.buffer());
    if (!path.empty()) {
        return pathToFileUri(path);
    }

    const auto file_name = source_manager.getFileName(location);
    if (file_name.empty()) {
        return {};
    }
    return withoutTrailingSlash(normalizeFileUri(file_name));
}

slang::Bag makeCompilationOptions() {
    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;
    compilation_options.flags |= slang::ast::CompilationFlags::IgnoreUnknownModules;
    compilation_options.flags |= slang::ast::CompilationFlags::AllowUseBeforeDeclare;
    compilation_options.errorLimit = 0;

    slang::Bag options;
    options.set(compilation_options);
    return options;
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

std::string symbolKindName(slang::ast::SymbolKind kind) {
    return std::string(slang::ast::toString(kind));
}

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

std::optional<SemanticLocation> declarationLocationForSymbol(const slang::SourceManager& source_manager,
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

std::optional<SemanticLocation> identifierRangeWithin(const SemanticEngineDocument& document,
                                                      const SemanticLocation& broad_location,
                                                      std::string_view name) {
    CompilationService compilation_service;
    std::optional<SemanticLocation> best;
    for (const auto& identifier : compilation_service.identifiers(document.text)) {
        if (identifier.name != name || !rangesOverlapOrTouch(identifier.range, broad_location.range)) {
            continue;
        }
        const auto location = SemanticLocation{.uri = broad_location.uri, .range = identifier.range};
        if (!best.has_value() || locationLess(location, *best)) {
            best = location;
        }
    }
    return best;
}

constexpr size_t kMaxSemanticLocations = 2000;

} // namespace

struct SemanticEngine::SnapshotData {
    struct IndexedSymbol {
        SemanticSymbolIdentity identity;
        const slang::ast::Symbol* symbol = nullptr;
        std::string type_display;
    };

    struct IndexedReference {
        std::string stable_id;
        std::string name;
        SemanticLocation location;
        bool is_declaration = false;
    };

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, IndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<IndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
};

namespace {

template<typename SnapshotData>
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
        std::string type_display;
        if (const auto* declared_type = symbol.getDeclaredType()) {
            type_display = declared_type->getType().toString();
        }
        else if (symbol.isType()) {
            if (const auto* type = symbol.as_if<slang::ast::Type>()) {
                type_display = type->toString();
            }
        }

        data.symbols_by_id.emplace(
            stable_id,
            typename SnapshotData::IndexedSymbol{
                .identity = SemanticSymbolIdentity{.stable_id = stable_id,
                                                   .name = std::string(symbol.name),
                                                   .kind = symbolKindName(symbol.kind),
                                                   .location = *location},
                .symbol = &symbol,
                .type_display = std::move(type_display)});
    }
    data.ids_by_symbol.emplace(&symbol, stable_id);

    auto& completions = data.completions_by_uri[location->uri];
    const auto duplicate = std::any_of(completions.begin(),
                                       completions.end(),
                                       [&](const SemanticCompletionItem& item) {
                                           return item.label == symbol.name;
                                       });
    if (!duplicate) {
        completions.push_back(SemanticCompletionItem{.label = std::string(symbol.name),
                                                     .detail = symbolKindName(symbol.kind),
                                                     .documentation = {},
                                                     .insert_text = std::string(symbol.name),
                                                     .kind = 0,
                                                     .unresolved = false});
    }
}

template<typename SnapshotData>
void insertReference(SnapshotData& data,
                     std::string stable_id,
                     std::string name,
                     SemanticLocation location,
                     bool is_declaration) {
    const auto duplicate = std::any_of(data.references.begin(),
                                       data.references.end(),
                                       [&](const auto& reference) {
                                           return reference.stable_id == stable_id &&
                                                  sameLocation(reference.location, location);
                                       });
    if (duplicate) {
        return;
    }

    const auto index = data.references.size();
    data.references.push_back(typename SnapshotData::IndexedReference{
        .stable_id = std::move(stable_id),
        .name = std::move(name),
        .location = std::move(location),
        .is_declaration = is_declaration});
    data.references_by_symbol[data.references.back().stable_id].push_back(index);
}

template<typename SnapshotData>
void indexSymbolReferences(SnapshotData& data,
                           const slang::SourceManager& source_manager,
                           const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                           const slang::ast::Expression& expression) {
    expression.visitSymbolReferences([&](const slang::ast::Expression& reference_expression,
                                          const slang::ast::Symbol& symbol) {
        const auto id_it = data.ids_by_symbol.find(&symbol);
        if (id_it == data.ids_by_symbol.end()) {
            return;
        }

        auto location = locationForSourceRange(source_manager, reference_expression.sourceRange);
        if (!location.has_value()) {
            return;
        }

        const auto document_it = documents.find(location->uri);
        if (document_it != documents.end()) {
            if (const auto narrow_location = identifierRangeWithin(document_it->second,
                                                                   *location,
                                                                   symbol.name)) {
                location = narrow_location;
            }
        }

        insertReference(data, id_it->second, std::string(symbol.name), *location, false);
    });
}

template<typename SnapshotData>
struct SemanticIndexVisitor
    : slang::ast::ASTVisitor<SemanticIndexVisitor<SnapshotData>,
                             slang::ast::VisitFlags::AllGood> {
    SnapshotData& data;
    const slang::SourceManager& source_manager;
    const std::unordered_map<std::string, SemanticEngineDocument>& documents;

    SemanticIndexVisitor(SnapshotData& data,
                         const slang::SourceManager& source_manager,
                         const std::unordered_map<std::string, SemanticEngineDocument>& documents) :
        data(data),
        source_manager(source_manager),
        documents(documents) {}

    template<typename T>
    void handle(const T& symbol)
        requires std::is_base_of_v<slang::ast::Symbol, T>
    {
        insertSymbol(data, source_manager, symbol);
        this->visitDefault(symbol);
    }

    template<typename T>
    void handle(const T& expression)
        requires std::is_base_of_v<slang::ast::Expression, T>
    {
        indexSymbolReferences(data, source_manager, documents, expression);
        this->visitDefault(expression);
    }
};

template<typename SnapshotData>
void addDeclarationReferences(SnapshotData& data) {
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        insertReference(data,
                        stable_id,
                        indexed_symbol.identity.name,
                        indexed_symbol.identity.location,
                        true);
    }
}

template<typename SnapshotData>
std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
                                                  std::string_view name) {
    std::optional<std::string> result;
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        if (indexed_symbol.identity.name == name && indexed_symbol.identity.kind == "Definition") {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = stable_id;
        }
    }
    return result;
}

template<typename SnapshotData>
void addModuleInstantiationReferences(SnapshotData& data,
                                      const std::unordered_map<std::string, SemanticEngineDocument>& documents) {
    CompilationService compilation_service;
    for (const auto& [document_uri, document] : documents) {
        for (const auto& module : compilation_service.moduleDefinitions(document.text, document_uri)) {
            for (const auto& instance : module.instances) {
                const auto target_id = findDefinitionSymbolId(data, instance.module_name);
                if (!target_id.has_value()) {
                    continue;
                }
                insertReference(data,
                                *target_id,
                                instance.module_name,
                                SemanticLocation{.uri = document_uri, .range = instance.module_selection_range},
                                false);
            }
        }
    }
}

template<typename SnapshotData>
void sortSnapshotIndexes(SnapshotData& data) {
    for (auto& [_, indexes] : data.references_by_symbol) {
        std::sort(indexes.begin(), indexes.end(), [&](size_t lhs, size_t rhs) {
            return locationLess(data.references[lhs].location, data.references[rhs].location);
        });
    }
    for (auto& [_, completions] : data.completions_by_uri) {
        std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.label < rhs.label;
        });
    }
}

template<typename SnapshotData>
std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                              std::string_view uri,
                                              int line,
                                              int character) {
    std::optional<std::string> best_id;
    std::optional<SemanticLocation> best_location;
    for (const auto& reference : data.references) {
        if (reference.location.uri != uri || !containsPosition(reference.location.range, line, character)) {
            continue;
        }
        if (!best_location.has_value() ||
            (reference.location.range.start_line >= best_location->range.start_line &&
             reference.location.range.start_character >= best_location->range.start_character)) {
            best_id = reference.stable_id;
            best_location = reference.location;
        }
    }
    return best_id;
}

template<typename SnapshotData>
std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                 std::string_view stable_id,
                                                 bool include_declaration,
                                                 bool& truncated) {
    std::vector<SemanticLocation> locations;
    const auto references_it = data.references_by_symbol.find(std::string(stable_id));
    if (references_it == data.references_by_symbol.end()) {
        return locations;
    }

    for (const auto index : references_it->second) {
        const auto& reference = data.references[index];
        if (!include_declaration && reference.is_declaration) {
            continue;
        }
        locations.push_back(reference.location);
        if (locations.size() >= kMaxSemanticLocations) {
            truncated = true;
            break;
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

bool prefixMatches(std::string_view value, std::string_view prefix) {
    return prefix.empty() || value.starts_with(prefix);
}

} // namespace

SemanticEngine::SemanticEngine() = default;

SemanticEngine::~SemanticEngine() = default;

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    includes_.clear();
    reverse_includes_.clear();
    snapshot_.reset();
    snapshot_data_.reset();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::configure(SemanticEngineConfig config) {
    config_ = std::move(config);
    std::sort(config_.top_modules.begin(), config_.top_modules.end());
    config_.top_modules.erase(std::unique(config_.top_modules.begin(), config_.top_modules.end()),
                              config_.top_modules.end());
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::updateDocument(std::string_view uri,
                                    std::string_view text,
                                    SemanticEngineDocumentState state) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.insert_or_assign(document_uri,
                                SemanticEngineDocument{.uri = document_uri,
                                                       .text = std::string(text),
                                                       .version = state.version,
                                                       .is_open = state.is_open,
                                                       .dirty = state.dirty});
    rebuildDependenciesFor(document_uri, text);
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::removeDocument(std::string_view uri) {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    documents_.erase(document_uri);
    includes_.erase(document_uri);
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                             including_uris.end());
    }
    reverse_includes_.erase(document_uri);
    snapshot_dirty_ = true;
    ++generation_;
}

const SemanticEngineDocument* SemanticEngine::document(std::string_view uri) const {
    const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
    if (document_it == documents_.end()) {
        return nullptr;
    }
    return &document_it->second;
}

std::vector<std::string> SemanticEngine::includedUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = includes_.find(document_uri);
    if (include_it == includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> SemanticEngine::includingUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto include_it = reverse_includes_.find(document_uri);
    if (include_it == reverse_includes_.end()) {
        return {};
    }
    return include_it->second;
}

std::vector<std::string> SemanticEngine::dirtyDocumentUris() const {
    std::vector<std::string> result;
    for (const auto& [uri, document] : documents_) {
        if (document.dirty) {
            result.push_back(uri);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> SemanticEngine::affectedDocumentUris(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::vector<std::string> pending{document_uri};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        result.push_back(current);
        const auto reverse_it = reverse_includes_.find(current);
        if (reverse_it == reverse_includes_.end()) {
            continue;
        }
        pending.insert(pending.end(), reverse_it->second.begin(), reverse_it->second.end());
    }
    std::sort(result.begin(), result.end());
    return result;
}

const SemanticEngineSnapshot& SemanticEngine::snapshot() const {
    if (!snapshot_.has_value() || snapshot_dirty_) {
        rebuildSnapshot();
    }
    return *snapshot_;
}

std::vector<SemanticEngineDiagnostic> SemanticEngine::diagnosticsFor(std::string_view uri) const {
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::vector<SemanticEngineDiagnostic> result;
    for (const auto& diagnostic : snapshot().diagnostics) {
        if (diagnostic.uri == document_uri) {
            result.push_back(diagnostic);
        }
    }
    return result;
}

const SemanticEngine::SnapshotData* SemanticEngine::snapshotData() const {
    (void)snapshot();
    return snapshot_data_.get();
}

SemanticLookupResult SemanticEngine::lookupAt(std::string_view uri, int line, int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticLookupResult result{.mode = current_snapshot.mode,
                                .generation = current_snapshot.generation,
                                .query_location = SemanticLocation{.uri = document_uri,
                                                                   .range = ParseRange{.start_line = line,
                                                                                       .start_character = character,
                                                                                       .end_line = line,
                                                                                       .end_character = character}},
                                .unresolved = true};
    const auto* data = snapshotData();
    if (data == nullptr || !data->compilation) {
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    const auto id = symbolIdAtLocation(*data, document_uri, line, character);
    if (!id.has_value()) {
        result.messages.push_back("no AST symbol at position");
        return result;
    }

    const auto symbol_it = data->symbols_by_id.find(*id);
    if (symbol_it == data->symbols_by_id.end()) {
        result.messages.push_back("AST symbol identity is not indexed");
        return result;
    }

    const auto reference_it = std::find_if(data->references.begin(),
                                           data->references.end(),
                                           [&](const SnapshotData::IndexedReference& reference) {
                                               return reference.stable_id == *id &&
                                                      reference.location.uri == document_uri &&
                                                      containsPosition(reference.location.range, line, character);
                                           });
    if (reference_it != data->references.end()) {
        result.query_location = reference_it->location;
    }
    else {
        result.query_location = symbol_it->second.identity.location;
    }
    result.symbol = symbol_it->second.identity;
    result.unresolved = false;
    return result;
}

SemanticReferenceResult SemanticEngine::definitionsAt(std::string_view uri,
                                                      int line,
                                                      int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (lookup.symbol.has_value()) {
        result.locations.push_back(lookup.symbol->location);
    }
    return result;
}

SemanticReferenceResult SemanticEngine::typeDefinitionsAt(std::string_view uri,
                                                          int line,
                                                          int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }

    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && symbol_it->second.symbol != nullptr) {
        const auto* declared_type = symbol_it->second.symbol->getDeclaredType();
        if (declared_type != nullptr) {
            const auto& type = declared_type->getType();
            if (const auto type_location = declarationLocationForSymbol(*data->source_manager, type)) {
                result.locations.push_back(*type_location);
                return result;
            }
        }
        }
    }

    result.locations.push_back(lookup.symbol->location);
    result.messages.push_back("type definition resolved to declaration because the type has no source location");
    return result;
}

SemanticReferenceResult SemanticEngine::referencesAt(std::string_view uri,
                                                     int line,
                                                     int character,
                                                     bool include_declaration) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }

    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    result.locations = locationsForSymbol(*data,
                                          lookup.symbol->stable_id,
                                          include_declaration,
                                          result.truncated);
    return result;
}

SemanticReferenceResult SemanticEngine::documentHighlightsAt(std::string_view uri,
                                                             int line,
                                                             int character) const {
    auto result = referencesAt(uri, line, character, true);
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    result.locations.erase(std::remove_if(result.locations.begin(),
                                          result.locations.end(),
                                          [&](const SemanticLocation& location) {
                                              return location.uri != document_uri;
                                          }),
                           result.locations.end());
    return result;
}

SemanticHoverResult SemanticEngine::hoverAt(std::string_view uri, int line, int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticHoverResult result{.generation = lookup.generation,
                               .messages = lookup.messages,
                               .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }

    result.contents = "**" + lookup.symbol->kind + "** `" + lookup.symbol->name + "`";
    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto symbol_it = data->symbols_by_id.find(lookup.symbol->stable_id);
        if (symbol_it != data->symbols_by_id.end() && !symbol_it->second.type_display.empty()) {
            result.contents += "\n\nType: `" + symbol_it->second.type_display + "`";
        }
    }
    result.range = lookup.query_location.range;
    return result;
}

SemanticPrepareRenameResult SemanticEngine::prepareRenameAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticPrepareRenameResult result{.generation = lookup.generation,
                                       .messages = lookup.messages,
                                       .unresolved = lookup.unresolved};
    if (!lookup.symbol.has_value()) {
        return result;
    }
    result.placeholder = lookup.symbol->name;
    result.range = lookup.query_location.range;
    return result;
}

SemanticRenameResult SemanticEngine::renameAt(std::string_view uri,
                                              int line,
                                              int character,
                                              std::string_view new_name) const {
    const auto references = referencesAt(uri, line, character, true);
    SemanticRenameResult result{.generation = references.generation,
                                .messages = references.messages,
                                .unresolved = references.unresolved,
                                .truncated = references.truncated};
    for (const auto& location : references.locations) {
        result.edits.push_back(SemanticTextEdit{.location = location,
                                                .new_text = std::string(new_name)});
    }
    return result;
}

SemanticCompletionResult SemanticEngine::completionsAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view prefix) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticCompletionResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    std::set<std::string> emitted;
    const auto append_items = [&](const std::vector<SemanticCompletionItem>& items) {
        for (const auto& item : items) {
            if (!prefixMatches(item.label, prefix) || !emitted.insert(item.label).second) {
                continue;
            }
            result.items.push_back(item);
            if (result.items.size() >= kMaxSemanticLocations) {
                result.truncated = true;
                return;
            }
        }
    };

    const auto document_it = data->completions_by_uri.find(document_uri);
    if (document_it != data->completions_by_uri.end()) {
        append_items(document_it->second);
    }
    for (const auto& [completion_uri, items] : data->completions_by_uri) {
        if (completion_uri == document_uri) {
            continue;
        }
        append_items(items);
        if (result.truncated) {
            break;
        }
    }
    (void)line;
    (void)character;
    return result;
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label) const {
    SemanticCompletionItem item{.label = std::string(label), .insert_text = std::string(label)};
    const auto* data = snapshotData();
    if (data == nullptr) {
        item.unresolved = true;
        return item;
    }

    const auto symbol_it = data->symbols_by_id.find(std::string(stable_id));
    if (symbol_it == data->symbols_by_id.end()) {
        item.unresolved = true;
        return item;
    }

    item.detail = symbol_it->second.identity.kind;
    item.documentation = "**" + symbol_it->second.identity.kind + "** `" +
                         symbol_it->second.identity.name + "`";
    if (!symbol_it->second.type_display.empty()) {
        item.documentation += "\n\nType: `" + symbol_it->second.type_display + "`";
    }
    return item;
}

SemanticSignatureHelpResult SemanticEngine::signatureHelpAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto& current_snapshot = snapshot();
    (void)uri;
    (void)line;
    (void)character;
    return SemanticSignatureHelpResult{.generation = current_snapshot.generation,
                                       .messages = {"AST-backed signature help is not indexed yet"},
                                       .unresolved = true};
}

SemanticInlayHintResult SemanticEngine::inlayHints(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticInlayHintResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
        if (indexed_symbol.identity.location.uri != document_uri || indexed_symbol.type_display.empty() ||
            !rangesOverlapOrTouch(indexed_symbol.identity.location.range, range)) {
            continue;
        }
        result.hints.push_back(SemanticInlayHint{.location = indexed_symbol.identity.location,
                                                 .label = ": " + indexed_symbol.type_display,
                                                 .kind = "type"});
    }
    std::sort(result.hints.begin(), result.hints.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticTokenResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }

    for (const auto& reference : data->references) {
        if (reference.location.uri != document_uri) {
            continue;
        }
        const auto symbol_it = data->symbols_by_id.find(reference.stable_id);
        result.tokens.push_back(SemanticToken{.location = reference.location,
                                              .token_type = symbol_it == data->symbols_by_id.end()
                                                                ? std::string("variable")
                                                                : symbol_it->second.identity.kind,
                                              .token_modifier = reference.is_declaration
                                                                    ? std::string("declaration")
                                                                    : std::string{}});
    }
    std::sort(result.tokens.begin(), result.tokens.end(), [](const auto& lhs, const auto& rhs) {
        return locationLess(lhs.location, rhs.location);
    });
    return result;
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto lookup = lookupAt(uri, line, character);
    SemanticSelectionRangeResult result{.generation = lookup.generation,
                                        .messages = lookup.messages,
                                        .unresolved = lookup.unresolved};
    if (!lookup.unresolved) {
        result.ranges.push_back(SemanticSelectionRange{.range = lookup.query_location.range});
    }
    return result;
}

void SemanticEngine::rebuildDependenciesFor(std::string_view document_uri, std::string_view text) {
    const auto normalized_uri = withoutTrailingSlash(normalizeFileUri(document_uri));
    for (auto& [_, including_uris] : reverse_includes_) {
        including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), normalized_uri),
                             including_uris.end());
    }

    CompilationService compilation_service;
    std::vector<std::string> included_uris;
    for (const auto& include : compilation_service.includeDirectives(text)) {
        included_uris.push_back(joinFileUri(uriDirectory(normalized_uri), include.target));
    }
    std::sort(included_uris.begin(), included_uris.end());
    included_uris.erase(std::unique(included_uris.begin(), included_uris.end()), included_uris.end());
    includes_[normalized_uri] = included_uris;

    for (const auto& included_uri : included_uris) {
        auto& including_uris = reverse_includes_[included_uri];
        including_uris.push_back(normalized_uri);
        std::sort(including_uris.begin(), including_uris.end());
        including_uris.erase(std::unique(including_uris.begin(), including_uris.end()), including_uris.end());
    }
}

void SemanticEngine::rebuildSnapshot() const {
    auto data = std::make_unique<SnapshotData>();
    data->source_manager = std::make_unique<slang::SourceManager>();
    data->source_manager->setDisableProximatePaths(true);
    const auto options = makeCompilationOptions();
    data->syntax_trees.reserve(documents_.size());

    SemanticEngineSnapshot next{};
    next.generation = generation_;
    next.mode = config_.build.has_value() || config_.build_pattern.has_value() ||
                        !config_.top_modules.empty()
                    ? SemanticEngineMode::Design
                    : SemanticEngineMode::Shallow;
    next.top_modules = config_.top_modules;
    next.dirty_document_uris = dirtyDocumentUris();
    next.has_shallow_ast = false;
    next.has_design_ast = false;

    for (const auto& document_entry : documents_) {
        next.document_uris.push_back(document_entry.first);
    }
    std::sort(next.document_uris.begin(), next.document_uris.end());

    for (const auto& uri : next.document_uris) {
        const auto document_it = documents_.find(uri);
        if (document_it == documents_.end()) {
            continue;
        }

        auto tree = slang::syntax::SyntaxTree::fromFileInMemory(document_it->second.text,
                                                                *data->source_manager,
                                                                "source",
                                                                fileUriToPath(uri),
                                                                options);
        if (tree) {
            data->syntax_trees.push_back(std::move(tree));
        }
    }

    if (!data->syntax_trees.empty()) {
        try {
            data->compilation = std::make_unique<slang::ast::Compilation>(options);
            for (auto& tree : data->syntax_trees) {
                data->compilation->addSyntaxTree(tree);
            }
            next.has_shallow_ast = true;
            next.has_design_ast = next.mode == SemanticEngineMode::Design;

            const auto& root = data->compilation->getRoot();
            SemanticIndexVisitor<SnapshotData> visitor(*data, *data->source_manager, documents_);
            root.visit(visitor);
            for (const auto* definition : data->compilation->getDefinitions()) {
                if (definition != nullptr) {
                    insertSymbol(*data, *data->source_manager, *definition);
                }
            }
            addDeclarationReferences(*data);
            addModuleInstantiationReferences(*data, documents_);
            sortSnapshotIndexes(*data);

            slang::DiagnosticEngine diagnostic_engine(*data->source_manager);
            for (const auto& diagnostic : data->compilation->getSemanticDiagnostics()) {
                const auto uri = diagnosticUri(*data->source_manager, diagnostic);
                if (uri.empty() || documents_.find(uri) == documents_.end()) {
                    continue;
                }
                const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                next.diagnostics.push_back(
                    SemanticEngineDiagnostic{.uri = uri,
                                             .code = std::string("slang:") +
                                                     std::string(slang::toString(diagnostic.code)),
                                             .message = diagnostic_engine.formatMessage(diagnostic),
                                             .range = sourceRangeForDiagnostic(*data->source_manager, diagnostic),
                                             .severity = toLspSeverity(severity)});
            }
        }
        catch (...) {
            next.has_shallow_ast = false;
            next.has_design_ast = false;
            data.reset();
        }
    }

    snapshot_ = std::move(next);
    snapshot_data_ = std::move(data);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
