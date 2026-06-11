#include "pristine/layout/LayoutBinaryProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace pristine::layout {
namespace {

constexpr std::uint8_t kFrameMagic[] = {'P', 'L', 'D', '1'};
constexpr std::uint8_t kCatalogMagic[] = {'P', 'L', 'C', 'T'};
constexpr std::uint8_t kGeometryMagic[] = {'P', 'L', 'G', 'E'};
constexpr std::uint16_t kFrameHeaderSize = 24;
constexpr std::uint16_t kCatalogHeaderSize = 80;
constexpr std::uint16_t kGeometryHeaderSize = 96;
constexpr std::size_t kMaxPayloadSize = 128U * 1024U * 1024U;

void requireAvailable(std::size_t size, std::size_t offset, std::size_t length) {
    if (offset > size || length > size - offset) {
        throw std::runtime_error("Layout binary payload is truncated");
    }
}

void appendCount(std::vector<std::uint8_t>& output, std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Layout binary count exceeds uint32 range");
    }
    appendU32(output, static_cast<std::uint32_t>(value));
}

std::uint32_t appendTableString(std::vector<std::uint8_t>& table, std::string_view value) {
    if (table.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Layout string table exceeds uint32 range");
    }
    const auto offset = static_cast<std::uint32_t>(table.size());
    appendString(table, value);
    return offset;
}

double toMicrons(std::int64_t value, std::uint32_t units_per_micron) {
    if (units_per_micron == 0) {
        return static_cast<double>(value);
    }
    return static_cast<double>(value) / static_cast<double>(units_per_micron);
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

bool containsLayer(const LayoutGeometryRequest& request, std::uint32_t layer_index) {
    return request.layer_indices.empty() ||
           std::find(request.layer_indices.begin(), request.layer_indices.end(), layer_index) !=
               request.layer_indices.end();
}

bool containsKind(const LayoutGeometryRequest& request, LayoutShapeKind kind) {
    return request.shape_kinds.empty() ||
           std::find(request.shape_kinds.begin(), request.shape_kinds.end(), kind) !=
               request.shape_kinds.end();
}

} // namespace

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffULL));
    }
}

void appendF64(std::vector<std::uint8_t>& output, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    appendU64(output, bits);
}

void appendString(std::vector<std::uint8_t>& output, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Layout string exceeds uint32 range");
    }
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void alignTo(std::vector<std::uint8_t>& output, std::size_t alignment) {
    if (alignment == 0) {
        return;
    }
    while (output.size() % alignment != 0) {
        output.push_back(0);
    }
}

std::uint16_t readU16(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(std::uint16_t));
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(std::uint32_t));
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t readU64(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    requireAvailable(size, offset, sizeof(std::uint64_t));
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)])
                 << (index * 8);
    }
    return value;
}

