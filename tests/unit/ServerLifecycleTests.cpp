#include "pristine/Version.h"
#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/server/ServerSession.h"
#include "pristine/transport/StdioTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pristine::server {
namespace {

namespace fs = std::filesystem;
const auto kTestServerVersion = ::pristine::kVersionString;

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

    void write(std::string_view payload) override {
        std::lock_guard lock(outputs_mutex_);
        outputs_.emplace_back(payload);
    }

    std::vector<std::string> outputs() const {
        std::lock_guard lock(outputs_mutex_);
        return outputs_;
    }

private:
    std::deque<std::string> inputs_;
    mutable std::mutex outputs_mutex_;
    std::vector<std::string> outputs_;
};

class WaitingTransport final : public transport::MessageTransport {
public:
    WaitingTransport(std::initializer_list<std::string> messages,
                     size_t minimum_outputs,
                     std::chrono::milliseconds timeout = std::chrono::seconds(1)) :
        inputs_(messages.begin(), messages.end()), minimum_outputs_(minimum_outputs), timeout_(timeout) {}

    std::optional<std::string> read() override {
        if (!inputs_.empty()) {
            std::string payload = std::move(inputs_.front());
            inputs_.pop_front();
            return payload;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(outputs_mutex_);
                if (outputs_.size() >= minimum_outputs_) {
                    return std::nullopt;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    void write(std::string_view payload) override {
        std::lock_guard lock(outputs_mutex_);
        outputs_.emplace_back(payload);
    }

    std::vector<std::string> outputs() const {
        std::lock_guard lock(outputs_mutex_);
        return outputs_;
    }

private:
    std::deque<std::string> inputs_;
    size_t minimum_outputs_ = 0;
    std::chrono::milliseconds timeout_ = std::chrono::seconds(1);
    mutable std::mutex outputs_mutex_;
    std::vector<std::string> outputs_;
};

class ActiveCancellationTransport final : public transport::MessageTransport {
public:
    explicit ActiveCancellationTransport(std::atomic_bool& handler_started) :
        handler_started_(handler_started) {}

    std::optional<std::string> read() override {
        switch (read_index_++) {
        case 0:
            return R"({"jsonrpc":"2.0","id":"slow","method":"test/slow","params":{}})";
        case 1:
            while (!handler_started_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":"slow"}})";
        default:
            return std::nullopt;
        }
    }

    void write(std::string_view payload) override {
        std::lock_guard lock(mutex_);
        outputs_.emplace_back(payload);
    }

    std::vector<std::string> outputs() const {
        std::lock_guard lock(mutex_);
        return outputs_;
    }

private:
    std::atomic_bool& handler_started_;
    size_t read_index_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> outputs_;
};

class QueuedCancellationTransport final : public transport::MessageTransport {
public:
    explicit QueuedCancellationTransport(std::atomic_bool& release_first) :
        release_first_(release_first) {}

    std::optional<std::string> read() override {
        switch (read_index_++) {
        case 0:
            return R"({"jsonrpc":"2.0","id":1,"method":"test/block","params":{}})";
        case 1:
            return R"({"jsonrpc":"2.0","id":2,"method":"test/queued","params":{}})";
        case 2:
            return R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":2}})";
        default:
            release_first_.store(true, std::memory_order_release);
            return std::nullopt;
        }
    }

    void write(std::string_view payload) override {
        std::lock_guard lock(mutex_);
        outputs_.emplace_back(payload);
    }

    std::vector<std::string> outputs() const {
        std::lock_guard lock(mutex_);
        return outputs_;
    }

private:
    std::atomic_bool& release_first_;
    size_t read_index_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> outputs_;
};

jsonrpc::Json parseOutput(const ScriptedTransport& transport, size_t index) {
    return jsonrpc::Json::parse(transport.outputs().at(index));
}

jsonrpc::Json parseOutput(const WaitingTransport& transport, size_t index) {
    return jsonrpc::Json::parse(transport.outputs().at(index));
}

std::optional<jsonrpc::Json> findResponse(const ScriptedTransport& transport, int id) {
    for (const auto& payload : transport.outputs()) {
        const auto message = jsonrpc::Json::parse(payload);
        const auto id_it = message.find("id");
        if (id_it != message.end() && id_it->is_number_integer() && id_it->get<int>() == id) {
            return message;
        }
    }
    return std::nullopt;
}

std::optional<jsonrpc::Json> findResponse(const WaitingTransport& transport, int id) {
    for (const auto& payload : transport.outputs()) {
        const auto message = jsonrpc::Json::parse(payload);
        const auto id_it = message.find("id");
        if (id_it != message.end() && id_it->is_number_integer() && id_it->get<int>() == id) {
            return message;
        }
    }
    return std::nullopt;
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

TEST_CASE("JsonRpcServer cancels an active contextual request", "[jsonrpc][cancellation]") {
    jsonrpc::JsonRpcServer server;
    std::atomic_bool handler_started = false;
    server.registerRequestHandler(
        "test/slow",
        [&](const jsonrpc::Json&, const jsonrpc::RequestContext& context) -> jsonrpc::Json {
            handler_started.store(true, std::memory_order_release);
            while (!context.cancellation.cancellationRequested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            context.cancellation.throwIfCancellationRequested();
            return nullptr;
        });
    ActiveCancellationTransport transport(handler_started);

    CHECK(server.run(transport) == 0);
    REQUIRE(transport.outputs().size() == 1);
    const auto response = jsonrpc::Json::parse(transport.outputs().front());
    CHECK(response.at("id") == "slow");
    CHECK(response.at("error").at("code") == -32800);
}

TEST_CASE("JsonRpcServer cancels a queued numeric request without invoking it",
          "[jsonrpc][cancellation]") {
    jsonrpc::JsonRpcServer server;
    std::atomic_bool release_first = false;
    std::atomic_int queued_invocations = 0;
    server.registerRequestHandler("test/block", [&](const jsonrpc::Json&) -> jsonrpc::Json {
        while (!release_first.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return "done";
    });
    server.registerRequestHandler(
        "test/queued",
        [&](const jsonrpc::Json&, const jsonrpc::RequestContext&) -> jsonrpc::Json {
            ++queued_invocations;
            return "unexpected";
        });
    QueuedCancellationTransport transport(release_first);

    CHECK(server.run(transport) == 0);
    CHECK(queued_invocations.load() == 0);
    REQUIRE(transport.outputs().size() == 2);
    const auto second = jsonrpc::Json::parse(transport.outputs().at(1));
    CHECK(second.at("id") == 2);
    CHECK(second.at("error").at("code") == -32800);
}

TEST_CASE("JsonRpcServer ignores cancellation for an unknown request id",
          "[jsonrpc][cancellation]") {
    jsonrpc::JsonRpcServer server;
    server.registerRequestHandler("test/value", [](const jsonrpc::Json&) {
        return jsonrpc::Json("ok");
    });
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":"missing"}})",
        R"({"jsonrpc":"2.0","id":"value","method":"test/value","params":{}})"};

    CHECK(server.run(transport) == 0);
    REQUIRE(transport.outputs().size() == 1);
    const auto response = jsonrpc::Json::parse(transport.outputs().front());
    CHECK(response.at("result") == "ok");
}

TEST_CASE("JsonRpcServer reports progress with the client supplied token",
          "[jsonrpc][progress]") {
    jsonrpc::JsonRpcServer server;
    server.registerRequestHandler(
        "test/progress",
        [](const jsonrpc::Json&, const jsonrpc::RequestContext& context) -> jsonrpc::Json {
            REQUIRE(context.work_done_token.has_value());
            context.report_progress(jsonrpc::Json{{"kind", "begin"}, {"title", "Semantic build"}});
            context.report_progress(jsonrpc::Json{{"kind", "end"}});
            return "done";
        });
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"test/progress","params":{"workDoneToken":"build-1"}})"};

    CHECK(server.run(transport) == 0);
    REQUIRE(transport.outputs().size() == 3);
    const auto begin = jsonrpc::Json::parse(transport.outputs().at(0));
    const auto end = jsonrpc::Json::parse(transport.outputs().at(1));
    const auto response = jsonrpc::Json::parse(transport.outputs().at(2));
    CHECK(begin.at("method") == "$/progress");
    CHECK(begin.at("params").at("token") == "build-1");
    CHECK(begin.at("params").at("value").at("kind") == "begin");
    CHECK(end.at("params").at("value").at("kind") == "end");
    CHECK(response.at("result") == "done");
}

TEST_CASE("ServerSession handles initialize-shutdown-exit", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    CHECK(initialize_response.at("result").at("serverInfo").at("name") == "pristine-engine");
    CHECK(initialize_response.at("result").at("capabilities").at("documentSymbolProvider") ==
          true);
    CHECK(initialize_response.at("result").at("capabilities").at("hoverProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("definitionProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("typeDefinitionProvider") == true);
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
              "resolveProvider") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineWaveformProvider").at("transport") == "pipe");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineWaveformProvider").at("protocol") == "pristine-waveform-columnar-v1");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineWaveformProvider").at("mock") == true);
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineWaveformProvider").at("sources").at(0) == "mock");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineWaveformProvider").at("sources").at(1) == "fst");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("transport") == "pipe");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("protocol") == "pristine-layout-columnar-v3");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("sources").at(0) == "lefdef");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("sources").at(1) == "gds");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("protocols").at(0) == "pristine-layout-columnar-v3");
    CHECK(initialize_response.at("result").at("capabilities").at("experimental").at(
              "pristineLayoutProvider").at("protocols").size() == 1);
    CHECK(initialize_response.at("result").at("capabilities").at("textDocumentSync").at(
              "openClose") == true);

    const auto shutdown_response = parseOutput(transport, 1);
    CHECK(shutdown_response.at("id") == 2);
    CHECK(shutdown_response.at("result").is_null());
}

