#include "pristine/Version.h"
#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/server/ServerSession.h"
#include "pristine/transport/StdioTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
constexpr auto kTestServerVersion = ::pristine::kVersionString;

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

    fs::path writeFile(std::string_view relative_path, std::string_view contents) const {
        const auto file_path = root_ / relative_path;
        fs::create_directories(file_path.parent_path());

        std::ofstream output(file_path);
        output << contents;
        output.close();
        return file_path;
    }

private:
    fs::path root_;
};

} // namespace

TEST_CASE("ServerSession handles initialize-shutdown-exit", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    CHECK(initialize_response.at("result").at("capabilities").at("hoverProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("definitionProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("implementationProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("documentHighlightProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("documentLinkProvider").at(
              "resolveProvider") == false);
    CHECK(initialize_response.at("result").at("capabilities").at("inlayHintProvider").at(
              "resolveProvider") == false);
    CHECK(initialize_response.at("result").at("capabilities").at("codeActionProvider").at(
              "resolveProvider") == false);
    CHECK(initialize_response.at("result").at("capabilities").at("foldingRangeProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("semanticTokensProvider").at(
              "full") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("selectionRangeProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("signatureHelpProvider").at(
              "triggerCharacters").at(0) == "(");
    CHECK(initialize_response.at("result").at("capabilities").at("callHierarchyProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("referencesProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("renameProvider").at(
              "prepareProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("workspaceSymbolProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("completionProvider").at(
              "resolveProvider") == false);
    CHECK(initialize_response.at("result").at("capabilities").at("textDocumentSync").at(
              "openClose") == true);

    const auto shutdown_response = parseOutput(transport, 1);
    CHECK(shutdown_response.at("id") == 2);
    CHECK(shutdown_response.at("result").is_null());
}

TEST_CASE("ServerSession exits with failure when shutdown is skipped", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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

TEST_CASE("ServerSession returns ansi header port symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/ansi-ports.sv","languageId":"systemverilog","version":1,"text":"module top(input logic clk, output logic [7:0] data);\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/ansi-ports.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 1);
    const auto& top_symbol = symbols_response.at("result").at(0);
    CHECK(top_symbol.at("name") == "top");
    REQUIRE(top_symbol.contains("children"));
    REQUIRE(top_symbol.at("children").size() == 2);
    CHECK(top_symbol.at("children").at(0).at("name") == "clk");
    CHECK(top_symbol.at("children").at(1).at("name") == "data");
}

TEST_CASE("ServerSession returns interface modport symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/interface-modport.sv","languageId":"systemverilog","version":1,"text":"interface bus_if(input logic clk);\n  logic ready;\n  function void sample();\n  endfunction\n  modport master(input clk, ready, import function sample);\nendinterface\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/interface-modport.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 1);
    const auto& interface_symbol = symbols_response.at("result").at(0);
    CHECK(interface_symbol.at("name") == "bus_if");
    REQUIRE(interface_symbol.contains("children"));
    REQUIRE(interface_symbol.at("children").size() == 4);
    CHECK(interface_symbol.at("children").at(0).at("name") == "clk");
    CHECK(interface_symbol.at("children").at(1).at("name") == "ready");
    CHECK(interface_symbol.at("children").at(2).at("name") == "sample");
    const auto& modport_symbol = interface_symbol.at("children").at(3);
    CHECK(modport_symbol.at("name") == "master");
    REQUIRE(modport_symbol.contains("children"));
    REQUIRE(modport_symbol.at("children").size() == 3);
    CHECK(modport_symbol.at("children").at(0).at("name") == "clk");
    CHECK(modport_symbol.at("children").at(1).at("name") == "ready");
    CHECK(modport_symbol.at("children").at(2).at("name") == "sample");
}

TEST_CASE("ServerSession returns class enum and instance symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/class-enum-instance.sv","languageId":"systemverilog","version":1,"text":"class Packet;\n  rand int len;\n  function void clear();\n  endfunction\nendclass\ntypedef enum logic [1:0] { Idle, Busy } state_t;\nmodule top;\n  child child_i();\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/class-enum-instance.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 3);
    const auto& class_symbol = symbols_response.at("result").at(0);
    CHECK(class_symbol.at("name") == "Packet");
    REQUIRE(class_symbol.contains("children"));
    REQUIRE(class_symbol.at("children").size() == 2);
    CHECK(class_symbol.at("children").at(0).at("name") == "len");
    CHECK(class_symbol.at("children").at(1).at("name") == "clear");
    const auto& enum_symbol = symbols_response.at("result").at(1);
    CHECK(enum_symbol.at("name") == "state_t");
    REQUIRE(enum_symbol.contains("children"));
    REQUIRE(enum_symbol.at("children").size() == 2);
    CHECK(enum_symbol.at("children").at(0).at("name") == "Idle");
    CHECK(enum_symbol.at("children").at(1).at("name") == "Busy");
    const auto& module_symbol = symbols_response.at("result").at(2);
    CHECK(module_symbol.at("name") == "top");
    REQUIRE(module_symbol.contains("children"));
    REQUIRE(module_symbol.at("children").size() == 1);
    CHECK(module_symbol.at("children").at(0).at("name") == "child_i");
}

TEST_CASE("ServerSession returns named generate block symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/generate-blocks.sv","languageId":"systemverilog","version":1,"text":"module top;\n  generate\n    begin : gen_blk\n      logic enabled;\n    end\n  endgenerate\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/generate-blocks.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 1);
    const auto& module_symbol = symbols_response.at("result").at(0);
    CHECK(module_symbol.at("name") == "top");
    REQUIRE(module_symbol.contains("children"));
    REQUIRE(module_symbol.at("children").size() == 1);
    const auto& block_symbol = module_symbol.at("children").at(0);
    CHECK(block_symbol.at("name") == "gen_blk");
    CHECK(block_symbol.at("kind") == 3);
    REQUIRE(block_symbol.contains("children"));
    REQUIRE(block_symbol.at("children").size() == 1);
    CHECK(block_symbol.at("children").at(0).at("name") == "enabled");
}

TEST_CASE("ServerSession returns if and loop generate symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/generate-branches.sv","languageId":"systemverilog","version":1,"text":"module top;\n  generate\n    if (1) begin : has_feature\n      logic enabled;\n    end else begin : no_feature\n      logic disabled;\n    end\n    for (genvar i = 0; i < 2; i++) begin : lane\n      logic ready;\n    end\n  endgenerate\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/generate-branches.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto symbols_response = parseOutput(transport, 2);
    REQUIRE(symbols_response.at("result").size() == 1);
    const auto& module_symbol = symbols_response.at("result").at(0);
    CHECK(module_symbol.at("name") == "top");
    REQUIRE(module_symbol.contains("children"));
    REQUIRE(module_symbol.at("children").size() == 3);
    CHECK(module_symbol.at("children").at(0).at("name") == "has_feature");
    CHECK(module_symbol.at("children").at(1).at("name") == "no_feature");
    CHECK(module_symbol.at("children").at(2).at("name") == "lane");
    REQUIRE(module_symbol.at("children").at(0).at("children").size() == 1);
    CHECK(module_symbol.at("children").at(0).at("children").at(0).at("name") == "enabled");
    REQUIRE(module_symbol.at("children").at(1).at("children").size() == 1);
    CHECK(module_symbol.at("children").at(1).at("children").at(0).at("name") == "disabled");
    REQUIRE(module_symbol.at("children").at(2).at("children").size() == 1);
    CHECK(module_symbol.at("children").at(2).at("children").at(0).at("name") == "ready");
}

TEST_CASE("ServerSession returns hover for declaration symbols", "[server][hover]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/hover.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///workspace/hover.sv"},"position":{"line":1,"character":8}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto hover_response = parseOutput(transport, 2);
    CHECK(hover_response.at("id") == 2);
    CHECK(hover_response.at("result").at("contents").at("kind") == "markdown");
    CHECK(hover_response.at("result").at("contents").at("value") == "**Variable** `ready`");
    CHECK(hover_response.at("result").at("range").at("start").at("line") == 1);
    CHECK(hover_response.at("result").at("range").at("start").at("character") == 8);
    CHECK(hover_response.at("result").at("range").at("end").at("character") == 13);
}

TEST_CASE("ServerSession handles Tier 1 LSP navigation and completion", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto child_path = workspace.writeFile("rtl/child.sv", "module child; endmodule\n");
    const auto top_path = workspace.writeFile(
        "rtl/top.sv",
        "module top;\n"
        "  child child_i();\n"
        "  logic ready;\n"
        "  assign ready = ready;\n"
        "endmodule\n");
    const auto child_uri = toFileUri(child_path);
    const auto top_uri = toFileUri(top_path);

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            top_uri +
            R"(","languageId":"systemverilog","version":1,"text":"module top;\n  child child_i();\n  logic ready;\n  assign ready = ready;\nendmodule\n"}}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":1,"character":3}}})",
        std::string(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/references","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":2,"character":9},"context":{"includeDeclaration":false}}})",
        R"({"jsonrpc":"2.0","id":4,"method":"workspace/symbol","params":{"query":"ch"}})",
        std::string(R"({"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":1,"character":4},"context":{"triggerKind":1}}})",
        std::string(R"({"jsonrpc":"2.0","id":6,"method":"textDocument/implementation","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":1,"character":3}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 7);

    const auto definition_response = parseOutput(transport, 2);
    CHECK(definition_response.at("id") == 2);
    REQUIRE(definition_response.at("result").size() == 1);
    CHECK(definition_response.at("result").at(0).at("uri") == child_uri);
    CHECK(definition_response.at("result").at(0).at("range").at("start").at("line") == 0);
    CHECK(definition_response.at("result").at(0).at("range").at("start").at("character") == 7);

    const auto references_response = parseOutput(transport, 3);
    CHECK(references_response.at("id") == 3);
    REQUIRE(references_response.at("result").size() == 2);
    CHECK(references_response.at("result").at(0).at("range").at("start").at("line") == 3);
    CHECK(references_response.at("result").at(1).at("range").at("start").at("line") == 3);

    const auto workspace_symbol_response = parseOutput(transport, 4);
    CHECK(workspace_symbol_response.at("id") == 4);
    const auto& workspace_symbols = workspace_symbol_response.at("result");
    CHECK(std::any_of(workspace_symbols.begin(), workspace_symbols.end(), [&](const jsonrpc::Json& symbol) {
        return symbol.at("name") == "child" && symbol.at("location").at("uri") == child_uri;
    }));

    const auto completion_response = parseOutput(transport, 5);
    CHECK(completion_response.at("id") == 5);
    const auto& completions = completion_response.at("result");
    CHECK(std::any_of(completions.begin(), completions.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "child";
    }));
    CHECK(std::any_of(completions.begin(), completions.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "child_i";
    }));

    const auto implementation_response = parseOutput(transport, 6);
    CHECK(implementation_response.at("id") == 6);
    REQUIRE(implementation_response.at("result").size() == 1);
    CHECK(implementation_response.at("result").at(0).at("uri") == top_uri);
    CHECK(implementation_response.at("result").at(0).at("range").at("start").at("line") == 1);
    CHECK(implementation_response.at("result").at(0).at("range").at("start").at("character") == 2);
    CHECK(implementation_response.at("result").at(0).at("range").at("end").at("character") == 7);
}

TEST_CASE("ServerSession returns implementations from module definitions", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto child_text = std::string("module child; endmodule\n");
    const auto top_text = std::string(
        "module top;\n"
        "  child child_i();\n"
        "endmodule\n");
    const auto child_path = workspace.writeFile("rtl/child.sv", child_text);
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto child_uri = toFileUri(child_path);
    const auto top_uri = toFileUri(top_path);

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            child_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
            jsonrpc::Json(child_text).dump() + R"(}}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            top_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
            jsonrpc::Json(top_text).dump() + R"(}}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/implementation","params":{"textDocument":{"uri":")") +
            child_uri + R"("},"position":{"line":0,"character":8}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    std::optional<jsonrpc::Json> implementation_response;
    for (const auto& payload : transport.outputs()) {
        const auto message = jsonrpc::Json::parse(payload);
        const auto id_it = message.find("id");
        if (id_it != message.end() && *id_it == 2) {
            implementation_response = message;
            break;
        }
    }

    REQUIRE(implementation_response.has_value());
    REQUIRE(implementation_response->at("result").size() == 1);
    CHECK(implementation_response->at("result").at(0).at("uri") == top_uri);
    CHECK(implementation_response->at("result").at(0).at("range").at("start").at("line") == 1);
    CHECK(implementation_response->at("result").at(0).at("range").at("start").at("character") == 2);
    CHECK(implementation_response->at("result").at(0).at("range").at("end").at("character") == 7);
}

TEST_CASE("ServerSession handles Tier 2 rename highlight and document links", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile("rtl/child.sv", "module child(input logic clk, output logic rst_n); endmodule\n");
    const auto include_path = workspace.writeFile("rtl/defs.svh", "`define FEATURE 1\n");
    const auto top_text = std::string(
        "`include \"defs.svh\"\n"
        "`include \"missing.svh\"\n"
        "module top;\n"
        "  child child_i(.clk(), .rst_n());\n"
        "  logic ready;\n"
        "  assign ready = ready;\n"
        "endmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto include_uri = toFileUri(include_path);
    const auto missing_uri = toFileUri(workspace.root() / "rtl" / "missing.svh");
    const auto top_uri = toFileUri(top_path);
    const auto top_text_json = jsonrpc::Json(top_text).dump();

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            top_uri + R"(","languageId":"systemverilog","version":1,"text":)" + top_text_json + R"(}}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentHighlight","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":4,"character":9}}})",
        std::string(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/rename","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":4,"character":9},"newName":"valid"}})",
        std::string(R"({"jsonrpc":"2.0","id":4,"method":"textDocument/rename","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":4,"character":9},"newName":"not valid"}})",
        std::string(R"({"jsonrpc":"2.0","id":5,"method":"textDocument/documentLink","params":{"textDocument":{"uri":")") +
            top_uri + R"("}}})",
        std::string(R"({"jsonrpc":"2.0","id":6,"method":"textDocument/inlayHint","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"range":{"start":{"line":0,"character":0},"end":{"line":7,"character":0}}}})",
        std::string(R"({"jsonrpc":"2.0","id":7,"method":"textDocument/codeAction","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"range":{"start":{"line":1,"character":10},"end":{"line":1,"character":21}},"context":{"diagnostics":[]}}})",
        std::string(R"({"jsonrpc":"2.0","id":8,"method":"textDocument/foldingRange","params":{"textDocument":{"uri":")") +
            top_uri + R"("}}})",
        std::string(R"({"jsonrpc":"2.0","id":9,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":")") +
            top_uri + R"("}}})",
        std::string(R"({"jsonrpc":"2.0","id":10,"method":"textDocument/selectionRange","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"positions":[{"line":4,"character":9}]}})",
        std::string(R"({"jsonrpc":"2.0","id":11,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":3,"character":25}}})",
        std::string(R"({"jsonrpc":"2.0","id":12,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":4,"character":9}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 13);

    const auto highlight_response = parseOutput(transport, 2);
    CHECK(highlight_response.at("id") == 2);
    REQUIRE(highlight_response.at("result").size() == 3);
    CHECK(highlight_response.at("result").at(0).at("range").at("start").at("line") == 4);
    CHECK(highlight_response.at("result").at(1).at("range").at("start").at("line") == 5);
    CHECK(highlight_response.at("result").at(2).at("kind") == 1);

    const auto rename_response = parseOutput(transport, 3);
    CHECK(rename_response.at("id") == 3);
    const auto& edits = rename_response.at("result").at("changes").at(top_uri);
    REQUIRE(edits.size() == 3);
    CHECK(edits.at(0).at("newText") == "valid");
    CHECK(edits.at(0).at("range").at("start").at("line") == 4);
    CHECK(edits.at(1).at("range").at("start").at("line") == 5);

    const auto invalid_rename_response = parseOutput(transport, 4);
    CHECK(invalid_rename_response.at("id") == 4);
    CHECK(invalid_rename_response.at("result").is_null());

    const auto document_link_response = parseOutput(transport, 5);
    CHECK(document_link_response.at("id") == 5);
    REQUIRE(document_link_response.at("result").size() == 1);
    CHECK(document_link_response.at("result").at(0).at("target") == include_uri);
    CHECK(document_link_response.at("result").at(0).at("range").at("start").at("line") == 0);
    CHECK(document_link_response.at("result").at(0).at("range").at("start").at("character") == 10);

    const auto inlay_hint_response = parseOutput(transport, 6);
    CHECK(inlay_hint_response.at("id") == 6);
    REQUIRE(inlay_hint_response.at("result").size() == 1);
    CHECK(inlay_hint_response.at("result").at(0).at("label") == ": child");
    CHECK(inlay_hint_response.at("result").at(0).at("kind") == 1);
    CHECK(inlay_hint_response.at("result").at(0).at("position").at("line") == 3);
    CHECK(inlay_hint_response.at("result").at(0).at("position").at("character") == 15);

    const auto code_action_response = parseOutput(transport, 7);
    CHECK(code_action_response.at("id") == 7);
    REQUIRE(code_action_response.at("result").size() == 1);
    const auto& code_action = code_action_response.at("result").at(0);
    CHECK(code_action.at("title") == "Create include file 'missing.svh'");
    CHECK(code_action.at("kind") == "quickfix");
    CHECK(code_action.at("edit").at("documentChanges").at(0).at("kind") == "create");
    CHECK(code_action.at("edit").at("documentChanges").at(0).at("uri") == missing_uri);

    const auto folding_response = parseOutput(transport, 8);
    CHECK(folding_response.at("id") == 8);
    REQUIRE_FALSE(folding_response.at("result").empty());
    CHECK(folding_response.at("result").at(0).at("startLine") == 2);
    CHECK(folding_response.at("result").at(0).at("endLine") == 6);

    const auto semantic_tokens_response = parseOutput(transport, 9);
    CHECK(semantic_tokens_response.at("id") == 9);
    const auto& semantic_data = semantic_tokens_response.at("result").at("data");
    REQUIRE_FALSE(semantic_data.empty());
    CHECK(semantic_data.size() % 5 == 0);

    const auto selection_range_response = parseOutput(transport, 10);
    CHECK(selection_range_response.at("id") == 10);
    REQUIRE(selection_range_response.at("result").size() == 1);
    const auto& selection_range = selection_range_response.at("result").at(0);
    CHECK(selection_range.at("range").at("start").at("line") == 4);
    CHECK(selection_range.at("range").at("start").at("character") == 8);
    CHECK(selection_range.at("parent").at("range").at("start").at("character") == 2);
    CHECK(selection_range.at("parent").at("parent").at("range").at("start").at("line") == 2);

    const auto signature_help_response = parseOutput(transport, 11);
    CHECK(signature_help_response.at("id") == 11);
    const auto& signature_help = signature_help_response.at("result");
    REQUIRE(signature_help.at("signatures").size() == 1);
    CHECK(signature_help.at("signatures").at(0).at("label") == "child(clk, rst_n)");
    REQUIRE(signature_help.at("signatures").at(0).at("parameters").size() == 2);
    CHECK(signature_help.at("signatures").at(0).at("parameters").at(0).at("label") == "clk");
    CHECK(signature_help.at("signatures").at(0).at("parameters").at(1).at("label") == "rst_n");
    CHECK(signature_help.at("activeParameter") == 1);

    const auto prepare_rename_response = parseOutput(transport, 12);
    CHECK(prepare_rename_response.at("id") == 12);
    CHECK(prepare_rename_response.at("result").at("placeholder") == "ready");
    CHECK(prepare_rename_response.at("result").at("range").at("start").at("line") == 4);
    CHECK(prepare_rename_response.at("result").at("range").at("start").at("character") == 8);
    CHECK(prepare_rename_response.at("result").at("range").at("end").at("character") == 13);
}

