#pragma once

#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace pristine::jsonrpc {

class MessageStream {
public:
    std::optional<std::string> read(std::istream& input) const;
    void write(std::ostream& output, std::string_view payload) const;

private:
    static std::optional<size_t> parseContentLength(std::string_view header_line);
};

} // namespace pristine::jsonrpc