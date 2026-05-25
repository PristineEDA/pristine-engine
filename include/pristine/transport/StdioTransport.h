#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace pristine::transport {

class MessageTransport {
public:
    virtual ~MessageTransport() = default;

    virtual std::optional<std::string> read() = 0;
    virtual void write(std::string_view payload) = 0;
};

class StdioTransport final : public MessageTransport {
public:
    StdioTransport(std::istream& input, std::ostream& output);

    std::optional<std::string> read() override;
    void write(std::string_view payload) override;

private:
    std::istream& input_;
    std::ostream& output_;
};

} // namespace pristine::transport