TEST_CASE("ServerSession opens and closes waveform pipe sessions", "[server][waveform]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/waveform/open","params":{"source":"mock"}})",
        R"({"jsonrpc":"2.0","id":3,"method":"systemverilog/waveform/close","params":{"sessionId":"1"}})",
        R"({"jsonrpc":"2.0","id":4,"method":"shutdown","params":null})",
        R"({"jsonrpc":"2.0","method":"exit","params":null})"};

    const int exit_code = rpc_server.run(transport);

    REQUIRE(exit_code == 0);
    REQUIRE(transport.outputs().size() == 4);

    const auto open_response = parseOutput(transport, 1);
    CHECK(open_response.at("id") == 2);
    const auto& result = open_response.at("result");
    CHECK(result.at("sessionId") == "1");
    CHECK(result.at("protocol") == "pristine-waveform-columnar-v1");
    CHECK(result.at("title") == "counter_tb");
    CHECK(result.at("duration") == 200.0);
    CHECK(result.at("timescaleUnit") == "ns");
    CHECK(result.at("groupCount") == 3);
    CHECK(result.at("signalCount") == 168);
    CHECK(result.at("source") == "mock");
    CHECK(result.at("endpoint").at("path").get<std::string>().find("pristine-engine-waveform") !=
          std::string::npos);

    const auto close_response = parseOutput(transport, 2);
    CHECK(close_response.at("id") == 3);
    CHECK(close_response.at("result").at("closed") == true);
}

TEST_CASE("ServerSession opens and closes layout pipe sessions", "[server][layout]") {
    TempWorkspace workspace;
    const auto lef = workspace.writeFile("stdcells.lef",
                                         R"(VERSION 5.8 ;
UNITS DATABASE MICRONS 1000 ;
LAYER M1 TYPE ROUTING ; END M1
MACRO invx1
  SIZE 1 BY 2 ;
  PIN A
    DIRECTION INPUT ;
    PORT LAYER M1 ; RECT 0 0 1 1 ; END
  END A
END invx1
END LIBRARY
)");
    const auto def = workspace.writeFile("top.def",
                                         R"(VERSION 5.8 ;
DESIGN top ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 1000 1000 ) ;
COMPONENTS 1 ;
  - U1 invx1 + PLACED ( 10 20 ) N ;
END COMPONENTS
END DESIGN
)");

    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    const auto root_uri = toFileUri(workspace.root());
    const auto lef_uri = toFileUri(lef);
    const auto def_uri = toFileUri(def);
    ScriptedTransport transport{
        jsonrpc::Json{{"jsonrpc", "2.0"},
                      {"id", 1},
                      {"method", "initialize"},
                      {"params",
                       jsonrpc::Json{{"rootUri", root_uri},
                                     {"workspaceFolders",
                                      jsonrpc::Json::array(
                                          {jsonrpc::Json{{"uri", root_uri}, {"name", "layout"}}})}}}}
            .dump(),
        jsonrpc::Json{{"jsonrpc", "2.0"},
                      {"id", 2},
                      {"method", "systemverilog/layout/open"},
                      {"params",
                       jsonrpc::Json{{"lefUris", jsonrpc::Json::array({lef_uri})},
                                     {"defUri", def_uri},
                                     {"title", "tiny-layout"}}}}
            .dump(),
        R"({"jsonrpc":"2.0","id":3,"method":"systemverilog/layout/close","params":{"sessionId":"1"}})",
        R"({"jsonrpc":"2.0","id":4,"method":"shutdown","params":null})",
        R"({"jsonrpc":"2.0","method":"exit","params":null})"};

    const int exit_code = rpc_server.run(transport);

    REQUIRE(exit_code == 0);
    REQUIRE(transport.outputs().size() == 4);

    const auto open_response = parseOutput(transport, 1);
    CHECK(open_response.at("id") == 2);
    const auto& result = open_response.at("result");
    CHECK(result.at("sessionId") == "1");
    CHECK(result.at("protocol") == "pristine-layout-columnar-v3");
    CHECK(result.at("source") == "lefdef");
    CHECK(result.at("status") == "ready");
    CHECK(result.at("deferred") == false);
    CHECK(result.at("title") == "tiny-layout");
    CHECK(result.at("lefCount") == 1);
    CHECK(result.at("defPresent") == true);
    CHECK(result.at("unitsPerMicron") == 1000);
    CHECK(result.at("layerCount") == 1);
    CHECK(result.at("macroCount") == 1);
    CHECK(result.at("componentCount") == 1);
    CHECK(result.at("netCount") == 0);
    CHECK(result.at("cellCount") == 0);
    CHECK(result.at("referenceCount") == 0);
    CHECK(result.at("elementCount") == 0);
    CHECK(result.at("diagnosticCount").get<std::size_t>() >= 0);
    CHECK(result.at("fileUris").size() == 2);
    CHECK(result.at("endpoint").at("path").get<std::string>().find("pristine-engine-layout") !=
          std::string::npos);

    const auto close_response = parseOutput(transport, 2);
    CHECK(close_response.at("id") == 3);
    CHECK(close_response.at("result").at("closed") == true);
}

TEST_CASE("ServerSession exits with failure when shutdown is skipped", "[server][lifecycle]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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

TEST_CASE("ServerSession refreshes indexes from watched file changes", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile("rtl/existing.sv", "module existing; endmodule\n");

    ScriptedTransport initialize_transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})"};

    CHECK(rpc_server.run(initialize_transport) == 0);
    REQUIRE(findResponse(initialize_transport, 1).has_value());

    const auto watched_path = workspace.writeFile("rtl/watched.sv", "module watched_new; endmodule\n");
    const auto watched_uri = toFileUri(watched_path);
    ScriptedTransport created_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            watched_uri + R"(","type":1}]}})" ,
        R"({"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"watched_new"}})"};

    CHECK(rpc_server.run(created_transport) == 0);
    const auto created_response = findResponse(created_transport, 2);
    REQUIRE(created_response.has_value());
    const auto& created_symbols = created_response->at("result");
    CHECK(std::any_of(created_symbols.begin(), created_symbols.end(), [&](const jsonrpc::Json& symbol) {
        return symbol.at("name") == "watched_new" && symbol.at("location").at("uri") == watched_uri;
    }));

    workspace.writeFile("rtl/watched.sv", "module watched_changed; endmodule\n");
    ScriptedTransport changed_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            watched_uri + R"(","type":2}]}})" ,
        R"({"jsonrpc":"2.0","id":3,"method":"workspace/symbol","params":{"query":"watched"}})"};

    CHECK(rpc_server.run(changed_transport) == 0);
    const auto changed_response = findResponse(changed_transport, 3);
    REQUIRE(changed_response.has_value());
    const auto& changed_symbols = changed_response->at("result");
    CHECK(std::any_of(changed_symbols.begin(), changed_symbols.end(), [](const jsonrpc::Json& symbol) {
        return symbol.at("name") == "watched_changed";
    }));
    CHECK(std::none_of(changed_symbols.begin(), changed_symbols.end(), [](const jsonrpc::Json& symbol) {
        return symbol.at("name") == "watched_new";
    }));

    const auto text_path = workspace.writeFile("rtl/not-source.txt", "module ignored_text; endmodule\n");
    const auto text_uri = toFileUri(text_path);
    ScriptedTransport ignored_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            text_uri + R"(","type":1}]}})" ,
        R"({"jsonrpc":"2.0","id":4,"method":"workspace/symbol","params":{"query":"ignored_text"}})"};

    CHECK(rpc_server.run(ignored_transport) == 0);
    const auto ignored_response = findResponse(ignored_transport, 4);
    REQUIRE(ignored_response.has_value());
    CHECK(ignored_response->at("result").empty());

    fs::remove(watched_path);
    ScriptedTransport deleted_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            watched_uri + R"(","type":3}]}})" ,
        R"({"jsonrpc":"2.0","id":5,"method":"workspace/symbol","params":{"query":"watched"}})"};

    CHECK(rpc_server.run(deleted_transport) == 0);
    const auto deleted_response = findResponse(deleted_transport, 5);
    REQUIRE(deleted_response.has_value());
    CHECK(deleted_response->at("result").empty());
}

