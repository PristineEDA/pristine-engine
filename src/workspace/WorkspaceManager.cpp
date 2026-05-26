#include "pristine/workspace/WorkspaceManager.h"

#include <cctype>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pristine::workspace {
namespace {

namespace fs = std::filesystem;

std::optional<std::string> getOptionalString(const lsp::Json& value, std::string_view field_name) {
    const auto field_it = value.find(field_name);
    if (field_it == value.end() || field_it->is_null()) {
        return std::nullopt;
    }

    if (!field_it->is_string()) {
        throw std::runtime_error("Expected '" + std::string(field_name) + "' to be a string");
    }

    return field_it->get<std::string>();
}

std::vector<std::string> parseStringArray(const lsp::Json& value, std::string_view field_name) {
    const auto field_it = value.find(field_name);
    if (field_it == value.end() || field_it->is_null()) {
        return {};
    }

    if (!field_it->is_array()) {
        throw std::runtime_error("Expected '" + std::string(field_name) + "' to be an array");
    }

    std::vector<std::string> result;
    for (const auto& entry : *field_it) {
        if (!entry.is_string()) {
            throw std::runtime_error("Expected '" + std::string(field_name) + "' entries to be strings");
        }
        result.push_back(entry.get<std::string>());
    }

    return result;
}

int decodeHexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    if (value >= 'A' && value <= 'F') {
        return 10 + (value - 'A');
    }

    throw std::runtime_error("Invalid percent-encoded path");
}

std::string percentDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());

    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }

        if (index + 2 >= value.size()) {
            throw std::runtime_error("Truncated percent-encoded path");
        }

        const auto upper = decodeHexNibble(value[index + 1]);
        const auto lower = decodeHexNibble(value[index + 2]);
        result.push_back(static_cast<char>((upper << 4) | lower));
        index += 2;
    }

    return result;
}

bool isSystemVerilogFile(const fs::path& path) {
    const auto extension = path.extension().string();
    return extension == ".sv" || extension == ".svh" || extension == ".v" || extension == ".vh";
}

bool isExcludedPath(const fs::path& path, const std::vector<std::string>& exclude_dirs) {
    const auto normalized = path.generic_string();
    return std::any_of(exclude_dirs.begin(), exclude_dirs.end(), [&](const std::string& exclude_dir) {
        return !exclude_dir.empty() && normalized.find(fs::path(exclude_dir).generic_string()) != std::string::npos;
    });
}

void collectSourceFiles(const fs::path& directory,
                        const std::vector<std::string>& exclude_dirs,
                        std::vector<fs::path>& result) {
    std::error_code error;
    if (!fs::exists(directory, error) || !fs::is_directory(directory, error)) {
        return;
    }

    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }

        const auto current_path = it->path();
        if (it->is_directory(error) && isExcludedPath(current_path, exclude_dirs)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(error) && isSystemVerilogFile(current_path) &&
            !isExcludedPath(current_path, exclude_dirs)) {
            result.push_back(current_path);
        }
    }
}

} // namespace

void WorkspaceManager::initialize(const lsp::InitializeParams& params) {
    state_ = {};

    state_.root_path = resolveRootPath(params);
    if (!state_.root_path.has_value()) {
        return;
    }

    loadConfig();
}

std::optional<fs::path> WorkspaceManager::resolveRootPath(const lsp::InitializeParams& params) {
    if (params.workspace_folders.has_value() && !params.workspace_folders->empty()) {
        if (auto path = pathFromFileUri(params.workspace_folders->front().uri)) {
            return path;
        }
    }

    if (params.root_uri.has_value()) {
        if (auto path = pathFromFileUri(*params.root_uri)) {
            return path;
        }
    }

    if (params.root_path.has_value() && !params.root_path->empty()) {
        return fs::path(*params.root_path);
    }

    return std::nullopt;
}

std::optional<fs::path> WorkspaceManager::pathFromFileUri(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) {
        return std::nullopt;
    }

    auto decoded = percentDecode(uri.substr(prefix.size()));
#ifdef _WIN32
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) &&
        decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif

    return fs::path(decoded);
}

std::vector<fs::path> WorkspaceManager::sourceFilesForIndex() const {
    std::vector<fs::path> result;
    if (!state_.root_path.has_value()) {
        return result;
    }

    if (state_.config.index.empty()) {
        collectSourceFiles(*state_.root_path, {}, result);
    }
    else {
        for (const auto& index_config : state_.config.index) {
            for (const auto& dir : index_config.dirs) {
                const auto directory = fs::path(dir).is_absolute() ? fs::path(dir) : *state_.root_path / dir;
                collectSourceFiles(directory, index_config.exclude_dirs, result);
            }
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

Config WorkspaceManager::parseConfig(const lsp::Json& config_json) {
    if (!config_json.is_object()) {
        throw std::runtime_error("Expected workspace config root to be a JSON object");
    }

    Config result{};
    result.build = getOptionalString(config_json, "build");
    result.build_pattern = getOptionalString(config_json, "buildPattern");
    result.flags = getOptionalString(config_json, "flags");

    const auto build_relative_paths_it = config_json.find("buildRelativePaths");
    if (build_relative_paths_it != config_json.end() && !build_relative_paths_it->is_null()) {
        if (!build_relative_paths_it->is_boolean()) {
            throw std::runtime_error("Expected 'buildRelativePaths' to be a boolean");
        }
        result.build_relative_paths = build_relative_paths_it->get<bool>();
    }

    const auto index_it = config_json.find("index");
    if (index_it != config_json.end() && !index_it->is_null()) {
        if (!index_it->is_array()) {
            throw std::runtime_error("Expected 'index' to be an array");
        }

        for (const auto& entry : *index_it) {
            if (!entry.is_object()) {
                throw std::runtime_error("Expected 'index' entries to be objects");
            }

            auto dirs = parseStringArray(entry, "dirs");
            if (dirs.empty()) {
                throw std::runtime_error("Expected each 'index' entry to provide at least one dir");
            }

            result.index.push_back(IndexConfig{.dirs = std::move(dirs),
                                               .exclude_dirs = parseStringArray(entry, "excludeDirs")});
        }
    }

    return result;
}

void WorkspaceManager::loadConfig() {
    if (!state_.root_path.has_value()) {
        return;
    }

    state_.config_path = *state_.root_path / ".slang" / "server.json";
    if (!fs::exists(*state_.config_path)) {
        state_.config_path.reset();
        return;
    }

    try {
        std::ifstream input(*state_.config_path);
        if (!input) {
            throw std::runtime_error("Failed to open workspace config for reading");
        }

        lsp::Json config_json;
        input >> config_json;
        state_.config = parseConfig(config_json);
        state_.config_loaded = true;
    }
    catch (const std::exception& exception) {
        state_.config_error = "Failed to parse workspace config: " + std::string(exception.what());
        std::cerr << *state_.config_error << '\n';
    }
}

} // namespace pristine::workspace