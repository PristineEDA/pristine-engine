#include "pristine/analysis/SemanticEngine.h"

#include "semantic/AstIndex.h"
#include "semantic/CodeActionProvider.h"
#include "semantic/CompletionProvider.h"
#include "semantic/DesignGraphProvider.h"
#include "semantic/DiagnosticProvider.h"
#include "semantic/NavigationProvider.h"
#include "semantic/QueryCache.h"
#include "semantic/SignatureInlayProvider.h"
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

constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";

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

bool isBuiltinTypeName(std::string_view name) {
    return name == "bit" || name == "logic" || name == "reg" || name == "wire" || name == "tri" ||
           name == "byte" || name == "shortint" || name == "int" || name == "integer" ||
           name == "longint" || name == "time" || name == "genvar";
}

bool isTypeDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "TypeAlias" || kind == "Type" || kind == "ClassType" ||
           kind == "EnumType" || kind == "Interface" || kind == "Modport";
}

bool isModuleDefinitionKind(std::string_view kind) {
    return kind == "Definition" || kind == "Instance" || kind == "InstanceBody";
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

std::string declarationLocationLabel(const SemanticLocation& location) {
    return location.uri + ":" + std::to_string(location.range.start_line + 1) + ":" +
           std::to_string(location.range.start_character + 1);
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

std::set<std::string> connectedNamedPortsBeforePosition(std::string_view text,
                                                        size_t open_paren_offset,
                                                        size_t position_offset) {
    std::set<std::string> connected_ports;
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
        if (value != '.' || depth != 0) {
            continue;
        }

        const auto name_start = offset + 1;
        if (name_start >= position_offset || !isIdentifierStart(text[name_start])) {
            continue;
        }

        size_t name_end = name_start + 1;
        while (name_end < position_offset && isIdentifierContinue(text[name_end])) {
            ++name_end;
        }

        size_t cursor = name_end;
        while (cursor < position_offset && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < position_offset && text[cursor] == '(') {
            connected_ports.insert(std::string(text.substr(name_start, name_end - name_start)));
        }

        offset = name_end;
    }
    return connected_ports;
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

std::optional<ParseRange> userTypeReferenceRange(std::string_view text,
                                                 const SemanticSymbolMetadata& metadata) {
    if (metadata.type_name.empty() || metadata.type_name == "enum" ||
        metadata.type_display_name.find("::") != std::string::npos ||
        isBuiltinTypeName(metadata.type_name)) {
        return std::nullopt;
    }

    CompilationService compilation_service;
    std::optional<ParseRange> best;
    for (const auto& identifier : compilation_service.identifiers(text)) {
        if (identifier.name != metadata.type_name ||
            identifier.range.start_line != metadata.selection_range.start_line ||
            identifier.range.end_line != metadata.selection_range.start_line ||
            identifier.range.end_character > metadata.selection_range.start_character) {
            continue;
        }
        if (!best.has_value() || identifier.range.start_character > best->start_character) {
            best = identifier.range;
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
        std::string target_stable_id;
        std::string uri;
        ParseRange range;
        ParseRange selection_range;
        ParseRange module_selection_range;
    };

    struct ModuleEntry {
        std::string uri;
        ModuleDefinition definition;
    };

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, IndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<IndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
    std::vector<ModuleEntry> module_entries;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, ModuleSchematic> schematics_by_name;
    std::unordered_map<std::string, std::string> schematic_uris_by_name;
    std::unordered_map<std::string, std::vector<ContinuousAssignment>> assignments_by_uri;
    std::unordered_map<std::string, std::vector<ModuleInstance>> module_instances_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
    std::unordered_map<std::string, std::vector<SemanticSymbolMetadata>> metadata_by_uri;
};

namespace {

template<typename SnapshotData>
semantic::DesignGraphContext designGraphContextFor(const SnapshotData* data,
                                                   const SemanticEngineSnapshot& snapshot,
                                                   const SemanticEngineConfig& config) {
    semantic::DesignGraphContext context{.generation = snapshot.generation,
                                         .snapshot_available = data != nullptr,
                                         .top_modules = config.top_modules};
    if (data == nullptr) {
        return context;
    }
    context.modules_by_name = data->modules_by_name;
    context.module_uris_by_name = data->module_uris_by_name;
    context.schematics_by_name = data->schematics_by_name;
    context.schematic_uris_by_name = data->schematic_uris_by_name;
    context.module_entries.reserve(data->module_entries.size());
    for (const auto& entry : data->module_entries) {
        context.module_entries.push_back(semantic::DesignGraphModuleEntry{.uri = entry.uri,
                                                                          .definition = entry.definition});
    }
    context.assignments_by_uri = data->assignments_by_uri;
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        context.symbols_by_id.emplace(stable_id,
                                      semantic::DesignGraphSymbol{.identity = indexed_symbol.identity});
    }
    for (const auto& reference : data->references) {
        context.symbol_ranges_by_uri[reference.location.uri].push_back(
            semantic::DesignGraphRangeSymbol{.range = reference.location.range,
                                             .stable_id = reference.stable_id});
    }
    return context;
}

template<typename SnapshotData>
semantic::AstIndexContext astIndexContextFor(const SnapshotData* data,
                                             const SemanticEngineSnapshot& snapshot) {
    semantic::AstIndexContext context{.generation = snapshot.generation,
                                     .snapshot_available = data != nullptr};
    if (data == nullptr) {
        return context;
    }
    context.symbols.reserve(data->symbols_by_id.size());
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        context.symbols.push_back(semantic::AstIndexSymbol{.stable_id = stable_id,
                                                           .identity = indexed_symbol.identity});
    }
    return context;
}

template<typename SnapshotData>
semantic::NavigationContext navigationContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 const std::string* document_text = nullptr) {
    semantic::NavigationContext context{.generation = snapshot.generation,
                                       .snapshot_available = data != nullptr,
                                       .document_uri = std::move(document_uri),
                                       .document_text = document_text};
    if (data == nullptr) {
        return context;
    }
    context.symbols_by_id.reserve(data->symbols_by_id.size());
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        context.symbols_by_id.emplace(stable_id, indexed_symbol.identity);
    }
    context.references.reserve(data->references.size());
    for (const auto& reference : data->references) {
        context.references.push_back(semantic::NavigationReference{.stable_id = reference.stable_id,
                                                                  .location = reference.location,
                                                                  .is_declaration = reference.is_declaration});
    }
    if (const auto ranges_it = data->selection_ranges_by_uri.find(context.document_uri);
        ranges_it != data->selection_ranges_by_uri.end()) {
        context.selection_ranges = ranges_it->second;
    }
    return context;
}

