#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view designGraphProviderName() {
    return "DesignGraphProvider";
}

} // namespace pristine::analysis::semantic