TEST_CASE("ServerSession returns inferred SystemVerilog module hierarchy", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto leaf_path = workspace.writeFile("rtl/leaf.sv", "module leaf; endmodule\n");
    const auto child_path = workspace.writeFile(
        "rtl/child.sv",
        "module child;\n"
        "  leaf u_leaf();\n"
        "endmodule\n");
    const auto top_path = workspace.writeFile(
        "rtl/top.sv",
        "module top;\n"
        "  child u_child();\n"
        "endmodule\n");

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/moduleHierarchy","params":{}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 2);

    const auto hierarchy_response = parseOutput(transport, 1);
    CHECK(hierarchy_response.at("id") == 2);
    const auto& result = hierarchy_response.at("result");
    REQUIRE(result.at("messages").empty());
    REQUIRE(result.at("roots").size() == 1);

    const auto& top = result.at("roots").at(0);
    CHECK(top.at("moduleName") == "top");
    CHECK(top.at("uri") == toFileUri(top_path));
    REQUIRE(top.at("children").size() == 1);

    const auto& child = top.at("children").at(0);
    CHECK(child.at("moduleName") == "child");
    CHECK(child.at("instanceName") == "u_child");
    CHECK(child.at("uri") == toFileUri(child_path));
    CHECK(child.at("instanceSelectionRange").at("start").at("line") == 1);
    REQUIRE(child.at("children").size() == 1);

    const auto& leaf = child.at("children").at(0);
    CHECK(leaf.at("moduleName") == "leaf");
    CHECK(leaf.at("instanceName") == "u_leaf");
    CHECK(leaf.at("uri") == toFileUri(leaf_path));
    CHECK(leaf.at("children").empty());
}

