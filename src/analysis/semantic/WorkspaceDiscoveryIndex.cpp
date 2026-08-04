#include "WorkspaceDiscoveryIndex.h"

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <exception>
#include <set>
#include <thread>
#include <unordered_set>
#include <utility>

namespace pristine::analysis::semantic {
namespace {

bool rangeLess(const ParseRange& lhs, const ParseRange& rhs) {
    if (lhs.start_line != rhs.start_line) {
        return lhs.start_line < rhs.start_line;
    }
    if (lhs.start_character != rhs.start_character) {
        return lhs.start_character < rhs.start_character;
    }
    if (lhs.end_line != rhs.end_line) {
        return lhs.end_line < rhs.end_line;
    }
    return lhs.end_character < rhs.end_character;
}

bool symbolLess(const DiscoverySymbol& lhs, const DiscoverySymbol& rhs) {
    if (lhs.name != rhs.name) {
        return lhs.name < rhs.name;
    }
    if (lhs.kind != rhs.kind) {
        return lhs.kind < rhs.kind;
    }
    if (lhs.location.uri != rhs.location.uri) {
        return lhs.location.uri < rhs.location.uri;
    }
    return rangeLess(lhs.location.range, rhs.location.range);
}

bool sameSymbol(const DiscoverySymbol& lhs, const DiscoverySymbol& rhs) {
    return lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.location.uri == rhs.location.uri &&
           lhs.location.range.start_line == rhs.location.range.start_line &&
           lhs.location.range.start_character == rhs.location.range.start_character &&
           lhs.location.range.end_line == rhs.location.range.end_line &&
           lhs.location.range.end_character == rhs.location.range.end_character;
}

std::string documentSymbolKindName(int kind) {
    switch (kind) {
    case 2:
        return "module";
    case 4:
        return "package";
    case 5:
        return "class";
    case 10:
        return "enum";
    case 11:
        return "interface";
    case 12:
        return "callable";
    case 23:
        return "struct";
    case 26:
        return "type";
    default:
        return "symbol";
    }
}

void appendDocumentSymbols(std::vector<DiscoverySymbol>& declarations,
                           std::string_view uri,
                           const std::vector<DocumentSymbol>& symbols,
                           int depth = 0) {
    for (const auto& symbol : symbols) {
        if (depth == 0) {
            declarations.push_back(DiscoverySymbol{.name = symbol.name,
                                                   .kind = documentSymbolKindName(symbol.kind),
                                                   .location = DiscoveryLocation{.uri = std::string(uri),
                                                                                 .range = symbol.range}});
        }
        appendDocumentSymbols(declarations, uri, symbol.children, depth + 1);
    }
}

void appendTopLevelDocumentSymbolNames(std::vector<std::string>& names,
                                       const std::vector<DocumentSymbol>& symbols) {
    for (const auto& symbol : symbols) {
        if (!symbol.name.empty()) {
            names.push_back(symbol.name);
        }
        if (symbol.kind == 4) {
            for (const auto& package_member : symbol.children) {
                if (!package_member.name.empty()) {
                    names.push_back(package_member.name);
                }
            }
        }
    }
}

void sortUniqueStrings(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void sortUniqueSymbols(std::vector<DiscoverySymbol>& symbols) {
    std::sort(symbols.begin(), symbols.end(), symbolLess);
    symbols.erase(std::unique(symbols.begin(), symbols.end(), sameSymbol), symbols.end());
}

void appendReference(std::vector<std::string>& references, std::string value) {
    if (!value.empty()) {
        references.push_back(std::move(value));
    }
}

bool isMacroIdentifierStart(char value) {
    const auto character = static_cast<unsigned char>(value);
    return std::isalpha(character) != 0 || value == '_' || value == '$';
}

bool isMacroIdentifierContinue(char value) {
    const auto character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_' || value == '$';
}

bool isPreprocessorDirective(std::string_view name) {
    static constexpr std::string_view kDirectives[] = {"begin_keywords", "celldefine", "default_nettype",
                                                        "define", "else", "elsif", "end_keywords",
                                                        "endcelldefine", "endif", "ifdef", "ifndef",
                                                        "include", "line", "pragma", "resetall",
                                                        "timescale", "undef"};
    return std::find(std::begin(kDirectives), std::end(kDirectives), name) != std::end(kDirectives);
}

// This scanner only identifies possible macro propagation for closure planning.
std::vector<std::string> macroInvocations(std::string_view text) {
    std::vector<std::string> result;
    for (size_t index = 0; index < text.size();) {
        if (text[index] == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            index = text.find('\n', index + 2);
            if (index == std::string_view::npos) {
                break;
            }
            continue;
        }
        if (text[index] == '/' && index + 1 < text.size() && text[index + 1] == '*') {
            const auto close = text.find("*/", index + 2);
            index = close == std::string_view::npos ? text.size() : close + 2;
            continue;
        }
        if (text[index] == '"') {
            ++index;
            while (index < text.size()) {
                if (text[index] == '\\') {
                    index += std::min<size_t>(2, text.size() - index);
                    continue;
                }
                if (text[index++] == '"') {
                    break;
                }
            }
            continue;
        }
        if (text[index] != '`') {
            ++index;
            continue;
        }
        ++index;
        if (index < text.size() && text[index] == '`') {
            ++index;
            continue;
        }
        if (index >= text.size() || !isMacroIdentifierStart(text[index])) {
            continue;
        }
        const auto start = index++;
        while (index < text.size() && isMacroIdentifierContinue(text[index])) {
            ++index;
        }
        const auto name = text.substr(start, index - start);
        if (!isPreprocessorDirective(name)) {
            result.emplace_back(name);
        }
    }
    sortUniqueStrings(result);
    return result;
}

std::vector<std::string> filesForTop(const WorkspaceDiscoveryIndex& index, std::string_view top_name) {
    const auto files_it = index.files_by_declaration.find(std::string(top_name));
    if (files_it == index.files_by_declaration.end()) {
        return {};
    }
    return files_it->second;
}

struct DiscoveryScanResult {
    DiscoveryFile file;
    std::vector<DiscoverySymbol> macros;
    std::vector<DiscoveryMacroDefinition> macro_definitions;
    std::vector<std::string> messages;
};

DiscoveryScanResult scanDiscoveryDocument(const DiscoveryDocumentInput& document) {
    DiscoveryScanResult result;
    auto& file = result.file;
    file.uri = document.uri;
    file.byte_count = document.text.size();
    CompilationService compilation_service;

    const auto scan = [&](std::string_view label, auto&& callback) {
        try {
            callback();
        }
        catch (const std::exception& error) {
            result.messages.push_back("Discovery " + std::string(label) + " scan failed for " +
                                      document.uri + ": " + error.what());
        }
        catch (...) {
            result.messages.push_back("Discovery " + std::string(label) + " scan failed for " +
                                      document.uri);
        }
    };

    scan("module", [&] {
        for (const auto& module : compilation_service.moduleDefinitions(document.text, document.uri)) {
            file.declarations.push_back(DiscoverySymbol{
                .name = module.name,
                .kind = module.kind.empty() ? "module" : module.kind,
                .location = DiscoveryLocation{.uri = document.uri, .range = module.range}});
            for (const auto& instance : module.instances) {
                appendReference(file.referenced_top_level_names, instance.module_name);
            }
        }
    });
    scan("symbol", [&] {
        const auto symbols = compilation_service.documentSymbols(document.text, document.uri);
        appendDocumentSymbols(file.declarations, document.uri, symbols);
        appendTopLevelDocumentSymbolNames(file.declared_visible_names, symbols);
    });
    scan("macro", [&] {
        for (const auto& macro : compilation_service.macroDefinitions(document.text)) {
            result.macros.push_back(DiscoverySymbol{
                .name = macro.name,
                .kind = macro.function_like ? "macro-function" : "macro",
                .location = DiscoveryLocation{.uri = document.uri, .range = macro.range}});
            auto body_identifiers = compilation_service.lexicalIdentifiers(macro.body);
            result.macro_definitions.push_back(DiscoveryMacroDefinition{
                .name = macro.name,
                .uri = document.uri,
                .body_identifiers = std::move(body_identifiers.names),
                .complete = body_identifiers.complete,
                .reasons = std::move(body_identifiers.reasons)});
        }
        file.macro_invocations = macroInvocations(document.text);
    });
    scan("package import", [&] {
        for (const auto& import : compilation_service.packageImports(document.text)) {
            appendReference(file.referenced_top_level_names, import.package_name);
        }
    });
    scan("package export", [&] {
        for (const auto& export_reference : compilation_service.packageExports(document.text)) {
            appendReference(file.referenced_top_level_names, export_reference.package_name);
        }
    });
    scan("include", [&] {
        for (const auto& include : compilation_service.includeDirectives(document.text)) {
            if (!include.target.empty()) {
                file.included_uris.push_back(joinFileUri(uriDirectory(document.uri), include.target));
            }
        }
    });
    scan("identifier", [&] {
        auto identifiers = compilation_service.lexicalIdentifiers(document.text);
        file.referenced_visible_names = std::move(identifiers.names);
        file.closure_complete = identifiers.complete;
        file.closure_reasons = std::move(identifiers.reasons);
    });

    sortUniqueSymbols(file.declarations);
    sortUniqueStrings(file.declared_visible_names);
    sortUniqueStrings(file.referenced_top_level_names);
    sortUniqueStrings(file.referenced_visible_names);
    sortUniqueStrings(file.included_uris);
    sortUniqueStrings(file.macro_invocations);
    sortUniqueStrings(file.closure_reasons);
    sortUniqueSymbols(result.macros);
    for (auto& definition : result.macro_definitions) {
        sortUniqueStrings(definition.body_identifiers);
        sortUniqueStrings(definition.reasons);
    }
    std::sort(result.macro_definitions.begin(), result.macro_definitions.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.name, lhs.uri) < std::tie(rhs.name, rhs.uri);
    });
    sortUniqueStrings(result.messages);
    return result;
}

void appendDiscoveryScan(WorkspaceDiscoveryIndex& index, DiscoveryScanResult scan) {
    auto& file = scan.file;
    ++index.file_count;
    index.byte_count += file.byte_count;
    index.macros.insert(index.macros.end(), scan.macros.begin(), scan.macros.end());
    index.macro_definitions.insert(index.macro_definitions.end(),
                                   scan.macro_definitions.begin(),
                                   scan.macro_definitions.end());
    index.messages.insert(index.messages.end(), scan.messages.begin(), scan.messages.end());
    if (!file.closure_complete) {
        index.reference_candidate_incomplete_reasons.insert(index.reference_candidate_incomplete_reasons.end(),
                                                            file.closure_reasons.begin(),
                                                            file.closure_reasons.end());
    }
    index.declarations.insert(index.declarations.end(),
                              file.declarations.begin(),
                              file.declarations.end());
    for (const auto& declaration : file.declarations) {
        index.files_by_declaration[declaration.name].push_back(file.uri);
        index.files_by_visible_name[declaration.name].push_back(file.uri);
        index.declarations_by_name[declaration.name].push_back(declaration);
    }
    for (const auto& name : file.declared_visible_names) {
        index.files_by_visible_name[name].push_back(file.uri);
    }
    for (const auto& macro : scan.macros) {
        index.files_by_visible_name[macro.name].push_back(file.uri);
    }
    for (const auto& reference : file.referenced_top_level_names) {
        index.referenced_files_by_name[reference].push_back(file.uri);
    }
    for (const auto& name : file.referenced_visible_names) {
        index.reference_candidate_uris_by_name[name].push_back(file.uri);
    }
    for (const auto& name : file.macro_invocations) {
        index.macro_invocation_uris_by_name[name].push_back(file.uri);
    }
    index.reference_count += file.referenced_top_level_names.size();
    index.files.push_back(std::move(file));
}

void finalizeDiscoveryIndex(WorkspaceDiscoveryIndex& index) {
    sortUniqueSymbols(index.declarations);
    sortUniqueSymbols(index.macros);
    index.declaration_count = index.declarations.size();
    index.macro_count = index.macros.size();
    for (auto& [_, symbols] : index.declarations_by_name) {
        sortUniqueSymbols(symbols);
    }
    for (auto& [_, files] : index.files_by_declaration) {
        sortUniqueStrings(files);
    }
    for (auto& [_, files] : index.files_by_visible_name) {
        sortUniqueStrings(files);
    }
    for (auto& [_, files] : index.referenced_files_by_name) {
        sortUniqueStrings(files);
    }
    for (auto& [_, files] : index.reference_candidate_uris_by_name) {
        sortUniqueStrings(files);
    }
    for (auto& [_, files] : index.macro_invocation_uris_by_name) {
        sortUniqueStrings(files);
    }
    std::sort(index.macro_definitions.begin(),
              index.macro_definitions.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.name, lhs.uri, lhs.body_identifiers) <
                         std::tie(rhs.name, rhs.uri, rhs.body_identifiers);
              });
    sortUniqueStrings(index.reference_candidate_incomplete_reasons);
    std::sort(index.files.begin(), index.files.end(), [](const DiscoveryFile& lhs,
                                                         const DiscoveryFile& rhs) {
        return lhs.uri < rhs.uri;
    });
    sortUniqueStrings(index.messages);
}

} // namespace

