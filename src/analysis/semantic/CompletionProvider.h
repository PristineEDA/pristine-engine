#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pristine::analysis::semantic {

struct CompletionContext {
    std::optional<size_t> prefix_start;
    std::optional<std::string> package_qualifier;
    std::optional<std::string> member_qualifier;
    bool macro_invocation = false;
    bool member_access = false;
    bool module_instantiation_position = false;
};

[[nodiscard]] constexpr std::string_view completionProviderName() {
    return "CompletionProvider";
}

[[nodiscard]] std::optional<size_t> completionPrefixStartOffset(std::string_view text,
                                                                int line,
                                                                int character,
                                                                std::string_view prefix);

[[nodiscard]] CompletionContext detectCompletionContext(std::string_view text,
                                                        int line,
                                                        int character,
                                                        std::string_view prefix);

} // namespace pristine::analysis::semantic
