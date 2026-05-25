#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/server/ServerSession.h"
#include "pristine/transport/StdioTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::server {
namespace {

namespace fs = std::filesystem;

class ScriptedTransport final : public transport::MessageTransport {
public:
    explicit ScriptedTransport(std::initializer_list<std::string> messages) :
        inputs_(messages.begin(), messages.end()) {}

    std::optional<std::string> read() override {
        if (inputs_.empty()) {
            return std::nullopt;
        }

        std::string payload = std::move(inputs_.front());
        inputs_.pop_front();
        return payload;
    }

    void write(std::string_view payload) override { outputs_.emplace_back(payload); }

    const std::vector<std::string>& outputs() const { return outputs_; }

private:
    std::deque<std::string> inputs_;
    std::vector<std::string> outputs_;
};

jsonrpc::Json parseOutput(const ScriptedTransport& transport, size_t index) {
    return jsonrpc::Json::parse(transport.outputs().at(index));
}

std::vector<jsonrpc::Json> findNotifications(const ScriptedTransport& transport,
                                             std::string_view method) {
    std::vector<jsonrpc::Json> result;
    for (const auto& payload : transport.outputs()) {
        const auto message = jsonrpc::Json::parse(payload);
        const auto method_it = message.find("method");
        if (method_it != message.end() && method_it->is_string() &&
            method_it->get_ref<const std::string&>() == method) {
            result.push_back(message);
        }
    }
    return result;
}

std::string percentEncodePath(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':') {
            result.push_back(static_cast<char>(ch));
            continue;
        }

        result.push_back('%');
        result.push_back(hex[(ch >> 4U) & 0x0FU]);
        result.push_back(hex[ch & 0x0FU]);
    }

    return result;
}

std::string toFileUri(const fs::path& path) {
    const auto normalized = fs::weakly_canonical(path).generic_string();
    return std::string("file://") + (normalized.starts_with('/') ? "" : "/") +
           percentEncodePath(normalized);
}

class TempWorkspace {
public:
    TempWorkspace() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root_ = fs::temp_directory_path() / ("pristine-workspace-" + suffix);
        fs::create_directories(root_);
    }

    ~TempWorkspace() { std::error_code error; fs::remove_all(root_, error); }

    const fs::path& root() const { return root_; }

    fs::path writeConfig(std::string_view contents) const {
        const auto config_dir = root_ / ".slang";
        fs::create_directories(config_dir);

        const auto config_path = config_dir / "server.json";
        std::ofstream output(config_path);
        output << contents;
        output.close();
        return config_path;
    }

private:
    fs::path root_;
};

} // namespace

TEST_CASE("ServerSession handles initialize-shutdown-exit", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})",
        R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})",
        R"({"jsonrpc":"2.0","method":"exit","params":null})"};

    const int exit_code = rpc_server.run(transport);

    REQUIRE(exit_code == 0);
    REQUIRE(transport.outputs().size() == 2);

    const auto initialize_response = parseOutput(transport, 0);
    CHECK(initialize_response.at("id") == 1);
    CHECK(initialize_response.at("result").at("serverInfo").at("name") == "pristine-lsp");
        CHECK(initialize_response.at("result").at("capabilities").at("documentSymbolProvider") ==
            true);
    CHECK(initialize_response.at("result").at("capabilities").at("textDocumentSync").at(
              "openClose") == true);

    const auto shutdown_response = parseOutput(transport, 1);
    CHECK(shutdown_response.at("id") == 2);
    CHECK(shutdown_response.at("result").is_null());
}

TEST_CASE("ServerSession exits with failure when shutdown is skipped", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"exit","params":null})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 1);
    REQUIRE(transport.outputs().size() == 1);
    CHECK(parseOutput(transport, 0).at("id") == 1);
}

TEST_CASE("ServerSession tracks open change save state", "[server][sync]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/top.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/top.sv","languageId":"systemverilog","version":1,"text":"module top;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///workspace/top.sv","version":2},"contentChanges":[{"range":{"start":{"line":0,"character":7},"end":{"line":0,"character":10}},"text":"demo"}]}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":"file:///workspace/top.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 4);

    const auto* document = session.documents().find(uri);
    REQUIRE(document != nullptr);
    CHECK(document->language_id == "systemverilog");
    CHECK(document->version == 2);
    CHECK(document->text == "module demo;\nendmodule\n");
    CHECK(document->dirty == false);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics.back().at("params").at("uri").get<std::string>() == uri);
    CHECK(diagnostics.back().at("params").at("diagnostics").empty());
}

TEST_CASE("ServerSession closes tracked documents", "[server][sync]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);
    constexpr std::string_view uri = "file:///workspace/top.sv";

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/top.sv","languageId":"systemverilog","version":1,"text":"module top;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///workspace/top.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    CHECK(session.documents().size() == 0);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics.back().at("params").at("uri").get<std::string>() == uri);
}

