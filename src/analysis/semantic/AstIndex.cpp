#include "AstIndex.h"

#include "CompletionProvider.h"
#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/ast/types/Type.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <type_traits>

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
    data.references.push_back(SnapshotIndexedReference{.stable_id = std::move(stable_id),
                                                       .name = std::move(name),
                                                       .location = std::move(location),
                                                       .is_declaration = is_declaration});
    data.references_by_symbol[data.references.back().stable_id].push_back(index);
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
            SnapshotIndexedSymbol{.identity = SemanticSymbolIdentity{.stable_id = stable_id,
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

void indexModuleInstanceBinding(SnapshotData& data,
                                const slang::SourceManager& source_manager,
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

    const auto ranges_equal = [](const ParseRange& lhs, const ParseRange& rhs) {
        return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
               lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
    };

    const auto instances_it = data.module_instances_by_uri.find(instance_location->uri);
    if (instances_it == data.module_instances_by_uri.end()) {
        return;
    }
    for (auto& module_instance : instances_it->second) {
        if (module_instance.instance_name == instance.name &&
            ranges_equal(module_instance.selection_range, instance_location->range)) {
            module_instance.target_stable_id = definition_id;
            return;
        }
    }
}

struct SemanticIndexVisitor
    : slang::ast::ASTVisitor<SemanticIndexVisitor, slang::ast::VisitFlags::AllGood> {
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
        if constexpr (std::is_same_v<T, slang::ast::InstanceSymbol>) {
            indexModuleInstanceBinding(data, source_manager, symbol);
        }
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

void addDeclarationReferences(SnapshotData& data) {
    for (const auto& [stable_id, indexed_symbol] : data.symbols_by_id) {
        insertReference(data,
                        stable_id,
                        indexed_symbol.identity.name,
                        indexed_symbol.identity.location,
                        true);
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
                                       : findDefinitionSymbolId(data, instance.module_name);
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

void sortSnapshotIndexes(SnapshotData& data) {
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
    SemanticIndexVisitor visitor(data, *data.source_manager, documents);
    root.visit(visitor);
    for (const auto* definition : data.compilation->getDefinitions()) {
        if (definition != nullptr) {
            insertSymbol(data, *data.source_manager, *definition);
        }
    }
    addDeclarationReferences(data);
    addModuleInstantiationReferences(data, documents);
    sortSnapshotIndexes(data);
}

std::optional<std::string> findDefinitionSymbolId(const SnapshotData& data,
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

std::vector<SemanticLocation> locationsForSymbol(const SnapshotData& data,
                                                 std::string_view stable_id,
                                                 bool include_declaration,
                                                 size_t max_locations,
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
        if (max_locations > 0 && locations.size() >= max_locations) {
            truncated = true;
            break;
        }
    }
    std::sort(locations.begin(), locations.end(), locationLess);
    locations.erase(std::unique(locations.begin(), locations.end(), sameLocation), locations.end());
    return locations;
}

std::vector<SemanticLocation> moduleImplementationLocations(const SnapshotData& data,
                                                            std::string_view module_name,
                                                            size_t max_locations,
                                                            bool& truncated) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            if (instance.module_name != module_name) {
                continue;
            }
            locations.push_back(SemanticLocation{.uri = instance.uri,
                                                 .range = instance.module_selection_range});
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

std::optional<SnapshotModuleInstance> moduleInstanceAt(const SnapshotData& data,
                                                       std::string_view uri,
                                                       int line,
                                                       int character) {
    const auto instances_it = data.module_instances_by_uri.find(std::string(uri));
    if (instances_it == data.module_instances_by_uri.end()) {
        return std::nullopt;
    }
    std::optional<SnapshotModuleInstance> best;
    for (const auto& instance : instances_it->second) {
        if (!containsPosition(instance.module_selection_range, line, character)) {
            continue;
        }
        if (!best.has_value() ||
            locationLess(SemanticLocation{.uri = instance.uri, .range = instance.module_selection_range},
                         SemanticLocation{.uri = best->uri, .range = best->module_selection_range})) {
            best = instance;
        }
    }
    return best;
}

AstIndexView buildAstIndexView(const SnapshotData* data, std::uint64_t generation) {
    AstIndexView view{.generation = generation, .snapshot_available = data != nullptr};
    if (data == nullptr) {
        return view;
    }

    view.symbols.reserve(data->symbols_by_id.size());
    view.navigation_symbols_by_id.reserve(data->symbols_by_id.size());
    view.diagnostic_symbols_by_id.reserve(data->symbols_by_id.size());
    view.design_graph_symbols_by_id.reserve(data->symbols_by_id.size());
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        view.symbols.push_back(AstIndexSymbol{.stable_id = stable_id,
                                              .identity = indexed_symbol.identity});
        view.navigation_symbols_by_id.emplace(stable_id, indexed_symbol.identity);
        view.diagnostic_symbols_by_id.emplace(stable_id,
                                              DiagnosticSymbol{.identity = indexed_symbol.identity,
                                                               .type_display = indexed_symbol.type_display});
        view.design_graph_symbols_by_id.emplace(stable_id,
                                                DesignGraphSymbol{.identity = indexed_symbol.identity});
    }

    view.navigation_references.reserve(data->references.size());
    view.diagnostic_references.reserve(data->references.size());
    for (const auto& reference : data->references) {
        view.navigation_references.push_back(NavigationReference{.stable_id = reference.stable_id,
                                                                 .location = reference.location,
                                                                 .is_declaration = reference.is_declaration});
        view.diagnostic_references.push_back(DiagnosticReference{.stable_id = reference.stable_id,
                                                                 .location = reference.location});
        view.design_graph_symbol_ranges_by_uri[reference.location.uri].push_back(
            DesignGraphRangeSymbol{.range = reference.location.range,
                                   .stable_id = reference.stable_id});
    }
    return view;
}

AstIndexContext workspaceSymbolContext(const AstIndexView& view) {
    return AstIndexContext{.generation = view.generation,
                           .snapshot_available = view.snapshot_available,
                           .symbols = view.symbols};
}

SemanticWorkspaceSymbolResult workspaceSymbols(const AstIndexContext& context,
                                               std::string_view query,
                                               size_t limit) {
    SemanticWorkspaceSymbolResult result{.generation = context.generation};
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