std::vector<jsonrpc::Json> findNotifications(const WaitingTransport& transport,
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

TEST_CASE("ServerSession refreshes SystemVerilog hierarchy and schematic from watched file changes",
          "[server][workspace][hierarchy][schematic]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile("rtl/top.sv",
                        "module top;\n"
                        "  child u_child();\n"
                        "endmodule\n");
    workspace.writeFile("rtl/unrelated.sv", "module unrelated; endmodule\n");
    const auto child_path = workspace.root() / "rtl" / "child.sv";
    const auto grandchild_path = workspace.root() / "rtl" / "grandchild.sv";
    const auto child_uri = toFileUri(child_path);
    const auto grandchild_uri = toFileUri(grandchild_path);

    ScriptedTransport initialize_transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})"};

    CHECK(rpc_server.run(initialize_transport) == 0);
    REQUIRE(findResponse(initialize_transport, 1).has_value());

    auto hierarchy_root = [](const jsonrpc::Json& response) -> const jsonrpc::Json& {
        const auto& roots = response.at("result").at("roots");
        REQUIRE(roots.size() == 1);
        return roots.at(0);
    };
    auto schematic_has_module = [](const jsonrpc::Json& response, std::string_view module_name) {
        const auto& modules = response.at("result").at("modules");
        return std::any_of(modules.begin(), modules.end(), [&](const jsonrpc::Json& module) {
            return module.at("id") == std::string(module_name) ||
                   module.at("name") == std::string(module_name);
        });
    };

    ScriptedTransport initial_query_transport{
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":8}})",
        R"({"jsonrpc":"2.0","id":3,"method":"systemverilog/schematic","params":{"moduleName":"top","maxDepth":8}})"};

    CHECK(rpc_server.run(initial_query_transport) == 0);
    const auto initial_hierarchy_response = findResponse(initial_query_transport, 2);
    REQUIRE(initial_hierarchy_response.has_value());
    const auto& initial_top = hierarchy_root(*initial_hierarchy_response);
    REQUIRE(initial_top.at("children").size() == 1);
    CHECK(initial_top.at("children").at(0).at("moduleName") == "child");
    CHECK(initial_top.at("children").at(0).at("unresolved") == true);
    CHECK(initial_hierarchy_response->at("result").at("discoveryClosureDocumentCount") == 1);

    const auto initial_schematic_response = findResponse(initial_query_transport, 3);
    REQUIRE(initial_schematic_response.has_value());
    CHECK(schematic_has_module(*initial_schematic_response, "top"));
    CHECK_FALSE(schematic_has_module(*initial_schematic_response, "child"));

    workspace.writeFile("rtl/child.sv", "module child; endmodule\n");
    ScriptedTransport created_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            child_uri + R"(","type":1}]}})" ,
        R"({"jsonrpc":"2.0","id":4,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":8}})",
        R"({"jsonrpc":"2.0","id":5,"method":"systemverilog/schematic","params":{"moduleName":"top","maxDepth":8}})",
        R"({"jsonrpc":"2.0","id":6,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":8}})"};

    CHECK(rpc_server.run(created_transport) == 0);
    const auto created_hierarchy_response = findResponse(created_transport, 4);
    REQUIRE(created_hierarchy_response.has_value());
    const auto& created_top = hierarchy_root(*created_hierarchy_response);
    REQUIRE(created_top.at("children").size() == 1);
    CHECK(created_top.at("children").at(0).at("moduleName") == "child");
    CHECK(created_top.at("children").at(0).at("unresolved") == false);
    CHECK(created_top.at("children").at(0).at("uri") == child_uri);
    CHECK(created_hierarchy_response->at("result").at("discoveryClosureCacheHit") == false);

    const auto created_schematic_response = findResponse(created_transport, 5);
    REQUIRE(created_schematic_response.has_value());
    CHECK(schematic_has_module(*created_schematic_response, "child"));
    CHECK(created_schematic_response->at("result").at("discoveryClosureCacheHit") == false);

    const auto cached_created_hierarchy_response = findResponse(created_transport, 6);
    REQUIRE(cached_created_hierarchy_response.has_value());
    CHECK(cached_created_hierarchy_response->at("result").at("discoveryClosureCacheHit") == true);

    workspace.writeFile("rtl/child.sv",
                        "module child;\n"
                        "  grandchild u_grandchild();\n"
                        "endmodule\n");
    workspace.writeFile("rtl/grandchild.sv", "module grandchild; endmodule\n");
    ScriptedTransport changed_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            child_uri + R"(","type":2},{"uri":")" + grandchild_uri + R"(","type":1}]}})" ,
        R"({"jsonrpc":"2.0","id":7,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":8}})",
        R"({"jsonrpc":"2.0","id":8,"method":"systemverilog/schematic","params":{"moduleName":"top","maxDepth":8}})"};

    CHECK(rpc_server.run(changed_transport) == 0);
    const auto changed_hierarchy_response = findResponse(changed_transport, 7);
    REQUIRE(changed_hierarchy_response.has_value());
    const auto& changed_top = hierarchy_root(*changed_hierarchy_response);
    REQUIRE(changed_top.at("children").size() == 1);
    const auto& changed_child = changed_top.at("children").at(0);
    REQUIRE(changed_child.at("children").size() == 1);
    CHECK(changed_child.at("children").at(0).at("moduleName") == "grandchild");
    CHECK(changed_child.at("children").at(0).at("uri") == grandchild_uri);
    CHECK(changed_hierarchy_response->at("result").at("discoveryClosureCacheHit") == false);

    const auto changed_schematic_response = findResponse(changed_transport, 8);
    REQUIRE(changed_schematic_response.has_value());
    CHECK(schematic_has_module(*changed_schematic_response, "grandchild"));

    fs::remove(child_path);
    ScriptedTransport deleted_transport{
        std::string(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")") +
            child_uri + R"(","type":3}]}})" ,
        R"({"jsonrpc":"2.0","id":9,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":8}})",
        R"({"jsonrpc":"2.0","id":10,"method":"systemverilog/schematic","params":{"moduleName":"top","maxDepth":8}})"};

    CHECK(rpc_server.run(deleted_transport) == 0);
    const auto deleted_hierarchy_response = findResponse(deleted_transport, 9);
    REQUIRE(deleted_hierarchy_response.has_value());
    const auto& deleted_top = hierarchy_root(*deleted_hierarchy_response);
    REQUIRE(deleted_top.at("children").size() == 1);
    CHECK(deleted_top.at("children").at(0).at("moduleName") == "child");
    CHECK(deleted_top.at("children").at(0).at("unresolved") == true);
    CHECK(deleted_hierarchy_response->at("result").at("discoveryClosureCacheHit") == false);

    const auto deleted_schematic_response = findResponse(deleted_transport, 10);
    REQUIRE(deleted_schematic_response.has_value());
    CHECK_FALSE(schematic_has_module(*deleted_schematic_response, "child"));
    CHECK_FALSE(schematic_has_module(*deleted_schematic_response, "grandchild"));
}

TEST_CASE("ServerSession applies incremental UTF-16 text edits", "[server][sync]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/broken.sv","languageId":"systemverilog","version":1,"text":"module broken; ? endmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE_FALSE(diagnostics.empty());
    const auto diagnostic = std::find_if(diagnostics.begin(), diagnostics.end(), [](const jsonrpc::Json& message) {
        return !message.at("params").at("diagnostics").empty();
    });
    REQUIRE(diagnostic != diagnostics.end());
    CHECK(diagnostic->at("params").at("diagnostics").front().at("source").get<std::string>() ==
          "pristine-engine");
}

TEST_CASE("ServerSession publishes semantic diagnostics for unresolved includes",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto top_text = std::string("`include \"missing.svh\"\nmodule top; endmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);
    const auto top_text_json = jsonrpc::Json(top_text).dump();

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            top_uri + R"(","languageId":"systemverilog","version":1,"text":)" + top_text_json + R"(}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unknownInclude";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("severity") == 1);
    CHECK(semantic_diagnostic->at("source") == "pristine-engine");
    CHECK(semantic_diagnostic->at("message") == "Include file 'missing.svh' could not be resolved.");
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 0);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 10);
}

TEST_CASE("ServerSession publishes semantic diagnostics for unresolved module instances",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/unresolved-module.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/unresolved-module.sv","languageId":"systemverilog","version":1,"text":"module top;\n  missing_child u_missing();\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedModule";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("message") == "Module 'missing_child' could not be resolved.");
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 1);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 2);
    CHECK(semantic_diagnostic->at("range").at("end").at("character") == 15);
}

TEST_CASE("ServerSession uses syntax-first diagnostics for large cold workspaces",
          "[server][diagnostics][perf]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    for (int index = 0; index < 130; ++index) {
        workspace.writeFile("rtl/filler_" + std::to_string(index) + ".sv",
                            "module filler_" + std::to_string(index) + "; endmodule\n");
    }

    const auto top_text =
        std::string("module top;\n  missing_child u_missing();\n  ?\nendmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);
    const auto top_text_json = jsonrpc::Json(top_text).dump();

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    const auto open_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
        top_uri + R"(","languageId":"systemverilog","version":1,"text":)" + top_text_json + R"(}}})";

    WaitingTransport transport{{initialize_message, open_message}, 3, std::chrono::seconds(10)};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() >= 2);
    CHECK(diagnostics.front().at("params").at("uri") == top_uri);
    const auto& syntax_items = diagnostics.front().at("params").at("diagnostics");
    REQUIRE_FALSE(syntax_items.empty());
    CHECK(std::none_of(syntax_items.begin(), syntax_items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedModule";
    }));
    const auto& semantic_items = diagnostics.back().at("params").at("diagnostics");
    CHECK(std::any_of(semantic_items.begin(), semantic_items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedModule";
    }));
}

TEST_CASE("ServerSession drops stale background diagnostics after document changes",
          "[server][diagnostics][perf]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    for (int index = 0; index < 130; ++index) {
        workspace.writeFile("rtl/filler_" + std::to_string(index) + ".sv",
                            "module filler_" + std::to_string(index) + "; endmodule\n");
    }

    const auto top_text = std::string("module top;\n  missing_child u_missing();\nendmodule\n");
    const auto clean_text = std::string("module top;\nendmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    const auto open_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
        top_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
        jsonrpc::Json(top_text).dump() + R"(}}})";
    const auto change_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")") +
        top_uri + R"(","version":2},"contentChanges":[{"text":)" + jsonrpc::Json(clean_text).dump() +
        R"(}]}})";

    WaitingTransport transport{{initialize_message, open_message, change_message}, 4};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const jsonrpc::Json& notification) {
        const auto& items = notification.at("params").at("diagnostics");
        return std::any_of(items.begin(), items.end(), [](const jsonrpc::Json& item) {
            const auto code_it = item.find("code");
            return code_it != item.end() && *code_it == "unresolvedModule";
        });
    }));
}