double readF64(const std::uint8_t* bytes, std::size_t size, std::size_t offset) {
    const auto bits = readU64(bytes, size, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<std::uint8_t> encodeFrame(const LayoutFrame& frame) {
    if (frame.payload.size() > kMaxPayloadSize ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Layout payload is too large");
    }
    std::vector<std::uint8_t> result;
    result.reserve(kFrameHeaderSize + frame.payload.size());
    result.insert(result.end(), std::begin(kFrameMagic), std::end(kFrameMagic));
    appendU16(result, kLayoutProtocolVersion);
    appendU16(result, static_cast<std::uint16_t>(frame.message_type));
    appendU32(result, frame.request_id);
    appendU32(result, frame.flags);
    appendU32(result, static_cast<std::uint32_t>(frame.payload.size()));
    appendU32(result, 0);
    result.insert(result.end(), frame.payload.begin(), frame.payload.end());
    return result;
}

LayoutFrame decodeFrame(const std::vector<std::uint8_t>& bytes) {
    return decodeFrame(bytes.data(), bytes.size());
}

LayoutFrame decodeFrame(const std::uint8_t* bytes, std::size_t size) {
    requireAvailable(size, 0, kFrameHeaderSize);
    if (bytes[0] != kFrameMagic[0] || bytes[1] != kFrameMagic[1] ||
        bytes[2] != kFrameMagic[2] || bytes[3] != kFrameMagic[3]) {
        throw std::runtime_error("Invalid layout frame magic");
    }
    const auto version = readU16(bytes, size, 4);
    if (version != kLayoutProtocolVersion) {
        throw std::runtime_error("Unsupported layout frame version");
    }
    const auto message_type = readU16(bytes, size, 6);
    const auto request_id = readU32(bytes, size, 8);
    const auto flags = readU32(bytes, size, 12);
    const auto payload_size = readU32(bytes, size, 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Layout payload is too large");
    }
    requireAvailable(size, kFrameHeaderSize, payload_size);
    if (size != kFrameHeaderSize + payload_size) {
        throw std::runtime_error("Layout frame has trailing bytes");
    }
    return LayoutFrame{.message_type = static_cast<LayoutMessageType>(message_type),
                       .request_id = request_id,
                       .flags = flags,
                       .payload = std::vector<std::uint8_t>(bytes + kFrameHeaderSize,
                                                            bytes + kFrameHeaderSize +
                                                                payload_size)};
}

std::vector<std::uint8_t> encodeHelloResponsePayload(const LayoutDataSet& data) {
    std::vector<std::uint8_t> result;
    appendU16(result, kLayoutProtocolVersion);
    appendU16(result, 0);
    appendU32(result, data.units_per_micron);
    appendCount(result, data.layers.size());
    appendCount(result, data.macros.size());
    appendCount(result, data.components.size());
    appendCount(result, data.nets.size());
    appendCount(result, data.shapes.size());
    appendCount(result, data.diagnostics.size());
    if (data.bounds.has_value()) {
        appendF64(result, toMicrons(data.bounds->x0, data.units_per_micron));
        appendF64(result, toMicrons(data.bounds->y0, data.units_per_micron));
        appendF64(result, toMicrons(data.bounds->x1, data.units_per_micron));
        appendF64(result, toMicrons(data.bounds->y1, data.units_per_micron));
    }
    else {
        appendF64(result, 0.0);
        appendF64(result, 0.0);
        appendF64(result, 0.0);
        appendF64(result, 0.0);
    }
    appendString(result, data.id);
    appendString(result, data.title);
    return result;
}

std::vector<std::uint8_t> encodeCatalogResponsePayload(const LayoutDataSet& data) {
    std::vector<std::uint8_t> result(kCatalogHeaderSize, 0);
    std::vector<std::uint8_t> strings;

    const auto layer_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& layer : data.layers) {
        appendU32(result, appendTableString(strings, layer.name));
        appendU16(result, static_cast<std::uint16_t>(layer.kind));
        appendU16(result, 0);
        appendF64(result, layer.pitch.value_or(0.0));
        appendF64(result, layer.width.value_or(0.0));
        appendF64(result, layer.spacing.value_or(0.0));
    }

    const auto macro_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& macro : data.macros) {
        appendU32(result, appendTableString(strings, macro.name));
        appendU32(result, appendTableString(strings, macro.class_name));
        appendF64(result, macro.origin_x);
        appendF64(result, macro.origin_y);
        appendF64(result, macro.size_x);
        appendF64(result, macro.size_y);
        appendCount(result, macro.pins.size());
    }

    const auto via_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& via : data.vias) {
        appendU32(result, appendTableString(strings, via.name));
        appendCount(result, via.shapes.size());
    }

    const auto component_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& component : data.components) {
        appendU32(result, appendTableString(strings, component.name));
        appendU32(result, appendTableString(strings, component.macro_name));
        appendU16(result, static_cast<std::uint16_t>(component.status));
        appendU16(result, 0);
        appendF64(result, toMicrons(component.x, data.units_per_micron));
        appendF64(result, toMicrons(component.y, data.units_per_micron));
        appendU32(result, appendTableString(strings, component.orientation));
    }

    const auto net_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& net : data.nets) {
        appendU32(result, appendTableString(strings, net.name));
        appendCount(result, net.connections.size());
        appendCount(result, net.shapes.size());
        appendU32(result, net.special ? 1U : 0U);
    }

    const auto diagnostic_offset = static_cast<std::uint32_t>(result.size());
    for (const auto& diagnostic : data.diagnostics) {
        appendU16(result, static_cast<std::uint16_t>(diagnostic.severity));
        appendU16(result, 0);
        appendU32(result, static_cast<std::uint32_t>(diagnostic.line));
        appendU32(result, static_cast<std::uint32_t>(diagnostic.column));
        appendU32(result, appendTableString(strings, diagnostic.message));
    }

    alignTo(result, 4);
    const auto string_offset = static_cast<std::uint32_t>(result.size());
    result.insert(result.end(), strings.begin(), strings.end());

    std::size_t offset = 0;
    result[offset++] = kCatalogMagic[0];
    result[offset++] = kCatalogMagic[1];
    result[offset++] = kCatalogMagic[2];
    result[offset++] = kCatalogMagic[3];
    result[offset++] = static_cast<std::uint8_t>(kLayoutProtocolVersion & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((kLayoutProtocolVersion >> 8U) & 0xffU);
    result[offset++] = static_cast<std::uint8_t>(kCatalogHeaderSize & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((kCatalogHeaderSize >> 8U) & 0xffU);
    auto writeHeaderU32 = [&](std::uint32_t value) {
        result[offset++] = static_cast<std::uint8_t>(value & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    };
    writeHeaderU32(data.units_per_micron);
    writeHeaderU32(static_cast<std::uint32_t>(data.layers.size()));
    writeHeaderU32(static_cast<std::uint32_t>(data.macros.size()));
    writeHeaderU32(static_cast<std::uint32_t>(data.vias.size()));
    writeHeaderU32(static_cast<std::uint32_t>(data.components.size()));
    writeHeaderU32(static_cast<std::uint32_t>(data.nets.size()));
    writeHeaderU32(static_cast<std::uint32_t>(data.diagnostics.size()));
    writeHeaderU32(layer_offset);
    writeHeaderU32(macro_offset);
    writeHeaderU32(via_offset);
    writeHeaderU32(component_offset);
    writeHeaderU32(net_offset);
    writeHeaderU32(diagnostic_offset);
    writeHeaderU32(string_offset);
    writeHeaderU32(static_cast<std::uint32_t>(strings.size()));
    writeHeaderU32(data.bounds.has_value() ? 1U : 0U);

    return result;
}

std::vector<std::uint8_t> encodeGeometryResponsePayload(const LayoutDataSet& data,
                                                        const LayoutGeometryRequest& request) {
    std::vector<const LayoutShape*> selected;
    selected.reserve(data.shapes.size());
    bool truncated = false;
    for (const auto& shape : data.shapes) {
        if (!containsLayer(request, shape.layer_index) || !containsKind(request, shape.kind)) {
            continue;
        }
        if (request.has_bbox && !intersects(shapeBounds(shape), request.bbox)) {
            continue;
        }
        if (request.max_shapes != 0 && selected.size() >= request.max_shapes) {
            truncated = true;
            break;
        }
        selected.push_back(&shape);
    }

    std::vector<std::uint8_t> result(kGeometryHeaderSize, 0);
    const auto shape_table_offset = static_cast<std::uint32_t>(result.size());
    std::vector<double> x0;
    std::vector<double> y0;
    std::vector<double> x1;
    std::vector<double> y1;
    std::vector<double> polygon_x;
    std::vector<double> polygon_y;
    x0.reserve(selected.size());
    y0.reserve(selected.size());
    x1.reserve(selected.size());
    y1.reserve(selected.size());

    for (const auto* shape : selected) {
        appendU32(result, shape->layer_index);
        appendU16(result, static_cast<std::uint16_t>(shape->kind));
        appendU16(result, static_cast<std::uint16_t>(shape->owner_kind));
        appendU32(result, shape->owner_index);
        appendU32(result, shape->flags);
        appendU32(result, static_cast<std::uint32_t>(polygon_x.size()));
        appendU32(result, static_cast<std::uint32_t>(shape->polygon.points.size()));
        const auto bounds = shapeBounds(*shape);
        x0.push_back(toMicrons(bounds.x0, data.units_per_micron));
        y0.push_back(toMicrons(bounds.y0, data.units_per_micron));
        x1.push_back(toMicrons(bounds.x1, data.units_per_micron));
        y1.push_back(toMicrons(bounds.y1, data.units_per_micron));
        for (const auto& point : shape->polygon.points) {
            polygon_x.push_back(toMicrons(point.x, data.units_per_micron));
            polygon_y.push_back(toMicrons(point.y, data.units_per_micron));
        }
    }

    alignTo(result, 8);
    const auto x0_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : x0) {
        appendF64(result, value);
    }
    const auto y0_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : y0) {
        appendF64(result, value);
    }
    const auto x1_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : x1) {
        appendF64(result, value);
    }
    const auto y1_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : y1) {
        appendF64(result, value);
    }
    const auto polygon_x_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : polygon_x) {
        appendF64(result, value);
    }
    const auto polygon_y_offset = static_cast<std::uint32_t>(result.size());
    for (const auto value : polygon_y) {
        appendF64(result, value);
    }

    std::size_t offset = 0;
    result[offset++] = kGeometryMagic[0];
    result[offset++] = kGeometryMagic[1];
    result[offset++] = kGeometryMagic[2];
    result[offset++] = kGeometryMagic[3];
    result[offset++] = static_cast<std::uint8_t>(kLayoutProtocolVersion & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((kLayoutProtocolVersion >> 8U) & 0xffU);
    result[offset++] = static_cast<std::uint8_t>(kGeometryHeaderSize & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((kGeometryHeaderSize >> 8U) & 0xffU);
    auto writeHeaderU32 = [&](std::uint32_t value) {
        result[offset++] = static_cast<std::uint8_t>(value & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        result[offset++] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    };
    writeHeaderU32(data.units_per_micron);
    writeHeaderU32(static_cast<std::uint32_t>(selected.size()));
    writeHeaderU32(static_cast<std::uint32_t>(polygon_x.size()));
    writeHeaderU32(truncated ? kLayoutFrameFlagTruncated : 0U);
    writeHeaderU32(shape_table_offset);
    writeHeaderU32(x0_offset);
    writeHeaderU32(y0_offset);
    writeHeaderU32(x1_offset);
    writeHeaderU32(y1_offset);
    writeHeaderU32(polygon_x_offset);
    writeHeaderU32(polygon_y_offset);
    writeHeaderU32(static_cast<std::uint32_t>(result.size()));
    return result;
}

std::vector<std::uint8_t> encodeErrorPayload(LayoutErrorCode code, std::string_view message) {
    std::vector<std::uint8_t> result;
    appendU32(result, static_cast<std::uint32_t>(code));
    appendString(result, message);
    return result;
}

LayoutGeometryRequest decodeGeometryRequestPayload(const std::vector<std::uint8_t>& payload) {
    const auto* bytes = payload.data();
    const auto size = payload.size();
    std::size_t offset = 0;
    requireAvailable(size, offset, 4);
    const auto flags = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    LayoutGeometryRequest request;
    request.has_bbox = (flags & 1U) != 0U;
    request.max_shapes = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    if (request.has_bbox) {
        const auto x0 = readF64(bytes, size, offset);
        offset += sizeof(double);
        const auto y0 = readF64(bytes, size, offset);
        offset += sizeof(double);
        const auto x1 = readF64(bytes, size, offset);
        offset += sizeof(double);
        const auto y1 = readF64(bytes, size, offset);
        offset += sizeof(double);
        request.bbox = LayoutRect{.x0 = static_cast<std::int64_t>(x0),
                                  .y0 = static_cast<std::int64_t>(y0),
                                  .x1 = static_cast<std::int64_t>(x1),
                                  .y1 = static_cast<std::int64_t>(y1)};
    }
    const auto layer_count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    for (std::uint32_t index = 0; index < layer_count; ++index) {
        request.layer_indices.push_back(readU32(bytes, size, offset));
        offset += sizeof(std::uint32_t);
    }
    const auto kind_count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    for (std::uint32_t index = 0; index < kind_count; ++index) {
        request.shape_kinds.push_back(
            static_cast<LayoutShapeKind>(readU32(bytes, size, offset)));
        offset += sizeof(std::uint32_t);
    }
    if (offset != size) {
        throw std::runtime_error("Layout geometry request has trailing bytes");
    }
    return request;
}

} // namespace pristine::layout
