#include "pristine/analysis/SemanticEngine.h"

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <utility>

namespace pristine::analysis {
namespace {

bool isFileUri(std::string_view value) {
    return value.starts_with("file://");
}

bool isWindowsAbsolutePath(std::string_view value) {
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

std::string toForwardSlashes(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool isDriveSegment(std::string_view value) {
    return value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':';
}

std::string normalizeFileUri(std::string_view uri) {
    if (!isFileUri(uri)) {
        const auto normalized_path = toForwardSlashes(uri);
        if (isWindowsAbsolutePath(normalized_path)) {
            return std::string("file:///") + normalized_path;
        }
        return normalized_path;
    }

    constexpr std::string_view prefix = "file://";
    const auto path = toForwardSlashes(uri.substr(prefix.size()));
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> segments;

    size_t position = 0;
    while (position <= path.size()) {
        const auto separator = path.find('/', position);
        const auto segment = path.substr(position, separator == std::string::npos
                                                       ? std::string::npos
                                                       : separator - position);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (!segments.empty() && !isDriveSegment(segments.back())) {
                    segments.pop_back();
                }
                else if (!absolute) {
                    segments.emplace_back(segment);
                }
            }
            else {
                segments.emplace_back(segment);
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        position = separator + 1;
    }

    std::string normalized_path = absolute ? "/" : "";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized_path.push_back('/');
        }
        normalized_path += segments[index];
    }
    return std::string(prefix) + normalized_path;
}

std::string withoutTrailingSlash(std::string value) {
    constexpr std::string_view root_uri = "file:///";
    while (value.size() > root_uri.size() && value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

std::string fileUriToPath(std::string_view uri) {
    auto normalized = withoutTrailingSlash(normalizeFileUri(uri));
    constexpr std::string_view prefix = "file://";
    if (!normalized.starts_with(prefix)) {
        return std::string(normalized);
    }

    auto path = std::string(normalized.substr(prefix.size()));
    if (path.size() >= 3 && path.front() == '/' && isDriveSegment(std::string_view(path).substr(1, 2))) {
        path.erase(path.begin());
    }
    return path;
}

std::string pathToFileUri(const std::filesystem::path& path) {
    auto normalized = toForwardSlashes(path.string());
    if (normalized.empty()) {
        return {};
    }
    if (!normalized.empty() && normalized.front() == '/') {
        return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
    }
    if (isWindowsAbsolutePath(normalized)) {
        return withoutTrailingSlash(normalizeFileUri(std::string("file:///") + normalized));
    }
    return withoutTrailingSlash(normalizeFileUri(std::string("file://") + normalized));
}

ParseRange sourceRangeForDiagnostic(const slang::SourceManager& source_manager,
                                    const slang::Diagnostic& diagnostic) {
    slang::SourceLocation start = diagnostic.location;
    slang::SourceLocation end = diagnostic.location;
    if (!diagnostic.ranges.empty()) {
        start = diagnostic.ranges.front().start();
        end = diagnostic.ranges.front().end();
    }

    if (!start.valid()) {
        return {};
    }
    if (!end.valid() || end.buffer() != start.buffer() || end.offset() < start.offset()) {
        end = start + 1;
    }

    return ParseRange{
        .start_line = static_cast<int>(source_manager.getLineNumber(start)) - 1,
        .start_character = static_cast<int>(source_manager.getColumnNumber(start)) - 1,
        .end_line = static_cast<int>(source_manager.getLineNumber(end)) - 1,
        .end_character = static_cast<int>(source_manager.getColumnNumber(end)) - 1};
}

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

} // namespace

void SemanticEngine::clear() {
    documents_.clear();
    snapshot_.reset();
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::setWorkspaceRoot(std::string_view root_uri) {
    workspace_root_uri_ = root_uri.empty() ? std::string{} : withoutTrailingSlash(normalizeFileUri(root_uri));
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
    snapshot_dirty_ = true;
    ++generation_;
}

void SemanticEngine::removeDocument(std::string_view uri) {
    documents_.erase(withoutTrailingSlash(normalizeFileUri(uri)));
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

void SemanticEngine::rebuildSnapshot() const {
    auto source_manager = std::make_unique<slang::SourceManager>();
    source_manager->setDisableProximatePaths(true);
    const auto options = makeCompilationOptions();

    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    syntax_trees.reserve(documents_.size());

    SemanticEngineSnapshot next{};
    next.generation = generation_;
    next.has_ast = false;

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
            next.has_ast = true;

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
            next.has_ast = false;
        }
    }

    snapshot_ = std::move(next);
    snapshot_dirty_ = false;
}

} // namespace pristine::analysis