TEST_CASE("ServerSession drops background diagnostics after document closes",
          "[server][diagnostics][perf]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    for (int index = 0; index < 130; ++index) {
        workspace.writeFile("rtl/filler_" + std::to_string(index) + ".sv",
                            "module filler_" + std::to_string(index) + "; endmodule\n");
    }

    const auto top_text = std::string("module top;\n  missing_child u_missing();\nendmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    const auto open_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
        top_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
        jsonrpc::Json(top_text).dump() + R"(}}})";
    const auto close_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":")") +
        top_uri + R"("}}})";

    WaitingTransport transport{{initialize_message, open_message, close_message}, 4};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE_FALSE(diagnostics.empty());
    size_t unresolved_count = 0;
    for (const auto& notification : diagnostics) {
        const auto& items = notification.at("params").at("diagnostics");
        unresolved_count += static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
            const auto code_it = item.find("code");
            return code_it != item.end() && *code_it == "unresolvedModule";
        }));
    }
    CHECK(unresolved_count == 0);
}

TEST_CASE("ServerSession publishes empty diagnostics after semantic diagnostics clear",
          "[server][diagnostics][sync]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/diagnostics-clear.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/diagnostics-clear.sv","languageId":"systemverilog","version":1,"text":"module top;\n  missing_child u_missing();\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///workspace/diagnostics-clear.sv","version":2},"contentChanges":[{"text":"module top;\nendmodule\n"}]}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    CHECK(diagnostics.back().at("params").at("uri") == std::string(uri));
    const auto& initial_items = diagnostics.front().at("params").at("diagnostics");
    REQUIRE(std::any_of(initial_items.begin(), initial_items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedModule";
    }));
    CHECK(diagnostics.back().at("params").at("diagnostics").empty());
}

TEST_CASE("ServerSession publishes semantic diagnostics for duplicate symbols",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/duplicate-symbol.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/duplicate-symbol.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic ready;\n  logic ready;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "duplicateSymbol";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("message") == "Duplicate symbol 'ready' in the same scope.");
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 2);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 8);
}

TEST_CASE("ServerSession publishes semantic diagnostics for ambiguous references",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/ambiguous-reference.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/pkg-a.sv","languageId":"systemverilog","version":1,"text":"package pkg_a;\n  typedef logic [7:0] word_t;\nendpackage\n"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/pkg-b.sv","languageId":"systemverilog","version":1,"text":"package pkg_b;\n  typedef logic [15:0] word_t;\nendpackage\n"}}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/ambiguous-reference.sv","languageId":"systemverilog","version":1,"text":"module top;\n  import pkg_a::*;\n  import pkg_b::*;\n  word_t value;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    const auto diagnostic_notification = std::find_if(diagnostics.begin(), diagnostics.end(), [uri](const jsonrpc::Json& item) {
        return item.at("params").at("uri") == std::string(uri);
    });
    REQUIRE(diagnostic_notification != diagnostics.end());
    const auto& items = diagnostic_notification->at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "ambiguousReference";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("severity") == 2);
    CHECK(semantic_diagnostic->at("message") == "Symbol 'word_t' has 2 possible definitions in scope.");
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 3);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 2);
}

TEST_CASE("ServerSession publishes semantic diagnostics for unresolved packages",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/unresolved-package.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/unresolved-package.sv","languageId":"systemverilog","version":1,"text":"module top;\n  import missing_pkg::*;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedPackage";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("message") == "Package 'missing_pkg' could not be resolved.");
    CHECK(semantic_diagnostic->at("severity") == 1);
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 1);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 9);
}

TEST_CASE("ServerSession publishes semantic diagnostics for unresolved types",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/unresolved-type.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/unresolved-type.sv","languageId":"systemverilog","version":1,"text":"module top;\n  missing_t value;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "unresolvedType";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("message") == "Type 'missing_t' could not be resolved.");
    CHECK(semantic_diagnostic->at("severity") == 1);
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 1);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 2);
    CHECK(semantic_diagnostic->at("range").at("end").at("character") == 11);
}

TEST_CASE("ServerSession publishes AST-backed slang semantic diagnostics",
          "[server][diagnostics][semantic-engine]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/slang-semantic.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/slang-semantic.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic [3:0] value;\n  assign value = missing_signal;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        const auto message_it = item.find("message");
        return code_it != item.end() && code_it->is_string() &&
               code_it->get_ref<const std::string&>().starts_with("slang:") &&
               message_it != item.end() && message_it->is_string() &&
               message_it->get_ref<const std::string&>().find("missing_signal") != std::string::npos;
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("severity") == 1);
    CHECK(semantic_diagnostic->at("source") == "pristine-engine");
}

TEST_CASE("ServerSession publishes semantic diagnostics for assignment width mismatches",
          "[server][diagnostics]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/width-mismatch.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/width-mismatch.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic [3:0] lhs;\n  logic [7:0] rhs;\n  assign lhs = rhs;\nendmodule\n"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().at("params").at("uri") == std::string(uri));
    const auto& items = diagnostics.front().at("params").at("diagnostics");
    const auto semantic_diagnostic = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        const auto code_it = item.find("code");
        return code_it != item.end() && *code_it == "widthMismatch";
    });
    REQUIRE(semantic_diagnostic != items.end());
    CHECK(semantic_diagnostic->at("message") == "Width mismatch: assigning 8-bit 'rhs' to 4-bit 'lhs'.");
    CHECK(semantic_diagnostic->at("severity") == 2);
    CHECK(semantic_diagnostic->at("range").at("start").at("line") == 3);
    CHECK(semantic_diagnostic->at("range").at("start").at("character") == 15);
    CHECK(semantic_diagnostic->at("range").at("end").at("character") == 18);
}

TEST_CASE("ServerSession returns top-level document symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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

TEST_CASE("ServerSession returns SystemVerilog outline for opened document", "[server][outline]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/outline.sv","languageId":"systemverilog","version":7,"text":"package pkg; parameter int Width = 8; endpackage\ninterface bus(input logic clk); endinterface\nmodule child; endmodule\nmodule top;\n  child u_child();\n  logic ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/outline","params":{"textDocument":{"uri":"file:///workspace/outline.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto outline_response = parseOutput(transport, 2);
    const auto& result = outline_response.at("result");
    CHECK(result.at("uri") == "file:///workspace/outline.sv");
    CHECK(result.at("version") == 7);
    CHECK(result.at("partial") == false);
    CHECK(result.at("truncated") == false);
    REQUIRE(result.at("roots").size() == 4);
    CHECK(result.at("roots").at(0).at("id") == "outline:0");
    CHECK(result.at("roots").at(0).at("kind") == "package");
    CHECK(result.at("roots").at(1).at("kind") == "interface");
    CHECK(result.at("roots").at(2).at("kind") == "module");
    CHECK(result.at("roots").at(3).at("kind") == "module");
    REQUIRE(result.at("items").size() == 8);
    CHECK(result.at("items").at(0).at("parentId").is_null());
    CHECK(result.at("items").at(1).at("name") == "Width");
    CHECK(result.at("items").at(1).at("kind") == "parameter");
    CHECK(result.at("items").at(1).at("detail") == "int = 8");
    CHECK(result.at("items").at(1).at("type") == "int");
    CHECK(result.at("items").at(1).at("value") == "8");
    CHECK(result.at("items").at(3).at("name") == "clk");
    CHECK(result.at("items").at(3).at("kind") == "port");
    CHECK(result.at("items").at(3).at("detail") == "input logic");
    CHECK(result.at("items").at(3).at("direction") == "input");
    CHECK(result.at("items").at(3).at("type") == "logic");
    CHECK(result.at("items").at(6).at("name") == "u_child");
    CHECK(result.at("items").at(6).at("kind") == "instance");
    CHECK(result.at("items").at(6).at("detail") == "child");
    CHECK(result.at("items").at(6).at("moduleName") == "child");
    CHECK(result.at("items").at(7).at("name") == "ready");
    CHECK(result.at("items").at(7).at("parentId") == "outline:3");
}

TEST_CASE("ServerSession outline honors depth limit and flat toggle", "[server][outline]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/outline-depth.sv","languageId":"systemverilog","version":1,"text":"module top;\n  generate\n    begin : gen_blk\n      logic enabled;\n    end\n  endgenerate\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/outline","params":{"textDocument":{"uri":"file:///workspace/outline-depth.sv"},"maxDepth":0,"includeFlat":false}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto outline_response = parseOutput(transport, 2);
    const auto& result = outline_response.at("result");
    REQUIRE(result.at("roots").size() == 1);
    CHECK(result.at("roots").at(0).at("name") == "top");
    REQUIRE(result.at("roots").at(0).at("children").empty());
    CHECK(result.at("items").empty());
    CHECK(result.at("partial") == true);
    CHECK(result.at("truncated") == false);
    REQUIRE(result.at("messages").size() == 1);
}

TEST_CASE("ServerSession outline truncates by item limit", "[server][outline]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/outline-limit.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic a;\n  logic b;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/outline","params":{"textDocument":{"uri":"file:///workspace/outline-limit.sv"},"limit":2}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto outline_response = parseOutput(transport, 2);
    const auto& result = outline_response.at("result");
    CHECK(result.at("truncated") == true);
    CHECK(result.at("partial") == true);
    REQUIRE(result.at("items").size() == 2);
    CHECK(result.at("items").at(0).at("name") == "top");
    CHECK(result.at("items").at(1).at("name") == "a");
    REQUIRE(result.at("messages").size() == 1);
}

TEST_CASE("ServerSession outline detail strips comments from declarations",
          "[server][outline]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/outline-comments.sv","languageId":"systemverilog","version":1,"text":"module top;\n  // verilog_format on\n  // sclk\n  logic s_sclk, s_sclk_en_d, s_sclk_en_q;\n  localparam logic [2:0] FSM_IDLE = 3'd0;\n  localparam FSM_CMD    = 3'd1;\n  localparam FSM_ADDR   = 3'd2;\n  localparam FSM_DUM    = 3'd3;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/outline","params":{"textDocument":{"uri":"file:///workspace/outline-comments.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 3);

    const auto outline_response = parseOutput(transport, 2);
    const auto& items = outline_response.at("result").at("items");
    const auto s_sclk = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("name") == "s_sclk";
    });
    REQUIRE(s_sclk != items.end());
    CHECK(s_sclk->at("detail") == "logic");
    CHECK(s_sclk->at("type") == "logic");
    CHECK(s_sclk->at("declaration") == "logic s_sclk");

    const auto parameter = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("name") == "FSM_IDLE";
    });
    REQUIRE(parameter != items.end());
    CHECK(parameter->at("detail") == "logic [2:0] = 3'd0");
    CHECK(parameter->at("type") == "logic [2:0]");
    CHECK(parameter->at("value") == "3'd0");

    const auto implicit_parameter =
        std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
            return item.at("name") == "FSM_DUM";
        });
    REQUIRE(implicit_parameter != items.end());
    CHECK(implicit_parameter->at("detail") == "3'd3");
    CHECK(implicit_parameter->at("declaration") == "FSM_DUM = 3'd3");
    CHECK_FALSE(implicit_parameter->contains("type"));
    CHECK(implicit_parameter->at("value") == "3'd3");
}

