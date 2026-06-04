#include <lsp/connection.h>
#include <lsp/io/stream.h>
#include <lsp/json/json.h>
#include <lsp/process.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string currentTraceTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    return std::to_string(millis);
}

struct SourceFile {
    fs::path path;
    std::string uri;
    std::string text;
};

struct WorkspaceStats {
    size_t file_count = 0;
    size_t byte_count = 0;
};

struct Metrics {
    std::string mode = "probe";
    std::string corpus_name = "retrosoc";
    std::string corpus_root;
    std::string corpus_commit;
    std::string server_path;
    size_t file_count = 0;
    size_t byte_count = 0;
    size_t opened_file_count = 0;
    size_t opened_byte_count = 0;
    std::string opened_source_path;
    std::string top_module;
    long long client_workspace_discovery_micros = 0;
    long long client_open_file_select_micros = 0;
    long long initialize_micros = 0;
    long long did_open_all_micros = 0;
    long long did_open_probe_micros = 0;
    long long hierarchy_cold_micros = 0;
    long long hierarchy_warm_micros = 0;
    long long schematic_micros = 0;
    long long hierarchy_cold_closure_build_micros = 0;
    long long hierarchy_warm_closure_build_micros = 0;
    long long schematic_closure_build_micros = 0;
    long long hierarchy_cold_closure_query_micros = 0;
    long long hierarchy_warm_closure_query_micros = 0;
    long long schematic_closure_query_micros = 0;
    std::string hierarchy_cold_closure_root;
    std::string hierarchy_warm_closure_root;
    std::string schematic_closure_root;
    size_t hierarchy_cold_closure_candidate_document_count = 0;
    size_t hierarchy_warm_closure_candidate_document_count = 0;
    size_t schematic_closure_candidate_document_count = 0;
    size_t hierarchy_cold_closure_document_count = 0;
    size_t hierarchy_warm_closure_document_count = 0;
    size_t schematic_closure_document_count = 0;
    size_t hierarchy_cold_closure_missing_candidate_count = 0;
    size_t hierarchy_warm_closure_missing_candidate_count = 0;
    size_t schematic_closure_missing_candidate_count = 0;
    size_t hierarchy_cold_closure_deduped_document_count = 0;
    size_t hierarchy_warm_closure_deduped_document_count = 0;
    size_t schematic_closure_deduped_document_count = 0;
    bool hierarchy_cold_closure_used = false;
    bool hierarchy_warm_closure_used = false;
    bool schematic_closure_used = false;
    bool hierarchy_cold_closure_cache_hit = false;
    bool hierarchy_warm_closure_cache_hit = false;
    bool schematic_closure_cache_hit = false;
    long long shutdown_micros = 0;
    long long total_micros = 0;
    size_t hierarchy_root_count = 0;
    size_t schematic_module_count = 0;
    size_t schematic_cell_count = 0;
    size_t schematic_net_count = 0;
    bool partial = false;
    bool truncated = false;
    size_t messages_count = 0;
    size_t diagnostics_notification_count = 0;
    bool trace_enabled = false;
    std::string trace_path;
};

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

bool isSourceFile(const fs::path& path) {
    const auto extension = lowerAscii(path.extension().string());
    return extension == ".sv" || extension == ".svh" || extension == ".v" || extension == ".vh";
}

bool shouldSkipDirectory(const fs::path& path) {
    const auto name = lowerAscii(path.filename().string());
    return name == ".git" || name == ".deps" || name == "build" || name == "out" ||
           name == "obj" || name == "node_modules";
}

std::string readFileText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) {
        throw std::runtime_error("Unable to read source file: " + path.string());
    }
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

std::string fileUriFor(const fs::path& path) {
    auto normalized = fs::absolute(path).lexically_normal().generic_string();
    if (normalized.empty() || normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    return "file://" + normalized;
}

WorkspaceStats collectWorkspaceStats(const fs::path& root) {
    WorkspaceStats result;
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
        else if (it->is_regular_file(ec) && isSourceFile(path)) {
            ++result.file_count;
            const auto size = it->file_size(ec);
            if (!ec) {
                result.byte_count += static_cast<size_t>(size);
            }
        }
        it.increment(ec);
    }
    return result;
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
        else if (it->is_regular_file(ec) && isSourceFile(path)) {
            result.push_back(fs::absolute(path).lexically_normal());
        }
        it.increment(ec);
    }
    std::sort(result.begin(), result.end(), [](const fs::path& lhs, const fs::path& rhs) {
        return lhs.generic_string() < rhs.generic_string();
    });
    return result;
}

std::string sanitizedIdentifier(std::string value, std::string fallback) {
    if (value.empty()) {
        value = std::move(fallback);
    }
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_') {
            result.push_back(static_cast<char>(ch));
        }
        else {
            result.push_back('_');
        }
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), '_');
    }
    return result;
}

