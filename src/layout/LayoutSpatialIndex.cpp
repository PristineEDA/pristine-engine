#include "pristine/layout/LayoutSpatialIndex.h"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pristine::layout {
namespace {

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

using BoostPoint = bg::model::d2::point_xy<double>;
using BoostBox = bg::model::box<BoostPoint>;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kTileGridElementThreshold = 4096U;
constexpr std::uint32_t kTileGridMaxAxisBins = 256U;

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedMicros(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

struct GdsAffineTransform {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double tx = 0.0;
    double ty = 0.0;
};

GdsAffineTransform composeTransforms(const GdsAffineTransform& parent,
                                     const GdsAffineTransform& child);
GdsAffineTransform referenceTransform(const LayoutGdsReference& reference,
                                      std::uint32_t column,
                                      std::uint32_t row);

struct SpatialEntry {
    LayoutSpatialObjectId object{};
    LayoutRect bounds{};
    LayoutShapeKind shape_kind = LayoutShapeKind::Polygon;
    std::uint32_t layer = 0;
    std::uint32_t datatype = 0;
};

using SpatialValue = std::pair<BoostBox, SpatialEntry>;
using SpatialTree = bgi::rtree<SpatialValue, bgi::rstar<16>>;

struct GdsTileGridEntry {
    std::uint32_t element_index = kNoLayoutIndex;
    LayoutRect bounds{};
    LayoutShapeKind shape_kind = LayoutShapeKind::Polygon;
    std::uint32_t layer_index = kNoLayoutIndex;
    std::uint32_t datatype = 0;
};

struct GdsTileGridIndex {
    LayoutRect bounds{};
    std::int64_t bin_width = 1;
    std::int64_t bin_height = 1;
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    std::vector<GdsTileGridEntry> entries{};
    std::vector<std::vector<std::uint32_t>> bins{};
    std::uint64_t build_micros = 0;
};

struct GdsReferenceGridEntry {
    std::uint32_t reference_index = kNoLayoutIndex;
    LayoutRect bounds{};
};

struct GdsReferenceGridIndex {
    LayoutRect bounds{};
    std::int64_t bin_width = 1;
    std::int64_t bin_height = 1;
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    std::vector<GdsReferenceGridEntry> entries{};
    std::vector<std::vector<std::uint32_t>> bins{};
    std::uint64_t build_micros = 0;
};

struct CellSpatialIndex {
    SpatialTree elements;
    SpatialTree references;
    bool elements_built = false;
    bool references_built = false;
    mutable std::unique_ptr<GdsTileGridIndex> tile_grid;
    mutable std::unique_ptr<GdsReferenceGridIndex> reference_grid;
};

struct InstanceRecord {
    std::uint32_t cell_index = kNoLayoutIndex;
    std::uint32_t reference_index = kNoLayoutIndex;
    std::uint32_t element_index = kNoLayoutIndex;
    GdsAffineTransform transform{};
    std::string path{};
};

std::span<const LayoutPoint> gdsElementPoints(const LayoutGdsLibrary& gds,
                                              const LayoutGdsElement& element) {
    if (element.point_count > 0U && element.first_point < gds.points.size()) {
        const auto available = gds.points.size() - element.first_point;
        const auto count = std::min<std::size_t>(element.point_count, available);
        return std::span<const LayoutPoint>(gds.points.data() + element.first_point, count);
    }
    return std::span<const LayoutPoint>(element.points.data(), element.points.size());
}

bool isDrawableGdsElement(const LayoutGdsLibrary& gds, const LayoutGdsElement& element) {
    return element.kind != LayoutGdsElementKind::Sref &&
           element.kind != LayoutGdsElementKind::Aref && !gdsElementPoints(gds, element).empty();
}

LayoutShapeKind shapeKindForGdsElement(const LayoutGdsElement& element) {
    switch (element.kind) {
        case LayoutGdsElementKind::Boundary:
            return LayoutShapeKind::Polygon;
        case LayoutGdsElementKind::Path:
            return LayoutShapeKind::Path;
        case LayoutGdsElementKind::Text:
            return LayoutShapeKind::Text;
        case LayoutGdsElementKind::Sref:
        case LayoutGdsElementKind::Aref:
        case LayoutGdsElementKind::Unknown:
            return LayoutShapeKind::Polygon;
    }
    return LayoutShapeKind::Polygon;
}

std::string gdsLayerName(std::uint32_t layer, std::uint32_t datatype) {
    return "GDS:" + std::to_string(layer) + "/" + std::to_string(datatype);
}

std::uint32_t findGdsLayer(const LayoutDataSet& data,
                           std::uint32_t layer,
                           std::uint32_t datatype) {
    const auto name = gdsLayerName(layer, datatype);
    for (std::size_t index = 0; index < data.layers.size(); ++index) {
        if (data.layers[index].name == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    throw std::runtime_error("GDS layer is missing from layout catalog");
}

LayoutRect normalize(LayoutRect rect) {
    if (rect.x0 > rect.x1) {
        std::swap(rect.x0, rect.x1);
    }
    if (rect.y0 > rect.y1) {
        std::swap(rect.y0, rect.y1);
    }
    return rect;
}

bool intersects(const LayoutRect& lhs, const LayoutRect& rhs) {
    return lhs.x0 <= rhs.x1 && lhs.x1 >= rhs.x0 && lhs.y0 <= rhs.y1 && lhs.y1 >= rhs.y0;
}

LayoutRect pointBounds(std::span<const LayoutPoint> points) {
    if (points.empty()) {
        return {};
    }
    LayoutRect bounds{.x0 = points.front().x,
                      .y0 = points.front().y,
                      .x1 = points.front().x,
                      .y1 = points.front().y};
    for (const auto& point : points) {
        bounds.x0 = std::min(bounds.x0, point.x);
        bounds.y0 = std::min(bounds.y0, point.y);
        bounds.x1 = std::max(bounds.x1, point.x);
        bounds.y1 = std::max(bounds.y1, point.y);
    }
    return bounds;
}

LayoutRect elementBounds(const LayoutGdsLibrary& gds, const LayoutGdsElement& element) {
    return element.bounds.value_or(pointBounds(gdsElementPoints(gds, element)));
}

LayoutRect shapeBounds(const LayoutShape& shape) {
    if (!shape.polygon.points.empty()) {
        return pointBounds(shape.polygon.points);
    }
    return normalize(shape.rect);
}

BoostBox toBoostBox(const LayoutRect& rect) {
    const auto normalized = normalize(rect);
    return BoostBox(BoostPoint(static_cast<double>(normalized.x0),
                               static_cast<double>(normalized.y0)),
                    BoostPoint(static_cast<double>(normalized.x1),
                               static_cast<double>(normalized.y1)));
}

LayoutPoint applyTransform(const GdsAffineTransform& transform, const LayoutPoint& point) {
    return LayoutPoint{.x = static_cast<std::int64_t>(
                           std::llround(transform.a * static_cast<double>(point.x) +
                                        transform.c * static_cast<double>(point.y) + transform.tx)),
                       .y = static_cast<std::int64_t>(
                           std::llround(transform.b * static_cast<double>(point.x) +
                                        transform.d * static_cast<double>(point.y) + transform.ty))};
}

LayoutRect transformBounds(const GdsAffineTransform& transform, const LayoutRect& rect) {
    const auto normalized = normalize(rect);
    std::vector<LayoutPoint> points;
    points.reserve(4);
    points.push_back(applyTransform(transform, LayoutPoint{.x = normalized.x0, .y = normalized.y0}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = normalized.x1, .y = normalized.y0}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = normalized.x1, .y = normalized.y1}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = normalized.x0, .y = normalized.y1}));
    return pointBounds(points);
}

LayoutRect mergeBounds(LayoutRect bounds, const LayoutRect& rect) {
    bounds.x0 = std::min(bounds.x0, rect.x0);
    bounds.y0 = std::min(bounds.y0, rect.y0);
    bounds.x1 = std::max(bounds.x1, rect.x1);
    bounds.y1 = std::max(bounds.y1, rect.y1);
    return bounds;
}

struct IndexRange {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    bool empty = false;
};

struct ArrayVisibleRange {
    IndexRange columns{};
    IndexRange rows{};
};

bool axisIntersects(double min_value, double max_value, double query_min, double query_max) {
    return min_value <= query_max && max_value >= query_min;
}

IndexRange axisIndexRange(double base_min,
                          double base_max,
                          double step,
                          std::uint32_t count,
                          double query_min,
                          double query_max) {
    if (count == 0U) {
        return IndexRange{.empty = true};
    }
    constexpr double kEpsilon = 1e-9;
    if (std::abs(step) < kEpsilon) {
        if (!axisIntersects(base_min, base_max, query_min, query_max)) {
            return IndexRange{.empty = true};
        }
        return IndexRange{.first = 0, .last = count - 1U};
    }

    double lower = 0.0;
    double upper = 0.0;
    if (step > 0.0) {
        lower = (query_min - base_max) / step;
        upper = (query_max - base_min) / step;
    }
    else {
        const auto positive_step = -step;
        lower = (base_min - query_max) / positive_step;
        upper = (base_max - query_min) / positive_step;
    }

    auto first = static_cast<std::int64_t>(std::ceil(lower - kEpsilon));
    auto last = static_cast<std::int64_t>(std::floor(upper + kEpsilon));
    first = std::max<std::int64_t>(first, 0);
    last = std::min<std::int64_t>(last, static_cast<std::int64_t>(count) - 1);
    if (first > last) {
        return IndexRange{.empty = true};
    }
    return IndexRange{.first = static_cast<std::uint32_t>(first),
                      .last = static_cast<std::uint32_t>(last)};
}

std::optional<ArrayVisibleRange> visibleArrayRange(const LayoutTileGeometryRequest& request,
                                                   const GdsAffineTransform& parent_transform,
                                                   const LayoutGdsReference& reference,
                                                   const LayoutRect& target_bounds,
                                                   std::uint32_t columns,
                                                   std::uint32_t rows) {
    if (!request.has_bbox) {
        return ArrayVisibleRange{.columns = IndexRange{.first = 0, .last = columns - 1U},
                                 .rows = IndexRange{.first = 0, .last = rows - 1U}};
    }

    const auto base_transform =
        composeTransforms(parent_transform, referenceTransform(reference, 0U, 0U));
    const auto base_bounds = normalize(transformBounds(base_transform, target_bounds));
    const auto col_dx = parent_transform.a * static_cast<double>(reference.column_vector.x) +
                        parent_transform.c * static_cast<double>(reference.column_vector.y);
    const auto col_dy = parent_transform.b * static_cast<double>(reference.column_vector.x) +
                        parent_transform.d * static_cast<double>(reference.column_vector.y);
    const auto row_dx = parent_transform.a * static_cast<double>(reference.row_vector.x) +
                        parent_transform.c * static_cast<double>(reference.row_vector.y);
    const auto row_dy = parent_transform.b * static_cast<double>(reference.row_vector.x) +
                        parent_transform.d * static_cast<double>(reference.row_vector.y);
    constexpr double kAxisEpsilon = 1e-9;
    const auto bbox = normalize(request.bbox);

    ArrayVisibleRange range{.columns = IndexRange{.first = 0, .last = columns - 1U},
                            .rows = IndexRange{.first = 0, .last = rows - 1U}};
    if (std::abs(col_dy) < kAxisEpsilon && std::abs(row_dx) < kAxisEpsilon) {
        range.columns = axisIndexRange(static_cast<double>(base_bounds.x0),
                                       static_cast<double>(base_bounds.x1),
                                       col_dx,
                                       columns,
                                       static_cast<double>(bbox.x0),
                                       static_cast<double>(bbox.x1));
        range.rows = axisIndexRange(static_cast<double>(base_bounds.y0),
                                    static_cast<double>(base_bounds.y1),
                                    row_dy,
                                    rows,
                                    static_cast<double>(bbox.y0),
                                    static_cast<double>(bbox.y1));
    }
    else if (std::abs(col_dx) < kAxisEpsilon && std::abs(row_dy) < kAxisEpsilon) {
        range.columns = axisIndexRange(static_cast<double>(base_bounds.y0),
                                       static_cast<double>(base_bounds.y1),
                                       col_dy,
                                       columns,
                                       static_cast<double>(bbox.y0),
                                       static_cast<double>(bbox.y1));
        range.rows = axisIndexRange(static_cast<double>(base_bounds.x0),
                                    static_cast<double>(base_bounds.x1),
                                    row_dx,
                                    rows,
                                    static_cast<double>(bbox.x0),
                                    static_cast<double>(bbox.x1));
    }
    else {
        return std::nullopt;
    }

    if (range.columns.empty || range.rows.empty) {
        return ArrayVisibleRange{.columns = IndexRange{.empty = true},
                                 .rows = IndexRange{.empty = true}};
    }
    return range;
}

std::optional<GdsAffineTransform> inverseTransform(const GdsAffineTransform& transform) {
    const auto determinant = transform.a * transform.d - transform.b * transform.c;
    if (std::abs(determinant) < 1e-12) {
        return std::nullopt;
    }
    const auto inv_det = 1.0 / determinant;
    GdsAffineTransform inverse{.a = transform.d * inv_det,
                               .b = -transform.b * inv_det,
                               .c = -transform.c * inv_det,
                               .d = transform.a * inv_det};
    inverse.tx = -(inverse.a * transform.tx + inverse.c * transform.ty);
    inverse.ty = -(inverse.b * transform.tx + inverse.d * transform.ty);
    return inverse;
}

GdsAffineTransform composeTransforms(const GdsAffineTransform& parent,
                                     const GdsAffineTransform& child) {
    return GdsAffineTransform{.a = parent.a * child.a + parent.c * child.b,
                              .b = parent.b * child.a + parent.d * child.b,
                              .c = parent.a * child.c + parent.c * child.d,
                              .d = parent.b * child.c + parent.d * child.d,
                              .tx = parent.a * child.tx + parent.c * child.ty + parent.tx,
                              .ty = parent.b * child.tx + parent.d * child.ty + parent.ty};
}

GdsAffineTransform referenceTransform(const LayoutGdsReference& reference,
                                      std::uint32_t column,
                                      std::uint32_t row) {
    constexpr double kPi = 3.14159265358979323846;
    const auto magnification = reference.transform.magnification == 0.0
        ? 1.0
        : reference.transform.magnification;
    const auto radians = reference.transform.angle * kPi / 180.0;
    const auto cos_angle = std::cos(radians);
    const auto sin_angle = std::sin(radians);
    const auto reflect = reference.transform.reflected ? -1.0 : 1.0;
    const auto dx = static_cast<double>(reference.column_vector.x) * static_cast<double>(column) +
                    static_cast<double>(reference.row_vector.x) * static_cast<double>(row);
    const auto dy = static_cast<double>(reference.column_vector.y) * static_cast<double>(column) +
                    static_cast<double>(reference.row_vector.y) * static_cast<double>(row);
    return GdsAffineTransform{.a = magnification * cos_angle,
                              .b = magnification * sin_angle,
                              .c = -magnification * sin_angle * reflect,
                              .d = magnification * cos_angle * reflect,
                              .tx = static_cast<double>(reference.origin.x) + dx,
                              .ty = static_cast<double>(reference.origin.y) + dy};
}

std::uint64_t mixPathHash(std::uint64_t hash, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t mixPathHash(std::uint64_t hash, std::string_view value) {
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string childInstancePath(std::string_view parent,
                              std::uint32_t reference_index,
                              std::uint32_t column,
                              std::uint32_t row) {
    std::string result(parent);
    result += "/ref:";
    result += std::to_string(reference_index);
    result += "[";
    result += std::to_string(column);
    result += ",";
    result += std::to_string(row);
    result += "]";
    return result;
}

bool containsAsciiCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset <= haystack.size() - needle.size(); ++offset) {
        bool matched = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            const auto lhs = static_cast<char>(
                std::tolower(static_cast<unsigned char>(haystack[offset + index])));
            const auto rhs =
                static_cast<char>(std::tolower(static_cast<unsigned char>(needle[index])));
            if (lhs != rhs) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

bool containsLayer(const std::vector<std::uint32_t>& layers, std::uint32_t layer_index) {
    return layers.empty() ||
           std::find(layers.begin(), layers.end(), layer_index) != layers.end();
}

bool containsKind(const std::vector<LayoutShapeKind>& kinds, LayoutShapeKind kind) {
    return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool containsDatatype(const std::vector<std::uint32_t>& datatypes, std::uint32_t datatype) {
    return datatypes.empty() ||
           std::find(datatypes.begin(), datatypes.end(), datatype) != datatypes.end();
}

void incrementCounter(std::uint32_t& counter, std::size_t amount = 1) {
    const auto max_value = std::numeric_limits<std::uint32_t>::max();
    if (counter > max_value - std::min<std::size_t>(amount, max_value)) {
        counter = max_value;
        return;
    }
    counter += static_cast<std::uint32_t>(std::min<std::size_t>(amount, max_value));
}

std::uint32_t saturatingU32(std::size_t value) {
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t ceilSqrtU32(std::size_t value) {
    if (value <= 1U) {
        return 1U;
    }
    auto root = static_cast<std::uint32_t>(
        std::ceil(std::sqrt(static_cast<double>(value))));
    return std::max<std::uint32_t>(1U, root);
}

bool pointOnSegment(const LayoutPoint& point,
                    const LayoutPoint& a,
                    const LayoutPoint& b,
                    double radius) {
    const auto px = static_cast<double>(point.x);
    const auto py = static_cast<double>(point.y);
    const auto ax = static_cast<double>(a.x);
    const auto ay = static_cast<double>(a.y);
    const auto bx = static_cast<double>(b.x);
    const auto by = static_cast<double>(b.y);
    const auto dx = bx - ax;
    const auto dy = by - ay;
    const auto length2 = dx * dx + dy * dy;
    if (length2 == 0.0) {
        const auto dist2 = (px - ax) * (px - ax) + (py - ay) * (py - ay);
        return dist2 <= radius * radius;
    }
    const auto t = std::clamp(((px - ax) * dx + (py - ay) * dy) / length2, 0.0, 1.0);
    const auto cx = ax + t * dx;
    const auto cy = ay + t * dy;
    const auto dist2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);
    return dist2 <= radius * radius;
}

bool pointInPolygon(const LayoutPoint& point, const std::vector<LayoutPoint>& polygon) {
    if (polygon.size() < 3) {
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto yi = static_cast<double>(polygon[i].y);
        const auto yj = static_cast<double>(polygon[j].y);
        const auto xi = static_cast<double>(polygon[i].x);
        const auto xj = static_cast<double>(polygon[j].x);
        const auto py = static_cast<double>(point.y);
        const auto px = static_cast<double>(point.x);
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi == 0.0 ? 1.0 : yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

double rectDistance(const LayoutPoint& point, const LayoutRect& rect) {
    const auto normalized = normalize(rect);
    const auto px = static_cast<double>(point.x);
    const auto py = static_cast<double>(point.y);
    const auto dx = std::max({static_cast<double>(normalized.x0) - px,
                              0.0,
                              px - static_cast<double>(normalized.x1)});
    const auto dy = std::max({static_cast<double>(normalized.y0) - py,
                              0.0,
                              py - static_cast<double>(normalized.y1)});
    return std::sqrt(dx * dx + dy * dy);
}

class TraversalBudget {
public:
    explicit TraversalBudget(const LayoutTileGeometryRequest& request) :
        max_shapes_(request.max_shapes),
        max_points_(request.max_points),
        skip_(request.continuation_token) {}

    bool shouldSkip() {
        if (skip_ == 0) {
            return false;
        }
        --skip_;
        ++seen_;
        return true;
    }

    bool canAppend(const LayoutShape& shape) {
        if (max_shapes_ != 0 && appended_ >= max_shapes_) {
            truncated_ = true;
            return false;
        }
        if (max_points_ != 0 &&
            point_count_ + shape.polygon.points.size() > static_cast<std::size_t>(max_points_)) {
            truncated_ = true;
            return false;
        }
        return true;
    }

    void appended(const LayoutShape& shape) {
        ++appended_;
        ++seen_;
        point_count_ += shape.polygon.points.size();
    }

    [[nodiscard]] bool truncated() const { return truncated_; }

    [[nodiscard]] bool bounded() const { return max_shapes_ != 0 || max_points_ != 0; }

    [[nodiscard]] std::uint32_t nextToken() const {
        if (!truncated_ || seen_ > std::numeric_limits<std::uint32_t>::max()) {
            return 0;
        }
        return static_cast<std::uint32_t>(seen_);
    }

private:
    std::uint32_t max_shapes_ = 0;
    std::uint32_t max_points_ = 0;
    std::uint32_t skip_ = 0;
    std::size_t appended_ = 0;
    std::size_t seen_ = 0;
    std::size_t point_count_ = 0;
    bool truncated_ = false;
};

} // namespace

class LayoutSpatialIndex::Impl {
public:
    explicit Impl(const LayoutDataSet& data) : data_(&data) {
        if (!data_->gds.has_value()) {
            return;
        }
        const auto& gds = *data_->gds;
        cells_.resize(gds.cells.size());
    }

    [[nodiscard]] LayoutSpatialIndexStats stats() const {
        LayoutSpatialIndexStats result;
        if (data_ == nullptr || !data_->gds.has_value()) {
            return result;
        }
        result.cell_count = static_cast<std::uint32_t>(cells_.size());
        for (const auto& cell : cells_) {
            result.element_count += static_cast<std::uint32_t>(cell.elements.size());
            result.reference_count += static_cast<std::uint32_t>(cell.references.size());
        }
        result.estimated_bytes =
            static_cast<std::uint64_t>(result.element_count + result.reference_count) *
            static_cast<std::uint64_t>(sizeof(SpatialValue) * 2U);
        return result;
    }

    [[nodiscard]] LayoutTileGeometryResult queryTile(
        const LayoutTileGeometryRequest& request) const {
        const auto start = Clock::now();
        const auto& gds = requireGds();
        validateRoot(request.root_cell_index, gds);
        LayoutTileGeometryResult result;
        TraversalBudget budget(request);
        std::set<std::uint32_t> stack;
        const auto root_path =
            std::string("cell:") + std::to_string(request.root_cell_index) + ":" +
            gds.cells[request.root_cell_index].name;
        const auto root_hash = mixPathHash(kFnvOffset, root_path);
        walkCell(request,
                 request.root_cell_index,
                 GdsAffineTransform{},
                 root_hash,
                 root_path,
                 stack,
                 budget,
                 result);
        result.truncated = budget.truncated();
        result.next_token = budget.nextToken();
        result.query_micros = elapsedMicros(start);
        return result;
    }

    void warmupTopCell() const {
        const auto& gds = requireGds();
        if (gds.top_cell_index >= gds.cells.size()) {
            return;
        }
        LayoutTileGeometryResult scratch;
        const auto& cell = gds.cells[gds.top_cell_index];
        if (!cell.reference_indices.empty()) {
            (void)ensureReferenceGridBuilt(gds.top_cell_index, scratch);
        }
        if (!cell.element_indices.empty()) {
            (void)ensureTileGridBuilt(gds.top_cell_index, scratch);
        }
    }

    [[nodiscard]] LayoutHitTestResponse hitTest(
        const LayoutHitTestRequest& request) const {
        const auto start = Clock::now();
        const auto& gds = requireGds();
        validateRoot(request.root_cell_index, gds);
        LayoutHitTestResponse response;
        LayoutTileGeometryRequest tile_request;
        tile_request.root_cell_index = request.root_cell_index;
        tile_request.has_bbox = true;
        tile_request.bbox = LayoutRect{.x0 = request.point.x - request.radius,
                                       .y0 = request.point.y - request.radius,
                                       .x1 = request.point.x + request.radius,
                                       .y1 = request.point.y + request.radius};
        tile_request.layer_indices = request.layer_indices;
        tile_request.shape_kinds = request.shape_kinds;
        tile_request.datatypes = request.datatypes;
        const auto candidate_limit =
            std::max<std::uint32_t>(256U, std::max<std::uint32_t>(request.max_results, 1U) * 32U);
        tile_request.max_shapes = candidate_limit;
        tile_request.max_points = candidate_limit * 16U;
        auto tile = queryTile(tile_request);
        response.tile_shape_count = saturatingU32(tile.shapes.size());
        for (const auto& shape : tile.shapes) {
            incrementCounter(response.precise_candidate_count);
            const auto bounds = shapeBounds(shape);
            bool precise = false;
            if (shape.kind == LayoutShapeKind::Polygon) {
                precise = pointInPolygon(request.point, shape.polygon.points);
            }
            else if (shape.kind == LayoutShapeKind::Path && shape.polygon.points.size() >= 2) {
                for (std::size_t index = 1; index < shape.polygon.points.size(); ++index) {
                    if (pointOnSegment(request.point,
                                       shape.polygon.points[index - 1U],
                                       shape.polygon.points[index],
                                       static_cast<double>(std::max<std::int64_t>(request.radius, 1)))) {
                        precise = true;
                        break;
                    }
                }
            }
            else {
                precise = rectDistance(request.point, bounds) <=
                          static_cast<double>(std::max<std::int64_t>(request.radius, 1));
            }
            if (!precise) {
                continue;
            }
            LayoutHitTestResult hit;
            hit.object = LayoutSpatialObjectId{.kind = shape.owner_kind ==
                                                       LayoutOwnerKind::GdsReference
                                                   ? LayoutSpatialObjectKind::Reference
                                                   : LayoutSpatialObjectKind::Element,
                                               .cell_index = shape.macro_index,
                                               .reference_index = shape.owner_kind ==
                                                       LayoutOwnerKind::GdsReference
                                                   ? shape.owner_index
                                                   : kNoLayoutIndex,
                                               .element_index = shape.owner_kind ==
                                                       LayoutOwnerKind::GdsElement
                                                   ? shape.owner_index
                                                   : kNoLayoutIndex,
                                               .layer_index = shape.layer_index,
                                               .datatype = shape.datatype,
                                               .instance_path_hash = shape.instance_path_hash};
            hit.bounds = bounds;
            hit.rank = shape.owner_kind == LayoutOwnerKind::GdsReference ? 3U
                : shape.kind == LayoutShapeKind::Polygon                ? 0U
                : shape.kind == LayoutShapeKind::Path                   ? 1U
                                                                         : 2U;
            hit.distance = rectDistance(request.point, bounds);
            response.hits.push_back(hit);
        }
        if (request.layer_indices.empty() && request.datatypes.empty() &&
            containsKind(request.shape_kinds, LayoutShapeKind::Placement)) {
            tile_request.lod = 2;
            auto reference_tile = queryTile(tile_request);
            incrementCounter(response.tile_shape_count, reference_tile.shapes.size());
            for (const auto& shape : reference_tile.shapes) {
                incrementCounter(response.precise_candidate_count);
                const auto bounds = shapeBounds(shape);
                if (rectDistance(request.point, bounds) >
                    static_cast<double>(std::max<std::int64_t>(request.radius, 1))) {
                    continue;
                }
                LayoutHitTestResult hit;
                hit.object = LayoutSpatialObjectId{.kind = LayoutSpatialObjectKind::Reference,
                                                   .cell_index = shape.macro_index,
                                                   .reference_index = shape.owner_index,
                                                   .element_index = kNoLayoutIndex,
                                                   .layer_index = kNoLayoutIndex,
                                                   .datatype = 0,
                                                   .instance_path_hash = shape.instance_path_hash};
                hit.bounds = bounds;
                hit.rank = 3U;
                hit.distance = rectDistance(request.point, bounds);
                response.hits.push_back(hit);
            }
        }
        std::sort(response.hits.begin(), response.hits.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.rank != rhs.rank) {
                return lhs.rank < rhs.rank;
            }
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }
            return lhs.object.element_index < rhs.object.element_index;
        });
        if (request.max_results != 0 && response.hits.size() > request.max_results) {
            response.hits.resize(request.max_results);
        }
        response.query_micros = elapsedMicros(start);
        return response;
    }

    [[nodiscard]] LayoutInspectResult inspect(const LayoutInspectRequest& request) const {
        const auto& gds = requireGds();
        LayoutInspectResult result;
        result.object = request.object;
        switch (request.object.kind) {
            case LayoutSpatialObjectKind::Cell: {
                if (request.object.cell_index >= gds.cells.size()) {
                    throw std::runtime_error("Layout inspect cell index is out of range");
                }
                const auto& cell = gds.cells[request.object.cell_index];
                result.name = cell.name;
                result.object_class = LayoutInspectClass::Cell;
                result.source_cell_index = request.object.cell_index;
                if (cell.bounds.has_value()) {
                    result.bounds = *cell.bounds;
                }
                break;
            }
            case LayoutSpatialObjectKind::Reference: {
                if (request.object.reference_index >= gds.references.size()) {
                    throw std::runtime_error("Layout inspect reference index is out of range");
                }
                const auto& reference = gds.references[request.object.reference_index];
                result.gds_reference_kind = reference.kind;
                result.name = reference.target_name;
                result.object_class = LayoutInspectClass::Cell;
                result.object.cell_index = reference.parent_cell_index;
                result.source_cell_index = reference.target_cell_index;
                if (reference.target_cell_index < gds.cells.size()) {
                    const auto& target = gds.cells[reference.target_cell_index];
                    if (target.bounds.has_value()) {
                        if (request.object.instance_path_hash != 0U) {
                            const auto instance = requireInstance(request.object.instance_path_hash);
                            result.instance_path = instance.path;
                            result.bounds = transformBounds(instance.transform, *target.bounds);
                        }
                        else {
                            result.bounds = transformBounds(referenceTransform(reference, 0, 0),
                                                            *target.bounds);
                        }
                    }
                }
                break;
            }
            case LayoutSpatialObjectKind::Element: {
                if (request.object.element_index >= gds.elements.size()) {
                    throw std::runtime_error("Layout inspect element index is out of range");
                }
                const auto& element = gds.elements[request.object.element_index];
                result.gds_element_kind = element.kind;
                result.object.cell_index = element.cell_index;
                result.source_cell_index = element.cell_index;
                result.object_class = classifyElement(element);
                result.layer = element.layer;
                result.datatype = element.datatype;
                result.texttype = element.texttype;
                result.text = element.text;
                if (request.object.instance_path_hash != 0U) {
                    const auto instance = requireInstance(request.object.instance_path_hash);
                    result.instance_path = instance.path;
                    result.bounds = transformBounds(instance.transform, elementBounds(gds, element));
                }
                else {
                    result.bounds = elementBounds(gds, element);
                }
                break;
            }
            case LayoutSpatialObjectKind::Unknown:
                throw std::runtime_error("Layout inspect object kind is unknown");
        }
        return result;
    }

    [[nodiscard]] LayoutTileGeometryResult selectionGeometry(
        const LayoutSelectionGeometryRequest& request) const {
        const auto start = Clock::now();
        const auto& gds = requireGds();
        LayoutTileGeometryResult result;
        if (request.object.kind == LayoutSpatialObjectKind::Element) {
            if (request.object.element_index >= gds.elements.size()) {
                throw std::runtime_error("Layout selection element index is out of range");
            }
            const auto& element = gds.elements[request.object.element_index];
            if (!isDrawableGdsElement(gds, element)) {
                return {};
            }
            GdsAffineTransform transform{};
            std::uint64_t path_hash = kFnvOffset;
            std::string path = "definition";
            if (request.object.instance_path_hash != 0U) {
                const auto instance = requireInstance(request.object.instance_path_hash);
                transform = instance.transform;
                path_hash = request.object.instance_path_hash;
                path = instance.path;
            }
            result.shapes.push_back(
                makeElementShape(element, transform, request.object.element_index, path_hash, path));
        }
        else if (request.object.kind == LayoutSpatialObjectKind::Reference) {
            if (request.object.reference_index >= gds.references.size()) {
                throw std::runtime_error("Layout selection reference index is out of range");
            }
            const auto& reference = gds.references[request.object.reference_index];
            if (reference.target_cell_index >= gds.cells.size() ||
                !gds.cells[reference.target_cell_index].bounds.has_value()) {
                return {};
            }
            GdsAffineTransform transform = referenceTransform(reference, 0U, 0U);
            std::uint64_t path_hash = kFnvOffset;
            std::string path = "reference";
            if (request.object.instance_path_hash != 0U) {
                const auto instance = requireInstance(request.object.instance_path_hash);
                transform = instance.transform;
                path_hash = request.object.instance_path_hash;
                path = instance.path;
            }
            result.shapes.push_back(makeReferenceShape(reference,
                                                       *gds.cells[reference.target_cell_index].bounds,
                                                       transform,
                                                       request.object.reference_index,
                                                       path_hash,
                                                       path));
        }
        else if (request.object.kind == LayoutSpatialObjectKind::Cell) {
            if (request.object.cell_index >= gds.cells.size()) {
                throw std::runtime_error("Layout selection cell index is out of range");
            }
            const auto& cell = gds.cells[request.object.cell_index];
            if (cell.bounds.has_value()) {
                result.shapes.push_back(LayoutShape{.kind = LayoutShapeKind::Placement,
                                                    .owner_kind = LayoutOwnerKind::GdsCell,
                                                    .owner_index = request.object.cell_index,
                                                    .macro_index = request.object.cell_index,
                                                    .layer_index = kNoLayoutIndex,
                                                    .rect = *cell.bounds});
            }
        }
        else {
            throw std::runtime_error("Layout selection object kind is unknown");
        }
        result.query_micros = elapsedMicros(start);
        return result;
    }

private:
    [[nodiscard]] const LayoutGdsLibrary& requireGds() const {
        if (data_ == nullptr || !data_->gds.has_value()) {
            throw std::runtime_error("Layout spatial index requires a GDS layout source");
        }
        return *data_->gds;
    }

    static void validateRoot(std::uint32_t root_cell_index, const LayoutGdsLibrary& gds) {
        if (root_cell_index >= gds.cells.size()) {
            throw std::runtime_error("Layout tile root cell index is out of range");
        }
    }

    [[nodiscard]] std::uint64_t rememberInstance(std::uint64_t hash,
                                                 std::uint32_t cell_index,
                                                 std::uint32_t reference_index,
                                                 std::uint32_t element_index,
                                                 const GdsAffineTransform& transform,
                                                 std::string_view path) const {
        if (hash == 0U) {
            hash = mixPathHash(kFnvOffset, path);
        }
        std::lock_guard lock(instance_mutex_);
        std::uint32_t salt = 0;
        for (;;) {
            const auto found = instance_records_.find(hash);
            if (found == instance_records_.end()) {
                instance_records_.emplace(hash,
                                          InstanceRecord{.cell_index = cell_index,
                                                         .reference_index = reference_index,
                                                         .element_index = element_index,
                                                         .transform = transform,
                                                         .path = std::string(path)});
                return hash;
            }
            if (found->second.path == path && found->second.cell_index == cell_index) {
                return hash;
            }
            ++salt;
            hash = mixPathHash(hash, salt);
        }
    }

    [[nodiscard]] InstanceRecord requireInstance(std::uint64_t hash) const {
        std::lock_guard lock(instance_mutex_);
        const auto found = instance_records_.find(hash);
        if (found == instance_records_.end()) {
            throw std::runtime_error("Layout spatial object instance id is stale");
        }
        return found->second;
    }

    [[nodiscard]] LayoutInspectClass classifyElement(const LayoutGdsElement& element) const {
        if (element.kind == LayoutGdsElementKind::Path) {
            return LayoutInspectClass::Wire;
        }
        if (element.kind == LayoutGdsElementKind::Text) {
            return LayoutInspectClass::Label;
        }
        const auto& gds = requireGds();
        const auto cell_name = element.cell_index < gds.cells.size()
            ? std::string_view(gds.cells[element.cell_index].name)
            : std::string_view{};
        const auto layer_name = data_->layers[layerIndexFor(element)].name;
        if (containsAsciiCaseInsensitive(cell_name, "pad") ||
            containsAsciiCaseInsensitive(cell_name, "io") ||
            containsAsciiCaseInsensitive(layer_name, "pad")) {
            return LayoutInspectClass::Pad;
        }
        return LayoutInspectClass::Shape;
    }

    [[nodiscard]] LayoutShape makeReferenceShape(const LayoutGdsReference& reference,
                                                 const LayoutRect& target_bounds,
                                                 const GdsAffineTransform& transform,
                                                 std::uint32_t reference_index,
                                                 std::uint64_t path_hash,
                                                 std::string_view path) const {
        const auto stable_hash = rememberInstance(path_hash,
                                                  reference.target_cell_index,
                                                  reference_index,
                                                  kNoLayoutIndex,
                                                  transform,
                                                  path);
        return LayoutShape{.kind = LayoutShapeKind::Placement,
                           .owner_kind = LayoutOwnerKind::GdsReference,
                           .owner_index = reference_index,
                           .macro_index = reference.target_cell_index,
                           .layer_index = kNoLayoutIndex,
                           .flags = 0,
                           .datatype = 0,
                           .instance_path_hash = stable_hash,
                           .rect = transformBounds(transform, target_bounds)};
    }

    [[nodiscard]] LayoutShape makeReferenceShapeForBounds(const LayoutGdsReference& reference,
                                                          const LayoutRect& world_bounds,
                                                          const GdsAffineTransform& transform,
                                                          std::uint32_t reference_index,
                                                          std::uint64_t path_hash,
                                                          std::string_view path) const {
        const auto stable_hash = rememberInstance(path_hash,
                                                  reference.target_cell_index,
                                                  reference_index,
                                                  kNoLayoutIndex,
                                                  transform,
                                                  path);
        return LayoutShape{.kind = LayoutShapeKind::Placement,
                           .owner_kind = LayoutOwnerKind::GdsReference,
                           .owner_index = reference_index,
                           .macro_index = reference.target_cell_index,
                           .layer_index = kNoLayoutIndex,
                           .flags = 0,
                           .datatype = 0,
                           .instance_path_hash = stable_hash,
                           .rect = world_bounds};
    }

    [[nodiscard]] LayoutShape makeCellOverviewShape(std::uint32_t cell_index,
                                                    const LayoutRect& cell_bounds,
                                                    const GdsAffineTransform& transform,
                                                    std::uint64_t path_hash,
                                                    std::string_view path) const {
        const auto stable_hash = rememberInstance(path_hash,
                                                  cell_index,
                                                  kNoLayoutIndex,
                                                  kNoLayoutIndex,
                                                  transform,
                                                  path);
        return LayoutShape{.kind = LayoutShapeKind::Placement,
                           .owner_kind = LayoutOwnerKind::GdsCell,
                           .owner_index = cell_index,
                           .macro_index = cell_index,
                           .layer_index = kNoLayoutIndex,
                           .flags = 0,
                           .datatype = 0,
                           .instance_path_hash = stable_hash,
                           .rect = transformBounds(transform, cell_bounds)};
    }

    [[nodiscard]] LayoutRect visibleReferenceBounds(const LayoutGdsReference& reference,
                                                    const LayoutRect& target_bounds,
                                                    const GdsAffineTransform& parent_transform,
                                                    std::uint32_t first_column,
                                                    std::uint32_t last_column,
                                                    std::uint32_t first_row,
                                                    std::uint32_t last_row) const {
        std::optional<LayoutRect> bounds;
        const auto expandInstance = [&](std::uint32_t column, std::uint32_t row) {
            const auto child_transform =
                composeTransforms(parent_transform, referenceTransform(reference, column, row));
            const auto transformed = transformBounds(child_transform, target_bounds);
            bounds = bounds.has_value() ? mergeBounds(*bounds, transformed) : transformed;
        };
        expandInstance(first_column, first_row);
        if (first_column != last_column) {
            expandInstance(last_column, first_row);
        }
        if (first_row != last_row) {
            expandInstance(first_column, last_row);
        }
        if (first_column != last_column && first_row != last_row) {
            expandInstance(last_column, last_row);
        }
        return bounds.value_or(transformBounds(
            composeTransforms(parent_transform, referenceTransform(reference, first_column, first_row)),
            target_bounds));
    }

    void ensureElementsBuilt(std::uint32_t cell_index) const {
        auto& index = cells_[cell_index];
        if (index.elements_built) {
            return;
        }
        const auto& gds = requireGds();
        const auto& cell = gds.cells[cell_index];
        std::vector<SpatialValue> element_values;
        element_values.reserve(cell.element_indices.size());

        for (const auto element_index : cell.element_indices) {
            if (element_index >= gds.elements.size()) {
                continue;
            }
            const auto& element = gds.elements[element_index];
            if (!isDrawableGdsElement(gds, element)) {
                continue;
            }
            const auto bounds = elementBounds(gds, element);
            const auto layer_index = layerIndexFor(element);
            SpatialEntry entry{.object = LayoutSpatialObjectId{.kind =
                                                                   LayoutSpatialObjectKind::Element,
                                                               .cell_index = element.cell_index,
                                                               .reference_index = kNoLayoutIndex,
                                                               .element_index = element_index,
                                                               .layer_index = layer_index,
                                                               .datatype = element.datatype,
                                                               .instance_path_hash = 0},
                               .bounds = bounds,
                               .shape_kind = shapeKindForGdsElement(element),
                               .layer = element.layer,
                               .datatype = element.datatype};
            element_values.emplace_back(toBoostBox(bounds), std::move(entry));
        }

        index.elements = SpatialTree(element_values.begin(), element_values.end());
        index.elements_built = true;
    }

    void ensureReferencesBuilt(std::uint32_t cell_index) const {
        auto& index = cells_[cell_index];
        if (index.references_built) {
            return;
        }
        const auto& gds = requireGds();
        const auto& cell = gds.cells[cell_index];
        std::vector<SpatialValue> reference_values;
        reference_values.reserve(cell.reference_indices.size());

        for (const auto reference_index : cell.reference_indices) {
            if (reference_index >= gds.references.size()) {
                continue;
            }
            const auto& reference = gds.references[reference_index];
            if (reference.target_cell_index >= gds.cells.size()) {
                continue;
            }
            const auto& target = gds.cells[reference.target_cell_index];
            if (!target.bounds.has_value()) {
                continue;
            }
            std::optional<LayoutRect> bounds;
            const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
            const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
            for (const auto column : std::array<std::uint32_t, 2>{0U, columns - 1U}) {
                for (const auto row : std::array<std::uint32_t, 2>{0U, rows - 1U}) {
                    const auto transformed =
                        transformBounds(referenceTransform(reference, column, row), *target.bounds);
                    if (!bounds.has_value()) {
                        bounds = transformed;
                    }
                    else {
                        bounds->x0 = std::min(bounds->x0, transformed.x0);
                        bounds->y0 = std::min(bounds->y0, transformed.y0);
                        bounds->x1 = std::max(bounds->x1, transformed.x1);
                        bounds->y1 = std::max(bounds->y1, transformed.y1);
                    }
                }
            }
            if (!bounds.has_value()) {
                continue;
            }
            SpatialEntry entry{.object = LayoutSpatialObjectId{.kind =
                                                                   LayoutSpatialObjectKind::Reference,
                                                               .cell_index = reference.parent_cell_index,
                                                               .reference_index = reference_index,
                                                               .element_index = kNoLayoutIndex,
                                                               .layer_index = kNoLayoutIndex,
                                                               .datatype = 0,
                                                               .instance_path_hash = 0},
                               .bounds = *bounds};
            reference_values.emplace_back(toBoostBox(*bounds), std::move(entry));
        }

        index.references = SpatialTree(reference_values.begin(), reference_values.end());
        index.references_built = true;
    }

    [[nodiscard]] GdsTileGridIndex& ensureTileGridBuilt(std::uint32_t cell_index,
                                                        LayoutTileGeometryResult& result) const {
        auto& index = cells_[cell_index];
        if (index.tile_grid != nullptr) {
            incrementCounter(result.grid_hit_count);
            return *index.tile_grid;
        }

        incrementCounter(result.grid_miss_count);
        const auto start = Clock::now();
        const auto& gds = requireGds();
        const auto& cell = gds.cells[cell_index];
        auto grid = std::make_unique<GdsTileGridIndex>();
        grid->bounds = normalize(cell.bounds.value_or(LayoutRect{}));
        const auto width = std::max<std::int64_t>(1, grid->bounds.x1 - grid->bounds.x0 + 1);
        const auto height = std::max<std::int64_t>(1, grid->bounds.y1 - grid->bounds.y0 + 1);
        const auto target_axis = std::clamp<std::uint32_t>(
            ceilSqrtU32(cell.element_indices.size() / 64U + 1U), 16U, kTileGridMaxAxisBins);
        grid->columns = target_axis;
        grid->rows = target_axis;
        grid->bin_width =
            std::max<std::int64_t>(1, (width + static_cast<std::int64_t>(grid->columns) - 1) /
                                        static_cast<std::int64_t>(grid->columns));
        grid->bin_height =
            std::max<std::int64_t>(1, (height + static_cast<std::int64_t>(grid->rows) - 1) /
                                        static_cast<std::int64_t>(grid->rows));
        grid->bins.resize(static_cast<std::size_t>(grid->columns) * grid->rows);
        grid->entries.reserve(cell.element_indices.size());

        const auto binRange = [&](const LayoutRect& bounds) {
            const auto normalized = normalize(bounds);
            auto first_column = normalized.x0 <= grid->bounds.x0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.x0 - grid->bounds.x0) / grid->bin_width,
                                             static_cast<std::int64_t>(grid->columns - 1U)));
            auto last_column = normalized.x1 <= grid->bounds.x0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.x1 - grid->bounds.x0) / grid->bin_width,
                                             static_cast<std::int64_t>(grid->columns - 1U)));
            auto first_row = normalized.y0 <= grid->bounds.y0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.y0 - grid->bounds.y0) / grid->bin_height,
                                             static_cast<std::int64_t>(grid->rows - 1U)));
            auto last_row = normalized.y1 <= grid->bounds.y0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.y1 - grid->bounds.y0) / grid->bin_height,
                                             static_cast<std::int64_t>(grid->rows - 1U)));
            if (first_column > last_column) {
                std::swap(first_column, last_column);
            }
            if (first_row > last_row) {
                std::swap(first_row, last_row);
            }
            struct Range {
                std::uint32_t first_column = 0;
                std::uint32_t last_column = 0;
                std::uint32_t first_row = 0;
                std::uint32_t last_row = 0;
            };
            return Range{.first_column = first_column,
                         .last_column = last_column,
                         .first_row = first_row,
                         .last_row = last_row};
        };

        for (const auto element_index : cell.element_indices) {
            if (element_index >= gds.elements.size()) {
                continue;
            }
            const auto& element = gds.elements[element_index];
            if (!isDrawableGdsElement(gds, element)) {
                continue;
            }
            const auto local_bounds = elementBounds(gds, element);
            const auto entry_index = static_cast<std::uint32_t>(grid->entries.size());
            grid->entries.push_back(GdsTileGridEntry{.element_index = element_index,
                                                     .bounds = local_bounds,
                                                     .shape_kind = shapeKindForGdsElement(element),
                                                     .layer_index = layerIndexFor(element),
                                                     .datatype = element.datatype});
            const auto range = binRange(local_bounds);
            for (auto column = range.first_column; column <= range.last_column; ++column) {
                for (auto row = range.first_row; row <= range.last_row; ++row) {
                    grid->bins[static_cast<std::size_t>(row) * grid->columns + column].push_back(entry_index);
                }
            }
        }

        grid->build_micros = elapsedMicros(start);
        result.grid_build_micros += grid->build_micros;
        result.grid_bin_count = saturatingU32(grid->bins.size());
        index.tile_grid = std::move(grid);
        return *index.tile_grid;
    }

    template <typename AppendElement>
    [[nodiscard]] bool queryTileGrid(std::uint32_t cell_index,
                                     const LayoutRect& local_bbox,
                                     LayoutTileGeometryResult& result,
                                     AppendElement append_element) const {
        auto& grid = ensureTileGridBuilt(cell_index, result);
        if (grid.entries.empty() || grid.bins.empty()) {
            return true;
        }
        result.grid_bin_count = saturatingU32(grid.bins.size());
        const auto normalized = normalize(local_bbox);
        const auto first_column = normalized.x0 <= grid.bounds.x0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.x0 - grid.bounds.x0) / grid.bin_width,
                                         static_cast<std::int64_t>(grid.columns - 1U)));
        const auto last_column = normalized.x1 <= grid.bounds.x0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.x1 - grid.bounds.x0) / grid.bin_width,
                                         static_cast<std::int64_t>(grid.columns - 1U)));
        const auto first_row = normalized.y0 <= grid.bounds.y0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.y0 - grid.bounds.y0) / grid.bin_height,
                                         static_cast<std::int64_t>(grid.rows - 1U)));
        const auto last_row = normalized.y1 <= grid.bounds.y0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.y1 - grid.bounds.y0) / grid.bin_height,
                                         static_cast<std::int64_t>(grid.rows - 1U)));
        const auto column0 = std::min(first_column, last_column);
        const auto column1 = std::max(first_column, last_column);
        const auto row0 = std::min(first_row, last_row);
        const auto row1 = std::max(first_row, last_row);

        std::vector<std::uint32_t> candidates;
        for (auto column = column0; column <= column1; ++column) {
            for (auto row = row0; row <= row1; ++row) {
                const auto& bin = grid.bins[static_cast<std::size_t>(row) * grid.columns + column];
                candidates.insert(candidates.end(), bin.begin(), bin.end());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        incrementCounter(result.grid_candidate_count, candidates.size());
        incrementCounter(result.element_candidate_count, candidates.size());
        for (const auto entry_index : candidates) {
            if (entry_index >= grid.entries.size()) {
                continue;
            }
            const auto& entry = grid.entries[entry_index];
            if (!intersects(entry.bounds, local_bbox)) {
                continue;
            }
            if (!append_element(entry.element_index,
                                entry.bounds,
                                entry.layer_index,
                                entry.shape_kind,
                                entry.datatype)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] GdsReferenceGridIndex& ensureReferenceGridBuilt(std::uint32_t cell_index,
                                                                  LayoutTileGeometryResult& result) const {
        auto& index = cells_[cell_index];
        if (index.reference_grid != nullptr) {
            incrementCounter(result.grid_hit_count);
            return *index.reference_grid;
        }

        incrementCounter(result.grid_miss_count);
        const auto start = Clock::now();
        const auto& gds = requireGds();
        const auto& cell = gds.cells[cell_index];
        auto grid = std::make_unique<GdsReferenceGridIndex>();
        grid->bounds = normalize(cell.bounds.value_or(LayoutRect{}));
        const auto width = std::max<std::int64_t>(1, grid->bounds.x1 - grid->bounds.x0 + 1);
        const auto height = std::max<std::int64_t>(1, grid->bounds.y1 - grid->bounds.y0 + 1);
        const auto target_axis = std::clamp<std::uint32_t>(
            ceilSqrtU32(cell.reference_indices.size() / 64U + 1U), 16U, kTileGridMaxAxisBins);
        grid->columns = target_axis;
        grid->rows = target_axis;
        grid->bin_width =
            std::max<std::int64_t>(1, (width + static_cast<std::int64_t>(grid->columns) - 1) /
                                        static_cast<std::int64_t>(grid->columns));
        grid->bin_height =
            std::max<std::int64_t>(1, (height + static_cast<std::int64_t>(grid->rows) - 1) /
                                        static_cast<std::int64_t>(grid->rows));
        grid->bins.resize(static_cast<std::size_t>(grid->columns) * grid->rows);
        grid->entries.reserve(cell.reference_indices.size());

        const auto binRange = [&](const LayoutRect& bounds) {
            const auto normalized = normalize(bounds);
            auto first_column = normalized.x0 <= grid->bounds.x0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.x0 - grid->bounds.x0) / grid->bin_width,
                                             static_cast<std::int64_t>(grid->columns - 1U)));
            auto last_column = normalized.x1 <= grid->bounds.x0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.x1 - grid->bounds.x0) / grid->bin_width,
                                             static_cast<std::int64_t>(grid->columns - 1U)));
            auto first_row = normalized.y0 <= grid->bounds.y0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.y0 - grid->bounds.y0) / grid->bin_height,
                                             static_cast<std::int64_t>(grid->rows - 1U)));
            auto last_row = normalized.y1 <= grid->bounds.y0
                ? 0U
                : static_cast<std::uint32_t>(
                      std::min<std::int64_t>((normalized.y1 - grid->bounds.y0) / grid->bin_height,
                                             static_cast<std::int64_t>(grid->rows - 1U)));
            if (first_column > last_column) {
                std::swap(first_column, last_column);
            }
            if (first_row > last_row) {
                std::swap(first_row, last_row);
            }
            struct Range {
                std::uint32_t first_column = 0;
                std::uint32_t last_column = 0;
                std::uint32_t first_row = 0;
                std::uint32_t last_row = 0;
            };
            return Range{.first_column = first_column,
                         .last_column = last_column,
                         .first_row = first_row,
                         .last_row = last_row};
        };

        for (const auto reference_index : cell.reference_indices) {
            if (reference_index >= gds.references.size()) {
                continue;
            }
            const auto& reference = gds.references[reference_index];
            if (reference.target_cell_index >= gds.cells.size()) {
                continue;
            }
            const auto& target = gds.cells[reference.target_cell_index];
            if (!target.bounds.has_value()) {
                continue;
            }
            std::optional<LayoutRect> bounds;
            const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
            const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
            for (const auto column : std::array<std::uint32_t, 2>{0U, columns - 1U}) {
                for (const auto row : std::array<std::uint32_t, 2>{0U, rows - 1U}) {
                    const auto transformed =
                        transformBounds(referenceTransform(reference, column, row), *target.bounds);
                    bounds = bounds.has_value() ? mergeBounds(*bounds, transformed) : transformed;
                }
            }
            if (!bounds.has_value()) {
                continue;
            }
            const auto entry_index = static_cast<std::uint32_t>(grid->entries.size());
            grid->entries.push_back(GdsReferenceGridEntry{.reference_index = reference_index,
                                                          .bounds = *bounds});
            const auto range = binRange(*bounds);
            for (auto column = range.first_column; column <= range.last_column; ++column) {
                for (auto row = range.first_row; row <= range.last_row; ++row) {
                    grid->bins[static_cast<std::size_t>(row) * grid->columns + column].push_back(entry_index);
                }
            }
        }

        grid->build_micros = elapsedMicros(start);
        result.grid_build_micros += grid->build_micros;
        result.grid_bin_count = saturatingU32(grid->bins.size());
        index.reference_grid = std::move(grid);
        return *index.reference_grid;
    }

    [[nodiscard]] std::vector<SpatialEntry> queryReferenceGrid(std::uint32_t cell_index,
                                                               const LayoutRect& local_bbox,
                                                               LayoutTileGeometryResult& result) const {
        auto& grid = ensureReferenceGridBuilt(cell_index, result);
        result.grid_bin_count = std::max(result.grid_bin_count, saturatingU32(grid.bins.size()));
        if (grid.entries.empty() || grid.bins.empty()) {
            return {};
        }
        const auto normalized = normalize(local_bbox);
        const auto first_column = normalized.x0 <= grid.bounds.x0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.x0 - grid.bounds.x0) / grid.bin_width,
                                         static_cast<std::int64_t>(grid.columns - 1U)));
        const auto last_column = normalized.x1 <= grid.bounds.x0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.x1 - grid.bounds.x0) / grid.bin_width,
                                         static_cast<std::int64_t>(grid.columns - 1U)));
        const auto first_row = normalized.y0 <= grid.bounds.y0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.y0 - grid.bounds.y0) / grid.bin_height,
                                         static_cast<std::int64_t>(grid.rows - 1U)));
        const auto last_row = normalized.y1 <= grid.bounds.y0
            ? 0U
            : static_cast<std::uint32_t>(
                  std::min<std::int64_t>((normalized.y1 - grid.bounds.y0) / grid.bin_height,
                                         static_cast<std::int64_t>(grid.rows - 1U)));
        const auto column0 = std::min(first_column, last_column);
        const auto column1 = std::max(first_column, last_column);
        const auto row0 = std::min(first_row, last_row);
        const auto row1 = std::max(first_row, last_row);

        std::vector<std::uint32_t> candidates;
        for (auto column = column0; column <= column1; ++column) {
            for (auto row = row0; row <= row1; ++row) {
                const auto& bin = grid.bins[static_cast<std::size_t>(row) * grid.columns + column];
                candidates.insert(candidates.end(), bin.begin(), bin.end());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        incrementCounter(result.grid_candidate_count, candidates.size());
        std::vector<SpatialEntry> entries;
        entries.reserve(candidates.size());
        for (const auto entry_index : candidates) {
            if (entry_index >= grid.entries.size()) {
                continue;
            }
            const auto& entry = grid.entries[entry_index];
            if (!intersects(entry.bounds, local_bbox)) {
                continue;
            }
            entries.push_back(SpatialEntry{.object = LayoutSpatialObjectId{
                                               .kind = LayoutSpatialObjectKind::Reference,
                                               .cell_index = kNoLayoutIndex,
                                               .reference_index = entry.reference_index,
                                               .element_index = kNoLayoutIndex,
                                               .layer_index = kNoLayoutIndex,
                                               .datatype = 0,
                                               .instance_path_hash = 0},
                                           .bounds = entry.bounds});
        }
        return entries;
    }

    [[nodiscard]] std::vector<SpatialEntry> queryReferenceGridIfBuilt(
        std::uint32_t cell_index,
        const LayoutRect& local_bbox,
        LayoutTileGeometryResult& result) const {
        if (cell_index >= cells_.size() || cells_[cell_index].reference_grid == nullptr) {
            return {};
        }
        return queryReferenceGrid(cell_index, local_bbox, result);
    }

    [[nodiscard]] LayoutShape makeElementShape(const LayoutGdsElement& element,
                                               const GdsAffineTransform& transform,
                                               std::uint32_t element_index,
                                               std::uint64_t path_hash,
                                               std::string_view path) const {
        const auto stable_hash = rememberInstance(path_hash,
                                                  element.cell_index,
                                                  kNoLayoutIndex,
                                                  element_index,
                                                  transform,
                                                  path);
        LayoutShape shape{.kind = shapeKindForGdsElement(element),
                          .owner_kind = LayoutOwnerKind::GdsElement,
                          .owner_index = element_index,
                          .macro_index = element.cell_index,
                          .layer_index = layerIndexFor(element),
                          .flags = element.texttype,
                          .datatype = element.datatype,
                          .instance_path_hash = stable_hash};
        const auto points = gdsElementPoints(requireGds(), element);
        shape.polygon.points.reserve(points.size());
        for (const auto& point : points) {
            shape.polygon.points.push_back(applyTransform(transform, point));
        }
        shape.rect = shapeBounds(shape);
        return shape;
    }

    void walkCell(const LayoutTileGeometryRequest& request,
                  std::uint32_t cell_index,
                  const GdsAffineTransform& transform,
                  std::uint64_t path_hash,
                  std::string_view path,
                  std::set<std::uint32_t>& stack,
                  TraversalBudget& budget,
                  LayoutTileGeometryResult& result) const {
        const auto& gds = requireGds();
        if (!stack.insert(cell_index).second) {
            return;
        }
        incrementCounter(result.visited_cell_count);
        const auto inverse = inverseTransform(transform);
        const auto local_bbox = request.has_bbox && inverse.has_value()
            ? transformBounds(*inverse, request.bbox)
            : (gds.cells[cell_index].bounds.value_or(LayoutRect{
                  .x0 = std::numeric_limits<std::int64_t>::min() / 4,
                  .y0 = std::numeric_limits<std::int64_t>::min() / 4,
                  .x1 = std::numeric_limits<std::int64_t>::max() / 4,
                  .y1 = std::numeric_limits<std::int64_t>::max() / 4}));
        const auto query_box = toBoostBox(local_bbox);

        if (request.lod >= 2U && request.layer_indices.empty() && request.datatypes.empty() &&
            containsKind(request.shape_kinds, LayoutShapeKind::Placement)) {
            const auto& cell = gds.cells[cell_index];
            if (!cell.reference_indices.empty()) {
                auto appendOverviewReference = [&](std::uint32_t reference_index) -> bool {
                    if (reference_index >= gds.references.size()) {
                        return true;
                    }
                    const auto& reference = gds.references[reference_index];
                    if (reference.target_cell_index >= gds.cells.size()) {
                        return true;
                    }
                    const auto& target = gds.cells[reference.target_cell_index];
                    if (!target.bounds.has_value()) {
                        return true;
                    }
                    const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
                    const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
                    const auto visible_range =
                        visibleArrayRange(request, transform, reference, *target.bounds, columns, rows);
                    if (visible_range.has_value() &&
                        (visible_range->columns.empty || visible_range->rows.empty)) {
                        return true;
                    }
                    const auto first_column = visible_range.has_value()
                        ? visible_range->columns.first
                        : 0U;
                    const auto last_column = visible_range.has_value()
                        ? visible_range->columns.last
                        : columns - 1U;
                    const auto first_row = visible_range.has_value() ? visible_range->rows.first : 0U;
                    const auto last_row = visible_range.has_value() ? visible_range->rows.last : rows - 1U;
                    auto child_hash = mixPathHash(path_hash, reference_index);
                    child_hash = mixPathHash(child_hash, first_column);
                    child_hash = mixPathHash(child_hash, first_row);
                    const auto child_path =
                        childInstancePath(path, reference_index, first_column, first_row);
                    const auto overview_bounds = visibleReferenceBounds(reference,
                                                                        *target.bounds,
                                                                        transform,
                                                                        first_column,
                                                                        last_column,
                                                                        first_row,
                                                                        last_row);
                    if (request.has_bbox && !intersects(overview_bounds, request.bbox)) {
                        return true;
                    }
                    const auto overview_transform = composeTransforms(
                        transform, referenceTransform(reference, first_column, first_row));
                    auto shape = makeReferenceShapeForBounds(reference,
                                                            overview_bounds,
                                                            overview_transform,
                                                            reference_index,
                                                            child_hash,
                                                            child_path);
                    incrementCounter(result.reference_candidate_count);
                    incrementCounter(result.traversed_reference_count);
                    if (budget.shouldSkip()) {
                        return true;
                    }
                    if (!budget.canAppend(shape)) {
                        return false;
                    }
                    budget.appended(shape);
                    incrementCounter(result.lod_shape_count);
                    result.shapes.push_back(std::move(shape));
                    return true;
                };
                const auto use_reference_grid =
                    request.has_bbox && cell.reference_indices.size() >= kTileGridElementThreshold &&
                    cell_index < cells_.size() && cells_[cell_index].reference_grid != nullptr;
                if (use_reference_grid) {
                    const auto reference_candidates =
                        queryReferenceGridIfBuilt(cell_index, local_bbox, result);
                    for (const auto& entry : reference_candidates) {
                        if (!appendOverviewReference(entry.object.reference_index)) {
                            stack.erase(cell_index);
                            return;
                        }
                    }
                }
                else {
                    for (const auto reference_index : cell.reference_indices) {
                        if (!appendOverviewReference(reference_index)) {
                            stack.erase(cell_index);
                            return;
                        }
                    }
                }
                stack.erase(cell_index);
                return;
            }
            if (cell.bounds.has_value()) {
                auto shape = makeCellOverviewShape(cell_index, *cell.bounds, transform, path_hash, path);
                if (!request.has_bbox || intersects(shape.rect, request.bbox)) {
                    if (!budget.shouldSkip()) {
                        if (!budget.canAppend(shape)) {
                            stack.erase(cell_index);
                            return;
                        }
                        budget.appended(shape);
                        incrementCounter(result.lod_shape_count);
                        result.shapes.push_back(std::move(shape));
                    }
                }
                stack.erase(cell_index);
                return;
            }
        }

        if (request.lod < 2U) {
            const auto& cell = gds.cells[cell_index];
            const auto appendElement = [&](std::uint32_t element_index,
                                           LayoutRect local_element_bounds,
                                           std::uint32_t layer_index,
                                           LayoutShapeKind shape_kind,
                                           std::uint32_t datatype) -> bool {
                if (!containsLayer(request.layer_indices, layer_index) ||
                    !containsKind(request.shape_kinds, shape_kind) ||
                    !containsDatatype(request.datatypes, datatype)) {
                    return true;
                }
                const auto& element = gds.elements[element_index];
                auto shape = makeElementShape(element, transform, element_index, path_hash, path);
                const auto lod_shape = request.lod == 1U;
                if (request.lod == 1U) {
                    shape.rect = transformBounds(transform, local_element_bounds);
                    shape.polygon.points.clear();
                    shape.kind = LayoutShapeKind::Rect;
                }
                if (request.has_bbox && !intersects(shapeBounds(shape), request.bbox)) {
                    return true;
                }
                if (budget.shouldSkip()) {
                    return true;
                }
                if (!budget.canAppend(shape)) {
                    return false;
                }
                budget.appended(shape);
                if (lod_shape) {
                    incrementCounter(result.lod_shape_count);
                }
                result.shapes.push_back(std::move(shape));
                return true;
            };

            if (budget.bounded() && request.has_bbox &&
                cell.element_indices.size() >= kTileGridElementThreshold) {
                if (!queryTileGrid(cell_index, local_bbox, result, appendElement)) {
                    stack.erase(cell_index);
                    return;
                }
            }
            else {
                ensureElementsBuilt(cell_index);
                std::vector<SpatialValue> element_candidates;
                cells_[cell_index].elements.query(bgi::intersects(query_box),
                                                  std::back_inserter(element_candidates));
                incrementCounter(result.element_candidate_count, element_candidates.size());
                std::sort(element_candidates.begin(),
                          element_candidates.end(),
                          [](const auto& lhs, const auto& rhs) {
                              return lhs.second.object.element_index <
                                     rhs.second.object.element_index;
                          });
                for (const auto& value : element_candidates) {
                    const auto& entry = value.second;
                    if (!appendElement(entry.object.element_index,
                                       entry.bounds,
                                       entry.object.layer_index,
                                       entry.shape_kind,
                                       entry.datatype)) {
                        stack.erase(cell_index);
                        return;
                    }
                }
            }
        }

        std::vector<SpatialEntry> reference_candidates;
        const auto use_reference_grid =
            request.has_bbox && gds.cells[cell_index].reference_indices.size() >= kTileGridElementThreshold;
        if (use_reference_grid) {
            reference_candidates = queryReferenceGrid(cell_index, local_bbox, result);
        }
        else {
            ensureReferencesBuilt(cell_index);
            std::vector<SpatialValue> spatial_candidates;
            cells_[cell_index].references.query(bgi::intersects(query_box),
                                                std::back_inserter(spatial_candidates));
            std::sort(spatial_candidates.begin(),
                      spatial_candidates.end(),
                      [](const auto& lhs, const auto& rhs) {
                          return lhs.second.object.reference_index <
                                 rhs.second.object.reference_index;
                      });
            reference_candidates.reserve(spatial_candidates.size());
            for (const auto& value : spatial_candidates) {
                reference_candidates.push_back(value.second);
            }
        }
        incrementCounter(result.reference_candidate_count, reference_candidates.size());
        for (const auto& entry : reference_candidates) {
            const auto reference_index = entry.object.reference_index;
            if (reference_index >= gds.references.size()) {
                continue;
            }
            const auto& reference = gds.references[reference_index];
            if (reference.target_cell_index >= gds.cells.size()) {
                continue;
            }
            const auto& target = gds.cells[reference.target_cell_index];
            if (!target.bounds.has_value()) {
                continue;
            }
            const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
            const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
            const auto visible_range =
                visibleArrayRange(request, transform, reference, *target.bounds, columns, rows);
            const auto first_column = visible_range.has_value() ? visible_range->columns.first : 0U;
            const auto last_column =
                visible_range.has_value() ? visible_range->columns.last : columns - 1U;
            const auto first_row = visible_range.has_value() ? visible_range->rows.first : 0U;
            const auto last_row = visible_range.has_value() ? visible_range->rows.last : rows - 1U;
            if (visible_range.has_value() &&
                (visible_range->columns.empty || visible_range->rows.empty)) {
                continue;
            }
            if (request.lod >= 2U && request.layer_indices.empty() &&
                request.datatypes.empty() &&
                containsKind(request.shape_kinds, LayoutShapeKind::Placement)) {
                auto child_hash = mixPathHash(path_hash, reference_index);
                child_hash = mixPathHash(child_hash, first_column);
                child_hash = mixPathHash(child_hash, first_row);
                const auto child_path =
                    childInstancePath(path, reference_index, first_column, first_row);
                const auto overview_bounds = visibleReferenceBounds(reference,
                                                                    *target.bounds,
                                                                    transform,
                                                                    first_column,
                                                                    last_column,
                                                                    first_row,
                                                                    last_row);
                auto overview_transform = composeTransforms(
                    transform, referenceTransform(reference, first_column, first_row));
                auto shape = makeReferenceShapeForBounds(reference,
                                                        overview_bounds,
                                                        overview_transform,
                                                        reference_index,
                                                        child_hash,
                                                        child_path);
                if (request.has_bbox && !intersects(shape.rect, request.bbox)) {
                    continue;
                }
                incrementCounter(result.traversed_reference_count);
                if (!budget.shouldSkip()) {
                    if (!budget.canAppend(shape)) {
                        stack.erase(cell_index);
                        return;
                    }
                    budget.appended(shape);
                    incrementCounter(result.lod_shape_count);
                    result.shapes.push_back(std::move(shape));
                }
                continue;
            }
            for (std::uint32_t column = first_column;; ++column) {
                for (std::uint32_t row = first_row;; ++row) {
                    const auto child_transform =
                        composeTransforms(transform, referenceTransform(reference, column, row));
                    const auto child_visible =
                        !request.has_bbox ||
                        intersects(transformBounds(child_transform, *target.bounds), request.bbox);
                    if (child_visible) {
                        auto child_hash = mixPathHash(path_hash, reference_index);
                        child_hash = mixPathHash(child_hash, column);
                        child_hash = mixPathHash(child_hash, row);
                        const auto child_path =
                            childInstancePath(path, reference_index, column, row);
                        incrementCounter(result.traversed_reference_count);
                        if (request.lod >= 2U && request.layer_indices.empty() &&
                            request.datatypes.empty() &&
                            containsKind(request.shape_kinds, LayoutShapeKind::Placement)) {
                            auto shape = makeReferenceShape(reference,
                                                            *target.bounds,
                                                            child_transform,
                                                            reference_index,
                                                            child_hash,
                                                            child_path);
                            if (!budget.shouldSkip()) {
                                if (!budget.canAppend(shape)) {
                                    stack.erase(cell_index);
                                    return;
                                }
                                budget.appended(shape);
                                incrementCounter(result.lod_shape_count);
                                result.shapes.push_back(std::move(shape));
                            }
                        }
                        else {
                            walkCell(request,
                                     reference.target_cell_index,
                                     child_transform,
                                     child_hash,
                                     child_path,
                                     stack,
                                     budget,
                                     result);
                            if (budget.truncated()) {
                                stack.erase(cell_index);
                                return;
                            }
                        }
                    }
                    if (row == last_row) {
                        break;
                    }
                }
                if (column == last_column) {
                    break;
                }
            }
        }
        stack.erase(cell_index);
    }

    const LayoutDataSet* data_ = nullptr;
    [[nodiscard]] std::uint32_t layerIndexFor(const LayoutGdsElement& element) const {
        const auto key = std::make_pair(element.layer, element.datatype);
        const auto found = layer_indices_.find(std::make_pair(element.layer, element.datatype));
        if (found == layer_indices_.end()) {
            const auto layer_index = findGdsLayer(*data_, element.layer, element.datatype);
            layer_indices_.emplace(key, layer_index);
            return layer_index;
        }
        return found->second;
    }

    mutable std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> layer_indices_;
    mutable std::vector<CellSpatialIndex> cells_;
    mutable std::mutex instance_mutex_;
    mutable std::map<std::uint64_t, InstanceRecord> instance_records_;
};