template<typename SnapshotData>
semantic::DiagnosticContext diagnosticContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                                 std::string workspace_root_uri) {
    semantic::DiagnosticContext context{.generation = snapshot.generation,
                                        .snapshot_available = data != nullptr,
                                        .workspace_root_uri = std::move(workspace_root_uri),
                                        .snapshot_diagnostics = snapshot.diagnostics};
    if (const auto document_it = documents.find(document_uri); document_it != documents.end()) {
        context.document = document_it->second;
    }
    else {
        context.document.uri = std::move(document_uri);
    }
    if (data == nullptr) {
        return context;
    }
    context.symbols_by_id.reserve(data->symbols_by_id.size());
    for (const auto& [stable_id, indexed_symbol] : data->symbols_by_id) {
        context.symbols_by_id.emplace(stable_id,
                                      semantic::DiagnosticSymbol{.identity = indexed_symbol.identity,
                                                                 .type_display = indexed_symbol.type_display});
    }
    context.references.reserve(data->references.size());
    for (const auto& reference : data->references) {
        context.references.push_back(semantic::DiagnosticReference{.stable_id = reference.stable_id,
                                                                  .location = reference.location});
    }
    context.assignments_by_uri = data->assignments_by_uri;
    context.package_imports_by_uri = data->package_imports_by_uri;
    context.metadata_by_uri = data->metadata_by_uri;
    context.modules_by_name = data->modules_by_name;
    return context;
}