TEST_CASE("ServerSession handles standard call hierarchy", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile("rtl/leaf.sv", "module leaf; endmodule\n");
    const auto child_path = workspace.writeFile(
        "rtl/child.sv",
        "module child;\n"
        "  leaf u_leaf();\n"
        "endmodule\n");
    const auto top_path = workspace.writeFile(
        "rtl/top.sv",
        "module top;\n"
        "  child u_child();\n"
        "endmodule\n");
    const auto child_uri = toFileUri(child_path);
    const auto top_uri = toFileUri(top_path);

    const auto top_item = std::string(
        R"({"name":"top","kind":2,"uri":")") + top_uri +
        R"(","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"selectionRange":{"start":{"line":0,"character":7},"end":{"line":0,"character":10}}})";
    const auto child_item = std::string(
        R"({"name":"child","kind":2,"uri":")") + child_uri +
        R"(","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"selectionRange":{"start":{"line":0,"character":7},"end":{"line":0,"character":12}}})";

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":0,"character":8}}})",
        std::string(R"({"jsonrpc":"2.0","id":3,"method":"callHierarchy/outgoingCalls","params":{"item":)" +
            top_item + R"(}})"),
        std::string(R"({"jsonrpc":"2.0","id":4,"method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":1,"character":3}}})",
        std::string(R"({"jsonrpc":"2.0","id":5,"method":"callHierarchy/incomingCalls","params":{"item":)" +
            child_item + R"(}})")};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 5);

    const auto prepare_top_response = parseOutput(transport, 1);
    CHECK(prepare_top_response.at("id") == 2);
    REQUIRE(prepare_top_response.at("result").size() == 1);
    CHECK(prepare_top_response.at("result").at(0).at("name") == "top");
    CHECK(prepare_top_response.at("result").at(0).at("uri") == top_uri);

    const auto outgoing_response = parseOutput(transport, 2);
    CHECK(outgoing_response.at("id") == 3);
    REQUIRE(outgoing_response.at("result").size() == 1);
    CHECK(outgoing_response.at("result").at(0).at("to").at("name") == "child");
    CHECK(outgoing_response.at("result").at(0).at("to").at("uri") == child_uri);
    CHECK(outgoing_response.at("result").at(0).at("fromRanges").at(0).at("start").at("line") == 1);
    CHECK(outgoing_response.at("result").at(0).at("fromRanges").at(0).at("start").at("character") == 2);

    const auto prepare_child_response = parseOutput(transport, 3);
    CHECK(prepare_child_response.at("id") == 4);
    REQUIRE(prepare_child_response.at("result").size() == 1);
    CHECK(prepare_child_response.at("result").at(0).at("name") == "child");
    CHECK(prepare_child_response.at("result").at(0).at("uri") == child_uri);

    const auto incoming_response = parseOutput(transport, 4);
    CHECK(incoming_response.at("id") == 5);
    REQUIRE(incoming_response.at("result").size() == 1);
    CHECK(incoming_response.at("result").at(0).at("from").at("name") == "top");
    CHECK(incoming_response.at("result").at(0).at("from").at("uri") == top_uri);
    CHECK(incoming_response.at("result").at(0).at("fromRanges").at(0).at("start").at("line") == 1);
}

TEST_CASE("ServerSession marks unresolved and cyclic module hierarchy entries", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile(
        "rtl/top.sv",
        "module top;\n"
        "  top u_self();\n"
        "  missing u_missing();\n"
        "endmodule\n");

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top"}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 2);

    const auto hierarchy_response = parseOutput(transport, 1);
    const auto& root = hierarchy_response.at("result").at("roots").at(0);
    REQUIRE(root.at("children").size() == 2);
    CHECK(root.at("children").at(0).at("cycle") == true);
    CHECK(root.at("children").at(0).at("moduleName") == "top");
    CHECK(root.at("children").at(1).at("unresolved") == true);
    CHECK(root.at("children").at(1).at("moduleName") == "missing");
}

TEST_CASE("ServerSession initializes workspace root without config", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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
    ServerSession session{"pristine-lsp", kTestServerVersion};
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