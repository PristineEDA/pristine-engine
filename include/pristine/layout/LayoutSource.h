#pragma once

#include "pristine/layout/LayoutData.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pristine::layout {

class LayoutSource {
public:
    virtual ~LayoutSource() = default;

    [[nodiscard]] virtual const LayoutDataSet& dataSet() const = 0;
    [[nodiscard]] virtual std::string_view sourceKind() const = 0;

    [[nodiscard]] virtual std::vector<std::uint8_t> encodeHelloResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeCatalogResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeGeometryResponse(
        const LayoutGeometryRequest& request) const;
};

[[nodiscard]] std::shared_ptr<LayoutSource> makeDataSetLayoutSource(LayoutDataSet data,
                                                                    std::string source_kind);

[[nodiscard]] std::shared_ptr<LayoutSource> openLefDefLayoutSource(
    const std::vector<std::filesystem::path>& lef_paths,
    const std::vector<std::string>& lef_uris,
    const std::optional<std::filesystem::path>& def_path,
    const std::optional<std::string>& def_uri,
    std::string title);

} // namespace pristine::layout
