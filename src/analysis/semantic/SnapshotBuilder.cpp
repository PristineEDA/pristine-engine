#include "SnapshotBuilder.h"

#include "AstIndex.h"
#include "DebugTrace.h"
#include "pristine/analysis/CompilationService.h"
#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <unordered_map>

namespace pristine::analysis::semantic {
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

slang::Bag makeCompilationOptions(const SemanticEngineConfig& config) {
    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;
    compilation_options.flags |= slang::ast::CompilationFlags::IgnoreUnknownModules;
    compilation_options.flags |= slang::ast::CompilationFlags::AllowUseBeforeDeclare;
    compilation_options.errorLimit = 0;
    for (const auto& top_module : config.top_modules) {
        compilation_options.topModules.emplace(top_module);
    }

    slang::Bag options;
    options.set(compilation_options);
    return options;
}

void addUnique(std::vector<std::string>& values, std::string value) {
    values.push_back(std::move(value));
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void sortUnique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string normalizedDocumentUri(std::string_view key, const SemanticEngineDocument& document) {
    const auto source_uri = document.uri.empty() ? std::string(key) : document.uri;
    return withoutTrailingSlash(normalizeFileUri(source_uri));
}

bool shouldReplaceDocument(const SemanticEngineDocument& existing,
                           const SemanticEngineDocument& candidate) {
    if (candidate.is_open != existing.is_open) {
        return candidate.is_open;
    }
    if (candidate.version != existing.version) {
        return candidate.version > existing.version;
    }
    return candidate.dirty && !existing.dirty;
}

} // namespace

SnapshotData::SnapshotData() = default;
SnapshotData::~SnapshotData() = default;
SnapshotData::SnapshotData(SnapshotData&&) noexcept = default;
SnapshotData& SnapshotData::operator=(SnapshotData&&) noexcept = default;

SnapshotBuildInput normalizeSnapshotBuildInput(SnapshotBuildInput input) {
    sortUnique(input.config.top_modules);
    for (auto& index_config : input.config.index) {
        sortUnique(index_config.dirs);
        sortUnique(index_config.exclude_dirs);
    }
    input.config.index.erase(std::remove_if(input.config.index.begin(),
                                            input.config.index.end(),
                                            [](const SemanticEngineConfig::IndexConfig& index_config) {
                                                return index_config.dirs.empty();
                                            }),
                             input.config.index.end());

    for (auto& uri : input.dirty_document_uris) {
        uri = withoutTrailingSlash(normalizeFileUri(uri));
    }
    sortUnique(input.dirty_document_uris);

    std::unordered_map<std::string, SemanticEngineDocument> normalized_documents;
    normalized_documents.reserve(input.documents.size());
    for (auto& document_entry : input.documents) {
        auto document = std::move(document_entry.second);
        const auto document_uri = normalizedDocumentUri(document_entry.first, document);
        document.uri = document_uri;

        const auto existing_it = normalized_documents.find(document_uri);
        if (existing_it == normalized_documents.end()) {
            normalized_documents.emplace(document_uri, std::move(document));
        }
        else if (shouldReplaceDocument(existing_it->second, document)) {
            existing_it->second = std::move(document);
        }
    }
    input.documents = std::move(normalized_documents);
    return input;
}

SnapshotBuildInputSummary snapshotBuildInputSummary(const SnapshotBuildInput& input) {
    SnapshotBuildInputSummary summary;
    summary.document_count = input.documents.size();
    summary.top_module_count = input.config.top_modules.size();
    summary.index_config_count = input.config.index.size();
    for (const auto& document_entry : input.documents) {
        if (document_entry.second.is_open) {
            ++summary.open_document_count;
        }
        if (document_entry.second.dirty) {
            ++summary.dirty_document_count;
        }
    }
    return summary;
}

SnapshotBuildOutput SnapshotBuilder::build(SnapshotBuildInput input) const {
    input = normalizeSnapshotBuildInput(std::move(input));
    PRISTINE_DEBUG_TRACE_SCOPE_LAZY("snapshotBuilder.build", [&] {
        const auto input_summary = snapshotBuildInputSummary(input);
        return std::to_string(input_summary.document_count) +
               " documents open=" + std::to_string(input_summary.open_document_count) +
               " dirty=" + std::to_string(input_summary.dirty_document_count) +
               " topModules=" + std::to_string(input_summary.top_module_count) +
               " generation=" + std::to_string(input.generation);
    });
    auto data = std::make_unique<SnapshotData>();
    data->source_manager = std::make_unique<slang::SourceManager>();
    data->source_manager->setDisableProximatePaths(true);
    const auto options = makeCompilationOptions(input.config);
    data->syntax_trees.reserve(input.documents.size());

    SnapshotBuildOutput output;
    output.snapshot.generation = input.generation;
    output.snapshot.mode = input.config.build.has_value() || input.config.build_pattern.has_value() ||
                                   !input.config.top_modules.empty()
                               ? SemanticEngineMode::Design
                               : SemanticEngineMode::Shallow;
    output.snapshot.top_modules = input.config.top_modules;
    output.snapshot.dirty_document_uris = std::move(input.dirty_document_uris);
    output.snapshot.has_shallow_ast = false;
    output.snapshot.has_design_ast = false;

    for (const auto& document_entry : input.documents) {
        output.snapshot.document_uris.push_back(document_entry.first);
    }
    std::sort(output.snapshot.document_uris.begin(), output.snapshot.document_uris.end());

    std::unordered_map<std::string, slang::SourceBuffer> source_buffers_by_uri;
    source_buffers_by_uri.reserve(output.snapshot.document_uris.size());
    {
        PRISTINE_DEBUG_TRACE_SCOPE("snapshotBuilder.assignBuffers",
                                   std::to_string(output.snapshot.document_uris.size()) + " documents");
        for (const auto& uri : output.snapshot.document_uris) {
            const auto document_it = input.documents.find(uri);
            if (document_it == input.documents.end()) {
                continue;
            }

            const auto path = fileUriToPath(uri);
            auto buffer = data->source_manager->assignText(path, document_it->second.text);
            if (!buffer) {
                continue;
            }
            data->source_manager->addLineDirective(slang::SourceLocation(buffer.id, 0), 2, "source", 0);
            source_buffers_by_uri.emplace(uri, buffer);
        }
    }

    {
        PRISTINE_DEBUG_TRACE_SCOPE("snapshotBuilder.syntaxTrees",
                                   std::to_string(output.snapshot.document_uris.size()) + " documents");
        for (const auto& uri : output.snapshot.document_uris) {
            const auto document_it = input.documents.find(uri);
            if (document_it == input.documents.end()) {
                continue;
            }

            CompilationService compilation_service;
            data->macros_by_uri[uri] = compilation_service.macroDefinitions(document_it->second.text);
            data->package_imports_by_uri[uri] = compilation_service.packageImports(document_it->second.text);

            auto include_directives = compilation_service.includeDirectives(document_it->second.text);
            data->include_directives_by_uri[uri] = include_directives;

            std::vector<std::string> included_uris;
            for (const auto& include : include_directives) {
                addUnique(included_uris, joinFileUri(uriDirectory(uri), include.target));
            }
            output.affected_dependencies.setIncludedUris(uri, std::move(included_uris));

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

            const auto buffer_it = source_buffers_by_uri.find(uri);
            if (buffer_it == source_buffers_by_uri.end()) {
                continue;
            }

            auto tree = slang::syntax::SyntaxTree::fromBuffer(buffer_it->second, *data->source_manager, options);
            if (tree) {
                data->syntax_trees.push_back(std::move(tree));
            }
        }
    }

    if (!data->syntax_trees.empty()) {
        try {
            PRISTINE_DEBUG_TRACE_SCOPE("snapshotBuilder.compilation",
                                       std::to_string(data->syntax_trees.size()) + " syntax trees");
            data->compilation = std::make_unique<slang::ast::Compilation>(options);
            for (auto& tree : data->syntax_trees) {
                data->compilation->addSyntaxTree(tree);
            }

            slang::DiagnosticEngine diagnostic_engine(*data->source_manager);
            for (auto& tree : data->syntax_trees) {
                for (const auto& diagnostic : tree->diagnostics()) {
                    const auto uri = diagnosticUri(*data->source_manager, diagnostic);
                    if (uri.empty() || input.documents.find(uri) == input.documents.end()) {
                        continue;
                    }
                    const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                    output.snapshot.diagnostics.push_back(
                        SemanticEngineDiagnostic{.uri = uri,
                                                 .code = std::string("slang:") +
                                                         std::string(slang::toString(diagnostic.code)),
                                                 .message = diagnostic_engine.formatMessage(diagnostic),
                                                 .range = sourceRangeForDiagnostic(*data->source_manager, diagnostic),
                                                 .severity = toLspSeverity(severity)});
                }
            }

            output.snapshot.has_shallow_ast = true;
            output.snapshot.has_design_ast = output.snapshot.mode == SemanticEngineMode::Design;

            {
                PRISTINE_DEBUG_TRACE_SCOPE("snapshotBuilder.buildAstIndexes",
                                           std::to_string(input.documents.size()) + " documents");
                buildAstIndexes(*data, input.documents);
            }

            {
                PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("snapshotBuilder.semanticDependencyEdges");
                std::unordered_map<std::string, std::vector<std::string>> package_uris_by_name;
                for (const auto& [_, symbol] : data->symbols_by_id) {
                    if (symbol.identity.kind != "Package" || symbol.identity.location.uri.empty()) {
                        continue;
                    }
                    addUnique(package_uris_by_name[symbol.identity.name], symbol.identity.location.uri);
                }

                for (const auto& [uri, imports] : data->package_imports_by_uri) {
                    for (const auto& package_import : imports) {
                        const auto package_it = package_uris_by_name.find(package_import.package_name);
                        if (package_it == package_uris_by_name.end()) {
                            continue;
                        }
                        for (const auto& package_uri : package_it->second) {
                            output.affected_dependencies.addSemanticDependency(
                                AffectedDependencyEdgeKind::SemanticImport,
                                package_uri,
                                uri);
                        }
                    }
                }

                for (const auto& [uri, instances] : data->module_instances_by_uri) {
                    for (const auto& instance : instances) {
                        const auto module_it = data->module_uris_by_name.find(instance.module_name);
                        if (module_it == data->module_uris_by_name.end() || module_it->second.empty() ||
                            module_it->second == uri) {
                            continue;
                        }
                        output.affected_dependencies.addSemanticDependency(
                            AffectedDependencyEdgeKind::ModuleInstance,
                            module_it->second,
                            uri);
                    }
                }
            }

            {
                PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE("snapshotBuilder.semanticDiagnostics");
                for (const auto& diagnostic : data->compilation->getSemanticDiagnostics()) {
                    const auto uri = diagnosticUri(*data->source_manager, diagnostic);
                    if (uri.empty() || input.documents.find(uri) == input.documents.end()) {
                        continue;
                    }
                    const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                    output.snapshot.diagnostics.push_back(
                        SemanticEngineDiagnostic{.uri = uri,
                                                 .code = std::string("slang:") +
                                                         std::string(slang::toString(diagnostic.code)),
                                                 .message = diagnostic_engine.formatMessage(diagnostic),
                                                 .range = sourceRangeForDiagnostic(*data->source_manager, diagnostic),
                                                 .severity = toLspSeverity(severity)});
                }
            }
        }
        catch (...) {
            output.snapshot.has_shallow_ast = false;
            output.snapshot.has_design_ast = false;
            data.reset();
        }
    }

    output.data = std::move(data);
    return output;
}

} // namespace pristine::analysis::semantic