SourceFile makeProbeSource(const fs::path& root, const std::string& requested_top) {
    if (requested_top.empty()) {
        std::ostringstream text;
        text << "module rtl_e2e_probe_child(input logic clk, output logic q);\n"
             << "  assign q = clk;\n"
             << "endmodule\n\n"
             << "module rtl_e2e_probe_top_a(input logic clk, output logic q);\n"
             << "  rtl_e2e_probe_child u_child_a(.clk(clk), .q(q));\n"
             << "endmodule\n\n"
             << "module rtl_e2e_probe_top_b(input logic clk, output logic q);\n"
             << "  rtl_e2e_probe_child u_child_b(.clk(clk), .q(q));\n"
             << "endmodule\n";
        const auto path = root / "__pristine_lsp_probe.sv";
        return SourceFile{.path = path, .uri = fileUriFor(path), .text = text.str()};
    }

    const auto top = sanitizedIdentifier(requested_top, "rtl_e2e_probe_top");
    const auto child = sanitizedIdentifier(top + "_child", "rtl_e2e_probe_child");
    std::ostringstream text;
    text << "module " << child << "(input logic clk, output logic q);\n"
         << "  assign q = clk;\n"
         << "endmodule\n\n"
         << "module " << top << "(input logic clk, output logic q);\n"
         << "  " << child << " u_child(.clk(clk), .q(q));\n"
         << "endmodule\n";
    const auto path = root / "__pristine_lsp_probe.sv";
    return SourceFile{.path = path, .uri = fileUriFor(path), .text = text.str()};
}

std::optional<std::string> firstModuleName(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    static const std::regex module_regex(R"(\b(?:module|interface)\s+([A-Za-z_][A-Za-z0-9_$]*))");
    const auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), module_regex);
    const auto end = std::cregex_iterator();
    if (begin == end) {
        return std::nullopt;
    }
    return (*begin)[1].str();
}

bool containsTopDeclaration(std::string_view text, std::string_view top) {
    if (text.empty() || top.empty()) {
        return false;
    }
    static const std::regex module_regex(R"(\b(?:module|interface)\s+([A-Za-z_][A-Za-z0-9_$]*))");
    const auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), module_regex);
    const auto end = std::cregex_iterator();
    return std::any_of(begin, end, [&](const std::cmatch& match) {
        return match[1].str() == top;
    });
}

SourceFile selectRealSource(const fs::path& root, std::string& top_module) {
    const auto files = collectSourceFiles(root);
    if (files.empty()) {
        throw std::runtime_error("No SystemVerilog or Verilog sources found under " + root.string());
    }

    std::optional<SourceFile> first_with_module;
    for (const auto& path : files) {
        auto text = readFileText(path);
        if (!top_module.empty() && containsTopDeclaration(text, top_module)) {
            return SourceFile{.path = path, .uri = fileUriFor(path), .text = std::move(text)};
        }
        if (!first_with_module.has_value()) {
            if (const auto module_name = firstModuleName(text)) {
                if (top_module.empty()) {
                    top_module = *module_name;
                }
                first_with_module = SourceFile{.path = path, .uri = fileUriFor(path), .text = std::move(text)};
            }
        }
    }

    if (!top_module.empty()) {
        throw std::runtime_error("No RTL file declares requested top module/interface '" + top_module + "'");
    }
    if (first_with_module.has_value()) {
        return std::move(*first_with_module);
    }

    auto text = readFileText(files.front());
    top_module = sanitizedIdentifier(files.front().stem().string(), "rtl_e2e_real_top");
    return SourceFile{.path = files.front(), .uri = fileUriFor(files.front()), .text = std::move(text)};
}

std::string commandQuote(std::string_view value) {
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            result += "\\\"";
        }
        else {
            result += ch;
        }
    }
    result += '"';
    return result;
}

std::string commandOutput(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
#if defined(_WIN32)
    (void)_pclose(pipe);
#else
    (void)pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::string gitHeadFor(const fs::path& root) {
    if (!fs::exists(root / ".git")) {
        return {};
    }
    return commandOutput("git -C " + commandQuote(root.string()) + " rev-parse HEAD");
}

lsp::json::Value textDocumentIdentifier(std::string uri, std::string text) {
    lsp::json::Object result;
    result["uri"] = lsp::json::Value(std::move(uri));
    result["languageId"] = lsp::json::Value(std::string("systemverilog"));
    result["version"] = lsp::json::Value(lsp::json::Integer(1));
    result["text"] = lsp::json::Value(std::move(text));
    return result;
}

lsp::json::Value initializeParams(const fs::path& root) {
    lsp::json::Object result;
    result["processId"] = lsp::json::Value(nullptr);
    result["rootUri"] = lsp::json::Value(fileUriFor(root));
    result["capabilities"] = lsp::json::Value(lsp::json::Object{});
    return result;
}

lsp::json::Value hierarchyParams(const std::string& top_module, int max_depth) {
    lsp::json::Object params;
    if (!top_module.empty()) {
        params["moduleName"] = lsp::json::Value(std::string(top_module));
    }
    params["maxDepth"] = lsp::json::Value(lsp::json::Integer(max_depth));
    return params;
}

size_t arraySize(const lsp::json::Object& object_value, std::string_view key) {
    const auto* value = object_value.find(key);
    if (value == nullptr || !value->isArray()) {
        return 0;
    }
    return value->array().size();
}

bool boolValue(const lsp::json::Object& object_value, std::string_view key) {
    const auto* value = object_value.find(key);
    return value != nullptr && value->isBoolean() && value->boolean();
}

long long integerValue(const lsp::json::Object& object_value, std::string_view key) {
    const auto* value = object_value.find(key);
    if (value == nullptr || !value->isInteger()) {
        return 0;
    }
    return static_cast<long long>(value->integer());
}

std::string stringValue(const lsp::json::Object& object_value, std::string_view key) {
    const auto* value = object_value.find(key);
    if (value == nullptr || !value->isString()) {
        return {};
    }
    return value->string();
}

lsp::json::Value messageIdToJson(const lsp::jsonrpc::MessageId& id) {
    return std::visit(
        [](const auto& value) -> lsp::json::Value {
            using ValueType = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<ValueType, lsp::json::String>) {
                return lsp::json::Value(lsp::json::String(value));
            }
            else {
                return lsp::json::Value(value);
            }
        },
        id);
}

