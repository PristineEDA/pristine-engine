#include "pristine/layout/LayoutSource.h"

#include "pristine/layout/LayoutBinaryProtocol.h"
#include "pristine/layout/LayoutParser.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace pristine::layout {
namespace {

class DataSetLayoutSource final : public LayoutSource {
public:
    DataSetLayoutSource(LayoutDataSet data,
                        std::string source_kind,
                        std::uint16_t protocol_version = kLayoutProtocolVersion) :
        data_(std::move(data)),
        source_kind_(std::move(source_kind)),
        protocol_version_(protocol_version) {}

    const LayoutDataSet& dataSet() const override { return data_; }
    std::string_view sourceKind() const override { return source_kind_; }
    std::uint16_t protocolVersion() const override { return protocol_version_; }

private:
    LayoutDataSet data_;
    std::string source_kind_;
    std::uint16_t protocol_version_ = kLayoutProtocolVersion;
};

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
    const auto name = "GDS:" + std::to_string(layer) + "/" + std::to_string(datatype);
    for (std::size_t index = 0; index < data.layers.size(); ++index) {
        if (data.layers[index].name == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    data.layers.push_back(LayoutLayer{.name = name, .kind = LayoutLayerKind::Unknown});
    return static_cast<std::uint32_t>(data.layers.size() - 1U);
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

void appendGdsElementShape(LayoutDataSet& data,
                           const LayoutGdsElement& element,
                           const GdsAffineTransform& transform,
                           std::uint32_t element_index) {
    if (element.kind == LayoutGdsElementKind::Sref || element.kind == LayoutGdsElementKind::Aref ||
        element.points.empty()) {
        return;
    }

    LayoutShape shape{.kind = shapeKindForGdsElement(element),
                      .owner_kind = LayoutOwnerKind::GdsElement,
                      .owner_index = element_index,
                      .macro_index = element.cell_index,
                      .layer_index = findOrAddGdsLayer(data, element.layer, element.datatype),
                      .flags = element.texttype};
    shape.polygon.points.reserve(element.points.size());
    for (const auto& point : element.points) {
        shape.polygon.points.push_back(applyTransform(transform, point));
    }
    if (!shape.polygon.points.empty()) {
        shape.rect = shapeBounds(shape);
    }
    appendShape(data, std::move(shape));
}

void flattenGdsCell(LayoutDataSet& data,
                    const LayoutGdsLibrary& gds,
                    std::uint32_t cell_index,
                    const GdsAffineTransform& transform,
                    std::set<std::uint32_t>& stack) {
    if (cell_index >= gds.cells.size()) {
        return;
    }
    if (!stack.insert(cell_index).second) {
        data.diagnostics.push_back(LayoutDiagnostic{.severity = LayoutDiagnosticSeverity::Warning,
                                                    .message = "GDS reference cycle while flattening cell '" +
                                                               gds.cells[cell_index].name + "'"});
        return;
    }
    const auto& cell = gds.cells[cell_index];
    for (const auto element_index : cell.element_indices) {
        if (element_index >= gds.elements.size()) {
            continue;
        }
        appendGdsElementShape(data, gds.elements[element_index], transform, element_index);
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
                flattenGdsCell(data,
                               gds,
                               reference.target_cell_index,
                               composeTransforms(transform, referenceTransform(reference, column, row)),
                               stack);
            }
        }
    }
    stack.erase(cell_index);
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
    return protocolVersion() == kLayoutProtocolVersionV3 ? kLayoutProtocolNameV3
                                                         : kLayoutProtocolName;
}

std::vector<std::uint8_t> LayoutSource::encodeHelloResponse() const {
    return encodeHelloResponsePayload(dataSet(), protocolVersion());
}

std::vector<std::uint8_t> LayoutSource::encodeCatalogResponse() const {
    return encodeCatalogResponsePayload(dataSet(), protocolVersion());
}

std::vector<std::uint8_t> LayoutSource::encodeGeometryResponse(
    const LayoutGeometryRequest& request) const {
    return encodeGeometryResponsePayload(dataSet(), request, protocolVersion());
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
    auto result = parseGdsFile(gds_path);
    LayoutDataSet data;
    data.id = "gds-layout";
    data.title = defaultGdsTitle(gds_path, std::move(title));
    data.units_per_micron = result.value.units_per_micron;
    data.file_uris.push_back(std::move(gds_uri));
    data.diagnostics = result.value.diagnostics;
    data.gds = std::move(result.value);
    if (data.gds.has_value() && data.gds->top_cell_index < data.gds->cells.size()) {
        std::set<std::uint32_t> stack;
        flattenGdsCell(data,
                       *data.gds,
                       data.gds->top_cell_index,
                       GdsAffineTransform{},
                       stack);
    }
    return std::make_shared<DataSetLayoutSource>(std::move(data),
                                                 "gds",
                                                 kLayoutProtocolVersionV3);
}

} // namespace pristine::layout
