#pragma once

#include "pristine/layout/LayoutData.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace pristine::layout {

struct LayoutSpatialIndexStats {
    std::uint32_t cell_count = 0;
    std::uint32_t element_count = 0;
    std::uint32_t reference_count = 0;
    std::uint64_t estimated_bytes = 0;
};

class LayoutSpatialIndex {
public:
    static std::unique_ptr<LayoutSpatialIndex> build(const LayoutDataSet& data);

    LayoutSpatialIndex();
    LayoutSpatialIndex(LayoutSpatialIndex&&) noexcept;
    LayoutSpatialIndex& operator=(LayoutSpatialIndex&&) noexcept;
    ~LayoutSpatialIndex();

    [[nodiscard]] LayoutSpatialIndexStats stats() const;
    [[nodiscard]] LayoutTileGeometryResult queryTile(const LayoutTileGeometryRequest& request) const;
    [[nodiscard]] LayoutHitTestResponse hitTest(
        const LayoutHitTestRequest& request) const;
    [[nodiscard]] LayoutInspectResult inspect(const LayoutInspectRequest& request) const;
    [[nodiscard]] LayoutTileGeometryResult selectionGeometry(
        const LayoutSelectionGeometryRequest& request) const;

private:
    class Impl;

    explicit LayoutSpatialIndex(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace pristine::layout
