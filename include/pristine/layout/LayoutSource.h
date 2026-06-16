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
    [[nodiscard]] virtual std::uint16_t protocolVersion() const;
    [[nodiscard]] virtual std::string_view protocolName() const;

    [[nodiscard]] virtual std::vector<std::uint8_t> encodeHelloResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeCatalogResponse() const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeGeometryResponse(
        const LayoutGeometryRequest& request) const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeTileGeometryResponse(
        const LayoutTileGeometryRequest& request) const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeHitTestResponse(
        const LayoutHitTestRequest& request) const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeInspectResponse(
        const LayoutInspectRequest& request) const;
    [[nodiscard]] virtual std::vector<std::uint8_t> encodeSelectionGeometryResponse(
        const LayoutSelectionGeometryRequest& request) const;
};

[[nodiscard]] std::shared_ptr<LayoutSource> makeDataSetLayoutSource(LayoutDataSet data,
                                                                    std::string source_kind);

[[nodiscard]] std::shared_ptr<LayoutSource> openLefDefLayoutSource(
    const std::vector<std::filesystem::path>& lef_paths,
    const std::vector<std::string>& lef_uris,
    const std::optional<std::filesystem::path>& def_path,
    const std::optional<std::string>& def_uri,
    std::string title);

[[nodiscard]] std::shared_ptr<LayoutSource> openGdsLayoutSource(
    const std::filesystem::path& gds_path,
    std::string gds_uri,
    std::string title);

} // namespace pristine::layout