std::unique_ptr<LayoutSpatialIndex> LayoutSpatialIndex::build(const LayoutDataSet& data) {
    if (!data.gds.has_value()) {
        return nullptr;
    }
    return std::unique_ptr<LayoutSpatialIndex>(
        new LayoutSpatialIndex(std::make_unique<Impl>(data)));
}

LayoutSpatialIndex::LayoutSpatialIndex() = default;
LayoutSpatialIndex::LayoutSpatialIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LayoutSpatialIndex::LayoutSpatialIndex(LayoutSpatialIndex&&) noexcept = default;
LayoutSpatialIndex& LayoutSpatialIndex::operator=(LayoutSpatialIndex&&) noexcept = default;
LayoutSpatialIndex::~LayoutSpatialIndex() = default;

LayoutSpatialIndexStats LayoutSpatialIndex::stats() const {
    if (!impl_) {
        return {};
    }
    return impl_->stats();
}

LayoutTileGeometryResult LayoutSpatialIndex::queryTile(
    const LayoutTileGeometryRequest& request) const {
    if (!impl_) {
        throw std::runtime_error("Layout spatial index is not available");
    }
    return impl_->queryTile(request);
}

void LayoutSpatialIndex::warmupTopCell() const {
    if (!impl_) {
        throw std::runtime_error("Layout spatial index is not available");
    }
    impl_->warmupTopCell();
}

LayoutHitTestResponse LayoutSpatialIndex::hitTest(
    const LayoutHitTestRequest& request) const {
    if (!impl_) {
        throw std::runtime_error("Layout spatial index is not available");
    }
    return impl_->hitTest(request);
}

LayoutInspectResult LayoutSpatialIndex::inspect(const LayoutInspectRequest& request) const {
    if (!impl_) {
        throw std::runtime_error("Layout spatial index is not available");
    }
    return impl_->inspect(request);
}

LayoutTileGeometryResult LayoutSpatialIndex::selectionGeometry(
    const LayoutSelectionGeometryRequest& request) const {
    if (!impl_) {
        throw std::runtime_error("Layout spatial index is not available");
    }
    return impl_->selectionGeometry(request);
}

} // namespace pristine::layout