lsp::json::Value requestToJsonValue(const lsp::jsonrpc::Request& request) {
    lsp::json::Object object_value;
    object_value["jsonrpc"] = lsp::json::Value(std::string("2.0"));
    if (request.id.has_value()) {
        object_value["id"] = messageIdToJson(*request.id);
    }
    object_value["method"] = lsp::json::Value(lsp::json::String(request.method));
    if (request.params.has_value()) {
        object_value["params"] = *request.params;
    }
    return object_value;
}

lsp::json::Value responseToJsonValue(const lsp::jsonrpc::Response& response) {
    lsp::json::Object object_value;
    object_value["jsonrpc"] = lsp::json::Value(std::string("2.0"));
    object_value["id"] = messageIdToJson(response.id);
    if (response.result.has_value()) {
        object_value["result"] = *response.result;
    }
    if (response.error.has_value()) {
        lsp::json::Object error;
        error["code"] = lsp::json::Value(response.error->code);
        error["message"] = lsp::json::Value(lsp::json::String(response.error->message));
        if (response.error->data.has_value()) {
            error["data"] = *response.error->data;
        }
        object_value["error"] = lsp::json::Value(std::move(error));
    }
    return object_value;
}

std::string messageKind(const lsp::jsonrpc::Message& message) {
    if (std::holds_alternative<lsp::jsonrpc::Request>(message)) {
        return std::get<lsp::jsonrpc::Request>(message).isNotification() ? "notification" : "request";
    }
    return "response";
}

std::string messageMethod(const lsp::jsonrpc::Message& message) {
    if (std::holds_alternative<lsp::jsonrpc::Request>(message)) {
        return std::get<lsp::jsonrpc::Request>(message).method;
    }
    return {};
}

lsp::json::Value messageToJsonValue(const lsp::jsonrpc::Message& message) {
    if (std::holds_alternative<lsp::jsonrpc::Request>(message)) {
        return requestToJsonValue(std::get<lsp::jsonrpc::Request>(message));
    }
    return responseToJsonValue(std::get<lsp::jsonrpc::Response>(message));
}

class ProtocolTracer {
public:
    ProtocolTracer() = default;

    explicit ProtocolTracer(fs::path path)
        : path_(std::move(path)) {
        if (!path_.parent_path().empty()) {
            fs::create_directories(path_.parent_path());
        }
        stream_.open(path_, std::ios::binary);
        if (!stream_.good()) {
            throw std::runtime_error("Unable to write LSP trace: " + path_.string());
        }
    }

    bool enabled() const {
        return stream_.is_open();
    }

    const fs::path& path() const {
        return path_;
    }

    void record(std::string_view direction, const lsp::jsonrpc::Request& request) {
        if (!enabled()) {
            return;
        }
        stream_ << "{\"timeUnixMillis\":" << currentTraceTimestamp()
                << ",\"direction\":" << jsonString(direction)
                << ",\"kind\":" << jsonString(request.isNotification() ? "notification" : "request");
        if (request.id.has_value()) {
            stream_ << ",\"id\":" << lsp::json::stringify(messageIdToJson(*request.id));
        }
        stream_ << ",\"method\":" << jsonString(request.method)
                << ",\"message\":" << lsp::json::stringify(requestToJsonValue(request))
                << "}\n";
        stream_.flush();
    }

    void record(std::string_view direction, const lsp::jsonrpc::Response& response) {
        if (!enabled()) {
            return;
        }
        stream_ << "{\"timeUnixMillis\":" << currentTraceTimestamp()
                << ",\"direction\":" << jsonString(direction)
                << ",\"kind\":\"response\""
                << ",\"id\":" << lsp::json::stringify(messageIdToJson(response.id))
                << ",\"message\":" << lsp::json::stringify(responseToJsonValue(response))
                << "}\n";
        stream_.flush();
    }