TEST_CASE("ServerSession applies incremental UTF-16 text edits", "[server][sync]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/unicode.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/unicode.sv","languageId":"systemverilog","version":1,"text":"alpha\ud83d\ude00beta"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///workspace/unicode.sv","version":2},"contentChanges":[{"range":{"start":{"line":0,"character":7},"end":{"line":0,"character":11}},"text":"gamma"}]}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);

    const auto* document = session.documents().find(uri);
    REQUIRE(document != nullptr);
    CHECK(document->version == 2);
    CHECK(document->text == "alpha\xF0\x9F\x98\x80gamma");
    CHECK(document->dirty == true);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics.back().at("params").at("uri").get<std::string>() == uri);
}

TEST_CASE("ServerSession publishes parse diagnostics for invalid text", "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/broken.sv","languageId":"systemverilog","version":1,"text":"module broken\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    REQUIRE_FALSE(diagnostics.front().at("params").at("diagnostics").empty());
    CHECK(diagnostics.front().at("params").at("diagnostics").front().at("source").get<std::string>() ==
          "pristine-lsp");
}

TEST_CASE("ServerSession returns top-level document symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/symbols.sv","languageId":"systemverilog","version":1,"text":"package pkg; endpackage\ninterface bus; endinterface\nmodule top; endmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/symbols.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    CHECK(symbols_response.at("id") == 2);
    REQUIRE(symbols_response.at("result").size() == 3);
    CHECK(symbols_response.at("result").at(0).at("name") == "pkg");
    CHECK(symbols_response.at("result").at(0).at("kind") == 4);
    CHECK(symbols_response.at("result").at(1).at("name") == "bus");
    CHECK(symbols_response.at("result").at(1).at("kind") == 11);
    CHECK(symbols_response.at("result").at(2).at("name") == "top");
    CHECK(symbols_response.at("result").at(2).at("kind") == 2);
}

TEST_CASE("ServerSession returns nested document symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/nested-symbols.sv","languageId":"systemverilog","version":1,"text":"module top #(parameter int WIDTH = 8);\n  logic ready;\n  wire clk, rst_n;\n  typedef logic [7:0] byte_t;\n  function automatic int sum();\n  endfunction\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/nested-symbols.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 1);
    const auto& top_symbol = symbols_response.at("result").at(0);
    CHECK(top_symbol.at("name") == "top");
    REQUIRE(top_symbol.contains("children"));
    REQUIRE(top_symbol.at("children").size() == 6);
    CHECK(top_symbol.at("children").at(0).at("name") == "WIDTH");
    CHECK(top_symbol.at("children").at(1).at("name") == "ready");
    CHECK(top_symbol.at("children").at(2).at("name") == "clk");
    CHECK(top_symbol.at("children").at(3).at("name") == "rst_n");
    CHECK(top_symbol.at("children").at(4).at("name") == "byte_t");
    CHECK(top_symbol.at("children").at(5).at("name") == "sum");
}

TEST_CASE("ServerSession initializes workspace root without config", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    TempWorkspace workspace;
    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"(","workspaceFolders":null}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 1);

    const auto& state = session.workspace().state();
    REQUIRE(state.root_path.has_value());
    CHECK(fs::weakly_canonical(*state.root_path) == fs::weakly_canonical(workspace.root()));
    CHECK(state.config_loaded == false);
    CHECK_FALSE(state.config_error.has_value());
    CHECK(parseOutput(transport, 0).at("result").at("capabilities").at("workspace").at(
              "workspaceFolders").at("supported") == true);
}

TEST_CASE("ServerSession loads workspace config from .slang server json", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeConfig(R"({
        "build": "rtl/top.f",
        "buildPattern": "builds/{}.f",
        "buildRelativePaths": true,
        "flags": "-Iinclude -DDEBUG",
        "index": [
            {
                "dirs": ["rtl", "tb"],
                "excludeDirs": ["third_party"]
            }
        ]
    })");

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"workspaceFolders":[{"uri":")") +
            toFileUri(workspace.root()) + R"(","name":"test-workspace"}]}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);

    const auto& state = session.workspace().state();
    REQUIRE(state.root_path.has_value());
    REQUIRE(state.config_path.has_value());
    CHECK(state.config_loaded == true);
    CHECK_FALSE(state.config_error.has_value());
    REQUIRE(state.config.build.has_value());
    CHECK(*state.config.build == "rtl/top.f");
    REQUIRE(state.config.build_pattern.has_value());
    CHECK(*state.config.build_pattern == "builds/{}.f");
    CHECK(state.config.build_relative_paths == true);
    REQUIRE(state.config.flags.has_value());
    CHECK(*state.config.flags == "-Iinclude -DDEBUG");
    REQUIRE(state.config.index.size() == 1);
    CHECK(state.config.index[0].dirs == std::vector<std::string>{"rtl", "tb"});
    CHECK(state.config.index[0].exclude_dirs == std::vector<std::string>{"third_party"});
}

TEST_CASE("ServerSession survives invalid workspace config", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", "0.1.0"};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeConfig("{ invalid json }");

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 1);

    const auto& state = session.workspace().state();
    REQUIRE(state.root_path.has_value());
    REQUIRE(state.config_path.has_value());
    CHECK(state.config_loaded == false);
    REQUIRE(state.config_error.has_value());
    CHECK(state.config_error->find("Failed to parse workspace config") != std::string::npos);
}

} // namespace pristine::server