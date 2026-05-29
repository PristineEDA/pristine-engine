#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view completionProviderName() {
    return "CompletionProvider";
}

} // namespace pristine::analysis::semantic
