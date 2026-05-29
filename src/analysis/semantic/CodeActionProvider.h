#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view codeActionProviderName() {
    return "CodeActionProvider";
}

} // namespace pristine::analysis::semantic