    void record(std::string_view direction, const lsp::jsonrpc::Message& message) {
        if (!enabled()) {
            return;
        }
        stream_ << "{\"timeUnixMillis\":" << currentTraceTimestamp()
                << ",\"direction\":" << jsonString(direction)
                << ",\"kind\":" << jsonString(messageKind(message));
        const auto method = messageMethod(message);
        if (!method.empty()) {
            stream_ << ",\"method\":" << jsonString(method);
        }
        if (std::holds_alternative<lsp::jsonrpc::Response>(message)) {
            stream_ << ",\"id\":" << lsp::json::stringify(
                messageIdToJson(std::get<lsp::jsonrpc::Response>(message).id));
        }
        else if (const auto& request = std::get<lsp::jsonrpc::Request>(message); request.id.has_value()) {
            stream_ << ",\"id\":" << lsp::json::stringify(messageIdToJson(*request.id));
        }
        stream_ << ",\"message\":" << lsp::json::stringify(messageToJsonValue(message))
                << "}\n";
        stream_.flush();
    }

private:
    fs::path path_;
    std::ofstream stream_;
};


void collectHierarchyMetrics(const lsp::json::Value& result, Metrics& metrics, bool warm) {
    if (!result.isObject()) {
        throw std::runtime_error("moduleHierarchy response result is not an object");
    }
    const auto& response = result.object();
    metrics.hierarchy_root_count = arraySize(response, "roots");
    metrics.partial = metrics.partial || boolValue(response, "partial");
    metrics.truncated = metrics.truncated || boolValue(response, "truncated");
    metrics.messages_count += arraySize(response, "messages");
    if (metrics.top_module.empty()) {
        const auto* roots = response.find("roots");
        if (roots != nullptr && roots->isArray() && !roots->array().empty() &&
            roots->array().front().isObject()) {
            metrics.top_module = stringValue(roots->array().front().object(), "moduleName");
        }
    }
    if (warm) {
        metrics.hierarchy_warm_closure_used = boolValue(response, "discoveryClosureUsed");
        metrics.hierarchy_warm_closure_root = stringValue(response, "discoveryClosureRoot");
        metrics.hierarchy_warm_closure_candidate_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureCandidateDocumentCount"));
        metrics.hierarchy_warm_closure_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureDocumentCount"));
        metrics.hierarchy_warm_closure_missing_candidate_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureMissingCandidateCount"));
        metrics.hierarchy_warm_closure_deduped_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureDedupedDocumentCount"));
        metrics.hierarchy_warm_closure_build_micros =
            integerValue(response, "discoveryClosureBuildMicros");
        metrics.hierarchy_warm_closure_query_micros =
            integerValue(response, "discoveryClosureQueryMicros");
        metrics.hierarchy_warm_closure_cache_hit = boolValue(response, "discoveryClosureCacheHit");
    }
    else {
        metrics.hierarchy_cold_closure_used = boolValue(response, "discoveryClosureUsed");
        metrics.hierarchy_cold_closure_root = stringValue(response, "discoveryClosureRoot");
        metrics.hierarchy_cold_closure_candidate_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureCandidateDocumentCount"));
        metrics.hierarchy_cold_closure_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureDocumentCount"));
        metrics.hierarchy_cold_closure_missing_candidate_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureMissingCandidateCount"));
        metrics.hierarchy_cold_closure_deduped_document_count =
            static_cast<size_t>(integerValue(response, "discoveryClosureDedupedDocumentCount"));
        metrics.hierarchy_cold_closure_build_micros =
            integerValue(response, "discoveryClosureBuildMicros");
        metrics.hierarchy_cold_closure_query_micros =
            integerValue(response, "discoveryClosureQueryMicros");
        metrics.hierarchy_cold_closure_cache_hit = boolValue(response, "discoveryClosureCacheHit");
    }
}

void collectSchematicMetrics(const lsp::json::Value& result, Metrics& metrics) {
    if (!result.isObject()) {
        throw std::runtime_error("schematic response result is not an object");
    }
    const auto& response = result.object();
    metrics.schematic_module_count = arraySize(response, "modules");
    metrics.partial = metrics.partial || boolValue(response, "partial");
    metrics.truncated = metrics.truncated || boolValue(response, "truncated");
    metrics.messages_count += arraySize(response, "messages");
    metrics.schematic_closure_used = boolValue(response, "discoveryClosureUsed");
    metrics.schematic_closure_root = stringValue(response, "discoveryClosureRoot");
    metrics.schematic_closure_candidate_document_count =
        static_cast<size_t>(integerValue(response, "discoveryClosureCandidateDocumentCount"));
    metrics.schematic_closure_document_count =
        static_cast<size_t>(integerValue(response, "discoveryClosureDocumentCount"));
    metrics.schematic_closure_missing_candidate_count =
        static_cast<size_t>(integerValue(response, "discoveryClosureMissingCandidateCount"));
    metrics.schematic_closure_deduped_document_count =
        static_cast<size_t>(integerValue(response, "discoveryClosureDedupedDocumentCount"));
    metrics.schematic_closure_build_micros = integerValue(response, "discoveryClosureBuildMicros");
    metrics.schematic_closure_query_micros = integerValue(response, "discoveryClosureQueryMicros");
    metrics.schematic_closure_cache_hit = boolValue(response, "discoveryClosureCacheHit");

    const auto* modules = response.find("modules");
    if (modules == nullptr || !modules->isArray()) {
        return;
    }
    for (const auto& module_value : modules->array()) {
        if (!module_value.isObject()) {
            continue;
        }
        const auto& module = module_value.object();
        const auto* module_payload = module.find("module");
        if (module_payload != nullptr && module_payload->isObject()) {
            metrics.schematic_cell_count += arraySize(module_payload->object(), "cells");
        }
        else {
            metrics.schematic_cell_count += arraySize(module, "cells");
        }
        metrics.schematic_net_count += arraySize(module, "nets");
    }
}

