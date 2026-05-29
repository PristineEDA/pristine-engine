#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view signatureInlayProviderName() {
    return "SignatureInlayProvider";
}

} // namespace pristine::analysis::semantic
