#include "pristine/layout/LayoutSource.h"

#include "pristine/layout/LayoutBinaryProtocol.h"
#include "pristine/layout/LayoutParser.h"
#include "pristine/layout/LayoutSpatialIndex.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pristine::layout {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedMicros(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

std::optional<std::string> environmentValue(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value, size == 0 ? 0 : size - 1U);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::uint64_t layoutCacheBudgetBytes() {
    constexpr std::uint64_t kDefaultBudget = 256ULL * 1024ULL * 1024ULL;
    const auto value = environmentValue("PRISTINE_LAYOUT_CACHE_BYTES");
    if (!value.has_value() || value->empty()) {
        return kDefaultBudget;
    }
    try {
        const auto parsed = std::stoull(*value);
        return parsed;
    }
    catch (const std::exception&) {
        return kDefaultBudget;
    }
}

void writeLeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        return;
    }
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void markTilePayloadCacheHit(std::vector<std::uint8_t>& payload) {
    constexpr std::size_t kPltgCacheHitCountOffset = 76;
    constexpr std::size_t kPltgCacheMissCountOffset = 80;
    writeLeU32(payload, kPltgCacheHitCountOffset, 1U);
    writeLeU32(payload, kPltgCacheMissCountOffset, 0U);
}

std::string tileRequestKey(const LayoutTileGeometryRequest& request) {
    std::ostringstream out;
    out << "root=" << request.root_cell_index << ";bbox=" << request.has_bbox;
    if (request.has_bbox) {
        out << "," << request.bbox.x0 << "," << request.bbox.y0 << "," << request.bbox.x1 << ","
            << request.bbox.y1;
    }
    out << ";maxs=" << request.max_shapes << ";maxp=" << request.max_points
        << ";maxb=" << request.max_bytes << ";lod=" << request.lod
        << ";cont=" << request.continuation_token << ";layers=";
    for (const auto layer : request.layer_indices) {
        out << layer << ",";
    }
    out << ";kinds=";
    for (const auto kind : request.shape_kinds) {
        out << static_cast<std::uint32_t>(kind) << ",";
    }
    out << ";datatypes=";
    for (const auto datatype : request.datatypes) {
        out << datatype << ",";
    }
    return out.str();
}

std::string asciiLower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool containsAsciiCaseInsensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lower_value = asciiLower(value);
    const auto lower_needle = asciiLower(needle);
    return lower_value.find(lower_needle) != std::string::npos;
}

std::uint32_t searchRank(std::string_view value, std::string_view query, std::uint32_t base_rank) {
    if (query.empty()) {
        return base_rank + 30U;
    }
    const auto lower_value = asciiLower(value);
    const auto lower_query = asciiLower(query);
    if (lower_value == lower_query) {
        return base_rank;
    }
    if (lower_value.rfind(lower_query, 0) == 0) {
        return base_rank + 10U;
    }
    return base_rank + 20U;
}

bool searchKindEnabled(std::uint32_t mask, std::uint32_t bit) {
    return mask == 0U || (mask & bit) != 0U;
}

constexpr std::uint32_t kSearchKindCell = 1U << 0U;
constexpr std::uint32_t kSearchKindReference = 1U << 1U;
constexpr std::uint32_t kSearchKindText = 1U << 2U;
constexpr std::uint32_t kSearchKindLayer = 1U << 3U;

LayoutSearchResponse searchGdsLayout(const LayoutDataSet& data,
                                     const LayoutSearchRequest& request);

class DataSetLayoutSource final : public LayoutSource {
public:
    DataSetLayoutSource(LayoutDataSet data, std::string source_kind) :
        data_(std::move(data)),
        source_kind_(std::move(source_kind)),
        tile_cache_budget_bytes_(layoutCacheBudgetBytes()) {}

