#include "pristine/layout/LayoutSpatialIndex.h"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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

struct CellSpatialIndex {
    SpatialTree elements;
    SpatialTree references;
};

bool isDrawableGdsElement(const LayoutGdsElement& element) {
    return element.kind != LayoutGdsElementKind::Sref &&
           element.kind != LayoutGdsElementKind::Aref && !element.points.empty();
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

LayoutRect pointBounds(const std::vector<LayoutPoint>& points) {
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
        for (const auto& element : gds.elements) {
            if (isDrawableGdsElement(element)) {
                const auto key = std::make_pair(element.layer, element.datatype);
                if (layer_indices_.find(key) == layer_indices_.end()) {
                    layer_indices_.emplace(key, findGdsLayer(*data_, element.layer, element.datatype));
                }
            }
        }
        cells_.resize(gds.cells.size());
        for (std::size_t cell_index = 0; cell_index < gds.cells.size(); ++cell_index) {
            buildCell(static_cast<std::uint32_t>(cell_index));
        }
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
        walkCell(request,
                 request.root_cell_index,
                 GdsAffineTransform{},
                 kFnvOffset,
                 stack,
                 budget,
                 result);
        result.truncated = budget.truncated();
        result.next_token = budget.nextToken();
        result.query_micros = elapsedMicros(start);
        return result;
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
        tile_request.max_shapes = 0;
        tile_request.max_points = 0;
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
            hit.object = LayoutSpatialObjectId{.kind = LayoutSpatialObjectKind::Element,
                                               .cell_index = shape.macro_index,
                                               .reference_index = kNoLayoutIndex,
                                               .element_index = shape.owner_index,
                                               .layer_index = shape.layer_index,
                                               .datatype = shape.flags,
                                               .instance_path_hash = 0};
            hit.bounds = bounds;
            hit.rank = shape.kind == LayoutShapeKind::Polygon ? 0U
                : shape.kind == LayoutShapeKind::Path          ? 1U
                                                              : 2U;
            hit.distance = rectDistance(request.point, bounds);
            response.hits.push_back(hit);
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
                result.object.cell_index = reference.parent_cell_index;
                if (reference.target_cell_index < gds.cells.size()) {
                    const auto& target = gds.cells[reference.target_cell_index];
                    if (target.bounds.has_value()) {
                        result.bounds = transformBounds(referenceTransform(reference, 0, 0),
                                                        *target.bounds);
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
                result.layer = element.layer;
                result.datatype = element.datatype;
                result.texttype = element.texttype;
                result.text = element.text;
                result.bounds = pointBounds(element.points);
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
        if (request.object.kind != LayoutSpatialObjectKind::Element) {
            throw std::runtime_error("Layout selection geometry currently requires an element id");
        }
        if (request.object.element_index >= gds.elements.size()) {
            throw std::runtime_error("Layout selection element index is out of range");
        }
        const auto& element = gds.elements[request.object.element_index];
        if (!isDrawableGdsElement(element)) {
            return {};
        }
        LayoutTileGeometryResult result;
        result.shapes.push_back(makeElementShape(element,
                                                 GdsAffineTransform{},
                                                 request.object.element_index,
                                                 kFnvOffset));
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

    void buildCell(std::uint32_t cell_index) {
        const auto& gds = requireGds();
        const auto& cell = gds.cells[cell_index];
        std::vector<SpatialValue> element_values;
        std::vector<SpatialValue> reference_values;
        element_values.reserve(cell.element_indices.size());
        reference_values.reserve(cell.reference_indices.size());

        for (const auto element_index : cell.element_indices) {
            if (element_index >= gds.elements.size()) {
                continue;
            }
            const auto& element = gds.elements[element_index];
            if (!isDrawableGdsElement(element)) {
                continue;
            }
            const auto bounds = pointBounds(element.points);
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

        cells_[cell_index].elements = SpatialTree(element_values.begin(), element_values.end());
        cells_[cell_index].references = SpatialTree(reference_values.begin(), reference_values.end());
    }

    [[nodiscard]] LayoutShape makeElementShape(const LayoutGdsElement& element,
                                               const GdsAffineTransform& transform,
                                               std::uint32_t element_index,
                                               std::uint64_t path_hash) const {
        LayoutShape shape{.kind = shapeKindForGdsElement(element),
                          .owner_kind = LayoutOwnerKind::GdsElement,
                          .owner_index = element_index,
                          .macro_index = element.cell_index,
                          .layer_index = layerIndexFor(element),
                          .flags = element.texttype};
        shape.polygon.points.reserve(element.points.size());
        for (const auto& point : element.points) {
            shape.polygon.points.push_back(applyTransform(transform, point));
        }
        shape.rect = shapeBounds(shape);
        (void)path_hash;
        return shape;
    }

    void walkCell(const LayoutTileGeometryRequest& request,
                  std::uint32_t cell_index,
                  const GdsAffineTransform& transform,
                  std::uint64_t path_hash,
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

        std::vector<SpatialValue> element_candidates;
        cells_[cell_index].elements.query(bgi::intersects(query_box),
                                          std::back_inserter(element_candidates));
        incrementCounter(result.element_candidate_count, element_candidates.size());
        std::sort(element_candidates.begin(),
                  element_candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.second.object.element_index < rhs.second.object.element_index;
                  });
        for (const auto& value : element_candidates) {
            const auto& entry = value.second;
            if (!containsLayer(request.layer_indices, entry.object.layer_index) ||
                !containsKind(request.shape_kinds, entry.shape_kind) ||
                !containsDatatype(request.datatypes, entry.datatype)) {
                continue;
            }
            const auto& element = gds.elements[entry.object.element_index];
            auto shape = makeElementShape(element, transform, entry.object.element_index, path_hash);
            if (request.has_bbox && !intersects(shapeBounds(shape), request.bbox)) {
                continue;
            }
            if (budget.shouldSkip()) {
                continue;
            }
            if (!budget.canAppend(shape)) {
                stack.erase(cell_index);
                return;
            }
            budget.appended(shape);
            result.shapes.push_back(std::move(shape));
        }

        std::vector<SpatialValue> reference_candidates;
        cells_[cell_index].references.query(bgi::intersects(query_box),
                                            std::back_inserter(reference_candidates));
        incrementCounter(result.reference_candidate_count, reference_candidates.size());
        std::sort(reference_candidates.begin(),
                  reference_candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.second.object.reference_index <
                             rhs.second.object.reference_index;
                  });
        for (const auto& value : reference_candidates) {
            const auto reference_index = value.second.object.reference_index;
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
            for (std::uint32_t column = first_column;; ++column) {
                for (std::uint32_t row = first_row;; ++row) {
                    const auto child_transform =
                        composeTransforms(transform, referenceTransform(reference, column, row));
                    if (request.has_bbox &&
                        !intersects(transformBounds(child_transform, *target.bounds),
                                    request.bbox)) {
                        continue;
                    }
                    auto child_hash = mixPathHash(path_hash, reference_index);
                    child_hash = mixPathHash(child_hash, column);
                    child_hash = mixPathHash(child_hash, row);
                    incrementCounter(result.traversed_reference_count);
                    walkCell(request,
                             reference.target_cell_index,
                             child_transform,
                             child_hash,
                             stack,
                             budget,
                             result);
                    if (budget.truncated()) {
                        stack.erase(cell_index);
                        return;
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
        const auto found = layer_indices_.find(std::make_pair(element.layer, element.datatype));
        if (found == layer_indices_.end()) {
            throw std::runtime_error("GDS layer is missing from layout spatial index");
        }
        return found->second;
    }

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> layer_indices_;
    std::vector<CellSpatialIndex> cells_;
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