class LspClient {
public:
    explicit LspClient(std::string server_path, ProtocolTracer* tracer)
        : process_(std::move(server_path), {"--stdio"})
        , connection_(process_.stdIO())
        , tracer_(tracer) {}

    ~LspClient() {
        if (process_.isRunning()) {
            process_.terminate();
        }
    }

    lsp::json::Value request(std::string_view method, lsp::json::Value params) {
        const auto request_id = next_id_++;
        lsp::jsonrpc::Request request{.id = lsp::jsonrpc::MessageId(request_id),
                                      .method = std::string(method),
                                      .params = std::move(params)};
        if (tracer_ != nullptr) {
            tracer_->record("client->server", request);
        }
        connection_.writeMessage(std::move(request));
        while (true) {
            auto message_variant = connection_.readMessage();
            if (!std::holds_alternative<lsp::jsonrpc::Message>(message_variant)) {
                continue;
            }
            auto message = std::get<lsp::jsonrpc::Message>(std::move(message_variant));
            if (tracer_ != nullptr) {
                tracer_->record("server->client", message);
            }
            if (std::holds_alternative<lsp::jsonrpc::Request>(message)) {
                const auto& incoming = std::get<lsp::jsonrpc::Request>(message);
                if (incoming.method == "textDocument/publishDiagnostics") {
                    ++diagnostics_notification_count_;
                }
                continue;
            }
            auto response = std::get<lsp::jsonrpc::Response>(std::move(message));
            if (response.id != lsp::jsonrpc::MessageId(request_id)) {
                continue;
            }
            if (response.error.has_value()) {
                throw std::runtime_error(std::string(method) + " failed: " + response.error->message);
            }
            if (!response.result.has_value()) {
                return lsp::json::Value(nullptr);
            }
            return std::move(*response.result);
        }
    }

    void notify(std::string_view method, lsp::json::Value params) {
        lsp::jsonrpc::Request request{.method = std::string(method),
                                      .params = std::move(params)};
        if (tracer_ != nullptr) {
            tracer_->record("client->server", request);
        }
        connection_.writeMessage(std::move(request));
    }

    void shutdown() {
        (void)request("shutdown", lsp::json::Value(nullptr));
        lsp::jsonrpc::Request request{.method = "exit"};
        if (tracer_ != nullptr) {
            tracer_->record("client->server", request);
        }
        connection_.writeMessage(std::move(request));
        process_.wait();
    }

    size_t diagnosticsNotificationCount() const {
        return diagnostics_notification_count_;
    }

private:
    lsp::Process process_;
    lsp::Connection connection_;
    ProtocolTracer* tracer_ = nullptr;
    lsp::json::Integer next_id_ = 1;
    size_t diagnostics_notification_count_ = 0;
};

void writeOperation(std::ofstream& log,
                    std::string_view name,
                    long long micros,
                    const lsp::json::Value* result = nullptr) {
    log << "{\"operation\":" << jsonString(name) << ",\"micros\":" << micros;
    if (result != nullptr) {
        log << ",\"result\":" << lsp::json::stringify(*result);
    }
    log << "}\n";
    log.flush();
}

void writeStage(std::ofstream& log, std::string_view stage, std::string_view detail = {}) {
    std::cerr << "[rtl-e2e-lsp] " << stage;
    if (!detail.empty()) {
        std::cerr << " " << detail;
    }
    std::cerr << std::endl;
    log << "{\"event\":\"stage\",\"stage\":" << jsonString(stage);
    if (!detail.empty()) {
        log << ",\"detail\":" << jsonString(detail);
    }
    log << "}\n";
    log.flush();
}

