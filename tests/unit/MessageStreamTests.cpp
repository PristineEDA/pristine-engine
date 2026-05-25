#include "pristine/jsonrpc/MessageStream.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace pristine::jsonrpc {

TEST_CASE("MessageStream writes LSP framing", "[jsonrpc]") {
    MessageStream stream;
    std::ostringstream output;

    stream.write(output, R"({"jsonrpc":"2.0"})");

    CHECK(output.str() == "Content-Length: 17\r\n\r\n{\"jsonrpc\":\"2.0\"}");
}

TEST_CASE("MessageStream reads LSP framing", "[jsonrpc]") {
    MessageStream stream;
    std::istringstream input("Content-Length: 17\r\n\r\n{\"jsonrpc\":\"2.0\"}");

    const auto payload = stream.read(input);

    REQUIRE(payload.has_value());
    CHECK(payload.value() == R"({"jsonrpc":"2.0"})");
}

} // namespace pristine::jsonrpc