WorkspaceDiscoveryIndex buildWorkspaceDiscoveryIndex(std::uint64_t generation,
                                                     std::vector<DiscoveryDocumentInput> documents) {
    std::sort(documents.begin(),
              documents.end(),
              [](const DiscoveryDocumentInput& lhs, const DiscoveryDocumentInput& rhs) {
                  return lhs.uri < rhs.uri;
              });

    WorkspaceDiscoveryIndex index;
    index.generation = generation;
    if (documents.size() >= 64) {
        std::vector<DiscoveryScanResult> scans(documents.size());
        std::atomic_size_t next_document = 0;
        const auto hardware_threads = std::max(1u, std::thread::hardware_concurrency());
        const auto worker_count = std::min<size_t>({documents.size(), hardware_threads, 8});
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                while (true) {
                    const auto index = next_document.fetch_add(1, std::memory_order_relaxed);
                    if (index >= documents.size()) {
                        return;
                    }
                    scans[index] = scanDiscoveryDocument(documents[index]);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        for (auto& scan : scans) {
            appendDiscoveryScan(index, std::move(scan));
        }
        finalizeDiscoveryIndex(index);
        return index;
    }

    for (const auto& document : documents) {
        appendDiscoveryScan(index, scanDiscoveryDocument(document));
    }

    finalizeDiscoveryIndex(index);
    return index;
}

DiscoveryDocumentClosure discoveryDocumentClosure(const WorkspaceDiscoveryIndex& index,
                                                   std::string_view root_uri,
                                                   size_t max_files) {
    DiscoveryDocumentClosure result;
    result.root_uri = std::string(root_uri);
    std::set<std::string> selected;
    std::deque<std::string> pending;

    const auto find_file = [&](std::string_view uri) -> const DiscoveryFile* {
        const auto it = std::lower_bound(index.files.begin(),
                                         index.files.end(),
                                         uri,
                                         [](const DiscoveryFile& file, std::string_view value) {
                                             return file.uri < value;
                                         });
        return it != index.files.end() && it->uri == uri ? &*it : nullptr;
    };
    const auto enqueue = [&](const std::string& uri) {
        if (selected.insert(uri).second) {
            pending.push_back(uri);
        }
    };

    if (find_file(root_uri) == nullptr) {
        result.reasons.push_back("root-document-not-indexed");
        return result;
    }
    enqueue(result.root_uri);

    while (!pending.empty()) {
        const auto uri = std::move(pending.front());
        pending.pop_front();
        if (max_files != 0 && selected.size() > max_files) {
            result.reasons.push_back("closure-file-limit");
            break;
        }
        const auto* file = find_file(uri);
        if (file == nullptr) {
            result.reasons.push_back("missing-document:" + uri);
            continue;
        }
        if (!file->closure_complete) {
            result.reasons.insert(result.reasons.end(),
                                  file->closure_reasons.begin(),
                                  file->closure_reasons.end());
        }
        for (const auto& included_uri : file->included_uris) {
            if (find_file(included_uri) == nullptr) {
                result.reasons.push_back("missing-include:" + included_uri);
            }
            else {
                enqueue(included_uri);
            }
        }
        for (const auto& name : file->referenced_visible_names) {
            if (const auto files = index.files_by_visible_name.find(name);
                files != index.files_by_visible_name.end()) {
                for (const auto& dependency_uri : files->second) {
                    enqueue(dependency_uri);
                }
            }
        }
    }

    result.uris.assign(selected.begin(), selected.end());
    sortUniqueStrings(result.reasons);
    result.confidence = result.reasons.empty() ? DiscoveryClosureConfidence::Complete
                                               : DiscoveryClosureConfidence::Incomplete;
    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto append_hash = [&](std::string_view value) {
        for (const auto byte : value) {
            fingerprint ^= static_cast<unsigned char>(byte);
            fingerprint *= 1099511628211ULL;
        }
        fingerprint ^= 0xffU;
        fingerprint *= 1099511628211ULL;
    };
    append_hash(result.root_uri);
    for (const auto& uri : result.uris) {
        append_hash(uri);
    }
    for (const auto& reason : result.reasons) {
        append_hash(reason);
    }
    result.fingerprint = fingerprint;
    return result;
}

std::vector<std::string> discoveryDependencyClosure(const WorkspaceDiscoveryIndex& index,
                                                    std::optional<std::string_view> top_name,
                                                    size_t max_files) {
    std::set<std::string> result;
    std::set<std::string> seen_names;
    std::set<std::string> seen_files;
    std::deque<std::string> pending_names;
    std::deque<std::string> pending_files;

    const auto find_file = [&](std::string_view uri) -> const DiscoveryFile* {
        const auto file_it = std::find_if(index.files.begin(), index.files.end(), [&](const DiscoveryFile& file) {
            return file.uri == uri;
        });
        return file_it == index.files.end() ? nullptr : &*file_it;
    };
    const auto enqueue_file = [&](std::string file_uri) {
        if (max_files != 0 && result.size() >= max_files && !result.contains(file_uri)) {
            return;
        }
        result.insert(file_uri);
        if (seen_files.insert(file_uri).second) {
            pending_files.push_back(std::move(file_uri));
        }
    };

    if (top_name.has_value() && !top_name->empty()) {
        pending_names.emplace_back(*top_name);
    }
    else {
        for (const auto& declaration : index.declarations) {
            if (declaration.kind == "module" || declaration.kind == "interface") {
                pending_names.push_back(declaration.name);
            }
        }
    }

    while (!pending_names.empty()) {
        while (!pending_names.empty()) {
            const auto name = std::move(pending_names.front());
            pending_names.pop_front();
            if (!seen_names.insert(name).second) {
                continue;
            }
            for (const auto& file_uri : filesForTop(index, name)) {
                enqueue_file(file_uri);
            }
        }

        while (!pending_files.empty()) {
            const auto file_uri = std::move(pending_files.front());
            pending_files.pop_front();
            const auto* file = find_file(file_uri);
            if (file == nullptr) {
                continue;
            }
            for (const auto& reference : file->referenced_top_level_names) {
                if (!seen_names.contains(reference)) {
                    pending_names.push_back(reference);
                }
            }
            for (const auto& included_uri : file->included_uris) {
                enqueue_file(included_uri);
            }
        }
    }

    return std::vector<std::string>(result.begin(), result.end());
}

} // namespace pristine::analysis::semantic
