#include "pristine/analysis/SemanticEngine.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

long long elapsedMicros(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

std::string jsonString(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch) << std::dec << std::setfill(' ');
            }
            else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

std::string boolJson(bool value) {
    return value ? "true" : "false";
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isSystemVerilogSource(const fs::path& path) {
    const auto extension = lowerAscii(path.extension().string());
    return extension == ".sv" || extension == ".svh" || extension == ".v" || extension == ".vh";
}

bool shouldSkipDirectory(const fs::path& path) {
    const auto name = lowerAscii(path.filename().string());
    return name == ".git" || name == ".deps" || name == "build" || name == "out" ||
           name == "obj" || name == "node_modules";
}

std::string fileUriFor(const fs::path& path) {
    auto normalized = fs::absolute(path).lexically_normal().generic_string();
    if (normalized.empty() || normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    return "file://" + normalized;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<fs::path> collectSourceFiles(const fs::path& root) {
    std::vector<fs::path> result;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (it != end) {
        const auto path = it->path();
        if (it->is_directory(ec)) {
            if (shouldSkipDirectory(path)) {
                it.disable_recursion_pending();
            }
        }
        else if (it->is_regular_file(ec) && isSystemVerilogSource(path)) {
            result.push_back(path);
        }
        it.increment(ec);
    }
    std::sort(result.begin(), result.end(), [](const fs::path& lhs, const fs::path& rhs) {
        return lhs.generic_string() < rhs.generic_string();
    });
    return result;
}

size_t schematicCellCount(const pristine::analysis::SemanticSchematicResult& schematic) {
    size_t count = 0;
    for (const auto& module : schematic.modules) {
        count += module.module.cells.size();
    }
    return count;
}

size_t schematicNetCount(const pristine::analysis::SemanticSchematicResult& schematic) {
    size_t count = 0;
    for (const auto& module : schematic.modules) {
        count += module.nets.size();
    }
    return count;
}

std::optional<std::string_view> maybeTop(std::string_view top_module) {
    if (top_module.empty()) {
        return std::nullopt;
    }
    return top_module;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: pristine_retrosoc_stress <RETROSOC_ROOT> [topModule]\n";
        return 2;
    }

    const fs::path root = fs::path(argv[1]);
    std::string top_module = argc >= 3 ? argv[2] : "";
    if (!fs::exists(root)) {
        std::cerr << "retroSoC root does not exist: " << root << "\n";
        return 2;
    }

    const auto files = collectSourceFiles(root);
    if (files.empty()) {
        std::cerr << "No SystemVerilog or Verilog sources found under " << root << "\n";
        return 2;
    }

    pristine::analysis::SemanticEngine engine;
    pristine::analysis::SemanticEngineConfig config;
    config.workspace_root_uri = fileUriFor(root);
    if (!top_module.empty()) {
        config.top_modules.push_back(top_module);
    }
    engine.configure(std::move(config));

    size_t byte_count = 0;
    const auto load_start = Clock::now();
    for (const auto& file : files) {
        auto text = readTextFile(file);
        byte_count += text.size();
        engine.updateDocument(fileUriFor(file),
                              text,
                              pristine::analysis::SemanticEngineDocumentState{
                                  .version = 1,
                                  .is_open = false,
                                  .dirty = false});
    }
    const auto load_end = Clock::now();

    const auto parse_start = Clock::now();
    const auto& snapshot = engine.snapshot();
    const auto parse_end = Clock::now();

    const auto hierarchy_start = Clock::now();
    auto hierarchy = engine.moduleHierarchy(maybeTop(top_module), 64);
    const auto hierarchy_end = Clock::now();
    if (top_module.empty() && !hierarchy.roots.empty()) {
        top_module = hierarchy.roots.front().module_name;
    }

    const auto schematic_start = Clock::now();
    const auto schematic = engine.schematic(maybeTop(top_module), 64);
    const auto schematic_end = Clock::now();
    if (top_module.empty() && schematic.root_module_id.has_value()) {
        top_module = *schematic.root_module_id;
    }

    const auto messages_count = hierarchy.messages.size() + schematic.messages.size();
    std::cout << "{"
              << "\"fileCount\":" << files.size() << ","
              << "\"byteCount\":" << byte_count << ","
              << "\"topModule\":" << jsonString(top_module) << ","
              << "\"documentLoadMicros\":" << elapsedMicros(load_start, load_end) << ","
              << "\"parseIndexMicros\":" << elapsedMicros(parse_start, parse_end) << ","
              << "\"moduleHierarchyMicros\":" << elapsedMicros(hierarchy_start, hierarchy_end) << ","
              << "\"schematicMicros\":" << elapsedMicros(schematic_start, schematic_end) << ","
              << "\"diagnosticCount\":" << snapshot.diagnostics.size() << ","
              << "\"hierarchyRootCount\":" << hierarchy.roots.size() << ","
              << "\"schematicModuleCount\":" << schematic.modules.size() << ","
              << "\"schematicCellCount\":" << schematicCellCount(schematic) << ","
              << "\"schematicNetCount\":" << schematicNetCount(schematic) << ","
              << "\"partial\":" << boolJson(hierarchy.partial || schematic.partial) << ","
              << "\"truncated\":" << boolJson(hierarchy.truncated || schematic.truncated) << ","
              << "\"messagesCount\":" << messages_count << "}\n";

    return hierarchy.unresolved || schematic.unresolved ? 1 : 0;
}