TEST_CASE("ServerSession outline returns message for unopened document", "[server][outline]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/outline","params":{"textDocument":{"uri":"file:///workspace/missing.sv"}}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() >= 2);

    const auto outline_response = findResponse(transport, 2);
    REQUIRE(outline_response.has_value());
    const auto& result = outline_response->at("result");
    CHECK(result.at("uri") == "file:///workspace/missing.sv");
    CHECK(result.at("roots").empty());
    CHECK(result.at("items").empty());
    REQUIRE(result.at("messages").size() == 1);
}

TEST_CASE("ServerSession returns nested document symbols", "[server][symbols]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/hover.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///workspace/hover.sv"},"position":{"line":1,"character":8}}})",
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 4);

    const auto hover_response = parseOutput(transport, 2);
    CHECK(hover_response.at("id") == 2);
    CHECK(hover_response.at("result").at("contents").at("kind") == "markdown");
    const auto hover_value = hover_response.at("result").at("contents").at("value").get<std::string>();
    CHECK(hover_value.find("ready") != std::string::npos);
    CHECK(hover_value.find("Type: `logic`") != std::string::npos);
    CHECK(hover_response.at("result").at("range").at("start").at("line") == 1);
    CHECK(hover_response.at("result").at("range").at("start").at("character") == 8);
    CHECK(hover_response.at("result").at("range").at("end").at("character") == 13);
}

TEST_CASE("ServerSession keeps cold large-workspace hover on the syntax fast path",
          "[server][hover][diagnostics][perf]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    for (int index = 0; index < 193; ++index) {
        workspace.writeFile("rtl/filler_" + std::to_string(index) + ".sv",
                            "module filler_" + std::to_string(index) + "; endmodule\n");
    }

    const auto top_text = std::string("module top;\n  logic ready;\nendmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    const auto open_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
        top_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
        jsonrpc::Json(top_text).dump() + R"(}}})";
    const auto hover_message =
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":")") +
        top_uri + R"("},"position":{"line":1,"character":8}}})";

    WaitingTransport transport{{initialize_message, open_message, hover_message}, 3};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto hover_response = findResponse(transport, 2);
    REQUIRE(hover_response.has_value());
    CHECK(hover_response->at("result").at("contents").at("kind") == "markdown");
    const auto hover_value = hover_response->at("result").at("contents").at("value").get<std::string>();
    CHECK(hover_value.find("ready") != std::string::npos);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics.front().at("params").at("uri") == top_uri);
}

TEST_CASE("ServerSession keeps foreground hover responsive while background diagnostics are pending",
          "[server][hover][diagnostics][perf]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    for (int index = 0; index < 140; ++index) {
        workspace.writeFile("rtl/filler_" + std::to_string(index) + ".sv",
                            "module filler_" + std::to_string(index) + "; endmodule\n");
    }

    const auto top_text = std::string("module top;\n  logic ready;\nendmodule\n");
    const auto changed_text = std::string("module top;\n  logic ready;\n  logic done;\nendmodule\n");
    const auto top_path = workspace.writeFile("rtl/top.sv", top_text);
    const auto top_uri = toFileUri(top_path);

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    const auto open_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
        top_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
        jsonrpc::Json(top_text).dump() + R"(}}})";
    const auto change_message =
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")") +
        top_uri + R"(","version":2},"contentChanges":[{"text":)" + jsonrpc::Json(changed_text).dump() +
        R"(}]}})";
    const auto hover_message =
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":")") +
        top_uri + R"("},"position":{"line":2,"character":8}}})";

    WaitingTransport transport{{initialize_message, open_message, change_message, hover_message}, 4};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    const auto hover_response = findResponse(transport, 2);
    REQUIRE(hover_response.has_value());
    CHECK(hover_response->at("result").at("contents").at("kind") == "markdown");
    const auto hover_value = hover_response->at("result").at("contents").at("value").get<std::string>();
    CHECK(hover_value.find("done") != std::string::npos);

    const auto diagnostics = findNotifications(transport, "textDocument/publishDiagnostics");
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const jsonrpc::Json& notification) {
        const auto& items = notification.at("params").at("diagnostics");
        return std::any_of(items.begin(), items.end(), [](const jsonrpc::Json& item) {
            const auto code_it = item.find("code");
            return code_it != item.end() && *code_it == "unresolvedModule";
        });
    }));
}

TEST_CASE("ServerSession handles Tier 1 LSP navigation and completion", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
        return item.at("label") == "child" && item.at("data").at("source") == "semanticEngine";
    }));
    CHECK(std::any_of(completions.begin(), completions.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "child_i" && item.at("data").at("source") == "semanticEngine";
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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

TEST_CASE("ServerSession returns type definitions for typedef references", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto typed_text = std::string(
        "module typed;\n"
        "  typedef logic [7:0] byte_t;\n"
        "  byte_t value;\n"
        "endmodule\n");
    const auto typed_path = workspace.writeFile("rtl/typed.sv", typed_text);
    const auto typed_uri = toFileUri(typed_path);

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            typed_uri + R"(","languageId":"systemverilog","version":1,"text":)" +
            jsonrpc::Json(typed_text).dump() + R"(}}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/typeDefinition","params":{"textDocument":{"uri":")") +
            typed_uri + R"("},"position":{"line":2,"character":3}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto type_definition_response = findResponse(transport, 2);
    REQUIRE(type_definition_response.has_value());
    REQUIRE(type_definition_response->at("result").size() == 1);
    CHECK(type_definition_response->at("result").at(0).at("uri") == typed_uri);
    CHECK(type_definition_response->at("result").at(0).at("range").at("start").at("line") == 1);
    CHECK(type_definition_response->at("result").at(0).at("range").at("start").at("character") == 22);
    CHECK(type_definition_response->at("result").at(0).at("range").at("end").at("character") == 28);
}

TEST_CASE("ServerSession resolves references within semantic scopes", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/shadowed.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/shadowed.sv","languageId":"systemverilog","version":1,"text":"module first;\n  logic ready;\n  assign ready = ready;\nendmodule\nmodule second;\n  logic ready;\n  assign ready = ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///workspace/shadowed.sv"},"position":{"line":1,"character":9},"context":{"includeDeclaration":false}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto references_response = findResponse(transport, 2);
    REQUIRE(references_response.has_value());
    REQUIRE(references_response->at("result").size() == 2);
    CHECK(references_response->at("result").at(0).at("range").at("start").at("line") == 2);
    CHECK(references_response->at("result").at(1).at("range").at("start").at("line") == 2);
    CHECK(references_response->at("result").at(0).at("uri").get<std::string>() == std::string(uri));
}

TEST_CASE("ServerSession renames only the resolved scoped symbol", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/rename-shadowed.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/rename-shadowed.sv","languageId":"systemverilog","version":1,"text":"module first;\n  logic ready;\n  assign ready = ready;\nendmodule\nmodule second;\n  logic ready;\n  assign ready = ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///workspace/rename-shadowed.sv"},"position":{"line":1,"character":9},"newName":"valid"}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto rename_response = findResponse(transport, 2);
    REQUIRE(rename_response.has_value());
    const auto& edits = rename_response->at("result").at("changes").at(std::string(uri));
    REQUIRE(edits.size() == 3);
    CHECK(edits.at(0).at("range").at("start").at("line") == 1);
    CHECK(edits.at(1).at("range").at("start").at("line") == 2);
    CHECK(edits.at(2).at("range").at("start").at("line") == 2);
}

TEST_CASE("ServerSession prefers scoped semantic completions", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/completion.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/completion.sv","languageId":"systemverilog","version":1,"text":"module ready; endmodule\nmodule top;\n  logic ready;\n  assign rea = ready;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/completion.sv"},"position":{"line":3,"character":12},"context":{"triggerKind":1}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    REQUIRE_FALSE(completion_response->at("result").empty());
    CHECK(completion_response->at("result").at(0).at("label") == "ready");
    CHECK((completion_response->at("result").at(0).at("detail") == "Variable" ||
           completion_response->at("result").at(0).at("detail") == "Instance"));
}

TEST_CASE("ServerSession resolves completion items lazily", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/resolve-completion.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/resolve-completion.sv","languageId":"systemverilog","version":1,"text":"module child(input logic clk, output logic rst_n); endmodule\nmodule top;\n  child child_i();\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/resolve-completion.sv"},"position":{"line":2,"character":2},"context":{"triggerKind":1}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    const auto& completions = completion_response->at("result");
    const auto child_it = std::find_if(completions.begin(), completions.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "child";
    });
    REQUIRE(child_it != completions.end());

    jsonrpc::JsonRpcServer resolve_server;
    ServerSession resolve_session{"pristine-engine", kTestServerVersion};
    resolve_session.bind(resolve_server);
    ScriptedTransport resolve_transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/resolve-completion.sv","languageId":"systemverilog","version":1,"text":"module child(input logic clk, output logic rst_n); endmodule\nmodule top;\n  child child_i();\nendmodule\n"}}})",
        jsonrpc::Json{{"jsonrpc", "2.0"},
                      {"id", 2},
                      {"method", "completionItem/resolve"},
                      {"params", *child_it}}.dump()};

    CHECK(resolve_server.run(resolve_transport) == 0);
    const auto resolve_response = findResponse(resolve_transport, 2);
    REQUIRE(resolve_response.has_value());
    const auto& item = resolve_response->at("result");
    CHECK(item.at("label") == "child");
    CHECK(item.at("detail").get<std::string>().find("child(input logic clk, output logic rst_n)") !=
          std::string::npos);
    CHECK(item.at("documentation").at("kind") == "markdown");
    CHECK(item.at("documentation").at("value").get<std::string>().find("Ports:") != std::string::npos);
    CHECK(item.at("insertTextFormat") == 2);
    CHECK(item.at("insertText").get<std::string>().find(".clk(${2:clk})") != std::string::npos);
    CHECK(item.at("data").at("stableId").get<std::string>().find(std::string(uri)) != std::string::npos);
}