std::string summaryJson(const Metrics& metrics) {
    std::ostringstream out;
    out << "{"
        << "\"mode\":" << jsonString(metrics.mode) << ","
        << "\"corpusName\":" << jsonString(metrics.corpus_name) << ","
        << "\"corpusRoot\":" << jsonString(metrics.corpus_root) << ","
        << "\"corpusCommit\":" << jsonString(metrics.corpus_commit) << ","
        << "\"serverPath\":" << jsonString(metrics.server_path) << ","
        << "\"fileCount\":" << metrics.file_count << ","
        << "\"byteCount\":" << metrics.byte_count << ","
        << "\"openedFileCount\":" << metrics.opened_file_count << ","
        << "\"openedByteCount\":" << metrics.opened_byte_count << ","
        << "\"openedSourcePath\":" << jsonString(metrics.opened_source_path) << ","
        << "\"topModule\":" << jsonString(metrics.top_module) << ","
        << "\"clientWorkspaceDiscoveryMicros\":" << metrics.client_workspace_discovery_micros << ","
        << "\"clientOpenFileSelectMicros\":" << metrics.client_open_file_select_micros << ","
        << "\"initializeMicros\":" << metrics.initialize_micros << ","
        << "\"didOpenAllMicros\":" << metrics.did_open_all_micros << ","
        << "\"didOpenProbeMicros\":" << metrics.did_open_probe_micros << ","
        << "\"moduleHierarchyColdMicros\":" << metrics.hierarchy_cold_micros << ","
        << "\"moduleHierarchyWarmMicros\":" << metrics.hierarchy_warm_micros << ","
        << "\"schematicMicros\":" << metrics.schematic_micros << ","
        << "\"moduleHierarchyColdClosureUsed\":" << boolJson(metrics.hierarchy_cold_closure_used) << ","
        << "\"moduleHierarchyColdClosureRoot\":"
        << jsonString(metrics.hierarchy_cold_closure_root) << ","
        << "\"moduleHierarchyColdClosureCandidateDocumentCount\":"
        << metrics.hierarchy_cold_closure_candidate_document_count << ","
        << "\"moduleHierarchyColdClosureDocumentCount\":"
        << metrics.hierarchy_cold_closure_document_count << ","
        << "\"moduleHierarchyColdClosureMissingCandidateCount\":"
        << metrics.hierarchy_cold_closure_missing_candidate_count << ","
        << "\"moduleHierarchyColdClosureDedupedDocumentCount\":"
        << metrics.hierarchy_cold_closure_deduped_document_count << ","
        << "\"moduleHierarchyColdClosureBuildMicros\":"
        << metrics.hierarchy_cold_closure_build_micros << ","
        << "\"moduleHierarchyColdClosureQueryMicros\":"
        << metrics.hierarchy_cold_closure_query_micros << ","
        << "\"moduleHierarchyColdClosureCacheHit\":"
        << boolJson(metrics.hierarchy_cold_closure_cache_hit) << ","
        << "\"moduleHierarchyWarmClosureUsed\":" << boolJson(metrics.hierarchy_warm_closure_used) << ","
        << "\"moduleHierarchyWarmClosureRoot\":"
        << jsonString(metrics.hierarchy_warm_closure_root) << ","
        << "\"moduleHierarchyWarmClosureCandidateDocumentCount\":"
        << metrics.hierarchy_warm_closure_candidate_document_count << ","
        << "\"moduleHierarchyWarmClosureDocumentCount\":"
        << metrics.hierarchy_warm_closure_document_count << ","
        << "\"moduleHierarchyWarmClosureMissingCandidateCount\":"
        << metrics.hierarchy_warm_closure_missing_candidate_count << ","
        << "\"moduleHierarchyWarmClosureDedupedDocumentCount\":"
        << metrics.hierarchy_warm_closure_deduped_document_count << ","
        << "\"moduleHierarchyWarmClosureBuildMicros\":"
        << metrics.hierarchy_warm_closure_build_micros << ","
        << "\"moduleHierarchyWarmClosureQueryMicros\":"
        << metrics.hierarchy_warm_closure_query_micros << ","
        << "\"moduleHierarchyWarmClosureCacheHit\":"
        << boolJson(metrics.hierarchy_warm_closure_cache_hit) << ","
        << "\"schematicClosureUsed\":" << boolJson(metrics.schematic_closure_used) << ","
        << "\"schematicClosureRoot\":" << jsonString(metrics.schematic_closure_root) << ","
        << "\"schematicClosureCandidateDocumentCount\":"
        << metrics.schematic_closure_candidate_document_count << ","
        << "\"schematicClosureDocumentCount\":" << metrics.schematic_closure_document_count << ","
        << "\"schematicClosureMissingCandidateCount\":"
        << metrics.schematic_closure_missing_candidate_count << ","
        << "\"schematicClosureDedupedDocumentCount\":"
        << metrics.schematic_closure_deduped_document_count << ","
        << "\"schematicClosureBuildMicros\":" << metrics.schematic_closure_build_micros << ","
        << "\"schematicClosureQueryMicros\":" << metrics.schematic_closure_query_micros << ","
        << "\"schematicClosureCacheHit\":" << boolJson(metrics.schematic_closure_cache_hit) << ","
        << "\"shutdownMicros\":" << metrics.shutdown_micros << ","
        << "\"totalMicros\":" << metrics.total_micros << ","
        << "\"hierarchyRootCount\":" << metrics.hierarchy_root_count << ","
        << "\"schematicModuleCount\":" << metrics.schematic_module_count << ","
        << "\"schematicCellCount\":" << metrics.schematic_cell_count << ","
        << "\"schematicNetCount\":" << metrics.schematic_net_count << ","
        << "\"partial\":" << boolJson(metrics.partial) << ","
        << "\"truncated\":" << boolJson(metrics.truncated) << ","
        << "\"messagesCount\":" << metrics.messages_count << ","
        << "\"diagnosticsNotificationCount\":" << metrics.diagnostics_notification_count << ","
        << "\"traceEnabled\":" << boolJson(metrics.trace_enabled) << ","
        << "\"tracePath\":" << jsonString(metrics.trace_path) << "}";
    return out.str();
}