template<typename SnapshotData>
semantic::CodeActionContext codeActionContextFor(const SnapshotData* data,
                                                 const SemanticEngineSnapshot& snapshot,
                                                 std::string document_uri,
                                                 ParseRange range,
                                                 const std::unordered_map<std::string, SemanticEngineDocument>& documents,
                                                 std::string workspace_root_uri,
                                                 std::vector<SemanticEngineDiagnostic> diagnostics) {
    semantic::CodeActionContext context{.generation = snapshot.generation,
                                       .snapshot_available = data != nullptr,
                                       .workspace_root_uri = std::move(workspace_root_uri),
                                       .range = range,
                                       .diagnostics = std::move(diagnostics)};
    if (const auto document_it = documents.find(document_uri); document_it != documents.end()) {
        context.document = document_it->second;
    }
    else {
        context.document.uri = std::move(document_uri);
    }
    if (data != nullptr) {
        context.modules_by_name = data->modules_by_name;
    }
    return context;
}

} // namespace

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
        const auto kind_name = symbolKindName(symbol.kind);
        completions.push_back(SemanticCompletionItem{.stable_id = stable_id,
                                                      .label = std::string(symbol.name),
                .detail = semantic::completionDetailForSemanticKind(kind_name),
                .documentation = {},
                .insert_text = std::string(symbol.name),
                .kind = semantic::completionKindForSemanticKind(kind_name),
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

template<typename SnapshotData>
std::optional<std::string> symbolIdAtLocation(const SnapshotData& data,
                                              std::string_view uri,
                                              int line,
                                              int character);

template<typename SnapshotData>
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

template<typename SnapshotData>
std::vector<SemanticLocation> moduleImplementationLocations(const SnapshotData& data,
                                                            std::string_view module_name,
                                                            bool& truncated) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, instances] : data.module_instances_by_uri) {
        for (const auto& instance : instances) {
            if (instance.module_name != module_name) {
                continue;
            }
            locations.push_back(SemanticLocation{.uri = instance.uri,
                                                 .range = instance.module_selection_range});
            if (locations.size() >= kMaxSemanticLocations) {
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

template<typename SnapshotData>
std::optional<typename SnapshotData::ModuleInstance> moduleInstanceAt(const SnapshotData& data,
                                                                      std::string_view uri,
                                                                      int line,
                                                                      int character) {
    const auto instances_it = data.module_instances_by_uri.find(std::string(uri));
    if (instances_it == data.module_instances_by_uri.end()) {
        return std::nullopt;
    }
    std::optional<typename SnapshotData::ModuleInstance> best;
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

template<typename SnapshotData>
void sortSnapshotIndexes(SnapshotData& data) {
    for (auto& [_, indexes] : data.references_by_symbol) {
        std::sort(indexes.begin(), indexes.end(), [&](size_t lhs, size_t rhs) {
            return locationLess(data.references[lhs].location, data.references[rhs].location);
        });
    }
    for (auto& [_, completions] : data.completions_by_uri) {
        std::sort(completions.begin(), completions.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhs_priority = semantic::completionPriorityForDetail(lhs.detail);
            const auto rhs_priority = semantic::completionPriorityForDetail(rhs.detail);
            if (lhs_priority != rhs_priority) {
                return lhs_priority < rhs_priority;
            }
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

template<typename SnapshotData>
void appendModulePortCompletions(std::vector<SemanticCompletionItem>& items,
                                 std::set<std::string>& emitted,
                                 const SnapshotData& data,
                                 const ModuleDefinition& module,
                                 std::string_view module_uri,
                                 std::string_view prefix,
                                 const std::set<std::string>& excluded_ports,
                                 bool& truncated) {
    const auto module_id = findSymbolIdByNameAndKind(data, module.name, "Definition")
                               .value_or(std::string("module|") + module.name);
    semantic::appendModulePortCompletions(items,
                                          emitted,
                                          module_id,
                                          module,
                                          module_uri,
                                          prefix,
                                          excluded_ports,
                                          truncated);
}

template<typename Map>
std::optional<std::string> firstUninstantiatedModuleName(const Map& modules_by_name) {
    std::set<std::string> instantiated;
    for (const auto& [_, module] : modules_by_name) {
        for (const auto& instance : module.instances) {
            instantiated.insert(instance.module_name);
        }
    }
    for (const auto& [name, _] : modules_by_name) {
        if (!instantiated.contains(name)) {
            return name;
        }
    }
    if (!modules_by_name.empty()) {
        return modules_by_name.begin()->first;
    }
    return std::nullopt;
}

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isLogicOutputPortName(std::string_view port_name) {
    const auto normalized = lowerAsciiCopy(std::string(port_name));
    return normalized == "y" || normalized == "out" || normalized == "o" || normalized == "q";
}

const SchematicPort* findSchematicPortByName(const ModuleSchematic& schematic, std::string_view name) {
    const auto found = std::find_if(schematic.ports.begin(), schematic.ports.end(), [&](const auto& port) {
        return port.name == name;
    });
    return found == schematic.ports.end() ? nullptr : &*found;
}

const SchematicPort* findSchematicPortByIndex(const ModuleSchematic& schematic, int index) {
    if (index < 0 || static_cast<size_t>(index) >= schematic.ports.size()) {
        return nullptr;
    }
    return &schematic.ports[static_cast<size_t>(index)];
}

bool sameParseRange(const ParseRange& lhs, const ParseRange& rhs) {
    return lhs.start_line == rhs.start_line && lhs.start_character == rhs.start_character &&
           lhs.end_line == rhs.end_line && lhs.end_character == rhs.end_character;
}

void appendEndpointByDirection(SemanticSchematicNet& net,
                               std::string direction,
                               SemanticSchematicEndpoint endpoint,
                               bool invert_direction = false) {
    if (invert_direction) {
        if (direction == "input") {
            direction = "output";
        }
        else if (direction == "output") {
            direction = "input";
        }
    }

    if (direction == "output") {
        net.drivers.push_back(std::move(endpoint));
        return;
    }
    if (direction == "input") {
        net.loads.push_back(std::move(endpoint));
        return;
    }

    net.drivers.push_back(endpoint);
    net.loads.push_back(std::move(endpoint));
}

template<typename SnapshotData>
std::vector<SemanticSchematicNet> buildSchematicNets(const ModuleSchematic& schematic,
                                                     const SnapshotData& data) {
    std::map<std::string, SemanticSchematicNet> nets;
    const auto ensure_net = [&](std::string_view signal) -> SemanticSchematicNet& {
        auto [it, inserted] = nets.try_emplace(std::string(signal),
                                               SemanticSchematicNet{.name = std::string(signal)});
        (void)inserted;
        return it->second;
    };

    for (const auto& port : schematic.ports) {
        if (port.name.empty()) {
            continue;
        }
        auto& net = ensure_net(port.name);
        appendEndpointByDirection(net,
                                  port.direction,
                                  SemanticSchematicEndpoint{.node_id = std::string("$port:") + port.name,
                                                            .port_name = port.name},
                                  true);
    }

    for (const auto& cell : schematic.cells) {
        const auto target_it = cell.kind == "module"
                                   ? data.schematics_by_name.find(cell.type)
                                   : data.schematics_by_name.end();
        for (const auto& connection : cell.connections) {
            if (connection.signal.empty()) {
                continue;
            }

            std::string port_name = connection.port_name;
            std::string direction;
            if (target_it != data.schematics_by_name.end()) {
                const auto* port = !port_name.empty()
                                       ? findSchematicPortByName(target_it->second, port_name)
                                       : findSchematicPortByIndex(target_it->second,
                                                                  connection.port_index);
                if (port != nullptr) {
                    port_name = port->name;
                    direction = port->direction;
                }
            }

            if (port_name.empty() && connection.port_index >= 0) {
                port_name = std::to_string(connection.port_index);
            }
            if (direction.empty()) {
                direction = isLogicOutputPortName(port_name) ? "output" : "input";
            }

            auto& net = ensure_net(connection.signal);
            appendEndpointByDirection(net,
                                      direction,
                                      SemanticSchematicEndpoint{.node_id = cell.id,
                                                                .port_name = port_name});
        }
    }

    std::vector<SemanticSchematicNet> result;
    result.reserve(nets.size());
    for (auto& [_, net] : nets) {
        result.push_back(std::move(net));
    }
    return result;
}

} // namespace

SemanticEngine::SemanticEngine() : query_cache_(std::make_unique<semantic::QueryCache>()) {}

SemanticEngine::~SemanticEngine() = default;

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    includes_.clear();
    reverse_includes_.clear();
    snapshot_.reset();
    snapshot_data_.reset();
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
    config_.workspace_root_uri = workspace_root_uri_.empty()
                                     ? std::optional<std::string>{}
                                     : std::optional<std::string>{workspace_root_uri_};
    query_cache_->clear();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::configure(SemanticEngineConfig config) {
    config_ = std::move(config);
    if (config_.workspace_root_uri.has_value()) {
        workspace_root_uri_ = withoutTrailingSlash(normalizeFileUri(*config_.workspace_root_uri));
        config_.workspace_root_uri = workspace_root_uri_;
    }
    std::sort(config_.top_modules.begin(), config_.top_modules.end());
    config_.top_modules.erase(std::unique(config_.top_modules.begin(), config_.top_modules.end()),
                              config_.top_modules.end());
    query_cache_->clear();
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
    query_cache_->clear();
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
    query_cache_->clear();
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
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->diagnostics(current_snapshot.generation, document_uri)) {
        return *cached;
    }

    const auto* data = snapshotData();
    auto context = diagnosticContextFor(data,
                                        current_snapshot,
                                        document_uri,
                                        documents_,
                                        workspace_root_uri_);
    auto result = semantic::diagnosticsFor(context);
    query_cache_->storeDiagnostics(current_snapshot.generation, document_uri, result);
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

    if (const auto instance = moduleInstanceAt(*data, document_uri, line, character)) {
        const auto target_id = !instance->target_stable_id.empty()
                                   ? std::optional<std::string>{instance->target_stable_id}
                                   : findDefinitionSymbolId(*data, instance->module_name);
        if (target_id.has_value()) {
            const auto symbol_it = data->symbols_by_id.find(*target_id);
            if (symbol_it != data->symbols_by_id.end()) {
                result.query_location = SemanticLocation{.uri = instance->uri,
                                                         .range = instance->module_selection_range};
                result.symbol = symbol_it->second.identity;
                result.unresolved = false;
                return result;
            }
        }
        result.messages.push_back("module instance target is not indexed");
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

    const auto* data = snapshotData();
    if (data != nullptr) {
        if (const auto document_it = documents_.find(withoutTrailingSlash(normalizeFileUri(uri)));
            document_it != documents_.end()) {
            for (const auto& metadata : CompilationService{}.semanticSymbolMetadata(document_it->second.text,
                                                                                    document_it->second.uri)) {
                const auto type_range = userTypeReferenceRange(document_it->second.text, metadata);
                if (!type_range.has_value() || !containsPosition(*type_range, line, character)) {
                    continue;
                }
                auto locations = typeDefinitionLocationsByName(*data, metadata.type_name);
                if (!locations.empty()) {
                    result.locations = std::move(locations);
                    result.unresolved = false;
                    return result;
                }
            }
        }
    }

    if (!lookup.symbol.has_value()) {
        return result;
    }

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
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->references(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     include_declaration)) {
        return *cached;
    }

    const auto lookup = lookupAt(uri, line, character);
    SemanticReferenceResult result{.generation = lookup.generation,
                                   .messages = lookup.messages,
                                   .unresolved = lookup.unresolved};
    const auto finish = [&](SemanticReferenceResult value) {
        query_cache_->storeReferences(current_snapshot.generation,
                                      document_uri,
                                      line,
                                      character,
                                      include_declaration,
                                      value);
        return value;
    };
    if (!lookup.symbol.has_value()) {
        return finish(std::move(result));
    }

    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return finish(std::move(result));
    }

    if (const auto instance = moduleInstanceAt(*data, document_uri, line, character)) {
        result.locations = moduleImplementationLocations(*data,
                                                         instance->module_name,
                                                         result.truncated);
        if (include_declaration) {
            const auto target_id = !instance->target_stable_id.empty()
                                       ? std::optional<std::string>{instance->target_stable_id}
                                       : findDefinitionSymbolId(*data, instance->module_name);
            if (target_id.has_value()) {
                const auto target_it = data->symbols_by_id.find(*target_id);
                if (target_it != data->symbols_by_id.end()) {
                    result.locations.push_back(target_it->second.identity.location);
                }
            }
        }
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
        result.unresolved = false;
        return finish(std::move(result));
    }

    result.locations = locationsForSymbol(*data,
                                          lookup.symbol->stable_id,
                                          include_declaration,
                                          result.truncated);
    if (lookup.symbol->kind == "Definition") {
        bool implementation_truncated = false;
        auto implementations = moduleImplementationLocations(*data,
                                                             lookup.symbol->name,
                                                             implementation_truncated);
        for (auto& location : implementations) {
            if (!include_declaration && sameLocation(location, lookup.query_location)) {
                continue;
            }
            if (result.locations.size() >= kMaxSemanticLocations) {
                result.truncated = true;
                break;
            }
            result.locations.push_back(std::move(location));
        }
        result.truncated = result.truncated || implementation_truncated;
        std::sort(result.locations.begin(), result.locations.end(), locationLess);
        result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                               result.locations.end());
    }
    return finish(std::move(result));
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

SemanticReferenceResult SemanticEngine::implementationsAt(std::string_view uri,
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
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return result;
    }
    result.locations = moduleImplementationLocations(*data, lookup.symbol->name, result.truncated);
    result.unresolved = false;
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
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->rename(current_snapshot.generation,
                                                document_uri,
                                                line,
                                                character,
                                                new_name)) {
        return *cached;
    }

    const auto references = referencesAt(uri, line, character, true);
    SemanticRenameResult result{.generation = references.generation,
                                .messages = references.messages,
                                .unresolved = references.unresolved,
                                .truncated = references.truncated};
    for (const auto& location : references.locations) {
        result.edits.push_back(SemanticTextEdit{.location = location,
                                                .new_text = std::string(new_name)});
    }
    query_cache_->storeRename(current_snapshot.generation,
                              document_uri,
                              line,
                              character,
                              new_name,
                              result);
    return result;
}

SemanticCompletionResult SemanticEngine::completionsAt(std::string_view uri,
                                                       int line,
                                                       int character,
                                                       std::string_view prefix) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->completions(current_snapshot.generation,
                                                     document_uri,
                                                     line,
                                                     character,
                                                     prefix)) {
        return *cached;
    }

    SemanticCompletionResult result{.generation = current_snapshot.generation};
    const auto finish = [&](SemanticCompletionResult value) {
        query_cache_->storeCompletions(current_snapshot.generation,
                                       document_uri,
                                       line,
                                       character,
                                       prefix,
                                       value);
        return value;
    };
    const auto* data = snapshotData();
    if (data == nullptr) {
        result.unresolved = true;
        result.messages.push_back("AST-backed SemanticEngine snapshot is unavailable");
        return finish(std::move(result));
    }

    std::set<std::string> emitted;
    const auto document_it = documents_.find(document_uri);
    const auto* document = document_it == documents_.end() ? nullptr : &document_it->second;
    const auto completion_context = document == nullptr
                                        ? semantic::CompletionContext{}
                                        : semantic::detectCompletionContext(document->text,
                                                                            line,
                                                                            character,
                                                                            prefix);
    const auto prefix_start = completion_context.prefix_start;

    const auto append_items = [&](const std::vector<SemanticCompletionItem>& items) {
        for (const auto& item : items) {
            semantic::appendCompletionItem(result.items, emitted, item, prefix, result.truncated);
            if (result.truncated) {
                return;
            }
        }
    };

    if (document != nullptr && completion_context.macro_invocation) {
        const auto append_macros = [&](const std::vector<MacroDefinition>& macros) {
            for (const auto& macro : macros) {
                semantic::appendCompletionItem(
                    result.items,
                    emitted,
                    SemanticCompletionItem{.stable_id = document_uri + "|macro|" + macro.name,
                                            .label = macro.name,
                                            .detail = macro.function_like ? "Macro function" : "Macro",
                                            .documentation = semantic::macroDocumentation(macro),
                                            .insert_text = semantic::macroInsertText(macro),
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
        return finish(std::move(result));
    }

    if (document != nullptr) {
        if (const auto package_name = completion_context.package_qualifier) {
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
                semantic::appendSymbolCompletion(result.items,
                                                 emitted,
                                                 indexed_symbol.identity,
                                                 prefix,
                                                 result.truncated);
                if (result.truncated) {
                    return finish(std::move(result));
                }
            }
            if (result.items.empty()) {
                result.messages.push_back("package completion had no indexed AST members");
            }
            return finish(std::move(result));
        }

        if (completion_context.member_access) {
            const auto instances_it = data->module_instances_by_uri.find(document_uri);
            if (instances_it == data->module_instances_by_uri.end()) {
                result.unresolved = true;
                result.messages.push_back("named member completion has no indexed module instances");
                return finish(std::move(result));
            }
            for (const auto& instance : instances_it->second) {
                if (!parseRangeContainsPosition(instance.range, line, character)) {
                    continue;
                }
                const auto module_it = data->modules_by_name.find(instance.module_name);
                if (module_it != data->modules_by_name.end()) {
                    std::set<std::string> connected_ports;
                    const auto position_offset = utf8OffsetAtUtf16Position(document->text, line, character);
                    const auto search_start = utf8OffsetAtUtf16Position(document->text,
                                                                        instance.selection_range.end_line,
                                                                        instance.selection_range.end_character);
                    const auto search_end = utf8OffsetAtUtf16Position(document->text,
                                                                      instance.range.end_line,
                                                                      instance.range.end_character);
                    if (position_offset.has_value() && search_start.has_value() && search_end.has_value()) {
                        const auto open_paren = openParenBeforePosition(document->text,
                                                                        *search_start,
                                                                        std::min(*position_offset, *search_end));
                        if (open_paren.has_value()) {
                            connected_ports = connectedNamedPortsBeforePosition(document->text,
                                                                                *open_paren,
                                                                                *position_offset);
                        }
                    }
                    const auto module_uri_it = data->module_uris_by_name.find(instance.module_name);
                    appendModulePortCompletions(result.items,
                                                emitted,
                                                *data,
                                                module_it->second,
                                                module_uri_it == data->module_uris_by_name.end()
                                                    ? std::string_view{}
                                                    : std::string_view(module_uri_it->second),
                                                prefix,
                                                connected_ports,
                                                result.truncated);
                    return finish(std::move(result));
                }
            }
        }

        if (const auto member_name = completion_context.member_qualifier) {
            for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
                if (indexed_symbol.identity.name == *member_name) {
                    continue;
                }
                if (indexed_symbol.identity.kind == "Field" || indexed_symbol.identity.kind == "Member" ||
                    indexed_symbol.identity.kind == "Net" || indexed_symbol.identity.kind == "Variable" ||
                    indexed_symbol.identity.kind == "Parameter" || indexed_symbol.identity.kind == "Subroutine") {
                    semantic::appendSymbolCompletion(result.items,
                                                     emitted,
                                                     indexed_symbol.identity,
                                                     prefix,
                                                     result.truncated);
                }
                if (result.truncated) {
                    return finish(std::move(result));
                }
            }
            result.messages.push_back("member completion used AST-backed member context provider");
            return finish(std::move(result));
        }

        if (completion_context.module_instantiation_position) {
            std::vector<std::string> module_names;
            module_names.reserve(data->modules_by_name.size());
            for (const auto& [module_name, _] : data->modules_by_name) {
                module_names.push_back(module_name);
            }
            std::sort(module_names.begin(), module_names.end());
            for (const auto& module_name : module_names) {
                const auto& module = data->modules_by_name.at(module_name);
                const auto module_id = findSymbolIdByNameAndKind(*data, module.name, "Definition")
                                           .value_or(std::string("module|") + module.name);
                const auto module_uri_it = data->module_uris_by_name.find(module.name);
                semantic::appendCompletionItem(
                    result.items,
                    emitted,
                    SemanticCompletionItem{.stable_id = module_id,
                                            .label = module.name,
                                            .detail = semantic::moduleSignatureLabel(module),
                                            .documentation = semantic::moduleDocumentation(
                                                module,
                                                module_uri_it == data->module_uris_by_name.end()
                                                    ? std::string_view{}
                                                    : std::string_view(module_uri_it->second)),
                                            .insert_text = module.name,
                                            .kind = 9,
                                            .unresolved = false},
                    prefix,
                    result.truncated);
                if (result.truncated) {
                    return finish(std::move(result));
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
    return finish(std::move(result));
}

SemanticCompletionItem SemanticEngine::resolveCompletion(std::string_view stable_id,
                                                         std::string_view label) const {
    const auto* data = snapshotData();
    if (data == nullptr) {
        return semantic::resolveCompletionItem(stable_id, label, semantic::CompletionResolveContext{});
    }

    semantic::CompletionResolveContext context;
    context.modules_by_name = &data->modules_by_name;
    context.module_uris_by_name = &data->module_uris_by_name;
    context.macros_by_uri = &data->macros_by_uri;

    const auto symbol_it = data->symbols_by_id.find(std::string(stable_id));
    if (symbol_it != data->symbols_by_id.end()) {
        context.symbol = semantic::CompletionResolveSymbol{.identity = symbol_it->second.identity,
                                                           .type_display = symbol_it->second.type_display};
    }
    return semantic::resolveCompletionItem(stable_id, label, context);
}

SemanticSignatureHelpResult SemanticEngine::signatureHelpAt(std::string_view uri,
                                                            int line,
                                                            int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto document_it = documents_.find(document_uri);

    semantic::SignatureInlayContext context{.generation = current_snapshot.generation,
                                            .document_uri = document_uri,
                                            .document_text = document_it == documents_.end()
                                                                 ? nullptr
                                                                 : &document_it->second.text,
                                            .modules_by_name = data == nullptr ? nullptr : &data->modules_by_name,
                                            .snapshot_available = data != nullptr};
    if (data != nullptr) {
        const auto instances_it = data->module_instances_by_uri.find(document_uri);
        if (instances_it != data->module_instances_by_uri.end()) {
            context.module_instances.reserve(instances_it->second.size());
            for (const auto& instance : instances_it->second) {
                context.module_instances.push_back(semantic::SignatureInlayModuleInstance{
                    .module_name = instance.module_name,
                    .range = instance.range,
                    .selection_range = instance.selection_range});
            }
        }
    }
    return semantic::signatureHelpAt(context, line, character);
}

SemanticInlayHintResult SemanticEngine::inlayHints(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();

    semantic::SignatureInlayContext context{.generation = current_snapshot.generation,
                                            .document_uri = document_uri,
                                            .modules_by_name = data == nullptr ? nullptr : &data->modules_by_name,
                                            .snapshot_available = data != nullptr};
    if (data != nullptr) {
        context.symbols.reserve(data->symbols_by_id.size());
        for (const auto& [_, indexed_symbol] : data->symbols_by_id) {
            context.symbols.push_back(semantic::SignatureInlaySymbol{
                .identity = indexed_symbol.identity,
                .type_display = indexed_symbol.type_display});
        }
    }
    if (data != nullptr) {
        const auto instances_it = data->module_instances_by_uri.find(document_uri);
        if (instances_it != data->module_instances_by_uri.end()) {
            context.module_instances.reserve(instances_it->second.size());
            for (const auto& instance : instances_it->second) {
                context.module_instances.push_back(semantic::SignatureInlayModuleInstance{
                    .module_name = instance.module_name,
                    .range = instance.range,
                    .selection_range = instance.selection_range});
            }
        }
    }
    return semantic::inlayHints(context, range);
}

SemanticTokenResult SemanticEngine::semanticTokens(std::string_view uri) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    auto context = navigationContextFor(data, current_snapshot, document_uri);
    return semantic::semanticTokens(context);
}

SemanticSelectionRangeResult SemanticEngine::selectionRangesAt(std::string_view uri,
                                                               int line,
                                                               int character) const {
    const auto lookup = lookupAt(uri, line, character);
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    const auto document_it = documents_.find(document_uri);
    auto context = navigationContextFor(data,
                                        snapshot(),
                                        document_uri,
                                        document_it == documents_.end() ? nullptr : &document_it->second.text);
    return semantic::selectionRangesAt(context, lookup, line, character);
}

SemanticModuleHierarchyResult SemanticEngine::moduleHierarchy(std::optional<std::string_view> module_name,
                                                              int max_depth) const {
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->moduleHierarchy(current_snapshot.generation,
                                                          module_name,
                                                          max_depth)) {
        return *cached;
    }

    SemanticModuleHierarchyResult result{.generation = current_snapshot.generation};
    const auto finish = [&](SemanticModuleHierarchyResult value) {
        query_cache_->storeModuleHierarchy(current_snapshot.generation,
                                           module_name,
                                           max_depth,
                                           value);
        return value;
    };
    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    result = semantic::moduleHierarchy(context, module_name, max_depth);
    return finish(std::move(result));
}

SemanticSchematicResult SemanticEngine::schematic(std::optional<std::string_view> module_name,
                                                  int max_depth) const {
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->schematic(current_snapshot.generation, module_name, max_depth)) {
        return *cached;
    }

    SemanticSchematicResult result{.generation = current_snapshot.generation};
    const auto finish = [&](SemanticSchematicResult value) {
        query_cache_->storeSchematic(current_snapshot.generation, module_name, max_depth, value);
        return value;
    };
    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    result = semantic::schematic(context, module_name, max_depth);
    return finish(std::move(result));
}

SemanticCallHierarchyPrepareResult SemanticEngine::prepareCallHierarchy(std::string_view uri,
                                                                        int line,
                                                                        int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    return semantic::prepareCallHierarchy(context, document_uri, line, character);
}

SemanticCallHierarchyCallsResult SemanticEngine::incomingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    return semantic::incomingCalls(context, item);
}

SemanticCallHierarchyCallsResult SemanticEngine::outgoingCalls(const SemanticCallHierarchyItem& item) const {
    const auto& current_snapshot = snapshot();
    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    return semantic::outgoingCalls(context, item);
}

SemanticConeTrace SemanticEngine::backwardConeAt(std::string_view uri,
                                                 int line,
                                                 int character) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->backwardCone(current_snapshot.generation,
                                                       document_uri,
                                                       line,
                                                       character)) {
        return *cached;
    }

    const auto lookup = lookupAt(uri, line, character);
    SemanticConeTrace trace{.generation = lookup.generation,
                            .messages = lookup.messages,
                            .unresolved = lookup.unresolved};
    const auto finish = [&](SemanticConeTrace value) {
        query_cache_->storeBackwardCone(current_snapshot.generation,
                                        document_uri,
                                        line,
                                        character,
                                        value);
        return value;
    };
    if (!lookup.symbol.has_value()) {
        if (trace.messages.empty()) {
            trace.messages.push_back("No signal symbol was found at the requested position.");
        }
        return finish(std::move(trace));
    }

    const auto* data = snapshotData();
    auto context = designGraphContextFor(data, current_snapshot, config_);
    if (const auto document_it = documents_.find(document_uri); document_it != documents_.end()) {
        CompilationService compilation_service;
        context.identifiers_by_uri[document_uri] = compilation_service.identifiers(document_it->second.text);
    }
    trace = semantic::backwardCone(context, document_uri, lookup, kMaxSemanticLocations);
    return finish(std::move(trace));
}