TEST_CASE("ServerSession returns context-aware completion items", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/context-completion.sv","languageId":"systemverilog","version":1,"text":"package defs;\n  parameter int WIDTH = 8;\nendpackage\nmodule child(input logic clk, output logic rst_n);\nendmodule\nmodule top;\n  localparam int USE = defs::W;\n  child child_i(.r());\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/context-completion.sv"},"position":{"line":6,"character":30},"context":{"triggerKind":1}}})",
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/context-completion.sv"},"position":{"line":7,"character":18},"context":{"triggerKind":1}}})"};

    CHECK(rpc_server.run(transport) == 0);

    const auto package_completion_response = findResponse(transport, 2);
    REQUIRE(package_completion_response.has_value());
    const auto& package_items = package_completion_response->at("result");
    REQUIRE_FALSE(package_items.empty());
    CHECK(package_items.at(0).at("label") == "WIDTH");
    CHECK(package_items.at(0).at("data").at("source") == "semanticEngine");

    const auto port_completion_response = findResponse(transport, 3);
    REQUIRE(port_completion_response.has_value());
    const auto& port_items = port_completion_response->at("result");
    REQUIRE_FALSE(port_items.empty());
    CHECK(port_items.at(0).at("label") == "rst_n");
    CHECK(port_items.at(0).at("detail") == "output logic rst_n");
    CHECK(port_items.at(0).at("data").at("source") == "semanticEngine");
}

TEST_CASE("ServerSession returns macro completions and resolves macro documentation",
          "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/macro-completion.sv","languageId":"systemverilog","version":1,"text":"`define LOCAL_FLAG 1\n`define LOCAL_ADD(a, b) ((a) + (b))\nmodule top;\n  logic ready;\n  assign ready = `LOC\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/macro-completion.sv"},"position":{"line":4,"character":21},"context":{"triggerKind":1}}})",
        R"json({"jsonrpc":"2.0","id":3,"method":"completionItem/resolve","params":{"label":"LOCAL_ADD","kind":3,"detail":"Macro function","data":{"source":"semanticEngine","stableId":"completion-macro:file:///workspace/macro-completion.sv:LOCAL_ADD:1:8:1:0","label":"LOCAL_ADD"}}})json"};

    CHECK(rpc_server.run(transport) == 0);

    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    const auto& items = completion_response->at("result");
    REQUIRE(items.size() == 2);
    CHECK(items.at(0).at("label") == "LOCAL_FLAG");
    CHECK(items.at(0).at("detail") == "Macro");
    CHECK(items.at(0).at("data").at("source") == "semanticEngine");
    CHECK(items.at(1).at("label") == "LOCAL_ADD");
    CHECK(items.at(1).at("kind") == 3);

    const auto resolve_response = findResponse(transport, 3);
    REQUIRE(resolve_response.has_value());
    const auto& resolved = resolve_response->at("result");
    CHECK(resolved.at("detail") == "Macro function LOCAL_ADD(a, b)");
    CHECK(resolved.at("insertText") == "LOCAL_ADD(${1:a}, ${2:b})");
    CHECK(resolved.at("insertTextFormat") == 2);
    CHECK(resolved.at("documentation").at("value").get<std::string>().find("((a) + (b))") !=
          std::string::npos);
}

TEST_CASE("ServerSession prioritizes module completions in instantiation context",
          "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/module-context.sv","languageId":"systemverilog","version":1,"text":"module child; endmodule\nmodule chip; endmodule\nmodule top;\n  logic chip_count;\n  ch\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/module-context.sv"},"position":{"line":4,"character":4},"context":{"triggerKind":1}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    const auto& items = completion_response->at("result");
    REQUIRE(items.size() >= 2);
    CHECK(items.at(0).at("label") == "child");
    CHECK(items.at(0).at("detail").get<std::string>().find("child(") != std::string::npos);
    CHECK(items.at(1).at("label") == "chip");
}

TEST_CASE("ServerSession excludes already connected named ports from completion",
          "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/port-filter.sv","languageId":"systemverilog","version":1,"text":"module child(input logic clk, output logic rst_n, input logic data); endmodule\nmodule top;\n  logic sig;\n  child u_child(.clk(sig), .);\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/port-filter.sv"},"position":{"line":3,"character":28},"context":{"triggerKind":2,"triggerCharacter":"."}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    const auto& items = completion_response->at("result");
    CHECK(std::none_of(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "clk";
    }));
    CHECK(std::any_of(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "rst_n";
    }));
    CHECK(std::any_of(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "data";
    }));
}

TEST_CASE("ServerSession offers a quickfix for missing named port connections",
          "[server][code-action]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/missing-ports.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/missing-ports.sv","languageId":"systemverilog","version":1,"text":"module child(input logic clk, output logic rst_n, input logic data); endmodule\nmodule top;\n  logic sig;\n  child u_child(.clk(sig));\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///workspace/missing-ports.sv"},"range":{"start":{"line":3,"character":8},"end":{"line":3,"character":15}},"context":{"diagnostics":[]}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto code_action_response = findResponse(transport, 2);
    REQUIRE(code_action_response.has_value());
    const auto& actions = code_action_response->at("result");
    REQUIRE(actions.size() == 1);
    const auto& action = actions.at(0);
    CHECK(action.at("title") == "Add missing port connections to 'u_child'");
    CHECK(action.at("kind") == "quickfix");
    const auto& edit = action.at("edit").at("changes").at(std::string(uri)).at(0);
    CHECK(edit.at("range").at("start").at("line") == 3);
    CHECK(edit.at("range").at("start").at("character") == 25);
    CHECK(edit.at("newText") == ", .rst_n(rst_n), .data(data)");
}

TEST_CASE("ServerSession offers a quickfix for unresolved type references",
          "[server][code-action]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/create-typedef.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/create-typedef.sv","languageId":"systemverilog","version":1,"text":"module top;\n  missing_t value;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///workspace/create-typedef.sv"},"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":11}},"context":{"diagnostics":[]}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto code_action_response = findResponse(transport, 2);
    REQUIRE(code_action_response.has_value());
    const auto& actions = code_action_response->at("result");
    REQUIRE(actions.size() == 1);
    const auto& action = actions.at(0);
    CHECK(action.at("title") == "Create typedef 'missing_t'");
    CHECK(action.at("kind") == "quickfix");
    REQUIRE(action.at("diagnostics").size() == 1);
    CHECK(action.at("diagnostics").at(0).at("code") == "unresolvedType");
    const auto& edit = action.at("edit").at("changes").at(std::string(uri)).at(0);
    CHECK(edit.at("range").at("start").at("line") == 3);
    CHECK(edit.at("range").at("start").at("character") == 0);
    CHECK(edit.at("newText") == "\ntypedef logic missing_t;\n");
}

TEST_CASE("ServerSession offers a quickfix for unresolved module instances",
          "[server][code-action]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/create-module.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/create-module.sv","languageId":"systemverilog","version":1,"text":"module top;\n  missing_child u_missing();\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///workspace/create-module.sv"},"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":15}},"context":{"diagnostics":[]}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto code_action_response = findResponse(transport, 2);
    REQUIRE(code_action_response.has_value());
    const auto& actions = code_action_response->at("result");
    REQUIRE(actions.size() == 1);
    const auto& action = actions.at(0);
    CHECK(action.at("title") == "Create stub module 'missing_child'");
    CHECK(action.at("kind") == "quickfix");
    REQUIRE(action.at("diagnostics").size() == 1);
    CHECK(action.at("diagnostics").at(0).at("code") == "unresolvedModule");
    const auto& edit = action.at("edit").at("changes").at(std::string(uri)).at(0);
    CHECK(edit.at("range").at("start").at("line") == 3);
    CHECK(edit.at("range").at("start").at("character") == 0);
    CHECK(edit.at("newText") == "\nmodule missing_child;\nendmodule\n");
}

