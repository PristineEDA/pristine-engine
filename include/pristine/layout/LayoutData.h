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
    std::uint32_t datatype = 0;
    std::uint64_t instance_path_hash = 0;
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
    std::uint32_t first_point = 0;
    std::uint32_t point_count = 0;
    std::vector<LayoutPoint> points{};
    std::string text{};
    std::optional<LayoutRect> bounds{};
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

struct LayoutGdsLayerSample {
    std::uint32_t layer = 0;
    std::uint32_t datatype = 0;
    std::uint32_t element_index = kNoLayoutIndex;
};

struct LayoutGdsParseMetrics {
    std::uint64_t read_micros = 0;
    std::uint64_t record_micros = 0;
    std::uint64_t xy_decode_micros = 0;
    std::uint64_t xy_bounds_micros = 0;
    std::uint64_t scalar_decode_micros = 0;
    std::uint64_t string_decode_micros = 0;
    std::uint64_t element_finalize_micros = 0;
    std::uint64_t element_finalize_bbox_micros = 0;
    std::uint64_t element_finalize_reference_micros = 0;
    std::uint64_t element_finalize_index_micros = 0;
    std::uint64_t element_finalize_sample_micros = 0;
    std::uint64_t diagnostic_micros = 0;
    std::uint64_t resolve_micros = 0;
    std::uint64_t resolve_lookup_micros = 0;
    std::uint64_t resolve_reference_micros = 0;
    std::uint64_t resolve_top_cell_micros = 0;
    std::uint64_t bbox_micros = 0;
    std::uint32_t record_count = 0;
    std::uint32_t xy_point_count = 0;
    std::uint32_t string_count = 0;
    std::uint32_t cell_count = 0;
    std::uint32_t reference_count = 0;
    std::uint32_t element_count = 0;
    std::uint32_t suppressed_diagnostic_count = 0;
    std::uint32_t arena_growth_count = 0;
    std::uint32_t cancel_check_count = 0;
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
    std::vector<LayoutPoint> points{};
    std::vector<std::uint32_t> text_element_indices{};
    std::vector<LayoutGdsLayerSample> layer_samples{};
    std::vector<LayoutDiagnostic> diagnostics{};
    LayoutGdsParseMetrics parse_metrics{};
};

struct LayoutGdsOpenMetrics {
    std::uint64_t parse_micros = 0;
    std::uint64_t layer_register_micros = 0;
    std::uint64_t bounds_micros = 0;
    std::uint64_t open_micros = 0;
    bool flattened_at_open = false;
    bool spatial_index_built_at_open = false;
    bool warmup_scheduled = false;
    std::uint32_t point_arena_count = 0;
    LayoutGdsParseMetrics parse{};
};

enum class LayoutSourceState : std::uint32_t {
    Parsing = 1,
    Ready = 2,
    Failed = 3,
    Closing = 4,
};

enum class LayoutSourcePhase : std::uint32_t {
    Unknown = 0,
    Read = 1,
    Records = 2,
    Finalize = 3,
    Resolve = 4,
    Ready = 5,
    Failed = 6,
};

struct LayoutStatus {
    LayoutSourceState state = LayoutSourceState::Ready;
    LayoutSourcePhase phase = LayoutSourcePhase::Ready;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t record_count = 0;
    std::uint32_t cell_count = 0;
    std::uint32_t reference_count = 0;
    std::uint32_t element_count = 0;
    std::uint32_t point_count = 0;
    std::uint32_t string_count = 0;
    std::uint32_t diagnostic_count = 0;
    std::uint64_t elapsed_micros = 0;
    std::uint64_t open_micros = 0;
    std::uint64_t parse_micros = 0;
    bool warmup_scheduled = false;
    bool warmup_ready = false;
    std::string error{};
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
    LayoutGdsOpenMetrics gds_open_metrics{};
};

struct LayoutGeometryRequest {
    bool has_bbox = false;
    LayoutRect bbox{};
    std::uint32_t max_shapes = 0;
    std::vector<std::uint32_t> layer_indices{};
    std::vector<LayoutShapeKind> shape_kinds{};
    std::vector<std::uint32_t> macro_indices{};
    std::vector<std::uint32_t> gds_root_cell_indices{};
};

enum class LayoutCatalogPageTableKind : std::uint32_t {
    Layers = 1,
    Cells = 2,
    References = 3,
    Elements = 4,
    Points = 5,
    Strings = 6,
    Diagnostics = 7,
};

struct LayoutCatalogPageRequest {
    LayoutCatalogPageTableKind table_kind = LayoutCatalogPageTableKind::Layers;
    std::uint32_t offset = 0;
    std::uint32_t limit = 0;
    std::uint32_t max_bytes = 0;
};

enum class LayoutSpatialObjectKind : std::uint16_t {
    Unknown = 0,
    Cell = 1,
    Reference = 2,
    Element = 3,
};