    const LayoutDataSet& dataSet() const override { return data_; }
    std::string_view sourceKind() const override { return source_kind_; }
    std::vector<std::uint8_t> encodeTileGeometryResponse(
        const LayoutTileGeometryRequest& request) const override {
        return encodeCachedTileGeometryResponse(request);
    }
    std::vector<std::uint8_t> encodeHitTestResponse(
        const LayoutHitTestRequest& request) const override {
        std::uint64_t index_build_micros = 0;
        auto response = spatialIndex(index_build_micros).hitTest(request);
        response.index_build_micros = index_build_micros;
        return encodeHitTestResponsePayload(data_, response);
    }
    std::vector<std::uint8_t> encodeInspectResponse(
        const LayoutInspectRequest& request) const override {
        std::uint64_t index_build_micros = 0;
        return encodeInspectResponsePayload(data_, spatialIndex(index_build_micros).inspect(request));
    }
    std::vector<std::uint8_t> encodeSelectionGeometryResponse(
        const LayoutSelectionGeometryRequest& request) const override {
        LayoutGeometryRequest geometry_request;
        std::uint64_t index_build_micros = 0;
        auto selection = spatialIndex(index_build_micros).selectionGeometry(request);
        selection.index_build_micros = index_build_micros;
        return encodeGeometryResponsePayload(data_, geometry_request, selection.shapes, selection.truncated);
    }
    std::vector<std::uint8_t> encodeSearchResponse(
        const LayoutSearchRequest& request) const override {
        return encodeSearchResponsePayload(data_, searchGdsLayout(data_, request));
    }

private:
    const LayoutSpatialIndex& spatialIndex(std::uint64_t& build_micros) const {
        build_micros = 0;
        std::lock_guard lock(spatial_mutex_);
        if (!spatial_index_) {
            const auto start = Clock::now();
            spatial_index_ = LayoutSpatialIndex::build(data_);
            build_micros = elapsedMicros(start);
        }
        if (!spatial_index_) {
            throw std::runtime_error("Layout spatial index requires a GDS layout source");
        }
        return *spatial_index_;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> lookupTileCache(
        const std::string& key) const {
        std::lock_guard lock(tile_cache_mutex_);
        const auto found = tile_cache_.find(key);
        if (found == tile_cache_.end()) {
            return std::nullopt;
        }
        tile_cache_lru_.splice(tile_cache_lru_.begin(), tile_cache_lru_, found->second.lru);
        auto payload = found->second.payload;
        markTilePayloadCacheHit(payload);
        return payload;
    }

    void insertTileCache(const std::string& key, std::vector<std::uint8_t> payload) const {
        if (tile_cache_budget_bytes_ == 0U || payload.size() > tile_cache_budget_bytes_) {
            return;
        }
        std::lock_guard lock(tile_cache_mutex_);
        const auto existing = tile_cache_.find(key);
        if (existing != tile_cache_.end()) {
            tile_cache_bytes_ -= existing->second.bytes;
            tile_cache_lru_.erase(existing->second.lru);
            tile_cache_.erase(existing);
        }
        tile_cache_lru_.push_front(key);
        const auto payload_size = payload.size();
        tile_cache_bytes_ += payload_size;
        tile_cache_.emplace(key,
                            TileCacheEntry{.payload = std::move(payload),
                                           .bytes = payload_size,
                                           .lru = tile_cache_lru_.begin()});
        while (tile_cache_bytes_ > tile_cache_budget_bytes_ && !tile_cache_lru_.empty()) {
            const auto evict_key = tile_cache_lru_.back();
            tile_cache_lru_.pop_back();
            const auto evict = tile_cache_.find(evict_key);
            if (evict != tile_cache_.end()) {
                tile_cache_bytes_ -= evict->second.bytes;
                tile_cache_.erase(evict);
            }
        }
    }

    [[nodiscard]] LayoutTileGeometryRequest resolveContinuation(
        const LayoutTileGeometryRequest& request,
        std::uint32_t& client_token) const {
        auto resolved = request;
        client_token = request.continuation_token;
        if (request.continuation_token == 0U) {
            return resolved;
        }
        auto key_request = request;
        key_request.continuation_token = 0;
        const auto continuation_key = tileRequestKey(key_request);
        std::lock_guard lock(tile_cache_mutex_);
        const auto found = continuations_.find(request.continuation_token);
        if (found == continuations_.end() || found->second.key != continuation_key) {
            throw std::runtime_error("Layout tile continuation token is invalid or expired");
        }
        resolved.continuation_token = found->second.skip;
        return resolved;
    }

    void updateContinuationToken(const LayoutTileGeometryRequest& original_request,
                                 LayoutTileGeometryResult& tile,
                                 std::uint32_t client_token) const {
        auto key_request = original_request;
        key_request.continuation_token = 0;
        const auto continuation_key = tileRequestKey(key_request);
        std::lock_guard lock(tile_cache_mutex_);
        if (!tile.truncated || tile.next_token == 0U) {
            tile.next_token = 0;
            return;
        }
        auto token = client_token;
        if (token == 0U) {
            do {
                token = next_continuation_token_++;
                if (next_continuation_token_ == 0U) {
                    next_continuation_token_ = 1U;
                }
            } while (continuations_.find(token) != continuations_.end());
        }
        continuations_[token] = ContinuationState{.key = continuation_key, .skip = tile.next_token};
        tile.next_token = token;
    }

    [[nodiscard]] std::vector<std::uint8_t> encodeCachedTileGeometryResponse(
        const LayoutTileGeometryRequest& request) const {
        std::uint32_t client_token = 0;
        auto spatial_request = resolveContinuation(request, client_token);
        const auto cache_key = tileRequestKey(spatial_request);
        if (auto cached = lookupTileCache(cache_key); cached.has_value()) {
            return *cached;
        }

        std::uint64_t index_build_micros = 0;
        auto tile = spatialIndex(index_build_micros).queryTile(spatial_request);
        tile.index_build_micros = index_build_micros;
        tile.cache_miss_count = 1;
        if (request.max_bytes != 0U) {
            for (;;) {
                auto payload = encodeTileGeometryResponsePayload(data_, request, tile);
                if (payload.size() <= request.max_bytes || tile.shapes.size() <= 1U) {
                    updateContinuationToken(request, tile, client_token);
                    payload = encodeTileGeometryResponsePayload(data_, request, tile);
                    insertTileCache(cache_key, payload);
                    return payload;
                }
                tile.truncated = true;
                tile.next_token = spatial_request.continuation_token +
                                  static_cast<std::uint32_t>(tile.shapes.size() / 2U);
                tile.shapes.resize(tile.shapes.size() / 2U);
            }
        }
        updateContinuationToken(request, tile, client_token);
        auto payload = encodeTileGeometryResponsePayload(data_, request, tile);
        insertTileCache(cache_key, payload);
        return payload;
    }

    LayoutDataSet data_;
    std::string source_kind_;
    mutable std::mutex spatial_mutex_;
    mutable std::unique_ptr<LayoutSpatialIndex> spatial_index_;
    struct TileCacheEntry {
        std::vector<std::uint8_t> payload{};
        std::size_t bytes = 0;
        std::list<std::string>::iterator lru{};
    };
    struct ContinuationState {
        std::string key{};
        std::uint32_t skip = 0;
    };
    std::uint64_t tile_cache_budget_bytes_ = 0;
    mutable std::mutex tile_cache_mutex_;
    mutable std::uint64_t tile_cache_bytes_ = 0;
    mutable std::list<std::string> tile_cache_lru_;
    mutable std::map<std::string, TileCacheEntry> tile_cache_;
    mutable std::map<std::uint32_t, ContinuationState> continuations_;
    mutable std::uint32_t next_continuation_token_ = 1;
};

std::string gdsLayerName(std::uint32_t layer, std::uint32_t datatype) {
    return "GDS:" + std::to_string(layer) + "/" + std::to_string(datatype);
}

std::uint32_t findOrAddLayer(LayoutDataSet& data, const LayoutLayer& layer) {
    for (std::size_t index = 0; index < data.layers.size(); ++index) {
        if (data.layers[index].name == layer.name) {
            if (data.layers[index].kind == LayoutLayerKind::Unknown) {
                data.layers[index].kind = layer.kind;
            }
            if (!data.layers[index].pitch.has_value()) {
                data.layers[index].pitch = layer.pitch;
            }
            if (!data.layers[index].width.has_value()) {
                data.layers[index].width = layer.width;
            }
            if (!data.layers[index].spacing.has_value()) {
                data.layers[index].spacing = layer.spacing;
            }
            return static_cast<std::uint32_t>(index);
        }
    }
    data.layers.push_back(layer);
    return static_cast<std::uint32_t>(data.layers.size() - 1U);
}

void expandBounds(std::optional<LayoutRect>& bounds, const LayoutRect& rect) {
    if (!bounds.has_value()) {
        bounds = rect;
        return;
    }
    bounds->x0 = std::min(bounds->x0, rect.x0);
    bounds->y0 = std::min(bounds->y0, rect.y0);
    bounds->x1 = std::max(bounds->x1, rect.x1);
    bounds->y1 = std::max(bounds->y1, rect.y1);
}

bool intersects(const LayoutRect& lhs, const LayoutRect& rhs) {
    return lhs.x0 <= rhs.x1 && lhs.x1 >= rhs.x0 && lhs.y0 <= rhs.y1 && lhs.y1 >= rhs.y0;
}

LayoutRect shapeBounds(const LayoutShape& shape) {
    if (shape.kind == LayoutShapeKind::Polygon && !shape.polygon.points.empty()) {
        LayoutRect bounds{.x0 = shape.polygon.points.front().x,
                          .y0 = shape.polygon.points.front().y,
                          .x1 = shape.polygon.points.front().x,
                          .y1 = shape.polygon.points.front().y};
        for (const auto& point : shape.polygon.points) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        return bounds;
    }
    return shape.rect;
}

void appendShape(LayoutDataSet& data, LayoutShape shape) {
    expandBounds(data.bounds, shapeBounds(shape));
    data.shapes.push_back(std::move(shape));
}

std::uint32_t findOrAddGdsLayer(LayoutDataSet& data,
                                std::uint32_t layer,
                                std::uint32_t datatype) {
    const auto name = gdsLayerName(layer, datatype);
    for (std::size_t index = 0; index < data.layers.size(); ++index) {
        if (data.layers[index].name == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    data.layers.push_back(LayoutLayer{.name = name, .kind = LayoutLayerKind::Unknown});
    return static_cast<std::uint32_t>(data.layers.size() - 1U);
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

bool isDrawableGdsElement(const LayoutGdsElement& element) {
    return element.kind != LayoutGdsElementKind::Sref &&
           element.kind != LayoutGdsElementKind::Aref && !element.points.empty();
}

void registerGdsLayers(LayoutDataSet& data, const LayoutGdsLibrary& gds) {
    for (const auto& element : gds.elements) {
        if (isDrawableGdsElement(element)) {
            findOrAddGdsLayer(data, element.layer, element.datatype);
        }
    }
}

struct GdsAffineTransform {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double tx = 0.0;
    double ty = 0.0;
};

LayoutPoint applyTransform(const GdsAffineTransform& transform, const LayoutPoint& point) {
    return LayoutPoint{.x = static_cast<std::int64_t>(
                           std::llround(transform.a * static_cast<double>(point.x) +
                                        transform.c * static_cast<double>(point.y) + transform.tx)),
                       .y = static_cast<std::int64_t>(
                           std::llround(transform.b * static_cast<double>(point.x) +
                                        transform.d * static_cast<double>(point.y) + transform.ty))};
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

GdsAffineTransform composeTransforms(const GdsAffineTransform& parent,
                                     const GdsAffineTransform& child) {
    return GdsAffineTransform{.a = parent.a * child.a + parent.c * child.b,
                              .b = parent.b * child.a + parent.d * child.b,
                              .c = parent.a * child.c + parent.c * child.d,
                              .d = parent.b * child.c + parent.d * child.d,
                              .tx = parent.a * child.tx + parent.c * child.ty + parent.tx,
                              .ty = parent.b * child.tx + parent.d * child.ty + parent.ty};
}

LayoutRect transformBounds(const GdsAffineTransform& transform, const LayoutRect& rect) {
    std::vector<LayoutPoint> points;
    points.push_back(applyTransform(transform, LayoutPoint{.x = rect.x0, .y = rect.y0}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = rect.x0, .y = rect.y1}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = rect.x1, .y = rect.y0}));
    points.push_back(applyTransform(transform, LayoutPoint{.x = rect.x1, .y = rect.y1}));
    return pointBounds(points);
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

std::optional<LayoutRect> referenceArrayBounds(const LayoutGdsReference& reference,
                                               const LayoutRect& target_bounds) {
    std::optional<LayoutRect> bounds;
    const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
    const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
    const auto expandInstance = [&](std::uint32_t column, std::uint32_t row) {
        expandBounds(bounds, transformBounds(referenceTransform(reference, column, row), target_bounds));
    };
    expandInstance(0U, 0U);
    if (columns > 1U) {
        expandInstance(columns - 1U, 0U);
    }
    if (rows > 1U) {
        expandInstance(0U, rows - 1U);
    }
    if (columns > 1U && rows > 1U) {
        expandInstance(columns - 1U, rows - 1U);
    }
    return bounds;
}

std::optional<LayoutRect> directDrawableGdsCellBounds(const LayoutGdsLibrary& gds,
                                                      const LayoutGdsCell& cell) {
    std::optional<LayoutRect> bounds;
    for (const auto element_index : cell.element_indices) {
        if (element_index >= gds.elements.size()) {
            continue;
        }
        const auto& element = gds.elements[element_index];
        if (isDrawableGdsElement(element)) {
            expandBounds(bounds, pointBounds(element.points));
        }
    }
    return bounds;
}

std::optional<LayoutRect> resolveGdsCellHierarchyBounds(LayoutGdsLibrary& gds,
                                                        std::uint32_t cell_index,
                                                        std::vector<std::uint8_t>& visiting,
                                                        std::vector<std::uint8_t>& resolved) {
    if (cell_index >= gds.cells.size()) {
        return std::nullopt;
    }
    if (resolved[cell_index] != 0U) {
        return gds.cells[cell_index].bounds;
    }
    if (visiting[cell_index] != 0U) {
        return gds.cells[cell_index].bounds;
    }

    visiting[cell_index] = 1U;
    auto bounds = directDrawableGdsCellBounds(gds, gds.cells[cell_index]);
    for (const auto reference_index : gds.cells[cell_index].reference_indices) {
        if (reference_index >= gds.references.size()) {
            continue;
        }
        const auto& reference = gds.references[reference_index];
        if (reference.target_cell_index >= gds.cells.size()) {
            continue;
        }
        const auto target_bounds =
            resolveGdsCellHierarchyBounds(gds, reference.target_cell_index, visiting, resolved);
        if (!target_bounds.has_value()) {
            continue;
        }
        const auto transformed = referenceArrayBounds(reference, *target_bounds);
        if (transformed.has_value()) {
            expandBounds(bounds, *transformed);
        }
    }
    visiting[cell_index] = 0U;
    resolved[cell_index] = 1U;
    gds.cells[cell_index].bounds = bounds;
    return bounds;
}

void resolveGdsHierarchyBounds(LayoutGdsLibrary& gds) {
    std::vector<std::uint8_t> visiting(gds.cells.size(), 0U);
    std::vector<std::uint8_t> resolved(gds.cells.size(), 0U);
    for (std::uint32_t cell_index = 0; cell_index < gds.cells.size(); ++cell_index) {
        (void)resolveGdsCellHierarchyBounds(gds, cell_index, visiting, resolved);
    }
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

void appendGdsElementShape(std::vector<LayoutShape>& shapes,
                           const LayoutDataSet& data,
                           const LayoutGdsElement& element,
                           const GdsAffineTransform& transform,
                           std::uint32_t element_index) {
    if (!isDrawableGdsElement(element)) {
        return;
    }

    LayoutShape shape{.kind = shapeKindForGdsElement(element),
                      .owner_kind = LayoutOwnerKind::GdsElement,
                      .owner_index = element_index,
                      .macro_index = element.cell_index,
                      .layer_index = findGdsLayer(data, element.layer, element.datatype),
                      .flags = element.texttype};
    shape.polygon.points.reserve(element.points.size());
    for (const auto& point : element.points) {
        shape.polygon.points.push_back(applyTransform(transform, point));
    }
    if (!shape.polygon.points.empty()) {
        shape.rect = shapeBounds(shape);
    }
    shapes.push_back(std::move(shape));
}

void flattenGdsCell(std::vector<LayoutShape>& shapes,
                    const LayoutDataSet& data,
                    const LayoutGdsLibrary& gds,
                    std::uint32_t cell_index,
                    const GdsAffineTransform& transform,
                    std::set<std::uint32_t>& stack) {
    if (cell_index >= gds.cells.size()) {
        throw std::runtime_error("Layout geometry request GDS cell index is out of range");
    }
    if (!stack.insert(cell_index).second) {
        return;
    }
    const auto& cell = gds.cells[cell_index];
    for (const auto element_index : cell.element_indices) {
        if (element_index >= gds.elements.size()) {
            continue;
        }
        appendGdsElementShape(shapes, data, gds.elements[element_index], transform, element_index);
    }
    for (const auto reference_index : cell.reference_indices) {
        if (reference_index >= gds.references.size()) {
            continue;
        }
        const auto& reference = gds.references[reference_index];
        if (reference.target_cell_index >= gds.cells.size()) {
            continue;
        }
        const auto columns = std::max<std::uint32_t>(reference.columns, 1U);
        const auto rows = std::max<std::uint32_t>(reference.rows, 1U);
        for (std::uint32_t column = 0; column < columns; ++column) {
            for (std::uint32_t row = 0; row < rows; ++row) {
                flattenGdsCell(shapes,
                               data,
                               gds,
                               reference.target_cell_index,
                               composeTransforms(transform, referenceTransform(reference, column, row)),
                               stack);
            }
        }
    }
    stack.erase(cell_index);
}

std::vector<LayoutShape> flattenRequestedGdsCells(const LayoutDataSet& data,
                                                  const LayoutGeometryRequest& request) {
    if (!data.gds.has_value()) {
        throw std::runtime_error("GDS cell geometry filter requires a GDS layout source");
    }
    const auto& gds = *data.gds;
    std::vector<std::uint32_t> root_cells = request.gds_root_cell_indices;
    if (root_cells.empty()) {
        if (gds.top_cell_index >= gds.cells.size()) {
            return {};
        }
        root_cells.push_back(gds.top_cell_index);
    }
    for (const auto cell_index : root_cells) {
        if (cell_index >= gds.cells.size()) {
            throw std::runtime_error("Layout geometry request GDS cell index is out of range");
        }
    }
    std::vector<LayoutShape> shapes;
    for (const auto cell_index : root_cells) {
        std::set<std::uint32_t> stack;
        flattenGdsCell(shapes, data, gds, cell_index, GdsAffineTransform{}, stack);
    }
    return shapes;
}

void collectReachableCells(const LayoutGdsLibrary& gds,
                           std::uint32_t cell_index,
                           std::set<std::uint32_t>& reachable,
                           std::set<std::uint32_t>& stack) {
    if (cell_index >= gds.cells.size() || !stack.insert(cell_index).second) {
        return;
    }
    if (reachable.insert(cell_index).second) {
        const auto& cell = gds.cells[cell_index];
        for (const auto reference_index : cell.reference_indices) {
            if (reference_index < gds.references.size()) {
                collectReachableCells(gds,
                                      gds.references[reference_index].target_cell_index,
                                      reachable,
                                      stack);
            }
        }
    }
    stack.erase(cell_index);
}

std::set<std::uint32_t> searchReachableCells(const LayoutGdsLibrary& gds,
                                             const LayoutSearchRequest& request) {
    std::set<std::uint32_t> reachable;
    if (request.root_cell_index == kNoLayoutIndex) {
        for (std::uint32_t index = 0; index < gds.cells.size(); ++index) {
            reachable.insert(index);
        }
        return reachable;
    }
    if (request.root_cell_index >= gds.cells.size()) {
        throw std::runtime_error("Layout search root cell index is out of range");
    }
    std::set<std::uint32_t> stack;
    collectReachableCells(gds, request.root_cell_index, reachable, stack);
    return reachable;
}

LayoutInspectClass classifySearchElement(const LayoutDataSet& data,
                                         const LayoutGdsLibrary& gds,
                                         const LayoutGdsElement& element) {
    if (element.kind == LayoutGdsElementKind::Path) {
        return LayoutInspectClass::Wire;
    }
    if (element.kind == LayoutGdsElementKind::Text) {
        return LayoutInspectClass::Label;
    }
    const auto cell_name = element.cell_index < gds.cells.size()
        ? std::string_view(gds.cells[element.cell_index].name)
        : std::string_view{};
    const auto layer_index = findGdsLayer(data, element.layer, element.datatype);
    const auto layer_name = layer_index < data.layers.size() ? std::string_view(data.layers[layer_index].name)
                                                             : std::string_view{};
    if (containsAsciiCaseInsensitive(cell_name, "pad") ||
        containsAsciiCaseInsensitive(cell_name, "io") ||
        containsAsciiCaseInsensitive(layer_name, "pad")) {
        return LayoutInspectClass::Pad;
    }
    return LayoutInspectClass::Shape;
}

bool matchesSearchBox(const LayoutSearchRequest& request, const std::optional<LayoutRect>& bounds) {
    if (!request.has_bbox) {
        return true;
    }
    return bounds.has_value() && intersects(*bounds, request.bbox);
}

bool matchesSearchBox(const LayoutSearchRequest& request, const LayoutRect& bounds) {
    return !request.has_bbox || intersects(bounds, request.bbox);
}

LayoutSearchResponse searchGdsLayout(const LayoutDataSet& data,
                                     const LayoutSearchRequest& request) {
    const auto query_start = Clock::now();
    if (!data.gds.has_value()) {
        throw std::runtime_error("Layout search requires a GDS layout source");
    }
    const auto& gds = *data.gds;
    const auto reachable = searchReachableCells(gds, request);

    LayoutSearchResponse response;
    auto addResult = [&](LayoutSearchResult result) {
        response.results.push_back(std::move(result));
    };

    if (searchKindEnabled(request.kind_mask, kSearchKindCell)) {
        for (const auto cell_index : reachable) {
            const auto& cell = gds.cells[cell_index];
            if (!containsAsciiCaseInsensitive(cell.name, request.query) ||
                !matchesSearchBox(request, cell.bounds)) {
                continue;
            }
            LayoutSearchResult result;
            result.object = LayoutSpatialObjectId{.kind = LayoutSpatialObjectKind::Cell,
                                                  .cell_index = cell_index,
                                                  .reference_index = kNoLayoutIndex,
                                                  .element_index = kNoLayoutIndex,
                                                  .layer_index = kNoLayoutIndex};
            result.bounds = cell.bounds.value_or(LayoutRect{});
            result.label = cell.name;
            result.object_class = LayoutInspectClass::Cell;
            result.source_cell_index = cell_index;
            result.rank = searchRank(cell.name, request.query, 0U);
            addResult(std::move(result));
        }
    }

    if (searchKindEnabled(request.kind_mask, kSearchKindReference)) {
        for (std::uint32_t reference_index = 0; reference_index < gds.references.size();
             ++reference_index) {
            const auto& reference = gds.references[reference_index];
            if (reachable.find(reference.parent_cell_index) == reachable.end() ||
                reference.target_cell_index >= gds.cells.size() ||
                !containsAsciiCaseInsensitive(reference.target_name, request.query)) {
                continue;
            }
            std::optional<LayoutRect> bounds;
            if (gds.cells[reference.target_cell_index].bounds.has_value()) {
                bounds = transformBounds(referenceTransform(reference, 0U, 0U),
                                         *gds.cells[reference.target_cell_index].bounds);
            }
            if (!matchesSearchBox(request, bounds)) {
                continue;
            }
            LayoutSearchResult result;
            result.object = LayoutSpatialObjectId{.kind = LayoutSpatialObjectKind::Reference,
                                                  .cell_index = reference.parent_cell_index,
                                                  .reference_index = reference_index,
                                                  .element_index = kNoLayoutIndex,
                                                  .layer_index = kNoLayoutIndex};
            result.bounds = bounds.value_or(LayoutRect{});
            result.label = reference.target_name;
            result.object_class = LayoutInspectClass::Cell;
            result.source_cell_index = reference.target_cell_index;
            result.rank = searchRank(reference.target_name, request.query, 100U);
            addResult(std::move(result));
        }
    }

    std::set<std::uint32_t> reported_layers;
    for (std::uint32_t element_index = 0; element_index < gds.elements.size(); ++element_index) {
        const auto& element = gds.elements[element_index];
        if (reachable.find(element.cell_index) == reachable.end() || !isDrawableGdsElement(element)) {
            continue;
        }
        const auto bounds = pointBounds(element.points);
        if (!matchesSearchBox(request, bounds)) {
            continue;
        }
        const auto layer_index = findGdsLayer(data, element.layer, element.datatype);
        const auto object_class = classifySearchElement(data, gds, element);
        const auto object = LayoutSpatialObjectId{.kind = LayoutSpatialObjectKind::Element,
                                                  .cell_index = element.cell_index,
                                                  .reference_index = kNoLayoutIndex,
                                                  .element_index = element_index,
                                                  .layer_index = layer_index,
                                                  .datatype = element.datatype};

        if (element.kind == LayoutGdsElementKind::Text &&
            searchKindEnabled(request.kind_mask, kSearchKindText) &&
            containsAsciiCaseInsensitive(element.text, request.query)) {
            LayoutSearchResult result;
            result.object = object;
            result.bounds = bounds;
            result.label = element.text;
            result.object_class = object_class;
            result.source_cell_index = element.cell_index;
            result.rank = searchRank(element.text, request.query, 200U);
            addResult(std::move(result));
        }

        if (searchKindEnabled(request.kind_mask, kSearchKindLayer) &&
            reported_layers.find(layer_index) == reported_layers.end() &&
            layer_index < data.layers.size() &&
            containsAsciiCaseInsensitive(data.layers[layer_index].name, request.query)) {
            LayoutSearchResult result;
            result.object = object;
            result.bounds = bounds;
            result.label = data.layers[layer_index].name;
            result.object_class = object_class;
            result.source_cell_index = element.cell_index;
            result.rank = searchRank(data.layers[layer_index].name, request.query, 300U);
            addResult(std::move(result));
            reported_layers.insert(layer_index);
        }
    }

    std::sort(response.results.begin(), response.results.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.rank != rhs.rank) {
            return lhs.rank < rhs.rank;
        }
        if (lhs.label != rhs.label) {
            return lhs.label < rhs.label;
        }
        if (lhs.object.cell_index != rhs.object.cell_index) {
            return lhs.object.cell_index < rhs.object.cell_index;
        }
        if (lhs.object.reference_index != rhs.object.reference_index) {
            return lhs.object.reference_index < rhs.object.reference_index;
        }
        return lhs.object.element_index < rhs.object.element_index;
    });
    if (request.max_results != 0U && response.results.size() > request.max_results) {
        response.results.resize(request.max_results);
    }
    response.query_micros = elapsedMicros(query_start);
    return response;
}

void appendLef(LayoutDataSet& data, const LayoutLefLibrary& lef) {
    if (data.units_per_micron == 0 || data.units_per_micron == 1000) {
        data.units_per_micron = lef.units_per_micron;
    }
    std::vector<std::uint32_t> layer_map;
    layer_map.reserve(lef.layers.size());
    for (const auto& layer : lef.layers) {
        layer_map.push_back(findOrAddLayer(data, layer));
    }

    for (const auto& via : lef.vias) {
        auto mapped_via = via;
        for (auto& shape : mapped_via.shapes) {
            if (shape.layer_index < layer_map.size()) {
                shape.layer_index = layer_map[shape.layer_index];
            }
        }
        data.vias.push_back(std::move(mapped_via));
    }
    data.sites.insert(data.sites.end(), lef.sites.begin(), lef.sites.end());

    for (auto macro : lef.macros) {
        const auto macro_index = static_cast<std::uint32_t>(data.macros.size());
        for (std::size_t pin_index = 0; pin_index < macro.pins.size(); ++pin_index) {
            auto& pin = macro.pins[pin_index];
            for (auto& port : pin.ports) {
                for (auto& shape : port.shapes) {
                    if (shape.layer_index < layer_map.size()) {
                        shape.layer_index = layer_map[shape.layer_index];
                    }
                    shape.owner_kind = LayoutOwnerKind::Pin;
                    shape.owner_index = static_cast<std::uint32_t>(pin_index);
                    shape.macro_index = macro_index;
                    appendShape(data, shape);
                }
            }
        }
        for (auto& shape : macro.obstructions) {
            if (shape.layer_index < layer_map.size()) {
                shape.layer_index = layer_map[shape.layer_index];
            }
            shape.owner_kind = LayoutOwnerKind::Obstruction;
            shape.owner_index = macro_index;
            shape.macro_index = macro_index;
            appendShape(data, shape);
        }
        data.macros.push_back(std::move(macro));
    }

    data.diagnostics.insert(data.diagnostics.end(), lef.diagnostics.begin(), lef.diagnostics.end());
}

void appendDef(LayoutDataSet& data, const LayoutDefDesign& def) {
    data.units_per_micron = def.units_per_micron == 0 ? data.units_per_micron : def.units_per_micron;
    if (def.die_area.has_value()) {
        data.bounds = def.die_area;
    }

    std::vector<std::uint32_t> layer_map;
    layer_map.reserve(def.layers.size());
    for (const auto& layer : def.layers) {
        layer_map.push_back(findOrAddLayer(data, layer));
    }

    for (const auto& component : def.components) {
        const auto owner_index = static_cast<std::uint32_t>(data.components.size());
        LayoutShape placement{.kind = LayoutShapeKind::Placement,
                              .owner_kind = LayoutOwnerKind::Component,
                              .owner_index = owner_index,
                              .layer_index = 0,
                              .rect = LayoutRect{.x0 = component.x,
                                                 .y0 = component.y,
                                                 .x1 = component.x,
                                                 .y1 = component.y}};
        appendShape(data, placement);
        data.components.push_back(component);
    }

    for (auto pin : def.pins) {
        const auto owner_index = static_cast<std::uint32_t>(data.pins.size());
        for (auto& shape : pin.shapes) {
            if (shape.layer_index < layer_map.size()) {
                shape.layer_index = layer_map[shape.layer_index];
            }
            shape.owner_kind = LayoutOwnerKind::Pin;
            shape.owner_index = owner_index;
            appendShape(data, shape);
        }
        data.pins.push_back(std::move(pin));
    }

    for (auto net : def.nets) {
        const auto owner_index = static_cast<std::uint32_t>(data.nets.size());
        for (auto& shape : net.shapes) {
            if (shape.layer_index < layer_map.size()) {
                shape.layer_index = layer_map[shape.layer_index];
            }
            shape.owner_kind = net.special ? LayoutOwnerKind::SpecialNet : LayoutOwnerKind::Net;
            shape.owner_index = owner_index;
            appendShape(data, shape);
        }
        data.nets.push_back(std::move(net));
    }

    for (auto shape : def.blockages) {
        if (shape.layer_index < layer_map.size()) {
            shape.layer_index = layer_map[shape.layer_index];
        }
        shape.owner_kind = LayoutOwnerKind::Blockage;
        shape.owner_index = static_cast<std::uint32_t>(data.shapes.size());
        appendShape(data, shape);
    }

    data.diagnostics.insert(data.diagnostics.end(), def.diagnostics.begin(), def.diagnostics.end());
}

std::string defaultTitle(const std::vector<std::filesystem::path>& lef_paths,
                         const std::optional<std::filesystem::path>& def_path,
                         std::string title) {
    if (!title.empty()) {
        return title;
    }
    if (def_path.has_value()) {
        return def_path->filename().generic_string();
    }
    if (!lef_paths.empty()) {
        return lef_paths.front().filename().generic_string();
    }
    return "layout";
}

std::string defaultGdsTitle(const std::filesystem::path& gds_path, std::string title) {
    if (!title.empty()) {
        return title;
    }
    return gds_path.filename().generic_string();
}

} // namespace

std::uint16_t LayoutSource::protocolVersion() const {
    return kLayoutProtocolVersion;
}

std::string_view LayoutSource::protocolName() const {
    return kLayoutProtocolName;
}

std::vector<std::uint8_t> LayoutSource::encodeHelloResponse() const {
    return encodeHelloResponsePayload(dataSet());
}

std::vector<std::uint8_t> LayoutSource::encodeCatalogResponse() const {
    return encodeCatalogResponsePayload(dataSet());
}

std::vector<std::uint8_t> LayoutSource::encodeGeometryResponse(
    const LayoutGeometryRequest& request) const {
    if (dataSet().gds.has_value()) {
        const auto shapes = flattenRequestedGdsCells(dataSet(), request);
        return encodeGeometryResponsePayload(dataSet(), request, shapes);
    }
    return encodeGeometryResponsePayload(dataSet(), request);
}

std::vector<std::uint8_t> LayoutSource::encodeTileGeometryResponse(
    const LayoutTileGeometryRequest& request) const {
    (void)request;
    throw std::runtime_error("Layout tile geometry requires a GDS layout source");
}

std::vector<std::uint8_t> LayoutSource::encodeHitTestResponse(
    const LayoutHitTestRequest& request) const {
    (void)request;
    throw std::runtime_error("Layout hit-test requires a GDS layout source");
}

std::vector<std::uint8_t> LayoutSource::encodeInspectResponse(
    const LayoutInspectRequest& request) const {
    (void)request;
    throw std::runtime_error("Layout inspect requires a GDS layout source");
}

std::vector<std::uint8_t> LayoutSource::encodeSelectionGeometryResponse(
    const LayoutSelectionGeometryRequest& request) const {
    (void)request;
    throw std::runtime_error("Layout selection geometry requires a GDS layout source");
}

std::vector<std::uint8_t> LayoutSource::encodeSearchResponse(
    const LayoutSearchRequest& request) const {
    (void)request;
    throw std::runtime_error("Layout search requires a GDS layout source");
}

std::shared_ptr<LayoutSource> makeDataSetLayoutSource(LayoutDataSet data, std::string source_kind) {
    return std::make_shared<DataSetLayoutSource>(std::move(data), std::move(source_kind));
}

std::shared_ptr<LayoutSource> openLefDefLayoutSource(
    const std::vector<std::filesystem::path>& lef_paths,
    const std::vector<std::string>& lef_uris,
    const std::optional<std::filesystem::path>& def_path,
    const std::optional<std::string>& def_uri,
    std::string title) {
    if (lef_paths.empty() && !def_path.has_value()) {
        throw std::runtime_error("Layout source requires at least one LEF or DEF file");
    }
    LayoutDataSet data;
    data.id = "lefdef-layout";
    data.title = defaultTitle(lef_paths, def_path, std::move(title));
    data.file_uris = lef_uris;

    for (const auto& lef_path : lef_paths) {
        auto result = parseLefFile(lef_path);
        appendLef(data, result.value);
    }

    if (def_path.has_value()) {
        auto result = parseDefFile(*def_path);
        if (def_uri.has_value()) {
            data.file_uris.push_back(*def_uri);
        }
        appendDef(data, result.value);
        if (!result.value.design_name.empty() && data.title == "layout") {
            data.title = result.value.design_name;
        }
    }

    return makeDataSetLayoutSource(std::move(data), "lefdef");
}

std::shared_ptr<LayoutSource> openGdsLayoutSource(const std::filesystem::path& gds_path,
                                                  std::string gds_uri,
                                                  std::string title) {
    const auto open_start = Clock::now();
    LayoutGdsOpenMetrics metrics;
    const auto parse_start = Clock::now();
    auto result = parseGdsFile(gds_path);
    metrics.parse_micros = elapsedMicros(parse_start);
    metrics.parse = result.value.parse_metrics;
    LayoutDataSet data;
    data.id = "gds-layout";
    data.title = defaultGdsTitle(gds_path, std::move(title));
    data.units_per_micron = result.value.units_per_micron;
    data.file_uris.push_back(std::move(gds_uri));
    data.diagnostics = result.value.diagnostics;
    data.gds = std::move(result.value);
    if (data.gds.has_value()) {
        const auto layer_start = Clock::now();
        registerGdsLayers(data, *data.gds);
        metrics.layer_register_micros = elapsedMicros(layer_start);
    }
    if (data.gds.has_value() && data.gds->top_cell_index < data.gds->cells.size()) {
        const auto bounds_start = Clock::now();
        resolveGdsHierarchyBounds(*data.gds);
        data.bounds = data.gds->cells[data.gds->top_cell_index].bounds;
        metrics.bounds_micros = elapsedMicros(bounds_start);
    }
    metrics.open_micros = elapsedMicros(open_start);
    metrics.flattened_at_open = false;
    metrics.spatial_index_built_at_open = false;
    data.gds_open_metrics = metrics;
    return std::make_shared<DataSetLayoutSource>(std::move(data), "gds");
}

} // namespace pristine::layout
