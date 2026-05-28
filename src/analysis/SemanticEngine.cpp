#include "pristine/analysis/SemanticEngine.h"

#include "pristine/analysis/SourceUtil.h"

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <map>
#include <set>
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

std::optional<Identifier> identifierAtDocument(const SemanticEngineDocument& document,
                                               int line,
                                               int character) {
    CompilationService compilation_service;
    return compilation_service.identifierAt(document.text, line, character);
}

SemanticLocation makeLocation(std::string uri, const ParseRange& range) {
    return SemanticLocation{.uri = std::move(uri), .range = range};
}

} // namespace

void SemanticEngine::clear() {
    workspace_root_uri_.clear();
    config_ = {};
    documents_.clear();
    includes_.clear();
    reverse_includes_.clear();
    snapshot_.reset();
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
    const auto document_it = documents_.find(document_uri);
    if (document_it == documents_.end()) {
        result.messages.push_back("document is not indexed by SemanticEngine");
        return result;
    }

    const auto identifier = identifierAtDocument(document_it->second, line, character);
    if (!identifier.has_value()) {
        result.messages.push_back("no identifier at position");
        return result;
    }

    result.query_location.range = identifier->range;
    result.symbol = SemanticSymbolIdentity{.stable_id = document_uri + "|" + identifier->name + "|" +
                                                        std::to_string(identifier->range.start_line) + ":" +
                                                        std::to_string(identifier->range.start_character),
                                           .name = identifier->name,
                                           .kind = "identifier",
                                           .location = makeLocation(document_uri, identifier->range)};
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
    auto result = definitionsAt(uri, line, character);
    if (!result.unresolved) {
        result.messages.push_back("type definition is using identifier-level SemanticEngine fallback");
    }
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

    const auto target_name = lookup.symbol->name;
    for (const auto& [document_uri, document] : documents_) {
        CompilationService compilation_service;
        for (const auto& identifier : compilation_service.identifiers(document.text)) {
            if (identifier.name != target_name) {
                continue;
            }
            const auto location = makeLocation(document_uri, identifier.range);
            if (!include_declaration && sameLocation(location, lookup.symbol->location)) {
                continue;
            }
            result.locations.push_back(location);
        }
    }

    std::sort(result.locations.begin(), result.locations.end(), locationLess);
    result.locations.erase(std::unique(result.locations.begin(), result.locations.end(), sameLocation),
                           result.locations.end());
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
    result.contents = "**Identifier** `" + lookup.symbol->name + "`";
    result.range = lookup.symbol->location.range;
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
    result.range = lookup.symbol->location.range;
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
    auto source_manager = std::make_unique<slang::SourceManager>();
    source_manager->setDisableProximatePaths(true);
    const auto options = makeCompilationOptions();

    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    syntax_trees.reserve(documents_.size());

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
                                                                *source_manager,
                                                                "source",
                                                                fileUriToPath(uri),
                                                                options);
        if (tree) {
            syntax_trees.push_back(std::move(tree));
        }
    }

    if (!syntax_trees.empty()) {
        try {
            slang::ast::Compilation compilation(options);
            for (auto& tree : syntax_trees) {
                compilation.addSyntaxTree(tree);
            }
            next.has_shallow_ast = true;
            next.has_design_ast = next.mode == SemanticEngineMode::Design;

            slang::DiagnosticEngine diagnostic_engine(*source_manager);
            for (const auto& diagnostic : compilation.getSemanticDiagnostics()) {
                const auto uri = diagnosticUri(*source_manager, diagnostic);
                if (uri.empty() || documents_.find(uri) == documents_.end()) {
                    continue;
                }
                const auto severity = diagnostic_engine.getSeverity(diagnostic.code, diagnostic.location);
                next.diagnostics.push_back(
                    SemanticEngineDiagnostic{.uri = uri,
                                             .code = std::string("slang:") +
                                                     std::string(slang::toString(diagnostic.code)),
                                             .message = diagnostic_engine.formatMessage(diagnostic),
                                             .range = sourceRangeForDiagnostic(*source_manager, diagnostic),
                                             .severity = toLspSeverity(severity)});
            }
        }
        catch (...) {
            next.has_shallow_ast = false;
            next.has_design_ast = false;
        }
    }

    snapshot_ = std::move(next);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
