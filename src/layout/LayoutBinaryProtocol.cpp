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
constexpr std::uint16_t kCatalogHeaderSize = 136;
constexpr std::uint16_t kGeometryHeaderSize = 96;
constexpr std::size_t kMaxPayloadSize = 128U * 1024U * 1024U;
constexpr std::uint32_t kGdsCatalogCellTopFlag = 1U;
constexpr std::uint32_t kLayoutCatalogSourceLefDef = 1U;
constexpr std::uint32_t kLayoutCatalogSourceGds = 2U;
constexpr std::uint32_t kGeometryRequestFlagHasBbox = 1U;
constexpr std::uint32_t kGeometryRequestFlagOwnerFilters = 2U;
constexpr std::uint32_t kGeometryRequestSupportedFlags =
    kGeometryRequestFlagHasBbox | kGeometryRequestFlagOwnerFilters;

struct LayoutCatalogPinShapeRange {
    std::uint32_t first_shape_index = kNoLayoutMacroIndex;
    std::uint32_t shape_count = 0;
};

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

std::uint32_t checkedCount(std::size_t value, std::string_view label) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("Layout ") + std::string(label) +
                                 " exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(value);
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

bool containsMacro(const LayoutGeometryRequest& request, std::uint32_t macro_index) {
    return request.macro_indices.empty() ||
           std::find(request.macro_indices.begin(), request.macro_indices.end(), macro_index) !=
               request.macro_indices.end();
}

void validateMacroFilters(const LayoutDataSet& data, const LayoutGeometryRequest& request) {
    if (data.gds.has_value() && !request.macro_indices.empty()) {
        throw std::runtime_error("LEF macro geometry filter is not valid for GDS layout sources");
    }
    if (data.gds.has_value()) {
        return;
    }
    for (const auto macro_index : request.macro_indices) {
        if (macro_index >= data.macros.size()) {
            throw std::runtime_error("Layout geometry request macro index is out of range");
        }
    }
}

void validateNoGdsRootFilters(const LayoutGeometryRequest& request) {
    if (!request.gds_root_cell_indices.empty()) {
        throw std::runtime_error("GDS cell geometry filter requires a GDS layout source");
    }
}

bool matchesGeometryRequest(const LayoutGeometryRequest& request, const LayoutShape& shape) {
    if (!containsLayer(request, shape.layer_index) || !containsKind(request, shape.kind) ||
        !containsMacro(request, shape.macro_index)) {
        return false;
    }
    if (request.has_bbox && !intersects(shapeBounds(shape), request.bbox)) {
        return false;
    }
    return true;
}

void readU32List(const std::uint8_t* bytes,
                 std::size_t size,
                 std::size_t& offset,
                 std::vector<std::uint32_t>& values,
                 std::string_view label) {
    const auto count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    if (static_cast<std::size_t>(count) > (size - offset) / sizeof(std::uint32_t)) {
        throw std::runtime_error(std::string("Layout geometry request ") + std::string(label) +
                                 " list is truncated");
    }
    values.reserve(values.size() + count);
    for (std::uint32_t index = 0; index < count; ++index) {
        values.push_back(readU32(bytes, size, offset));
        offset += sizeof(std::uint32_t);
    }
}

std::vector<const LayoutShape*> selectGeometryShapes(const std::vector<LayoutShape>& shapes,
                                                    const LayoutGeometryRequest& request,
                                                    bool& truncated) {
    std::vector<const LayoutShape*> selected;
    selected.reserve(shapes.size());
    truncated = false;
    for (const auto& shape : shapes) {
        if (!matchesGeometryRequest(request, shape)) {
            continue;
        }
        if (request.max_shapes != 0 && selected.size() >= request.max_shapes) {
            truncated = true;
            break;
        }
        selected.push_back(&shape);
    }
    return selected;
}

std::vector<std::vector<LayoutCatalogPinShapeRange>> buildPinShapeRanges(const LayoutDataSet& data) {
    std::vector<std::vector<LayoutCatalogPinShapeRange>> ranges;
    ranges.reserve(data.macros.size());
    for (const auto& macro : data.macros) {
        ranges.emplace_back(macro.pins.size());
    }
    for (std::size_t shape_index = 0; shape_index < data.shapes.size(); ++shape_index) {
        const auto& shape = data.shapes[shape_index];
        if (shape.owner_kind != LayoutOwnerKind::Pin ||
            shape.macro_index == kNoLayoutMacroIndex ||
            shape.macro_index >= ranges.size() ||
            shape.owner_index >= ranges[shape.macro_index].size()) {
            continue;
        }
        auto& range = ranges[shape.macro_index][shape.owner_index];
        if (range.shape_count == 0) {
            if (shape_index > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Layout shape index exceeds uint32 range");
            }
            range.first_shape_index = static_cast<std::uint32_t>(shape_index);
        }
        ++range.shape_count;
    }
    return ranges;
}