TEST_CASE("ServerSession handles Tier 2 rename highlight and document links", "[server][lsp-core]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    CHECK(highlight_response.at("result").at(0).at("kind") == 1);
    CHECK(highlight_response.at("result").at(1).at("range").at("start").at("line") == 5);
    CHECK(highlight_response.at("result").at(1).at("kind") == 3);
    CHECK(highlight_response.at("result").at(2).at("kind") == 2);

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
    const auto& hints = inlay_hint_response.at("result");
    REQUIRE(hints.size() >= 1);
    const auto child_hint = std::find_if(hints.begin(), hints.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == ": child";
    });
    REQUIRE(child_hint != hints.end());
    CHECK(child_hint->at("kind") == 1);
    CHECK(child_hint->at("position").at("line") == 3);
    CHECK(child_hint->at("position").at("character") == 15);

    const auto code_action_response = parseOutput(transport, 7);
    CHECK(code_action_response.at("id") == 7);
    REQUIRE(code_action_response.at("result").size() == 1);
    const auto& code_action = code_action_response.at("result").at(0);
    CHECK(code_action.at("title") == "Create include file 'missing.svh'");
    CHECK(code_action.at("kind") == "quickfix");
    REQUIRE(code_action.at("diagnostics").size() == 1);
    CHECK(code_action.at("diagnostics").at(0).at("code") == "unknownInclude");
    CHECK(code_action.at("diagnostics").at(0).at("message") ==
          "Include file 'missing.svh' could not be resolved.");
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
        CHECK(signature_help.at("signatures").at(0).at("label") ==
            "child(input logic clk, output logic rst_n)");
    REQUIRE(signature_help.at("signatures").at(0).at("parameters").size() == 2);
        CHECK(signature_help.at("signatures").at(0).at("parameters").at(0).at("label") ==
            "input logic clk");
        CHECK(signature_help.at("signatures").at(0).at("parameters").at(1).at("label") ==
            "output logic rst_n");
    CHECK(signature_help.at("activeParameter") == 1);

    const auto prepare_rename_response = parseOutput(transport, 12);
    CHECK(prepare_rename_response.at("id") == 12);
    CHECK(prepare_rename_response.at("result").at("placeholder") == "ready");
    CHECK(prepare_rename_response.at("result").at("range").at("start").at("line") == 4);
    CHECK(prepare_rename_response.at("result").at("range").at("start").at("character") == 8);
    CHECK(prepare_rename_response.at("result").at("range").at("end").at("character") == 13);
}

TEST_CASE("ServerSession resolves SemanticEngine completion items", "[server][completion]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/engine-completion.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/engine-completion.sv","languageId":"systemverilog","version":1,"text":"module child(input logic clk, output logic rst_n); endmodule\nmodule top;\n  child u_child(.r);\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///workspace/engine-completion.sv"},"position":{"line":2,"character":18}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto completion_response = findResponse(transport, 2);
    REQUIRE(completion_response.has_value());
    const auto& items = completion_response->at("result");
    const auto item_it = std::find_if(items.begin(), items.end(), [](const jsonrpc::Json& item) {
        return item.at("label") == "rst_n";
    });
    REQUIRE(item_it != items.end());
    CHECK(item_it->at("data").at("source") == "semanticEngine");
}

TEST_CASE("ServerSession returns inferred SystemVerilog module hierarchy", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    const auto leaf_path = workspace.writeFile("rtl/leaf.sv", "module leaf; endmodule\n");
    const auto interface_path = workspace.writeFile("rtl/bus_if.sv", "interface bus_if; endinterface\n");
    const auto child_path = workspace.writeFile(
        "rtl/child.sv",
        "module child;\n"
        "  leaf u_leaf();\n"
        "endmodule\n");
    const auto top_path = workspace.writeFile(
        "rtl/top.sv",
        "module top;\n"
        "  child u_child();\n"
        "  bus_if bus();\n"
        "endmodule\n");
    const auto zz_top_path = workspace.writeFile(
        "rtl/zz_top.sv",
        "module zz_top;\n"
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
    REQUIRE(result.at("roots").size() == 2);
    CHECK(result.contains("queryCacheCompletionEntries"));
    CHECK(result.contains("queryCacheSignatureHelpEntries"));
    CHECK(result.contains("queryCacheInlayHintEntries"));
    CHECK(result.contains("queryCacheCodeActionEntries"));
    CHECK(result.contains("backgroundDiagnosticsState"));
    CHECK(result.contains("backgroundDiagnosticsPhase"));
    CHECK(result.contains("backgroundDiagnosticsElapsedMicros"));
    CHECK(result.at("backgroundDiagnosticsState").is_string());

    const auto& top = result.at("roots").at(0);
    CHECK(top.at("moduleName") == "top");
    CHECK(top.at("kind") == "module");
    CHECK(top.at("uri") == toFileUri(top_path));
    REQUIRE(top.at("children").size() == 2);

    const auto& top_children = top.at("children");
    const auto child_it = std::find_if(top_children.begin(), top_children.end(), [](const auto& item) {
        return item.at("moduleName") == "child" && item.at("instanceName") == "u_child";
    });
    REQUIRE(child_it != top_children.end());
    const auto& child = *child_it;
    CHECK(child.at("moduleName") == "child");
    CHECK(child.at("kind") == "module");
    CHECK(child.at("instanceName") == "u_child");
    CHECK(child.at("uri") == toFileUri(child_path));
    CHECK(child.at("instanceSelectionRange").at("start").at("line") == 1);
    REQUIRE(child.at("children").size() == 1);

    const auto& zz_top = result.at("roots").at(1);
    CHECK(zz_top.at("moduleName") == "zz_top");
    CHECK(zz_top.at("kind") == "module");
    CHECK(zz_top.at("uri") == toFileUri(zz_top_path));
    REQUIRE(zz_top.at("children").size() == 1);

    const auto& leaf = child.at("children").at(0);
    CHECK(leaf.at("moduleName") == "leaf");
    CHECK(leaf.at("kind") == "module");
    CHECK(leaf.at("instanceName") == "u_leaf");
    CHECK(leaf.at("uri") == toFileUri(leaf_path));
    CHECK(leaf.at("children").empty());

    const auto interface_it = std::find_if(top_children.begin(), top_children.end(), [](const auto& item) {
        return item.at("moduleName") == "bus_if" && item.at("instanceName") == "bus";
    });
    REQUIRE(interface_it != top_children.end());
    const auto& interface_instance = *interface_it;
    CHECK(interface_instance.at("moduleName") == "bus_if");
    CHECK(interface_instance.at("kind") == "interface");
    CHECK(interface_instance.at("instanceName") == "bus");
    CHECK(interface_instance.at("uri") == toFileUri(interface_path));
    CHECK(interface_instance.at("children").empty());
}

TEST_CASE("ServerSession returns schematic data for a selected top module", "[server][schematic]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeFile("rtl/child.sv", "module child(input logic a, output logic y); endmodule\n");
    const auto top_path = workspace.writeFile(
        "rtl/top.sv",
        "module top(input logic a, input logic b, input logic sel, output logic y);\n"
        "  logic n1, n2;\n"
        "  child u_child(.a(a), .y(n1));\n"
        "  and u_and(n2, a, b);\n"
        "  assign y = sel ? n1 : (a | b);\n"
        "endmodule\n");

    ScriptedTransport transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/schematic","params":{"moduleName":"top"}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    REQUIRE(transport.outputs().size() == 2);

    const auto schematic_response = parseOutput(transport, 1);
    CHECK(schematic_response.at("id") == 2);
    const auto& result = schematic_response.at("result");
    CHECK(result.at("rootModuleId") == "top");
    REQUIRE(result.at("modules").size() == 2);

    const auto top_it = std::find_if(result.at("modules").begin(), result.at("modules").end(),
                                    [](const jsonrpc::Json& module) {
                                        return module.at("id") == "top";
                                    });
    REQUIRE(top_it != result.at("modules").end());
    CHECK(top_it->at("uri") == toFileUri(top_path));
    REQUIRE(top_it->at("ports").size() == 4);
    CHECK(top_it->at("ports").at(0).at("direction") == "input");
    CHECK(top_it->at("ports").at(3).at("direction") == "output");

    const auto& cells = top_it->at("cells");
    CHECK(std::any_of(cells.begin(), cells.end(), [](const jsonrpc::Json& cell) {
        return cell.at("name") == "u_child" && cell.at("kind") == "module" && cell.at("type") == "child";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const jsonrpc::Json& cell) {
        return cell.at("name") == "u_and" && cell.at("kind") == "and";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const jsonrpc::Json& cell) {
        return cell.at("kind") == "or";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const jsonrpc::Json& cell) {
        return cell.at("kind") == "mux";
    }));

    const auto& nets = top_it->at("nets");
    CHECK(std::any_of(nets.begin(), nets.end(), [](const jsonrpc::Json& net) {
        return net.at("name") == "y" && !net.at("drivers").empty() && !net.at("loads").empty();
    }));
}

TEST_CASE("ServerSession returns local backward cone data for a selected signal",
          "[server][cone]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/cone.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/cone.sv","languageId":"systemverilog","version":1,"text":"module top;\n  logic a;\n  logic b;\n  logic mid;\n  logic out;\n  assign mid = a & b;\n  assign out = mid;\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/backwardCone","params":{"textDocument":{"uri":"file:///workspace/cone.sv"},"position":{"line":4,"character":9}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto cone_response = findResponse(transport, 2);
    REQUIRE(cone_response.has_value());
    const auto& result = cone_response->at("result");
    CHECK(result.at("messages").empty());

    const auto node_id = [&](std::string_view name) -> std::string {
        const auto& nodes = result.at("nodes");
        const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const jsonrpc::Json& item) {
            return item.at("name") == std::string(name);
        });
        REQUIRE(node != nodes.end());
        CHECK(node->at("uri") == std::string(uri));
        return node->at("id").get<std::string>();
    };

    const auto out_id = node_id("out");
    const auto mid_id = node_id("mid");
    const auto a_id = node_id("a");
    const auto b_id = node_id("b");
    CHECK(result.at("rootSymbolId") == out_id);

    const auto has_edge = [&](const std::string& from, const std::string& to) {
        const auto& edges = result.at("edges");
        return std::any_of(edges.begin(), edges.end(), [&](const jsonrpc::Json& edge) {
            return edge.at("from") == from && edge.at("to") == to;
        });
    };
    CHECK(has_edge(out_id, mid_id));
    CHECK(has_edge(mid_id, a_id));
    CHECK(has_edge(mid_id, b_id));
}