struct LayoutSpatialObjectId {
    LayoutSpatialObjectKind kind = LayoutSpatialObjectKind::Unknown;
    std::uint32_t cell_index = kNoLayoutIndex;
    std::uint32_t reference_index = kNoLayoutIndex;
    std::uint32_t element_index = kNoLayoutIndex;
    std::uint32_t layer_index = kNoLayoutIndex;
    std::uint32_t datatype = 0;
    std::uint64_t instance_path_hash = 0;
};

struct LayoutTileGeometryRequest {
    bool has_bbox = false;
    LayoutRect bbox{};
    std::uint32_t root_cell_index = kNoLayoutIndex;
    std::uint32_t max_shapes = 0;
    std::uint32_t max_points = 0;
    std::uint32_t max_bytes = 0;
    std::uint32_t lod = 0;
    std::uint32_t continuation_token = 0;
    std::vector<std::uint32_t> layer_indices{};
    std::vector<LayoutShapeKind> shape_kinds{};
    std::vector<std::uint32_t> datatypes{};
};

struct LayoutTileGeometryResult {
    std::vector<LayoutShape> shapes{};
    bool truncated = false;
    bool cache_hit = false;
    std::uint32_t next_token = 0;
    std::uint64_t index_build_micros = 0;
    std::uint64_t query_micros = 0;
    std::uint64_t encode_micros = 0;
    std::uint32_t visited_cell_count = 0;
    std::uint32_t element_candidate_count = 0;
    std::uint32_t reference_candidate_count = 0;
    std::uint32_t traversed_reference_count = 0;
    std::uint32_t lod_shape_count = 0;
    std::uint32_t cache_hit_count = 0;
    std::uint32_t cache_miss_count = 0;
    std::uint64_t grid_build_micros = 0;
    std::uint32_t grid_hit_count = 0;
    std::uint32_t grid_miss_count = 0;
    std::uint32_t grid_candidate_count = 0;
    std::uint32_t grid_bin_count = 0;
};

struct LayoutHitTestRequest {
    LayoutPoint point{};
    std::int64_t radius = 0;
    std::uint32_t root_cell_index = kNoLayoutIndex;
    std::uint32_t max_results = 16;
    std::vector<std::uint32_t> layer_indices{};
    std::vector<LayoutShapeKind> shape_kinds{};
    std::vector<std::uint32_t> datatypes{};
};

struct LayoutHitTestResult {
    LayoutSpatialObjectId object{};
    LayoutRect bounds{};
    std::uint32_t rank = 0;
    double distance = 0.0;
};

struct LayoutHitTestResponse {
    std::vector<LayoutHitTestResult> hits{};
    std::uint64_t index_build_micros = 0;
    std::uint64_t query_micros = 0;
    std::uint64_t encode_micros = 0;
    std::uint32_t tile_shape_count = 0;
    std::uint32_t precise_candidate_count = 0;
};

enum class LayoutInspectClass : std::uint16_t {
    Unknown = 0,
    Shape = 1,
    Wire = 2,
    Pad = 3,
    Label = 4,
    Cell = 5,
};

struct LayoutInspectRequest {
    LayoutSpatialObjectId object{};
};

struct LayoutInspectResult {
    LayoutSpatialObjectId object{};
    LayoutRect bounds{};
    std::string name{};
    std::string text{};
    std::string instance_path{};
    LayoutInspectClass object_class = LayoutInspectClass::Unknown;
    LayoutGdsElementKind gds_element_kind = LayoutGdsElementKind::Unknown;
    LayoutGdsElementKind gds_reference_kind = LayoutGdsElementKind::Unknown;
    std::uint32_t source_cell_index = kNoLayoutIndex;
    std::uint32_t layer = 0;
    std::uint32_t datatype = 0;
    std::uint32_t texttype = 0;
};

struct LayoutSearchRequest {
    bool has_bbox = false;
    LayoutRect bbox{};
    std::uint32_t max_results = 0;
    std::uint32_t kind_mask = 0;
    std::uint32_t root_cell_index = kNoLayoutIndex;
    std::string query{};
};

struct LayoutSearchResult {
    LayoutSpatialObjectId object{};
    LayoutRect bounds{};
    std::string label{};
    LayoutInspectClass object_class = LayoutInspectClass::Unknown;
    std::uint32_t source_cell_index = kNoLayoutIndex;
    std::uint32_t rank = 0;
};

struct LayoutSearchResponse {
    std::vector<LayoutSearchResult> results{};
    std::uint64_t index_build_micros = 0;
    std::uint64_t query_micros = 0;
    std::uint64_t encode_micros = 0;
};

struct LayoutSelectionGeometryRequest {
    LayoutSpatialObjectId object{};
};

template<typename T>
struct ParseResult {
    T value;
    std::vector<LayoutDiagnostic> diagnostics;
};

} // namespace pristine::layout
