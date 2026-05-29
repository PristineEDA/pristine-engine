#include "DiagnosticProvider.h"

#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace pristine::analysis::semantic {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUnknownIncludeDiagnosticCode = "unknownInclude";
constexpr std::string_view kUnresolvedModuleDiagnosticCode = "unresolvedModule";
constexpr std::string_view kUnresolvedTypeDiagnosticCode = "unresolvedType";

bool diagnosticLess(const SemanticEngineDiagnostic& lhs, const SemanticEngineDiagnostic& rhs) {
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

bool sameDiagnostic(const SemanticEngineDiagnostic& lhs, const SemanticEngineDiagnostic& rhs) {
    return lhs.uri == rhs.uri && lhs.code == rhs.code && lhs.message == rhs.message &&
           lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line &&
           lhs.range.end_character == rhs.range.end_character;
}

bool isIdentifierStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) != 0 || value == '_' || value == '$';
}

bool isIdentifierContinue(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

bool isValidIdentifier(std::string_view value) {
    if (value.empty() || !isIdentifierStart(value.front())) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), isIdentifierContinue);
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

std::string unknownIncludeMessage(std::string_view target) {
    return std::string("Include file '") + std::string(target) + "' could not be resolved.";
}

std::string unresolvedModuleMessage(std::string_view module_name) {
    return std::string("Module '") + std::string(module_name) + "' could not be resolved.";
}

std::string unresolvedTypeMessage(std::string_view name) {
    return std::string("Type '") + std::string(name) + "' could not be resolved.";
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

std::string widthMismatchMessage(std::string_view left_name,
                                 std::int64_t left_width,
                                 std::string_view right_name,
                                 std::int64_t right_width) {
    return std::string("Width mismatch: assigning ") + std::to_string(right_width) + "-bit '" +
           std::string(right_name) + "' to " + std::to_string(left_width) + "-bit '" +
           std::string(left_name) + "'.";
}

std::optional<fs::path> resolveIncludeTarget(std::string_view workspace_root_uri,
                                             std::string_view document_uri,
                                             std::string_view target) {
    const auto target_path = fs::path(std::string(target));
    std::vector<fs::path> candidates;

    if (target_path.is_absolute()) {
        candidates.push_back(target_path);
    }
    else {
        const auto document_path = fs::path(fileUriToPath(document_uri));
        candidates.push_back(document_path.parent_path() / target_path);
    }

    if (!target_path.is_absolute() && !workspace_root_uri.empty()) {
        candidates.push_back(fs::path(fileUriToPath(workspace_root_uri)) / target_path);
    }

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::exists(candidate, error) && fs::is_regular_file(candidate, error)) {
            return fs::weakly_canonical(candidate, error);
        }
    }
    return std::nullopt;
}

bool canHaveUserDefinedTypeReference(const SemanticSymbolMetadata& metadata) {
    return metadata.kind == 13 || metadata.kind == 14;
}