TEST_CASE("ServerSession exposes indexed assertion temporal cone metadata",
          "[server][cone][assertion][temporal]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/assertion-temporal.sv","languageId":"systemverilog","version":1,"text":"module top(input logic clk, input logic choose, input logic a, input logic b);\n  assert property (@(posedge clk) if (choose) a else b);\nendmodule\n"}}})",
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/backwardCone","params":{"textDocument":{"uri":"file:///workspace/assertion-temporal.sv"},"position":{"line":1,"character":2}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto cone_response = findResponse(transport, 2);
    REQUIRE(cone_response.has_value());
    const auto& result = cone_response->at("result");
    REQUIRE(result.contains("coneAssertionTemporalEdges"));
    CHECK(result.at("coneAssertionTemporalEdges").get<size_t>() == 3);
    const auto selector = std::find_if(result.at("edges").begin(), result.at("edges").end(),
                                       [](const jsonrpc::Json& edge) {
                                           return edge.at("expression") == "choose" &&
                                                  edge.value("sourceRole", "") == "control" &&
                                                  edge.value("controlOrigin", "") ==
                                                      "assertionConditional";
                                       });
    REQUIRE(selector != result.at("edges").end());
    REQUIRE(selector->contains("temporalPath"));
    CHECK(std::any_of(selector->at("temporalPath").begin(), selector->at("temporalPath").end(),
                      [](const jsonrpc::Json& step) {
                          return step.at("relation") == "conditionalBranch";
                      }));
}

TEST_CASE("ServerSession handles standard call hierarchy", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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

    ScriptedTransport prepare_transport{
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
            toFileUri(workspace.root()) + R"("}})",
        std::string(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":0,"character":8}}})",
        std::string(R"({"jsonrpc":"2.0","id":4,"method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")") +
            top_uri + R"("},"position":{"line":1,"character":3}}})"};

    const int prepare_exit_code = rpc_server.run(prepare_transport);

    CHECK(prepare_exit_code == 0);
    REQUIRE(prepare_transport.outputs().size() == 3);

    const auto prepare_top_response = parseOutput(prepare_transport, 1);
    CHECK(prepare_top_response.at("id") == 2);
    REQUIRE(prepare_top_response.at("result").size() == 1);
    CHECK(prepare_top_response.at("result").at(0).at("name") == "top");
    CHECK(prepare_top_response.at("result").at(0).at("uri") == top_uri);
    REQUIRE(prepare_top_response.at("result").at(0).contains("data"));

    const auto prepare_child_response = parseOutput(prepare_transport, 2);
    CHECK(prepare_child_response.at("id") == 4);
    REQUIRE(prepare_child_response.at("result").size() == 1);
    CHECK(prepare_child_response.at("result").at(0).at("name") == "child");
    CHECK(prepare_child_response.at("result").at(0).at("uri") == child_uri);
    REQUIRE(prepare_child_response.at("result").at(0).contains("data"));

    ScriptedTransport calls_transport{
        std::string(R"({"jsonrpc":"2.0","id":3,"method":"callHierarchy/outgoingCalls","params":{"item":)") +
            prepare_top_response.at("result").at(0).dump() + R"(}})",
        std::string(R"({"jsonrpc":"2.0","id":5,"method":"callHierarchy/incomingCalls","params":{"item":)") +
            prepare_child_response.at("result").at(0).dump() + R"(}})"};
    const int calls_exit_code = rpc_server.run(calls_transport);
    CHECK(calls_exit_code == 0);
    REQUIRE(calls_transport.outputs().size() == 2);

    const auto outgoing_response = parseOutput(calls_transport, 0);
    CHECK(outgoing_response.at("id") == 3);
    REQUIRE(outgoing_response.at("result").size() == 1);
    CHECK(outgoing_response.at("result").at(0).at("to").at("name") == "child");
    CHECK(outgoing_response.at("result").at(0).at("to").at("uri") == child_uri);
    CHECK(outgoing_response.at("result").at(0).at("fromRanges").at(0).at("start").at("line") == 1);
    CHECK(outgoing_response.at("result").at(0).at("fromRanges").at(0).at("start").at("character") == 2);

    const auto incoming_response = parseOutput(calls_transport, 1);
    CHECK(incoming_response.at("id") == 5);
    REQUIRE(incoming_response.at("result").size() == 1);
    CHECK(incoming_response.at("result").at(0).at("from").at("name") == "top");
    CHECK(incoming_response.at("result").at(0).at("from").at("uri") == top_uri);
    CHECK(incoming_response.at("result").at(0).at("fromRanges").at(0).at("start").at("line") == 1);
}

TEST_CASE("ServerSession marks unresolved and cyclic module hierarchy entries", "[server][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
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
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeConfig(R"({
        "build": "rtl/top.f",
        "buildPattern": "builds/{}.f",
        "buildRelativePaths": true,
        "flags": "-Iinclude -DDEBUG",
        "top": "top",
        "topModules": ["top", "tb_top"],
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
    REQUIRE(state.config.top.has_value());
    CHECK(*state.config.top == "top");
    CHECK(state.config.top_modules == std::vector<std::string>{"tb_top", "top"});
    REQUIRE(state.config.index.size() == 1);
    CHECK(state.config.index[0].dirs == std::vector<std::string>{"rtl", "tb"});
    CHECK(state.config.index[0].exclude_dirs == std::vector<std::string>{"third_party"});
}

TEST_CASE("ServerSession applies workspace index config to discovery-backed hierarchy",
          "[server][workspace][discovery][hierarchy]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    TempWorkspace workspace;
    workspace.writeConfig(R"({
        "topModules": ["top"],
        "index": [
            {
                "dirs": ["rtl"],
                "excludeDirs": ["rtl/vendor"]
            }
        ]
    })");
    workspace.writeFile("rtl/top.sv",
                        "module top;\n"
                        "  child u_child();\n"
                        "endmodule\n");
    workspace.writeFile("rtl/child.sv", "module child; endmodule\n");
    workspace.writeFile("rtl/vendor/hidden.sv", "module hidden; endmodule\n");
    workspace.writeFile("tb/tb_top.sv", "module tb_top; endmodule\n");

    const auto initialize_message =
        std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":")") +
        toFileUri(workspace.root()) + R"("}})";
    ScriptedTransport transport{
        initialize_message,
        R"({"jsonrpc":"2.0","id":2,"method":"systemverilog/moduleHierarchy","params":{"moduleName":"top","maxDepth":4}})"};

    const int exit_code = rpc_server.run(transport);

    CHECK(exit_code == 0);
    CHECK(session.workspace().state().config_loaded);
    REQUIRE(session.workspace().state().config.index.size() == 1);
    const auto hierarchy_response = findResponse(transport, 2);
    REQUIRE(hierarchy_response.has_value());
    const auto& result = hierarchy_response->at("result");
    REQUIRE(result.at("roots").is_array());
    REQUIRE(result.at("roots").size() == 1);
    CHECK(result.at("roots").at(0).at("moduleName") == "top");
    REQUIRE(result.at("roots").at(0).at("children").is_array());
    REQUIRE(result.at("roots").at(0).at("children").size() == 1);
    CHECK(result.at("roots").at(0).at("children").at(0).at("moduleName") == "child");
}

TEST_CASE("ServerSession survives invalid workspace config", "[server][workspace]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
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

TEST_CASE("ServerSession publishes client-gated inactive regions and clears them on close",
          "[server][preprocessor][inactive-regions]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/inactive.sv";
    const auto text = std::string("`ifdef DISABLED\n"
                                  "  logic disabled_value;\n"
                                  "`else\n"
                                  "  logic active_value;\n"
                                  "`endif\n"
                                  "module top; endmodule\n");
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{"experimental":{"inactiveRegions":{"inactiveRegions":true}}}}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            std::string(uri) + R"(","languageId":"systemverilog","version":1,"text":)" +
            jsonrpc::Json(text).dump() + R"(}}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":")") +
            std::string(uri) + R"("}}})"};

    CHECK(rpc_server.run(transport) == 0);
    const auto notifications = findNotifications(transport, "textDocument/inactiveRegions");
    REQUIRE(notifications.size() == 2);
    CHECK(notifications.front().at("params").at("uri").get<std::string>() == std::string(uri));
    REQUIRE_FALSE(notifications.front().at("params").at("regions").empty());
    CHECK(notifications.back().at("params").at("regions").empty());
}

TEST_CASE("ServerSession does not publish inactive regions without client capability",
          "[server][preprocessor][inactive-regions][capability]") {
    jsonrpc::JsonRpcServer rpc_server;
    ServerSession session{"pristine-engine", kTestServerVersion};
    session.bind(rpc_server);

    constexpr std::string_view uri = "file:///workspace/inactive-unsupported.sv";
    ScriptedTransport transport{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
            std::string(uri) +
            R"(","languageId":"systemverilog","version":1,"text":"`ifdef DISABLED\nlogic hidden;\n`endif\nmodule top; endmodule\n"}}})"};

    CHECK(rpc_server.run(transport) == 0);
    CHECK(findNotifications(transport, "textDocument/inactiveRegions").empty());
}

} // namespace pristine::server

