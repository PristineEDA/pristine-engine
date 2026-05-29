#pragma once

#include <string_view>

namespace pristine::analysis::semantic {

[[nodiscard]] constexpr std::string_view snapshotBuilderProviderName() {
    return "SnapshotBuilder";
}

} // namespace pristine::analysis::semantic
