#include "WorkspaceDiscoveryIndex.h"

#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SourceUtil.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <set>
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

std::vector<std::string> filesForTop(const WorkspaceDiscoveryIndex& index, std::string_view top_name) {
    const auto files_it = index.files_by_declaration.find(std::string(top_name));
    if (files_it == index.files_by_declaration.end()) {
        return {};
    }
    return files_it->second;
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
    CompilationService compilation_service;

    for (const auto& document : documents) {
        DiscoveryFile file;
        file.uri = document.uri;
        file.byte_count = document.text.size();
        ++index.file_count;
        index.byte_count += file.byte_count;

        try {
            for (const auto& module : compilation_service.moduleDefinitions(document.text, document.uri)) {
                file.declarations.push_back(DiscoverySymbol{.name = module.name,
                                                            .kind = module.kind.empty() ? "module" : module.kind,
                                                            .location = DiscoveryLocation{.uri = document.uri,
                                                                                          .range = module.range}});
                for (const auto& instance : module.instances) {
                    appendReference(file.referenced_top_level_names, instance.module_name);
                }
            }
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery module scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery module scan failed for " + document.uri);
        }

        try {
            appendDocumentSymbols(file.declarations,
                                  document.uri,
                                  compilation_service.documentSymbols(document.text, document.uri));
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery symbol scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery symbol scan failed for " + document.uri);
        }

        try {
            for (const auto& macro : compilation_service.macroDefinitions(document.text)) {
                index.macros.push_back(DiscoverySymbol{.name = macro.name,
                                                       .kind = macro.function_like ? "macro-function" : "macro",
                                                       .location = DiscoveryLocation{.uri = document.uri,
                                                                                     .range = macro.range}});
            }
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery macro scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery macro scan failed for " + document.uri);
        }

        try {
            for (const auto& import : compilation_service.packageImports(document.text)) {
                appendReference(file.referenced_top_level_names, import.package_name);
            }
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery package import scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery package import scan failed for " + document.uri);
        }

        try {
            for (const auto& export_reference : compilation_service.packageExports(document.text)) {
                appendReference(file.referenced_top_level_names, export_reference.package_name);
            }
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery package export scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery package export scan failed for " + document.uri);
        }

        try {
            for (const auto& include : compilation_service.includeDirectives(document.text)) {
                if (!include.target.empty()) {
                    file.included_uris.push_back(joinFileUri(uriDirectory(document.uri), include.target));
                }
            }
        }
        catch (const std::exception& error) {
            index.messages.push_back("Discovery include scan failed for " + document.uri + ": " + error.what());
        }
        catch (...) {
            index.messages.push_back("Discovery include scan failed for " + document.uri);
        }

        sortUniqueSymbols(file.declarations);
        sortUniqueStrings(file.referenced_top_level_names);
        sortUniqueStrings(file.included_uris);
        index.declarations.insert(index.declarations.end(), file.declarations.begin(), file.declarations.end());
        for (const auto& declaration : file.declarations) {
            index.files_by_declaration[declaration.name].push_back(file.uri);
            index.declarations_by_name[declaration.name].push_back(declaration);
        }
        for (const auto& reference : file.referenced_top_level_names) {
            index.referenced_files_by_name[reference].push_back(file.uri);
        }
        index.reference_count += file.referenced_top_level_names.size();
        index.files.push_back(std::move(file));
    }

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
    for (auto& [_, files] : index.referenced_files_by_name) {
        sortUniqueStrings(files);
    }
    std::sort(index.files.begin(), index.files.end(), [](const DiscoveryFile& lhs, const DiscoveryFile& rhs) {
        return lhs.uri < rhs.uri;
    });
    sortUniqueStrings(index.messages);
    return index;
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