SemanticCodeActionResult SemanticEngine::codeActionsAt(std::string_view uri, ParseRange range) const {
    const auto& current_snapshot = snapshot();
    const auto document_uri = withoutTrailingSlash(normalizeFileUri(uri));
    if (const auto cached = query_cache_->codeActions(current_snapshot.generation, document_uri, range)) {
        return *cached;
    }

    SemanticCodeActionResult result{.generation = current_snapshot.generation};
    const auto finish = [&](SemanticCodeActionResult value) {
        query_cache_->storeCodeActions(current_snapshot.generation,
                                       document_uri,
                                       range,
                                       value);
        return value;
    };
    const auto* data = snapshotData();
    auto context = codeActionContextFor(data,
                                        current_snapshot,
                                        document_uri,
                                        range,
                                        documents_,
                                        workspace_root_uri_,
                                        diagnosticsFor(document_uri));
    result = semantic::codeActionsAt(context);

    return finish(std::move(result));
}

SemanticWorkspaceSymbolResult SemanticEngine::workspaceSymbols(std::string_view query,
                                                               size_t limit) const {
    const auto& current_snapshot = snapshot();
    if (const auto cached = query_cache_->workspaceSymbols(current_snapshot.generation, query, limit)) {
        return *cached;
    }

    const auto* data = snapshotData();
    auto context = astIndexContextFor(data, current_snapshot);
    auto result = semantic::workspaceSymbols(context, query, limit);
    query_cache_->storeWorkspaceSymbols(current_snapshot.generation, query, limit, result);
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
        data->metadata_by_uri[uri] = compilation_service.semanticSymbolMetadata(document_it->second.text,
                                                                                uri);
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
            data->module_entries.push_back(SnapshotData::ModuleEntry{.uri = uri, .definition = module});
            data->modules_by_name.try_emplace(module.name, module);
            data->module_uris_by_name.try_emplace(module.name, uri);
            for (const auto& instance : module.instances) {
                data->module_instances_by_uri[uri].push_back(SnapshotData::ModuleInstance{
                    .module_name = instance.module_name,
                    .instance_name = instance.instance_name,
                    .target_stable_id = {},
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
        for (const auto& schematic : compilation_service.moduleSchematics(document_it->second.text, uri)) {
            data->schematics_by_name.try_emplace(schematic.name, schematic);
            data->schematic_uris_by_name.try_emplace(schematic.name, uri);
        }
        const auto assignments = compilation_service.continuousAssignments(document_it->second.text, uri);
        data->assignments_by_uri[uri] = assignments;
        for (const auto& assignment : assignments) {
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
