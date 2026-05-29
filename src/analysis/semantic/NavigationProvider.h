#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view navigationProviderName() {
    return "NavigationProvider";
}

} // namespace pristine::analysis::semantic