bool isTypeDefinitionMetadata(const SemanticSymbolMetadata& metadata) {
    switch (metadata.kind) {
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

bool isAssignableMetadata(const SemanticSymbolMetadata& metadata) {
    return metadata.kind == 13 && metadata.type_display_name.find('[') != std::string::npos;
}

std::optional<std::int64_t> bitWidthFromDisplayName(std::string_view display_name) {
    const auto open = display_name.find('[');
    const auto close = display_name.find(']', open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1) {
        if (display_name == "logic" || display_name == "bit" || display_name == "wire" ||
            display_name == "reg") {
            return 1;
        }
        return std::nullopt;
    }

    const auto colon = display_name.find(':', open + 1);
    if (colon == std::string_view::npos || colon >= close) {
        return std::nullopt;
    }

    try {
        const auto msb = std::stoll(std::string(display_name.substr(open + 1,
                                                                    colon - open - 1)));
        const auto lsb = std::stoll(std::string(display_name.substr(colon + 1,
                                                                    close - colon - 1)));
        return std::llabs(msb - lsb) + 1;
    }
    catch (...) {
        return std::nullopt;
    }
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

std::optional<std::string> symbolIdAtRangeStart(const DiagnosticContext& context,
                                                const ParseRange& range) {
    std::optional<std::string> best_id;
    std::optional<SemanticLocation> best_location;
    for (const auto& reference : context.references) {
        if (reference.location.uri != context.document.uri ||
            !parseRangeContainsPosition(reference.location.range,
                                        range.start_line,
                                        range.start_character)) {
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

std::optional<SemanticSymbolMetadata> metadataForSymbolId(const DiagnosticContext& context,
                                                          std::string_view stable_id) {
    const auto symbol_it = context.symbols_by_id.find(std::string(stable_id));
    if (symbol_it == context.symbols_by_id.end()) {
        return std::nullopt;
    }
    const auto& identity = symbol_it->second.identity;
    const auto metadata_it = context.metadata_by_uri.find(identity.location.uri);
    if (metadata_it == context.metadata_by_uri.end()) {
        return std::nullopt;
    }
    for (const auto& metadata : metadata_it->second) {
        if (metadata.name == identity.name &&
            metadata.selection_range.start_line == identity.location.range.start_line &&
            metadata.selection_range.start_character == identity.location.range.start_character) {
            return metadata;
        }
    }
    return std::nullopt;
}

std::vector<std::string> packageDefinitionIds(const DiagnosticContext& context,
                                              std::string_view package_name) {
    std::vector<std::string> result;
    for (const auto& [stable_id, symbol] : context.symbols_by_id) {
        if (symbol.identity.name == package_name && symbol.identity.kind == "Package") {
            result.push_back(stable_id);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<SemanticLocation> typeDefinitionLocationsByName(const DiagnosticContext& context,
                                                            std::string_view name) {
    std::vector<SemanticLocation> locations;
    for (const auto& [_, symbol] : context.symbols_by_id) {
        if (symbol.identity.name == name && isTypeDefinitionKind(symbol.identity.kind) &&
            !isModuleDefinitionKind(symbol.identity.kind)) {
            locations.push_back(symbol.identity.location);
        }
    }
    return locations;
}

bool hasTypeDefinitionSymbol(const DiagnosticContext& context, std::string_view name) {
    return !typeDefinitionLocationsByName(context, name).empty();
}

void appendDuplicateSymbolDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                      const DiagnosticContext& context) {
    const auto metadata_it = context.metadata_by_uri.find(context.document.uri);
    if (metadata_it == context.metadata_by_uri.end()) {
        return;
    }

    std::map<std::string, std::vector<SemanticSymbolMetadata>> by_name;
    for (const auto& metadata : metadata_it->second) {
        if (!metadata.name.empty()) {
            by_name[metadata.name].push_back(metadata);
        }
    }
    for (auto& [_, symbols] : by_name) {
        if (symbols.size() < 2) {
            continue;
        }
        std::sort(symbols.begin(), symbols.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.selection_range.start_line != rhs.selection_range.start_line) {
                return lhs.selection_range.start_line < rhs.selection_range.start_line;
            }
            return lhs.selection_range.start_character < rhs.selection_range.start_character;
        });
        for (size_t index = 1; index < symbols.size(); ++index) {
            if (symbols[index].selection_range.start_line != symbols[index - 1].selection_range.start_line + 1) {
                continue;
            }
            result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                      .code = "duplicateSymbol",
                                                      .message = duplicateSymbolMessage(symbols[index].name),
                                                      .range = symbols[index].selection_range,
                                                      .severity = 1});
        }
    }
}

void appendUnresolvedPackageDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                        const DiagnosticContext& context) {
    const auto imports_it = context.package_imports_by_uri.find(context.document.uri);
    if (imports_it == context.package_imports_by_uri.end()) {
        return;
    }
    for (const auto& import : imports_it->second) {
        if (!packageDefinitionIds(context, import.package_name).empty()) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = "unresolvedPackage",
                                                  .message = unresolvedPackageMessage(import.package_name),
                                                  .range = import.package_range,
                                                  .severity = 1});
    }
}

void appendWidthMismatchDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                    const DiagnosticContext& context) {
    const auto assignments_it = context.assignments_by_uri.find(context.document.uri);
    if (assignments_it == context.assignments_by_uri.end()) {
        return;
    }
    std::set<std::pair<int, int>> reported;
    for (const auto& assignment : assignments_it->second) {
        const auto left_id = symbolIdAtRangeStart(context, assignment.left_range);
        const auto right_id = symbolIdAtRangeStart(context, assignment.right_range);
        if (!left_id.has_value() || !right_id.has_value()) {
            continue;
        }
        const auto left_metadata = metadataForSymbolId(context, *left_id);
        const auto right_metadata = metadataForSymbolId(context, *right_id);
        if (!left_metadata.has_value() || !right_metadata.has_value() ||
            !isAssignableMetadata(*left_metadata) || !isAssignableMetadata(*right_metadata)) {
            continue;
        }
        const auto left_width = bitWidthFromDisplayName(left_metadata->type_display_name);
        const auto right_width = bitWidthFromDisplayName(right_metadata->type_display_name);
        if (!left_width.has_value() || !right_width.has_value() || *left_width <= 0 ||
            *right_width <= 0 || *left_width == *right_width) {
            continue;
        }
        const auto key = std::pair(assignment.right_range.start_line,
                                   assignment.right_range.start_character);
        if (!reported.insert(key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = "widthMismatch",
                                                  .message = widthMismatchMessage(assignment.left_expression,
                                                                                  *left_width,
                                                                                  assignment.right_expression,
                                                                                  *right_width),
                                                  .range = assignment.right_range,
                                                  .severity = 2});
    }
}

void appendAmbiguousReferenceDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                         const DiagnosticContext& context) {
    const auto imports_it = context.package_imports_by_uri.find(context.document.uri);
    if (imports_it == context.package_imports_by_uri.end() || imports_it->second.size() < 2) {
        return;
    }

    CompilationService compilation_service;
    for (const auto& identifier : compilation_service.identifiers(context.document.text)) {
        size_t definition_count = 0;
        for (const auto& import : imports_it->second) {
            for (const auto& package_id : packageDefinitionIds(context, import.package_name)) {
                const auto package_it = context.symbols_by_id.find(package_id);
                if (package_it == context.symbols_by_id.end()) {
                    continue;
                }
                const auto package_location = package_it->second.identity.location;
                const auto candidate_it = context.metadata_by_uri.find(package_location.uri);
                if (candidate_it == context.metadata_by_uri.end()) {
                    continue;
                }
                definition_count += static_cast<size_t>(
                    std::count_if(candidate_it->second.begin(),
                                  candidate_it->second.end(),
                                  [&](const SemanticSymbolMetadata& candidate) {
                                      return candidate.name == identifier.name &&
                                             isTypeDefinitionMetadata(candidate);
                                  }));
            }
        }
        if (definition_count < 2) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = "ambiguousReference",
                                                  .message = ambiguousReferenceMessage(identifier.name,
                                                                                      definition_count),
                                                  .range = identifier.range,
                                                  .severity = 2});
    }
}

void appendUnresolvedTypeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const DiagnosticContext& context) {
    CompilationService compilation_service;
    std::set<std::pair<int, int>> reported_type_ranges;
    for (const auto& metadata : compilation_service.semanticSymbolMetadata(context.document.text,
                                                                           context.document.uri)) {
        if (!canHaveUserDefinedTypeReference(metadata)) {
            continue;
        }
        const auto type_range = userTypeReferenceRange(context.document.text, metadata);
        if (!type_range.has_value() || hasTypeDefinitionSymbol(context, metadata.type_name)) {
            continue;
        }
        const auto range_key = std::pair(type_range->start_line, type_range->start_character);
        if (!reported_type_ranges.insert(range_key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = std::string(kUnresolvedTypeDiagnosticCode),
                                                  .message = unresolvedTypeMessage(metadata.type_name),
                                                  .range = *type_range,
                                                  .severity = 1});
    }
}

void appendUnknownIncludeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const DiagnosticContext& context) {
    CompilationService compilation_service;
    for (const auto& include : compilation_service.includeDirectives(context.document.text)) {
        if (resolveIncludeTarget(context.workspace_root_uri,
                                 context.document.uri,
                                 include.target).has_value()) {
            continue;
        }
        const auto key = std::tuple(std::string(kUnknownIncludeDiagnosticCode),
                                    include.range.start_line,
                                    include.range.start_character,
                                    unknownIncludeMessage(include.target));
        const auto already_reported = std::any_of(result.begin(), result.end(), [&](const auto& diagnostic) {
            return std::tuple(diagnostic.code,
                              diagnostic.range.start_line,
                              diagnostic.range.start_character,
                              diagnostic.message) == key;
        });
        if (already_reported) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = std::string(kUnknownIncludeDiagnosticCode),
                                                  .message = unknownIncludeMessage(include.target),
                                                  .range = include.range,
                                                  .severity = 1});
    }
}

void appendUnresolvedModuleDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                       const DiagnosticContext& context) {
    CompilationService compilation_service;
    for (const auto& module : compilation_service.moduleDefinitions(context.document.text,
                                                                    context.document.uri)) {
        for (const auto& instance : module.instances) {
            if (context.modules_by_name.contains(instance.module_name) ||
                !isValidIdentifier(instance.module_name)) {
                continue;
            }
            const auto key = std::tuple(std::string(kUnresolvedModuleDiagnosticCode),
                                        instance.module_selection_range.start_line,
                                        instance.module_selection_range.start_character,
                                        unresolvedModuleMessage(instance.module_name));
            const auto already_reported = std::any_of(result.begin(), result.end(), [&](const auto& diagnostic) {
                return std::tuple(diagnostic.code,
                                  diagnostic.range.start_line,
                                  diagnostic.range.start_character,
                                  diagnostic.message) == key;
            });
            if (already_reported) {
                continue;
            }
            result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                      .code = std::string(kUnresolvedModuleDiagnosticCode),
                                                      .message = unresolvedModuleMessage(instance.module_name),
                                                      .range = instance.module_selection_range,
                                                      .severity = 1});
        }
    }
}

} // namespace

std::vector<SemanticEngineDiagnostic> diagnosticsFor(const DiagnosticContext& context) {
    std::vector<SemanticEngineDiagnostic> result;
    for (const auto& diagnostic : context.snapshot_diagnostics) {
        if (diagnostic.uri == context.document.uri) {
            result.push_back(diagnostic);
        }
    }
    if (context.snapshot_available) {
        appendDuplicateSymbolDiagnostics(result, context);
        appendUnresolvedPackageDiagnostics(result, context);
        appendUnknownIncludeDiagnostics(result, context);
        appendUnresolvedModuleDiagnostics(result, context);
        appendUnresolvedTypeDiagnostics(result, context);
        appendWidthMismatchDiagnostics(result, context);
        appendAmbiguousReferenceDiagnostics(result, context);
    }
    sortAndDedupeDiagnostics(result);
    return result;
}

void sortAndDedupeDiagnostics(std::vector<SemanticEngineDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(), diagnosticLess);
    diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end(), sameDiagnostic),
                      diagnostics.end());
}

} // namespace pristine::analysis::semantic