std::vector<LayoutCatalogPinShapeRange> buildDefPinShapeRanges(const LayoutDataSet& data) {
    std::vector<LayoutCatalogPinShapeRange> ranges(data.pins.size());
    for (std::size_t shape_index = 0; shape_index < data.shapes.size(); ++shape_index) {
        const auto& shape = data.shapes[shape_index];
        if (shape.owner_kind != LayoutOwnerKind::Pin ||
            shape.macro_index != kNoLayoutMacroIndex ||
            shape.owner_index >= ranges.size()) {
            continue;
        }
        auto& range = ranges[shape.owner_index];
        if (range.shape_count == 0) {
            if (shape_index > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Layout shape index exceeds uint32 range");
            }
            range.first_shape_index = static_cast<std::uint32_t>(shape_index);
        }
        ++range.shape_count;
    }
    return ranges;
}

void writeU32Header(std::vector<std::uint8_t>& result,
                    std::size_t& offset,
                    std::uint32_t value) {
    result[offset++] = static_cast<std::uint8_t>(value & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    result[offset++] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

std::vector<std::uint8_t> encodeCatalogV3ResponsePayload(const LayoutDataSet& data) {
    std::vector<std::uint8_t> result(kCatalogHeaderSize, 0);
    std::vector<std::uint8_t> strings;
    const auto pin_shape_ranges = buildPinShapeRanges(data);
    const auto def_pin_shape_ranges = buildDefPinShapeRanges(data);
    std::size_t macro_pin_count = 0;
    for (const auto& macro : data.macros) {
        macro_pin_count += macro.pins.size();
    }
    const auto* gds = data.gds.has_value() ? &*data.gds : nullptr;

    const auto layer_offset = checkedCount(result.size(), "layer offset");
    for (const auto& layer : data.layers) {
        appendU32(result, appendTableString(strings, layer.name));
        appendU16(result, static_cast<std::uint16_t>(layer.kind));
        appendU16(result, 0);
        appendF64(result, layer.pitch.value_or(0.0));
        appendF64(result, layer.width.value_or(0.0));
        appendF64(result, layer.spacing.value_or(0.0));
    }

    const auto macro_offset = checkedCount(result.size(), "macro offset");
    for (const auto& macro : data.macros) {
        appendU32(result, appendTableString(strings, macro.name));
        appendU32(result, appendTableString(strings, macro.class_name));
        appendF64(result, macro.origin_x);
        appendF64(result, macro.origin_y);
        appendF64(result, macro.size_x);
        appendF64(result, macro.size_y);
        appendCount(result, macro.pins.size());
    }

    const auto macro_pin_offset = checkedCount(result.size(), "macro pin offset");
    for (std::size_t macro_index = 0; macro_index < data.macros.size(); ++macro_index) {
        const auto& macro = data.macros[macro_index];
        const auto macro_index_u32 = checkedCount(macro_index, "macro index");
        for (std::size_t pin_index = 0; pin_index < macro.pins.size(); ++pin_index) {
            const auto& pin = macro.pins[pin_index];
            const auto pin_index_u32 = checkedCount(pin_index, "pin index");
            const auto& range = pin_shape_ranges[macro_index][pin_index];
            appendU32(result, macro_index_u32);
            appendU32(result, pin_index_u32);
            appendU32(result, appendTableString(strings, pin.name));
            appendU32(result, appendTableString(strings, pin.use));
            appendU16(result, static_cast<std::uint16_t>(pin.direction));
            appendU16(result, 0);
            appendU32(result, range.first_shape_index);
            appendU32(result, range.shape_count);
        }
    }

    const auto via_offset = checkedCount(result.size(), "via offset");
    for (const auto& via : data.vias) {
        appendU32(result, appendTableString(strings, via.name));
        appendCount(result, via.shapes.size());
    }

    const auto component_offset = checkedCount(result.size(), "component offset");
    for (const auto& component : data.components) {
        appendU32(result, appendTableString(strings, component.name));
        appendU32(result, appendTableString(strings, component.macro_name));
        appendU16(result, static_cast<std::uint16_t>(component.status));
        appendU16(result, 0);
        appendF64(result, toMicrons(component.x, data.units_per_micron));
        appendF64(result, toMicrons(component.y, data.units_per_micron));
        appendU32(result, appendTableString(strings, component.orientation));
    }

    const auto def_pin_offset = checkedCount(result.size(), "DEF pin offset");
    for (std::size_t pin_index = 0; pin_index < data.pins.size(); ++pin_index) {
        const auto& pin = data.pins[pin_index];
        const auto& range = def_pin_shape_ranges[pin_index];
        appendU32(result, appendTableString(strings, pin.name));
        appendU32(result, appendTableString(strings, pin.net_name));
        appendU16(result, static_cast<std::uint16_t>(pin.status));
        appendU16(result, 0);
        appendF64(result, toMicrons(pin.x, data.units_per_micron));
        appendF64(result, toMicrons(pin.y, data.units_per_micron));
        appendU32(result, appendTableString(strings, pin.orientation));
        appendU32(result, range.first_shape_index);
        appendU32(result, range.shape_count);
    }

    const auto net_offset = checkedCount(result.size(), "net offset");
    for (const auto& net : data.nets) {
        appendU32(result, appendTableString(strings, net.name));
        appendCount(result, net.connections.size());
        appendCount(result, net.shapes.size());
        appendU32(result, net.special ? 1U : 0U);
    }

    const auto cell_offset = checkedCount(result.size(), "GDS cell offset");
    if (gds != nullptr) {
        for (const auto& cell : gds->cells) {
            appendU32(result, appendTableString(strings, cell.name));
            appendCount(result, cell.reference_indices.empty() ? 0U : cell.reference_indices.front());
            appendCount(result, cell.reference_indices.size());
            appendCount(result, cell.element_indices.empty() ? 0U : cell.element_indices.front());
            appendCount(result, cell.element_indices.size());
            appendU32(result, cell.is_top ? kGdsCatalogCellTopFlag : 0U);
            if (cell.bounds.has_value()) {
                appendF64(result, toMicrons(cell.bounds->x0, data.units_per_micron));
                appendF64(result, toMicrons(cell.bounds->y0, data.units_per_micron));
                appendF64(result, toMicrons(cell.bounds->x1, data.units_per_micron));
                appendF64(result, toMicrons(cell.bounds->y1, data.units_per_micron));
            }
            else {
                appendF64(result, 0.0);
                appendF64(result, 0.0);
                appendF64(result, 0.0);
                appendF64(result, 0.0);
            }
        }
    }

    const auto reference_offset = checkedCount(result.size(), "GDS reference offset");
    if (gds != nullptr) {
        for (const auto& reference : gds->references) {
            appendU32(result, reference.parent_cell_index);
            appendU32(result, reference.target_cell_index);
            appendU16(result, static_cast<std::uint16_t>(reference.kind));
            appendU16(result, reference.transform.reflected ? 1U : 0U);
            appendF64(result, toMicrons(reference.origin.x, data.units_per_micron));
            appendF64(result, toMicrons(reference.origin.y, data.units_per_micron));
            appendF64(result, reference.transform.magnification);
            appendF64(result, reference.transform.angle);
            appendU32(result, reference.columns);
            appendU32(result, reference.rows);
            appendF64(result, toMicrons(reference.column_vector.x, data.units_per_micron));
            appendF64(result, toMicrons(reference.column_vector.y, data.units_per_micron));
            appendF64(result, toMicrons(reference.row_vector.x, data.units_per_micron));
            appendF64(result, toMicrons(reference.row_vector.y, data.units_per_micron));
            appendU32(result, appendTableString(strings, reference.target_name));
        }
    }

    const auto element_offset = checkedCount(result.size(), "GDS element offset");
    std::uint32_t first_point_index = 0;
    if (gds != nullptr) {
        for (const auto& element : gds->elements) {
            const auto point_count = checkedCount(element.points.size(), "GDS point count");
            appendU32(result, element.cell_index);
            appendU16(result, static_cast<std::uint16_t>(element.kind));
            appendU16(result, 0);
            appendU32(result, element.layer);
            appendU32(result, element.datatype);
            appendU32(result, element.texttype);
            appendU32(result, element.reference_index);
            appendU32(result, first_point_index);
            appendU32(result, point_count);
            appendU32(result, appendTableString(strings, element.text));
            if (first_point_index > std::numeric_limits<std::uint32_t>::max() - point_count) {
                throw std::runtime_error("Layout GDS point count exceeds uint32 range");
            }
            first_point_index += point_count;
        }
    }

    const auto point_offset = checkedCount(result.size(), "GDS point offset");
    if (gds != nullptr) {
        for (const auto& element : gds->elements) {
            for (const auto& point : element.points) {
                appendF64(result, toMicrons(point.x, data.units_per_micron));
                appendF64(result, toMicrons(point.y, data.units_per_micron));
            }
        }
    }

    const auto diagnostic_offset = checkedCount(result.size(), "diagnostic offset");
    for (const auto& diagnostic : data.diagnostics) {
        appendU16(result, static_cast<std::uint16_t>(diagnostic.severity));
        appendU16(result, 0);
        appendU32(result, static_cast<std::uint32_t>(diagnostic.line));
        appendU32(result, static_cast<std::uint32_t>(diagnostic.column));
        appendU32(result, appendTableString(strings, diagnostic.message));
    }

    alignTo(result, 4);
    const auto string_offset = checkedCount(result.size(), "GDS string offset");
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
    writeU32Header(result, offset, data.units_per_micron);
    writeU32Header(result, offset, gds != nullptr ? kLayoutCatalogSourceGds : kLayoutCatalogSourceLefDef);
    writeU32Header(result, offset, checkedCount(data.shapes.size(), "shape count"));
    writeU32Header(result, offset, data.bounds.has_value() ? 1U : 0U);
    writeU32Header(result, offset, gds != nullptr ? gds->top_cell_index : kNoLayoutIndex);
    writeU32Header(result, offset, string_offset);
    writeU32Header(result, offset, checkedCount(strings.size(), "string table"));
    writeU32Header(result, offset, checkedCount(data.layers.size(), "layer count"));
    writeU32Header(result, offset, layer_offset);
    writeU32Header(result, offset, checkedCount(data.macros.size(), "macro count"));
    writeU32Header(result, offset, macro_offset);
    writeU32Header(result, offset, checkedCount(macro_pin_count, "macro pin count"));
    writeU32Header(result, offset, macro_pin_offset);
    writeU32Header(result, offset, checkedCount(data.vias.size(), "via count"));
    writeU32Header(result, offset, via_offset);
    writeU32Header(result, offset, checkedCount(data.components.size(), "component count"));
    writeU32Header(result, offset, component_offset);
    writeU32Header(result, offset, checkedCount(data.pins.size(), "DEF pin count"));
    writeU32Header(result, offset, def_pin_offset);
    writeU32Header(result, offset, checkedCount(data.nets.size(), "net count"));
    writeU32Header(result, offset, net_offset);
    writeU32Header(result, offset, gds != nullptr ? checkedCount(gds->cells.size(), "GDS cell count") : 0U);
    writeU32Header(result, offset, cell_offset);
    writeU32Header(result, offset, gds != nullptr ? checkedCount(gds->references.size(), "GDS reference count") : 0U);
    writeU32Header(result, offset, reference_offset);
    writeU32Header(result, offset, gds != nullptr ? checkedCount(gds->elements.size(), "GDS element count") : 0U);
    writeU32Header(result, offset, element_offset);
    writeU32Header(result, offset, first_point_index);
    writeU32Header(result, offset, point_offset);
    writeU32Header(result, offset, checkedCount(data.diagnostics.size(), "diagnostic count"));
    writeU32Header(result, offset, diagnostic_offset);
    writeU32Header(result, offset, 0);

    return result;
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
    appendU16(result, frame.version);
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
                       .version = version,
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
    appendCount(result, data.gds.has_value() ? data.gds->cells.size() : data.macros.size());
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
    return encodeCatalogV3ResponsePayload(data);
}

std::vector<std::uint8_t> encodeGeometryResponsePayload(const LayoutDataSet& data,
                                                        const LayoutGeometryRequest& request) {
    validateMacroFilters(data, request);
    validateNoGdsRootFilters(request);
    return encodeGeometryResponsePayload(data, request, data.shapes);
}

std::vector<std::uint8_t> encodeGeometryResponsePayload(const LayoutDataSet& data,
                                                        const LayoutGeometryRequest& request,
                                                        const std::vector<LayoutShape>& shapes) {
    validateMacroFilters(data, request);
    if (!data.gds.has_value()) {
        validateNoGdsRootFilters(request);
    }
    bool truncated = false;
    const auto selected = selectGeometryShapes(shapes, request, truncated);

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
        appendU32(result, shape->macro_index);
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
    if ((flags & ~kGeometryRequestSupportedFlags) != 0U) {
        throw std::runtime_error("Layout geometry request has unsupported flags");
    }
    LayoutGeometryRequest request;
    request.has_bbox = (flags & kGeometryRequestFlagHasBbox) != 0U;
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
    readU32List(bytes, size, offset, request.layer_indices, "layer index");
    const auto kind_count = readU32(bytes, size, offset);
    offset += sizeof(std::uint32_t);
    if (static_cast<std::size_t>(kind_count) > (size - offset) / sizeof(std::uint32_t)) {
        throw std::runtime_error("Layout geometry request shape kind list is truncated");
    }
    for (std::uint32_t index = 0; index < kind_count; ++index) {
        request.shape_kinds.push_back(
            static_cast<LayoutShapeKind>(readU32(bytes, size, offset)));
        offset += sizeof(std::uint32_t);
    }
    if ((flags & kGeometryRequestFlagOwnerFilters) != 0U) {
        readU32List(bytes, size, offset, request.macro_indices, "macro index");
        readU32List(bytes, size, offset, request.gds_root_cell_indices, "GDS root cell index");
    }
    if (offset != size) {
        throw std::runtime_error("Layout geometry request has trailing bytes");
    }
    return request;
}

} // namespace pristine::layout
