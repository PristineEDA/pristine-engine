#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/server/ServerSession.h"
#include "pristine/transport/StdioTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::server {
namespace {

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

} // namespace pristine::server