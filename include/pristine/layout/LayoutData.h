#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pristine::layout {

inline constexpr std::uint32_t kNoLayoutMacroIndex = 0xffffffffU;
inline constexpr std::uint32_t kNoLayoutIndex = 0xffffffffU;

enum class LayoutDiagnosticSeverity : std::uint8_t {
    Warning = 1,
    Error = 2,
};

struct LayoutDiagnostic {
    LayoutDiagnosticSeverity severity = LayoutDiagnosticSeverity::Warning;
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct LayoutPoint {
    std::int64_t x = 0;
    std::int64_t y = 0;
};

struct LayoutRect {
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
    std::int64_t x1 = 0;
    std::int64_t y1 = 0;
};

struct LayoutPolygon {
    std::vector<LayoutPoint> points;
};

enum class LayoutShapeKind : std::uint16_t {
    Rect = 1,
    Polygon = 2,
    Placement = 3,
    Path = 4,
    Text = 5,
};

enum class LayoutOwnerKind : std::uint16_t {
    Unknown = 0,
    Layer = 1,
    Via = 2,
    Macro = 3,
    Pin = 4,
    Obstruction = 5,
    Component = 6,
    Net = 7,
    Blockage = 8,
    SpecialNet = 9,
    GdsCell = 10,
    GdsElement = 11,
    GdsReference = 12,
};

struct LayoutShape {
    LayoutShapeKind kind = LayoutShapeKind::Rect;
    LayoutOwnerKind owner_kind = LayoutOwnerKind::Unknown;
    std::uint32_t owner_index = 0;
    std::uint32_t macro_index = kNoLayoutMacroIndex;
    std::uint32_t layer_index = 0;
    std::uint32_t flags = 0;
    LayoutRect rect{};
    LayoutPolygon polygon{};
};

enum class LayoutLayerKind : std::uint16_t {
    Unknown = 0,
    Routing = 1,
    Cut = 2,
    Implant = 3,
    Masterslice = 4,
    Overlap = 5,
};

struct LayoutLayer {
    std::string name{};
    LayoutLayerKind kind = LayoutLayerKind::Unknown;
    std::optional<double> pitch{};
    std::optional<double> width{};
    std::optional<double> spacing{};
};

struct LayoutViaShape {
    std::uint32_t layer_index = 0;
    LayoutRect rect{};
};

struct LayoutVia {
    std::string name{};
    std::vector<LayoutViaShape> shapes{};
};

struct LayoutSite {
    std::string name{};
    double width = 0.0;
    double height = 0.0;
};

enum class LayoutPinDirection : std::uint16_t {
    Unknown = 0,
    Input = 1,
    Output = 2,
    Inout = 3,
    Feedthru = 4,
};

struct LayoutPort {
    std::vector<LayoutShape> shapes{};
};

struct LayoutPin {
    std::string name{};
    LayoutPinDirection direction = LayoutPinDirection::Unknown;
    std::string use{};
    std::vector<LayoutPort> ports{};
};

struct LayoutMacro {
    std::string name{};
    std::string class_name{};
    double origin_x = 0.0;
    double origin_y = 0.0;
    double size_x = 0.0;
    double size_y = 0.0;
    std::vector<LayoutPin> pins{};
    std::vector<LayoutShape> obstructions{};
};

struct LayoutLefLibrary {
    std::string version{};
    std::uint32_t units_per_micron = 1000;
    std::optional<double> manufacturing_grid{};
    std::vector<LayoutLayer> layers{};
    std::vector<LayoutVia> vias{};
    std::vector<LayoutSite> sites{};
    std::vector<LayoutMacro> macros{};
    std::vector<LayoutDiagnostic> diagnostics{};
};

enum class LayoutPlacementStatus : std::uint16_t {
    Unknown = 0,
    Placed = 1,
    Fixed = 2,
    Cover = 3,
    Unplaced = 4,
};

struct LayoutComponent {
    std::string name{};
    std::string macro_name{};
    LayoutPlacementStatus status = LayoutPlacementStatus::Unknown;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::string orientation{};
};

struct LayoutDefPin {
    std::string name{};
    std::string net_name{};
    LayoutPlacementStatus status = LayoutPlacementStatus::Unknown;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::string orientation{};
    std::vector<LayoutShape> shapes{};
};

struct LayoutNetConnection {
    std::string instance{};
    std::string pin{};
};

struct LayoutNet {
    std::string name{};
    std::vector<LayoutNetConnection> connections{};
    std::vector<LayoutShape> shapes{};
    bool special = false;
};

struct LayoutDefDesign {
    std::string version{};
    std::string design_name{};
    std::uint32_t units_per_micron = 1000;
    std::optional<LayoutRect> die_area{};
    std::vector<LayoutLayer> layers{};
    std::vector<LayoutComponent> components{};
    std::vector<LayoutDefPin> pins{};
    std::vector<LayoutNet> nets{};
    std::vector<LayoutShape> blockages{};
    std::vector<LayoutDiagnostic> diagnostics{};
};

enum class LayoutGdsElementKind : std::uint16_t {
    Unknown = 0,
    Boundary = 1,
    Path = 2,
    Sref = 3,
    Aref = 4,
    Text = 5,
};

struct LayoutGdsTransform {
    bool reflected = false;
    double magnification = 1.0;
    double angle = 0.0;
};

struct LayoutGdsElement {
    LayoutGdsElementKind kind = LayoutGdsElementKind::Unknown;
    std::uint32_t cell_index = kNoLayoutIndex;
    std::uint32_t layer = 0;
    std::uint32_t datatype = 0;
    std::uint32_t texttype = 0;
    std::uint32_t reference_index = kNoLayoutIndex;
    std::vector<LayoutPoint> points{};
    std::string text{};
};

struct LayoutGdsReference {
    LayoutGdsElementKind kind = LayoutGdsElementKind::Sref;
    std::uint32_t parent_cell_index = kNoLayoutIndex;
    std::uint32_t target_cell_index = kNoLayoutIndex;
    std::string target_name{};
    LayoutGdsTransform transform{};
    LayoutPoint origin{};
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    LayoutPoint column_vector{};
    LayoutPoint row_vector{};
};

struct LayoutGdsCell {
    std::string name{};
    std::vector<std::uint32_t> element_indices{};
    std::vector<std::uint32_t> reference_indices{};
    std::optional<LayoutRect> bounds{};
    bool is_top = false;
};

struct LayoutGdsLibrary {
    std::uint16_t version = 0;
    std::string name{};
    double user_unit_meters = 0.0;
    double database_unit_meters = 0.0;
    std::uint32_t units_per_micron = 1000;
    std::uint32_t top_cell_index = kNoLayoutIndex;
    std::vector<LayoutGdsCell> cells{};
    std::vector<LayoutGdsElement> elements{};
    std::vector<LayoutGdsReference> references{};
    std::vector<LayoutDiagnostic> diagnostics{};
};

struct LayoutDataSet {
    std::string id{};
    std::string title{};
    std::uint32_t units_per_micron = 1000;
    std::optional<LayoutRect> bounds{};
    std::vector<LayoutLayer> layers{};
    std::vector<LayoutVia> vias{};
    std::vector<LayoutSite> sites{};
    std::vector<LayoutMacro> macros{};
    std::vector<LayoutComponent> components{};
    std::vector<LayoutDefPin> pins{};
    std::vector<LayoutNet> nets{};
    std::vector<LayoutShape> shapes{};
    std::vector<LayoutDiagnostic> diagnostics{};
    std::vector<std::string> file_uris{};
    std::optional<LayoutGdsLibrary> gds{};
};

struct LayoutGeometryRequest {
    bool has_bbox = false;
    LayoutRect bbox{};
    std::uint32_t max_shapes = 0;
    std::vector<std::uint32_t> layer_indices{};
    std::vector<LayoutShapeKind> shape_kinds{};
};

template<typename T>
struct ParseResult {
    T value;
    std::vector<LayoutDiagnostic> diagnostics;
};

} // namespace pristine::layout
