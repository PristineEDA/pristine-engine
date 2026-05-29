#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view astIndexProviderName() {
    return "AstIndex";
}

} // namespace pristine::analysis::semantic
