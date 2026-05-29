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
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
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

std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                  int line,
                                                  int character,
                                                  std::string_view prefix) {
    const auto offset = utf8OffsetAtUtf16Position(text, line, character);
    if (!offset.has_value() || *offset < prefix.size()) {
        return std::nullopt;
    }
    return *offset - prefix.size();
}

bool hasOnlyWhitespaceSinceLineStart(std::string_view text, size_t offset) {
    size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    for (size_t index = line_start; index < offset; ++index) {
        if (std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::string portSignatureLabel(const SchematicPort& port) {
    std::string label;
    const auto append_part = [&](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!label.empty()) {
            label.push_back(' ');
        }
        label += part;
    };
    append_part(port.direction);
    append_part(port.width_text);
    append_part(port.name);
    return label.empty() ? port.name : label;
}

std::string moduleSignatureLabel(const ModuleDefinition& module) {
    std::string label = module.name + "(";
    const auto port_count = module.port_details.empty() ? module.ports.size() : module.port_details.size();
    for (size_t index = 0; index < port_count; ++index) {
        if (index != 0) {
            label += ", ";
        }
        label += module.port_details.empty() ? module.ports[index] : portSignatureLabel(module.port_details[index]);
    }
    label += ")";
    return label;
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

std::optional<std::string> packageQualifierBeforeCompletion(std::string_view text,
                                                            int line,
                                                            int character,
                                                            std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!prefix_start.has_value() || *prefix_start < 2 || text[*prefix_start - 1] != ':' ||
        text[*prefix_start - 2] != ':') {
        return std::nullopt;
    }

    size_t name_end = *prefix_start - 2;
    size_t name_start = name_end;
    while (name_start > 0 && isIdentifierContinue(text[name_start - 1])) {
        --name_start;
    }
    const auto qualifier = text.substr(name_start, name_end - name_start);
    if (qualifier.empty() || !isIdentifierStart(qualifier.front())) {
        return std::nullopt;
    }
    return std::string(qualifier);
}

std::optional<std::string> memberQualifierBeforeCompletion(std::string_view text,
                                                           int line,
                                                           int character,
                                                           std::string_view prefix) {
    const auto prefix_start = completionPrefixStartOffset(text, line, character, prefix);
    if (!prefix_start.has_value() || *prefix_start == 0 || text[*prefix_start - 1] != '.') {
        return std::nullopt;
    }

    size_t name_end = *prefix_start - 1;
    size_t name_start = name_end;
    while (name_start > 0 && isIdentifierContinue(text[name_start - 1])) {
        --name_start;
    }
    const auto qualifier = text.substr(name_start, name_end - name_start);
    if (qualifier.empty() || !isIdentifierStart(qualifier.front())) {
        return std::nullopt;
    }
    return std::string(qualifier);
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

    struct ModuleInstance {
        std::string module_name;
        std::string instance_name;
        std::string uri;
        ParseRange range;
        ParseRange selection_range;
        ParseRange module_selection_range;
    };

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, IndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<IndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::vector<ModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
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
        completions.push_back(SemanticCompletionItem{.stable_id = stable_id,
                                                     .label = std::string(symbol.name),
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
    for (auto& [_, ranges] : data.selection_ranges_by_uri) {
        std::sort(ranges.begin(), ranges.end(), rangeLessWideFirst);
        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const ParseRange& lhs,
                                                                 const ParseRange& rhs) {
                         return lhs.start_line == rhs.start_line &&
                                lhs.start_character == rhs.start_character &&
                                lhs.end_line == rhs.end_line &&
                                lhs.end_character == rhs.end_character;
                     }),
                     ranges.end());
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
    return startsWithInsensitive(prefix, value);
}

void appendCompletionItem(std::vector<SemanticCompletionItem>& items,
                          std::set<std::string>& emitted,
                          SemanticCompletionItem item,
                          std::string_view prefix,
                          bool& truncated) {
    if (!prefixMatches(item.label, prefix) || !emitted.insert(item.label).second) {
        return;
    }
    items.push_back(std::move(item));
    if (items.size() >= kMaxSemanticLocations) {
        truncated = true;
    }
}

template<typename SnapshotData>
void appendSymbolCompletion(std::vector<SemanticCompletionItem>& items,
                            std::set<std::string>& emitted,
                            const SnapshotData& data,
                            const typename SnapshotData::IndexedSymbol& symbol,
                            std::string_view prefix,
                            bool& truncated) {
    if (truncated) {
        return;
    }
    appendCompletionItem(items,
                         emitted,
                         SemanticCompletionItem{.stable_id = symbol.identity.stable_id,
                                                .label = symbol.identity.name,
                                                .detail = symbol.identity.kind,
                                                .documentation = {},
                                                .insert_text = symbol.identity.name,
                                                .kind = 0,
                                                .unresolved = false},
                         prefix,
                         truncated);
    (void)data;
}

template<typename SnapshotData>
void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const SnapshotData& data,
                                 const ModuleDefinition& module,
                                 std::string_view prefix,
                                 bool& truncated) {
    const auto module_id = findSymbolIdByNameAndKind(data, module.name, "Definition")
                               .value_or(std::string("module|") + module.name);
    if (module.port_details.empty()) {
        for (const auto& port_name : module.ports) {
            if (truncated) {
                return;
            }
            appendCompletionItem(
                items,
                emitted,
                SemanticCompletionItem{.stable_id = module_id + "|port|" + port_name,
                                       .label = port_name,
                                       .detail = "Port",
                                       .documentation = "**Port** `" + port_name + "`\n\nModule: `" + module.name + "`",
                                       .insert_text = "." + port_name + "(${1:" + port_name + "})",
                                       .kind = 5,
                                       .unresolved = false},
                prefix,
                truncated);
        }
        return;
    }

    for (const auto& port : module.port_details) {
        if (truncated) {
            return;
        }
        appendCompletionItem(
            items,
            emitted,
            SemanticCompletionItem{.stable_id = module_id + "|port|" + port.name,
                                   .label = port.name,
                                   .detail = portSignatureLabel(port),
                                   .documentation = "**Port** `" + portSignatureLabel(port) + "`\n\nModule: `" +
                                                    module.name + "`",
                                   .insert_text = "." + port.name + "(${1:" + port.name + "})",
                                   .kind = 5,
                                   .unresolved = false},
            prefix,
            truncated);
    }
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
    try {
        rebuildDependenciesFor(document_uri, text);
    }
    catch (...) {
        includes_[document_uri] = {};
        for (auto& [_, including_uris] : reverse_includes_) {
            including_uris.erase(std::remove(including_uris.begin(), including_uris.end(), document_uri),
                                 including_uris.end());
        }
    }
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
    const auto document_it = documents_.find(document_uri);
    const auto* document = document_it == documents_.end() ? nullptr : &document_it->second;
    const auto prefix_start = document == nullptr
                                  ? std::optional<size_t>{}
                                  : completionPrefixStartOffset(document->text, line, character, prefix);

    const auto append_items = [&](const std::vector<SemanticCompletionItem>& items) {
        for (const auto& item : items) {
            appendCompletionItem(result.items, emitted, item, prefix, result.truncated);
            if (result.truncated) {
                return;
            }
        }
    };

    if (document != nullptr && prefix_start.has_value() && *prefix_start > 0 &&
        document->text[*prefix_start - 1] == '`') {
        const auto append_macros = [&](const std::vector<MacroDefinition>& macros) {
            for (const auto& macro : macros) {
                std::string insert_text = macro.name;
                if (macro.function_like) {
                    insert_text += "(";
                    for (size_t index = 0; index < macro.parameters.size(); ++index) {
                        if (index != 0) {
                            insert_text += ", ";
                        }
                        insert_text += "${" + std::to_string(index + 1) + ":" + macro.parameters[index] + "}";
                    }
                    insert_text += ")";
                }
                std::string documentation = "**Macro** `" + macro.name + "`";
                if (!macro.parameters.empty()) {
                    documentation += "\n\nParameters: `";
                    for (size_t index = 0; index < macro.parameters.size(); ++index) {
                        if (index != 0) {
                            documentation += ", ";
                        }
                        documentation += macro.parameters[index];
                    }
                    documentation += "`";
                }
                appendCompletionItem(result.items,
                                     emitted,
                                     SemanticCompletionItem{.stable_id = document_uri + "|macro|" + macro.name,
                                                            .label = macro.name,
                                                            .detail = macro.function_like ? "Macro function"
                                                                                          : "Macro",
                                                            .documentation = std::move(documentation),
                                                            .insert_text = std::move(insert_text),
                                                            .kind = macro.function_like ? 3 : 21,
                                                            .unresolved = false},
                                     prefix,
                                     result.truncated);
                if (result.truncated) {
                    return;
                }
            }
        };
        if (const auto macros_it = data->macros_by_uri.find(document_uri); macros_it != data->macros_by_uri.end()) {
            append_macros(macros_it->second);
        }
        for (const auto& [macro_uri, macros] : data->macros_by_uri) {
            if (macro_uri != document_uri) {
                append_macros(macros);
            }
            if (result.truncated) {
                break;
            }
        }
        return result;
    }

    if (document != nullptr) {
        if (const auto package_name = packageQualifierBeforeCompletion(document->text, line, character, prefix)) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                const auto& symbol = indexed_symbol.identity;
                if (symbol.name == *package_name || symbol.location.uri.empty()) {
                    continue;
                }
                if (symbol.stable_id.find("|" + *package_name + "::") == std::string::npos &&
                    symbol.stable_id.find("." + *package_name + ".") == std::string::npos &&
                    symbol.stable_id.find(*package_name) == std::string::npos) {
                    continue;
                }
                appendSymbolCompletion(result.items,
                                       emitted,
                                       *data,
                                       indexed_symbol,
                                       prefix,
                                       result.truncated);
                if (result.truncated) {
                    return result;
                }
            }
            if (result.items.empty()) {
                result.messages.push_back("package completion had no indexed AST members");
            }
            return result;
        }

        if (prefix_start.has_value() && *prefix_start > 0 && document->text[*prefix_start - 1] == '.') {
            const auto instances_it = data->module_instances_by_uri.find(document_uri);
            if (instances_it == data->module_instances_by_uri.end()) {
                result.unresolved = true;
                result.messages.push_back("named member completion has no indexed module instances");
                return result;
            }
            for (const auto& instance : instances_it->second) {
                if (!parseRangeContainsPosition(instance.range, line, character)) {
                    continue;
                }
                const auto module_it = data->modules_by_name.find(instance.module_name);
                if (module_it != data->modules_by_name.end()) {
                    appendModulePortCompletions(result.items,
                                                emitted,
                                                *data,
                                                module_it->second,
                                                prefix,
                                                result.truncated);
                    return result;
                }
            }
        }

        if (const auto member_name = memberQualifierBeforeCompletion(document->text, line, character, prefix)) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                if (indexed_symbol.identity.name == *member_name) {
                    continue;
                }
                if (indexed_symbol.identity.kind == "Field" || indexed_symbol.identity.kind == "Member" ||
                    indexed_symbol.identity.kind == "Net" || indexed_symbol.identity.kind == "Variable" ||
                    indexed_symbol.identity.kind == "Parameter" || indexed_symbol.identity.kind == "Subroutine") {
                    appendSymbolCompletion(result.items,
                                           emitted,
                                           *data,
                                           indexed_symbol,
                                           prefix,
                                           result.truncated);
                }
                if (result.truncated) {
                    return result;
                }
            }
            result.messages.push_back("member completion used AST symbol index fallback");
            return result;
        }

        if (prefix_start.has_value() && hasOnlyWhitespaceSinceLineStart(document->text, *prefix_start)) {
            for (const auto& [_, module] : data->modules_by_name) {
                const auto module_id = findSymbolIdByNameAndKind(*data, module.name, "Definition")
                                           .value_or(std::string("module|") + module.name);
                appendCompletionItem(result.items,
                                     emitted,
                                     SemanticCompletionItem{.stable_id = module_id,
                                                            .label = module.name,
                                                            .detail = module.kind,
                                                            .documentation = "**" + module.kind + "** `" +
                                                                             moduleSignatureLabel(module) + "`",
                                                            .insert_text = module.name,
                                                            .kind = 9,
                                                            .unresolved = false},
                                     prefix,
                                     result.truncated);
                if (result.truncated) {
                    return result;
                }
            }
        }
    }

    const auto completion_it = data->completions_by_uri.find(document_uri);
    if (completion_it != data->completions_by_uri.end()) {
        append_items(completion_it->second);
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
    return result;
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label) const {
    SemanticCompletionItem item{.stable_id = std::string(stable_id),
                                .label = std::string(label),
                                .insert_text = std::string(label)};
    const auto* data = snapshotData();
    if (data == nullptr) {
        item.unresolved = true;
        return item;
    }

    const auto stable_id_text = std::string(stable_id);
    const auto port_marker = stable_id_text.find("|port|");
    if (port_marker != std::string::npos) {
        const auto port_name = stable_id_text.substr(port_marker + 6);
        for (const auto& [_, module] : data->modules_by_name) {
            if (module.port_details.empty()) {
                if (std::find(module.ports.begin(), module.ports.end(), port_name) != module.ports.end()) {
                    item.detail = "Port";
                    item.documentation = "**Port** `" + port_name + "`\n\nModule: `" + module.name + "`";
                    item.insert_text = "." + port_name + "(${1:" + port_name + "})";
                    return item;
                }
            }
            for (const auto& port : module.port_details) {
                if (port.name != port_name) {
                    continue;
                }
                item.detail = portSignatureLabel(port);
                item.documentation = "**Port** `" + portSignatureLabel(port) + "`\n\nModule: `" +
                                     module.name + "`";
                item.insert_text = "." + port.name + "(${1:" + port.name + "})";
                return item;
            }
        }
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
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    SemanticSignatureHelpResult result{.generation = current_snapshot.generation};
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    const auto document_it = documents_.find(document_uri);
    if (document_it == documents_.end()) {
        result.unresolved = true;
        result.messages.push_back("document is not indexed in the AST snapshot");
        return result;
    }
    const auto position_offset = utf8OffsetAtUtf16Position(document_it->second.text, line, character);
    if (!position_offset.has_value()) {
        result.unresolved = true;
        result.messages.push_back("signature help position could not be mapped to a source offset");
        return result;
    }

    const auto instances_it = data->module_instances_by_uri.find(document_uri);
    if (instances_it != data->module_instances_by_uri.end()) {
        for (const auto& instance : instances_it->second) {
            if (!parseRangeContainsPosition(instance.range, line, character)) {
                continue;
            }
            const auto search_start = utf8OffsetAtUtf16Position(document_it->second.text,
                                                                instance.selection_range.end_line,
                                                                instance.selection_range.end_character);
            const auto search_end = utf8OffsetAtUtf16Position(document_it->second.text,
                                                              instance.range.end_line,
                                                              instance.range.end_character);
            if (!search_start.has_value() || !search_end.has_value()) {
                continue;
            }
            const auto open_paren = openParenBeforePosition(document_it->second.text,
                                                            *search_start,
                                                            std::min(*position_offset, *search_end));
            if (!open_paren.has_value()) {
                continue;
            }
            const auto module_it = data->modules_by_name.find(instance.module_name);
            if (module_it == data->modules_by_name.end()) {
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
                                          : std::min(activeParameterAt(document_it->second.text,
                                                                       *open_paren,
                                                                       *position_offset),
                                                     static_cast<int>(parameter_count) - 1);
            return result;
        }
    }

    result.unresolved = true;
    result.messages.push_back("no AST-backed signature invocation at position");
    return result;
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
                                                 .kind = "type",
                                                 .tooltip = "Resolved type"});
    }

    const auto instances_it = data->module_instances_by_uri.find(document_uri);
    if (instances_it != data->module_instances_by_uri.end()) {
        for (const auto& instance : instances_it->second) {
            if (!rangesOverlapOrTouch(instance.selection_range, range)) {
                continue;
            }
            const auto module_it = data->modules_by_name.find(instance.module_name);
            result.hints.push_back(SemanticInlayHint{
                .location = SemanticLocation{.uri = document_uri,
                                             .range = ParseRange{
                                                 .start_line = instance.selection_range.end_line,
                                                 .start_character = instance.selection_range.end_character,
                                                 .end_line = instance.selection_range.end_line,
                                                 .end_character = instance.selection_range.end_character}},
                .label = ": " + instance.module_name,
                .kind = "type",
                .tooltip = module_it == data->modules_by_name.end()
                               ? std::string{}
                               : moduleSignatureLabel(module_it->second)});
        }
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
        auto token_type = std::string("variable");
        if (symbol_it != data->symbols_by_id.end()) {
            const auto& kind = symbol_it->second.identity.kind;
            if (kind == "Package" || kind == "Namespace") {
                token_type = "namespace";
            }
            else if (kind == "Definition" || kind == "TypeAlias" || kind == "Type") {
                token_type = "type";
            }
            else if (kind == "ClassType") {
                token_type = "class";
            }
            else if (kind == "EnumType") {
                token_type = "enum";
            }
            else if (kind == "Interface" || kind == "Modport") {
                token_type = "interface";
            }
            else if (kind == "Subroutine" || kind == "SubroutinePort") {
                token_type = "function";
            }
            else if (kind == "Parameter") {
                token_type = "parameter";
            }
            else if (kind == "EnumValue") {
                token_type = "enumMember";
            }
        }
        result.tokens.push_back(SemanticToken{.location = reference.location,
                                              .token_type = std::move(token_type),
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
    if (lookup.unresolved) {
        return result;
    }

    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    std::vector<ParseRange> ranges;
    ranges.push_back(lookup.query_location.range);
    const auto* data = snapshotData();
    if (data != nullptr) {
        const auto ranges_it = data->selection_ranges_by_uri.find(document_uri);
        if (ranges_it != data->selection_ranges_by_uri.end()) {
            for (const auto& candidate : ranges_it->second) {
                if (parseRangeContainsPosition(candidate, line, character) &&
                    rangeContainsRange(candidate, lookup.query_location.range)) {
                    ranges.push_back(candidate);
                }
            }
        }
    }
    const auto document_it = documents_.find(document_uri);
    if (document_it != documents_.end()) {
        if (const auto line_range = lineRangeAtPosition(document_it->second.text, line, character)) {
            if (rangeContainsRange(*line_range, lookup.query_location.range)) {
                ranges.push_back(*line_range);
            }
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const ParseRange& lhs, const ParseRange& rhs) {
        if (lhs.start_line != rhs.start_line) {
            return lhs.start_line > rhs.start_line;
        }
        if (lhs.start_character != rhs.start_character) {
            return lhs.start_character > rhs.start_character;
        }
        if (lhs.end_line != rhs.end_line) {
            return lhs.end_line < rhs.end_line;
        }
        return lhs.end_character < rhs.end_character;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const ParseRange& lhs,
                                                             const ParseRange& rhs) {
                     return lhs.start_line == rhs.start_line &&
                            lhs.start_character == rhs.start_character &&
                            lhs.end_line == rhs.end_line &&
                            lhs.end_character == rhs.end_character;
                 }),
                 ranges.end());

    std::vector<ParseRange> chain;
    for (const auto& candidate : ranges) {
        if (chain.empty() || rangeContainsRange(candidate, chain.back())) {
            chain.push_back(candidate);
        }
    }

    for (size_t index = 0; index < chain.size(); ++index) {
        const auto parent = index + 1 < chain.size()
                                ? std::optional<size_t>{index + 1}
                                : std::optional<size_t>{};
        result.ranges.push_back(SemanticSelectionRange{.range = chain[index], .parent = parent});
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

        CompilationService compilation_service;
        data->macros_by_uri[uri] = compilation_service.macroDefinitions(document_it->second.text);
        data->package_imports_by_uri[uri] = compilation_service.packageImports(document_it->second.text);
        const auto append_symbol_ranges = [&](const auto& self,
                                              const std::vector<DocumentSymbol>& symbols) -> void {
            for (const auto& symbol : symbols) {
                data->selection_ranges_by_uri[uri].push_back(symbol.range);
                data->selection_ranges_by_uri[uri].push_back(symbol.selection_range);
                self(self, symbol.children);
            }
        };
        append_symbol_ranges(append_symbol_ranges,
                             compilation_service.documentSymbols(document_it->second.text, uri));

        const auto modules = compilation_service.moduleDefinitions(document_it->second.text, uri);
        for (const auto& module : modules) {
            data->modules_by_name.try_emplace(module.name, module);
            for (const auto& instance : module.instances) {
                data->module_instances_by_uri[uri].push_back(SnapshotData::ModuleInstance{
                    .module_name = instance.module_name,
                    .instance_name = instance.instance_name,
                    .uri = uri,
                    .range = instance.range,
                    .selection_range = instance.selection_range,
                    .module_selection_range = instance.module_selection_range});
            }
            data->selection_ranges_by_uri[uri].push_back(module.range);
            data->selection_ranges_by_uri[uri].push_back(module.selection_range);
            for (const auto& instance : module.instances) {
                data->selection_ranges_by_uri[uri].push_back(instance.range);
                data->selection_ranges_by_uri[uri].push_back(instance.selection_range);
                data->selection_ranges_by_uri[uri].push_back(instance.module_selection_range);
            }
        }
        for (const auto& assignment : compilation_service.continuousAssignments(document_it->second.text, uri)) {
            data->selection_ranges_by_uri[uri].push_back(assignment.range);
            data->selection_ranges_by_uri[uri].push_back(assignment.left_range);
            data->selection_ranges_by_uri[uri].push_back(assignment.right_range);
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
            slang::DiagnosticEngine diagnostic_engine(*data->source_manager);
            for (auto& tree : data->syntax_trees) {
                for (const auto& diagnostic : tree->diagnostics()) {
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
