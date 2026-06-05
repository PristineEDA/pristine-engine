#ifdef _WIN32
#    include <fcntl.h>
#    include <io.h>
#endif

#include "pristine/Version.h"
#include "pristine/jsonrpc/JsonRpcServer.h"
#include "pristine/server/ServerSession.h"
#include "pristine/transport/StdioTransport.h"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kServerName = "pristine-engine";

int runStdioServer() {
    pristine::transport::StdioTransport transport(std::cin, std::cout);
    pristine::server::ServerSession session{std::string(kServerName),
                                            std::string(pristine::kVersion)};
    pristine::jsonrpc::JsonRpcServer rpc_server;
    session.bind(rpc_server);
    return rpc_server.run(transport);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc == 1) {
        return runStdioServer();
    }

    const std::string_view command = argv[1];
    if (command == "--version") {
        std::cout << pristine::kVersionLine << '\n';
        return 0;
    }

    if (command == "--stdio") {
        return runStdioServer();
    }

    std::cerr << "Unknown argument: " << command << '\n';
    return 1;
}