struct Args {
    fs::path server;
    fs::path root;
    fs::path log_dir;
    fs::path trace_file;
    std::string top_module;
    std::string mode = "probe";
    std::string corpus_name = "retrosoc";
    int max_depth = 64;
    bool trace = false;
};

Args parseArgs(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const auto arg = std::string_view(argv[index]);
        const auto read_value = [&](std::string_view name) -> std::string {
            const auto prefix = std::string(name) + "=";
            if (arg.starts_with(prefix)) {
                return std::string(arg.substr(prefix.size()));
            }
            if (arg == name && index + 1 < argc) {
                return argv[++index];
            }
            return {};
        };

        if (auto value = read_value("--server"); !value.empty()) {
            args.server = value;
        }
        else if (auto value = read_value("--root"); !value.empty()) {
            args.root = value;
        }
        else if (auto value = read_value("--log-dir"); !value.empty()) {
            args.log_dir = value;
        }
        else if (auto value = read_value("--trace-file"); !value.empty()) {
            args.trace_file = value;
            args.trace = true;
        }
        else if (auto value = read_value("--top"); !value.empty()) {
            args.top_module = value;
        }
        else if (auto value = read_value("--mode"); !value.empty()) {
            args.mode = lowerAscii(value);
        }
        else if (auto value = read_value("--corpus"); !value.empty()) {
            args.corpus_name = lowerAscii(value);
        }
        else if (auto value = read_value("--max-depth"); !value.empty()) {
            args.max_depth = std::max(0, std::stoi(value));
        }
        else if (arg == "--trace") {
            args.trace = true;
        }
        else if (arg == "--no-trace") {
            args.trace = false;
            args.trace_file.clear();
        }
        else {
            throw std::runtime_error("Unknown or incomplete argument: " + std::string(arg));
        }
    }

    if (args.server.empty() || args.root.empty() || args.log_dir.empty()) {
        throw std::runtime_error("Usage: pristine_rtl_e2e_lsp_stress --server <path> --root <path> --log-dir <path> [--mode probe|real] [--corpus <name>] [--top <module>] [--max-depth <n>] [--trace] [--trace-file <path>] [--no-trace]");
    }
    if (args.mode != "probe" && args.mode != "real") {
        throw std::runtime_error("--mode must be 'probe' or 'real'");
    }
    if (args.trace && args.trace_file.empty()) {
        args.trace_file = args.log_dir / "lsp-trace.jsonl";
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parseArgs(argc, argv);
        fs::create_directories(args.log_dir);
        const auto operation_log_path = args.log_dir / "operations.jsonl";
        const auto summary_path = args.log_dir / "summary.json";
        std::ofstream operation_log(operation_log_path, std::ios::binary);
        if (!operation_log.good()) {
            throw std::runtime_error("Unable to write operation log: " + operation_log_path.string());
        }
        std::optional<ProtocolTracer> tracer;
        if (args.trace) {
            tracer.emplace(args.trace_file);
            writeStage(operation_log,
                       "trace:enabled",
                       fs::absolute(args.trace_file).lexically_normal().string());
        }

        writeStage(operation_log, "source-discovery:begin", args.root.string());
        auto start = Clock::now();
        auto workspace_stats = collectWorkspaceStats(args.root);
        const auto client_workspace_discovery_micros = elapsedMicros(start, Clock::now());
        writeOperation(operation_log, "client/sourceDiscovery", client_workspace_discovery_micros);
        writeStage(operation_log,
                   "source-discovery:end",
                   std::to_string(client_workspace_discovery_micros) + "us");
        if (workspace_stats.file_count == 0) {
            throw std::runtime_error("No SystemVerilog or Verilog sources found under " + args.root.string());
        }
        auto selected_top = args.top_module;
        writeStage(operation_log, "open-source-select:begin", args.mode);
        start = Clock::now();
        auto opened_source = args.mode == "real" ? selectRealSource(args.root, selected_top)
                                                 : makeProbeSource(args.root, args.top_module);
        const auto client_open_file_select_micros = elapsedMicros(start, Clock::now());
        writeOperation(operation_log, "client/openFileSelect", client_open_file_select_micros);
        writeStage(operation_log,
                   "open-source-select:end",
                   std::to_string(client_open_file_select_micros) + "us " + opened_source.path.string());

        Metrics metrics;
        metrics.mode = args.mode;
        metrics.corpus_name = args.corpus_name;
        metrics.corpus_root = fs::absolute(args.root).lexically_normal().string();
        metrics.corpus_commit = gitHeadFor(args.root);
        metrics.server_path = fs::absolute(args.server).lexically_normal().string();
        metrics.file_count = workspace_stats.file_count;
        metrics.byte_count = workspace_stats.byte_count;
        metrics.opened_file_count = 1;
        metrics.opened_byte_count = opened_source.text.size();
        metrics.opened_source_path = fs::absolute(opened_source.path).lexically_normal().string();
        metrics.top_module = args.top_module.empty()
                                 ? std::string{}
                                 : (args.mode == "real"
                                        ? selected_top
                                        : sanitizedIdentifier(args.top_module, "rtl_e2e_probe_top"));
        metrics.client_workspace_discovery_micros = client_workspace_discovery_micros;
        metrics.client_open_file_select_micros = client_open_file_select_micros;
        metrics.trace_enabled = args.trace;
        metrics.trace_path = args.trace ? fs::absolute(args.trace_file).lexically_normal().string() : std::string{};

        const auto total_start = Clock::now();
        writeStage(operation_log,
                   "start",
                   "mode=" + metrics.mode +
                       " files=" + std::to_string(metrics.file_count) +
                       " bytes=" + std::to_string(metrics.byte_count));

        writeStage(operation_log, "start-server", metrics.server_path);
        LspClient client(args.server.string(), tracer.has_value() ? &*tracer : nullptr);

        writeStage(operation_log, "initialize:begin");
        start = Clock::now();
        auto initialize_result = client.request("initialize", initializeParams(args.root));
        metrics.initialize_micros = elapsedMicros(start, Clock::now());
        writeOperation(operation_log, "initialize", metrics.initialize_micros, &initialize_result);
        writeStage(operation_log, "initialize:end", std::to_string(metrics.initialize_micros) + "us");
        client.notify("initialized", lsp::json::Object{});

        writeStage(operation_log, "didOpen:begin", opened_source.uri);
        start = Clock::now();
        lsp::json::Object open_params;
        open_params["textDocument"] = textDocumentIdentifier(opened_source.uri, opened_source.text);
        client.notify("textDocument/didOpen", std::move(open_params));
        metrics.did_open_probe_micros = elapsedMicros(start, Clock::now());
        metrics.did_open_all_micros = metrics.did_open_probe_micros;
        writeOperation(operation_log,
                       metrics.mode == "real" ? "textDocument/didOpen:real" : "textDocument/didOpen:probe",
                       metrics.did_open_probe_micros);
        writeStage(operation_log, "didOpen:end", std::to_string(metrics.did_open_probe_micros) + "us");

        const auto hierarchy_request_top = args.top_module.empty() ? std::string{} : metrics.top_module;

        writeStage(operation_log, "moduleHierarchy:cold:begin", hierarchy_request_top);
        start = Clock::now();
        auto hierarchy_cold = client.request("systemverilog/moduleHierarchy",
                                             hierarchyParams(hierarchy_request_top, args.max_depth));
        metrics.hierarchy_cold_micros = elapsedMicros(start, Clock::now());
        collectHierarchyMetrics(hierarchy_cold, metrics, false);
        writeOperation(operation_log,
                       "systemverilog/moduleHierarchy:cold",
                       metrics.hierarchy_cold_micros,
                       &hierarchy_cold);
        writeStage(operation_log, "moduleHierarchy:cold:end", std::to_string(metrics.hierarchy_cold_micros) + "us");

        writeStage(operation_log, "moduleHierarchy:warm:begin", hierarchy_request_top);
        start = Clock::now();
        auto hierarchy_warm = client.request("systemverilog/moduleHierarchy",
                                             hierarchyParams(hierarchy_request_top, args.max_depth));
        metrics.hierarchy_warm_micros = elapsedMicros(start, Clock::now());
        collectHierarchyMetrics(hierarchy_warm, metrics, true);
        writeOperation(operation_log,
                       "systemverilog/moduleHierarchy:warm",
                       metrics.hierarchy_warm_micros,
                       &hierarchy_warm);
        writeStage(operation_log, "moduleHierarchy:warm:end", std::to_string(metrics.hierarchy_warm_micros) + "us");

        const auto schematic_top = metrics.top_module;
        writeStage(operation_log, "schematic:begin", schematic_top);
        start = Clock::now();
        auto schematic = client.request("systemverilog/schematic",
                                        hierarchyParams(schematic_top, args.max_depth));
        metrics.schematic_micros = elapsedMicros(start, Clock::now());
        collectSchematicMetrics(schematic, metrics);
        writeOperation(operation_log, "systemverilog/schematic", metrics.schematic_micros, &schematic);
        writeStage(operation_log, "schematic:end", std::to_string(metrics.schematic_micros) + "us");

        writeStage(operation_log, "shutdown:begin");
        start = Clock::now();
        client.shutdown();
        metrics.shutdown_micros = elapsedMicros(start, Clock::now());
        writeOperation(operation_log, "shutdown", metrics.shutdown_micros);
        writeStage(operation_log, "shutdown:end", std::to_string(metrics.shutdown_micros) + "us");
        metrics.diagnostics_notification_count = client.diagnosticsNotificationCount();
        metrics.total_micros = elapsedMicros(total_start, Clock::now());

        const auto summary = summaryJson(metrics);
        std::ofstream summary_stream(summary_path, std::ios::binary);
        summary_stream << summary << "\n";
        std::cout << summary << "\n";
        std::cerr << "[rtl-e2e-lsp] done " << summary << std::endl;
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
