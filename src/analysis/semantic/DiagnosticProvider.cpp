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

bool isUserTypeReferenceName(std::string_view name) {
    if (name.empty() || name == "enum" || name.find("::") != std::string_view::npos) {
        return false;
    }
    const auto first_non_identifier = std::find_if(name.begin(), name.end(), [](char value) {
        return !isIdentifierContinue(value);
    });
    const auto head = name.substr(0, static_cast<size_t>(first_non_identifier - name.begin()));
    return !isBuiltinTypeName(head);
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

std::optional<DiagnosticSymbol> symbolForId(const DiagnosticContext& context,
                                            std::string_view stable_id) {
    const auto symbol_it = context.symbols_by_id.find(std::string(stable_id));
    if (symbol_it == context.symbols_by_id.end()) {
        return std::nullopt;
    }
    return symbol_it->second;
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
    std::map<std::string, std::vector<SemanticSymbolIdentity>> by_name;
    for (const auto& [_, symbol] : context.symbols_by_id) {
        const auto& identity = symbol.identity;
        if (identity.location.uri == context.document.uri && !identity.name.empty()) {
            by_name[identity.name].push_back(identity);
        }
    }
    for (auto& [_, symbols] : by_name) {
        if (symbols.size() < 2) {
            continue;
        }
        std::sort(symbols.begin(), symbols.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.location.range.start_line != rhs.location.range.start_line) {
                return lhs.location.range.start_line < rhs.location.range.start_line;
            }
            return lhs.location.range.start_character < rhs.location.range.start_character;
        });
        for (size_t index = 1; index < symbols.size(); ++index) {
            if (symbols[index].location.range.start_line !=
                symbols[index - 1].location.range.start_line + 1) {
                continue;
            }
            result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                      .code = "duplicateSymbol",
                                                      .message = duplicateSymbolMessage(symbols[index].name),
                                                      .range = symbols[index].location.range,
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
    const auto edges_it = context.assignment_edges_by_uri.find(context.document.uri);
    if (edges_it == context.assignment_edges_by_uri.end()) {
        return;
    }
    std::set<std::pair<int, int>> reported;
    for (const auto& edge : edges_it->second) {
        const auto left_symbol = symbolForId(context, edge.from_symbol_id);
        const auto right_symbol = symbolForId(context, edge.to_symbol_id);
        if (!left_symbol.has_value() || !right_symbol.has_value()) {
            continue;
        }
        const auto left_width = bitWidthFromDisplayName(left_symbol->type_display);
        const auto right_width = bitWidthFromDisplayName(right_symbol->type_display);
        if (!left_width.has_value() || !right_width.has_value() || *left_width <= 0 ||
            *right_width <= 0 || *left_width == *right_width) {
            continue;
        }
        const auto key = std::pair(edge.expression_location.range.start_line,
                                   edge.expression_location.range.start_character);
        if (!reported.insert(key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = "widthMismatch",
                                                  .message = widthMismatchMessage(left_symbol->identity.name,
                                                                                  *left_width,
                                                                                  right_symbol->identity.name,
                                                                                  *right_width),
                                                  .range = edge.expression_location.range,
                                                  .severity = 2});
    }
}

void appendAmbiguousReferenceDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                         const DiagnosticContext& context) {
    const auto imports_it = context.package_imports_by_uri.find(context.document.uri);
    if (imports_it == context.package_imports_by_uri.end() || imports_it->second.size() < 2) {
        return;
    }

    const auto type_references_it = context.type_references_by_uri.find(context.document.uri);
    if (type_references_it == context.type_references_by_uri.end()) {
        return;
    }
    for (const auto& reference : type_references_it->second) {
        if (!isUserTypeReferenceName(reference.type_name)) {
            continue;
        }
        size_t definition_count = 0;
        for (const auto& import : imports_it->second) {
            for (const auto& package_id : packageDefinitionIds(context, import.package_name)) {
                const auto package_it = context.symbols_by_id.find(package_id);
                if (package_it == context.symbols_by_id.end()) {
                    continue;
                }
                definition_count += static_cast<size_t>(
                    std::count_if(context.symbols_by_id.begin(),
                                  context.symbols_by_id.end(),
                                  [&](const auto& candidate_entry) {
                                      const auto& candidate = candidate_entry.second.identity;
                                      return candidate.location.uri == package_it->second.identity.location.uri &&
                                             candidate.name == reference.type_name &&
                                             isPackageMemberDefinitionKind(candidate.kind);
                                  }));
            }
        }
        if (definition_count < 2) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = "ambiguousReference",
                                                  .message = ambiguousReferenceMessage(reference.type_name,
                                                                                      definition_count),
                                                  .range = reference.reference.range,
                                                  .severity = 2});
    }
}

void appendUnresolvedTypeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const DiagnosticContext& context) {
    std::set<std::pair<int, int>> reported_type_ranges;
    const auto references_it = context.type_references_by_uri.find(context.document.uri);
    if (references_it == context.type_references_by_uri.end()) {
        return;
    }
    for (const auto& reference : references_it->second) {
        if (!reference.definitions.empty() || !isUserTypeReferenceName(reference.type_name)) {
            continue;
        }
        if (hasTypeDefinitionSymbol(context, reference.type_name)) {
            continue;
        }
        const auto range_key = std::pair(reference.reference.range.start_line,
                                         reference.reference.range.start_character);
        if (!reported_type_ranges.insert(range_key).second) {
            continue;
        }
        result.push_back(SemanticEngineDiagnostic{.uri = context.document.uri,
                                                  .code = std::string(kUnresolvedTypeDiagnosticCode),
                                                  .message = unresolvedTypeMessage(reference.type_name),
                                                  .range = reference.reference.range,
                                                  .severity = 1});
    }
}

void appendUnknownIncludeDiagnostics(std::vector<SemanticEngineDiagnostic>& result,
                                     const DiagnosticContext& context) {
    const auto includes_it = context.include_directives_by_uri.find(context.document.uri);
    if (includes_it == context.include_directives_by_uri.end()) {
        return;
    }
    for (const auto& include : includes_it->second) {
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
    const auto instances_it = context.module_instances_by_uri.find(context.document.uri);
    if (instances_it == context.module_instances_by_uri.end()) {
        return;
    }
    for (const auto& instance : instances_it->second) {
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
