#pragma once

#include "pristine/lsp/Protocol.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pristine::workspace {

struct IndexConfig {
    std::vector<std::string> dirs;
    std::vector<std::string> exclude_dirs;
};

struct Config {
    std::optional<std::string> build;
    std::optional<std::string> build_pattern;
    bool build_relative_paths = false;
    std::optional<std::string> flags;
    std::vector<IndexConfig> index;
};

struct State {
    std::optional<std::filesystem::path> root_path;
    std::optional<std::filesystem::path> config_path;
    Config config;
    bool config_loaded = false;
    std::optional<std::string> config_error;
};

class WorkspaceManager {
public:
    void initialize(const lsp::InitializeParams& params);

    const State& state() const { return state_; }

private:
    static std::optional<std::filesystem::path> resolveRootPath(const lsp::InitializeParams& params);
    static Config parseConfig(const lsp::Json& config_json);
    void loadConfig();

    State state_;
};

} // namespace pristine::workspace