#include "pristine/layout/LayoutSource.h"

#include "pristine/layout/LayoutBinaryProtocol.h"
#include "pristine/layout/LayoutParser.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace pristine::layout {
namespace {

class DataSetLayoutSource final : public LayoutSource {
public:
    DataSetLayoutSource(LayoutDataSet data, std::string source_kind) :
        data_(std::move(data)), source_kind_(std::move(source_kind)) {}

    const LayoutDataSet& dataSet() const override { return data_; }
    std::string_view sourceKind() const override { return source_kind_; }

private:
    LayoutDataSet data_;
    std::string source_kind_;
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

} // namespace

std::vector<std::uint8_t> LayoutSource::encodeHelloResponse() const {
    return encodeHelloResponsePayload(dataSet());
}

std::vector<std::uint8_t> LayoutSource::encodeCatalogResponse() const {
    return encodeCatalogResponsePayload(dataSet());
}

std::vector<std::uint8_t> LayoutSource::encodeGeometryResponse(
    const LayoutGeometryRequest& request) const {
    return encodeGeometryResponsePayload(dataSet(), request);
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

} // namespace pristine::layout
