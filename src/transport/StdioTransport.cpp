#include "pristine/transport/StdioTransport.h"

#include "pristine/jsonrpc/MessageStream.h"

namespace pristine::transport {

StdioTransport::StdioTransport(std::istream& input, std::ostream& output) :
    input_(input), output_(output) {}

std::optional<std::string> StdioTransport::read() {
    jsonrpc::MessageStream message_stream;
    return message_stream.read(input_);
}

void StdioTransport::write(std::string_view payload) {
    jsonrpc::MessageStream message_stream;
    message_stream.write(output_, payload);
}

} // namespace pristine::transport