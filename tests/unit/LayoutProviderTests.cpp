#include "pristine/layout/LayoutBinaryProtocol.h"
#include "pristine/layout/LayoutParser.h"
#include "pristine/layout/LayoutSource.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace pristine::layout {
namespace {

constexpr std::string_view kTinyLef = R"(
VERSION 5.8 ;
UNITS DATABASE MICRONS 2000 ;
MANUFACTURINGGRID 0.005 ;
LAYER M1
  TYPE ROUTING ;
  PITCH 0.48 ;
  WIDTH 0.16 ;
  SPACING 0.16 ;
END M1
VIA VIA12
  LAYER M1 ;
    RECT -0.05 -0.05 0.05 0.05 ;
END VIA12
SITE core
  SIZE 0.48 BY 3.78 ;
END core
MACRO invx1
  CLASS CORE ;
  ORIGIN 0 0 ;
  SIZE 1.44 BY 3.78 ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER M1 ;
        RECT 0.1 0.2 0.3 0.4 ;
        POLYGON 0.4 0.4 0.6 0.4 0.6 0.6 ;
    END
  END A
  PIN VDD
    DIRECTION INOUT ;
    USE POWER ;
    PORT
      LAYER M1 ;
        RECT 0.0 3.6 1.44 3.78 ;
    END
  END VDD
  PIN VSS
    DIRECTION INOUT ;
    USE GROUND ;
    PORT
      LAYER M1 ;
        RECT 0.0 0.0 1.44 0.18 ;
    END
  END VSS
  OBS
    LAYER M1 ;
      RECT 0 0 1.44 0.1 ;
  END
END invx1
END LIBRARY
)";

constexpr std::string_view kTinyDef = R"(
VERSION 5.8 ;
DESIGN top ;
UNITS DISTANCE MICRONS 2000 ;
DIEAREA ( 0 0 ) ( 4000 2000 ) ;
COMPONENTS 1 ;
  - U1 invx1 + PLACED ( 100 200 ) N ;
END COMPONENTS
PINS 1 ;
  - IN + NET n1 + LAYER M1 ( 0 0 ) ( 20 20 ) + FIXED ( 10 10 ) N ;
END PINS
NETS 1 ;
  - n1 ( U1 A ) + ROUTED M1 ( 100 200 ) ( 300 200 ) ;
END NETS
BLOCKAGES 1 ;
  - LAYER M1 ( 5 5 ) ( 25 25 ) ;
END BLOCKAGES
END DESIGN
)";

std::filesystem::path writeFixture(std::string_view name, std::string_view contents) {
    auto path = std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream output(path);
    output << contents;
    return path;
}

std::vector<std::uint8_t> geometryRequest(std::uint32_t max_shapes) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, 0);
    appendU32(payload, max_shapes);
    appendU32(payload, 0);
    appendU32(payload, 0);
    return payload;
}

std::vector<std::uint8_t> ownerFilteredGeometryRequest(
    std::uint32_t max_shapes,
    std::initializer_list<std::uint32_t> macro_indices,
    std::initializer_list<std::uint32_t> gds_root_cell_indices) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, 2);
    appendU32(payload, max_shapes);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, static_cast<std::uint32_t>(macro_indices.size()));
    for (const auto macro_index : macro_indices) {
        appendU32(payload, macro_index);
    }
    appendU32(payload, static_cast<std::uint32_t>(gds_root_cell_indices.size()));
    for (const auto cell_index : gds_root_cell_indices) {
        appendU32(payload, cell_index);
    }
    return payload;
}

std::vector<std::uint8_t> catalogPageRequest(std::uint32_t table_kind,
                                             std::uint32_t offset = 0,
                                             std::uint32_t limit = 0,
                                             std::uint32_t max_bytes = 0,
                                             std::uint32_t flags = 0) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, flags);
    appendU32(payload, table_kind);
    appendU32(payload, offset);
    appendU32(payload, limit);
    appendU32(payload, max_bytes);
    return payload;
}

std::vector<std::uint8_t> tileGeometryRequest(std::uint32_t root_cell_index,
                                              LayoutRect bbox,
                                              std::uint32_t max_shapes = 0,
                                              std::uint32_t max_points = 0,
                                              std::uint32_t max_bytes = 0,
                                              std::uint32_t continuation_token = 0,
                                              std::uint32_t lod = 2) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, 1);
    appendU32(payload, root_cell_index);
    appendU32(payload, max_shapes);
    appendU32(payload, max_points);
    appendU32(payload, max_bytes);
    appendU32(payload, lod);
    appendU32(payload, continuation_token);
    appendF64(payload, static_cast<double>(bbox.x0));
    appendF64(payload, static_cast<double>(bbox.y0));
    appendF64(payload, static_cast<double>(bbox.x1));
    appendF64(payload, static_cast<double>(bbox.y1));
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    return payload;
}

std::vector<std::uint8_t> hitTestRequest(std::uint32_t root_cell_index,
                                         LayoutPoint point,
                                         std::int64_t radius,
                                         std::uint32_t max_results = 16) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, 0);
    appendU32(payload, root_cell_index);
    appendU32(payload, max_results);
    appendF64(payload, static_cast<double>(point.x));
    appendF64(payload, static_cast<double>(point.y));
    appendF64(payload, static_cast<double>(radius));
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    return payload;
}

std::vector<std::uint8_t> inspectRequest(LayoutSpatialObjectKind kind,
                                         std::uint32_t cell_index,
                                         std::uint32_t reference_index,
                                         std::uint32_t element_index,
                                         std::uint32_t layer_index = kNoLayoutIndex,
                                         std::uint32_t datatype = 0,
                                         std::uint64_t instance_path_hash = 0) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, 0);
    appendU32(payload, static_cast<std::uint32_t>(kind));
    appendU32(payload, cell_index);
    appendU32(payload, reference_index);
    appendU32(payload, element_index);
    appendU32(payload, layer_index);
    appendU32(payload, datatype);
    appendU64(payload, instance_path_hash);
    return payload;
}

std::vector<std::uint8_t> searchRequest(std::string_view query,
                                        std::uint32_t max_results = 16,
                                        std::uint32_t kind_mask = 0,
                                        std::uint32_t root_cell_index = kNoLayoutIndex,
                                        std::optional<LayoutRect> bbox = std::nullopt) {
    std::vector<std::uint8_t> payload;
    appendU32(payload, bbox.has_value() ? 1U : 0U);
    appendU32(payload, max_results);
    appendU32(payload, kind_mask);
    appendU32(payload, root_cell_index);
    if (bbox.has_value()) {
        appendF64(payload, static_cast<double>(bbox->x0));
        appendF64(payload, static_cast<double>(bbox->y0));
        appendF64(payload, static_cast<double>(bbox->x1));
        appendF64(payload, static_cast<double>(bbox->y1));
    }
    appendU32(payload, static_cast<std::uint32_t>(query.size()));
    payload.insert(payload.end(), query.begin(), query.end());
    return payload;
}

std::uint32_t geometryShapeCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 12);
}

std::uint32_t geometryShapeTableOffset(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 24);
}

std::uint32_t tileShapeCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 24);
}

std::uint32_t tileNextToken(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 12);
}

std::uint32_t tileLodShapeCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 72);
}

std::uint32_t tileCacheHitCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 76);
}

std::uint32_t tileCacheMissCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 80);
}

std::uint32_t searchResultCount(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 8);
}

std::uint32_t searchRowOffset(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 12);
}

std::uint32_t searchRowStride(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 16);
}

std::uint32_t searchStringOffset(const std::vector<std::uint8_t>& payload) {
    return readU32(payload.data(), payload.size(), 20);
}

std::vector<std::uint8_t> tileGeometryPayload(const std::vector<std::uint8_t>& payload) {
    const auto offset = readU32(payload.data(), payload.size(), 16);
    const auto size = readU32(payload.data(), payload.size(), 20);
    REQUIRE(offset + size <= payload.size());
    return std::vector<std::uint8_t>(payload.begin() + offset, payload.begin() + offset + size);
}

std::string readTableString(const std::vector<std::uint8_t>& payload,
                            std::uint32_t string_table_offset,
                            std::uint32_t string_offset) {
    const auto offset = static_cast<std::size_t>(string_table_offset) + string_offset;
    const auto size = readU32(payload.data(), payload.size(), offset);
    const auto begin = offset + sizeof(std::uint32_t);
    REQUIRE(begin + size <= payload.size());
    return std::string(reinterpret_cast<const char*>(payload.data() + begin), size);
}

void appendBeU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void appendBeI32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>((raw >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((raw >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((raw >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(raw & 0xffU));
}

void appendGdsRecord(std::vector<std::uint8_t>& bytes,
                     std::uint8_t record_type,
                     std::uint8_t data_type,
                     const std::vector<std::uint8_t>& payload = {}) {
    appendBeU16(bytes, static_cast<std::uint16_t>(payload.size() + 4U));
    bytes.push_back(record_type);
    bytes.push_back(data_type);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> gdsInt2(std::initializer_list<std::uint16_t> values) {
    std::vector<std::uint8_t> payload;
    for (const auto value : values) {
        appendBeU16(payload, value);
    }
    return payload;
}

std::vector<std::uint8_t> gdsInt4(std::initializer_list<std::int32_t> values) {
    std::vector<std::uint8_t> payload;
    for (const auto value : values) {
        appendBeI32(payload, value);
    }
    return payload;
}

std::vector<std::uint8_t> gdsString(std::string_view value) {
    std::vector<std::uint8_t> payload(value.begin(), value.end());
    if ((payload.size() % 2U) != 0U) {
        payload.push_back(0);
    }
    return payload;
}

std::uint64_t gdsReal8Bits(double value) {
    if (value == 0.0) {
        return 0;
    }
    std::uint64_t sign = 0;
    if (value < 0.0) {
        sign = 0x80ULL;
        value = -value;
    }
    int exponent = 64;
    while (value >= 1.0) {
        value /= 16.0;
        ++exponent;
    }
    while (value < 0.0625) {
        value *= 16.0;
        --exponent;
    }
    auto mantissa = static_cast<std::uint64_t>(
        std::llround(value * static_cast<double>(1ULL << 56U)));
    if (mantissa >= (1ULL << 56U)) {
        mantissa >>= 4U;
        ++exponent;
    }
    return ((sign | static_cast<std::uint64_t>(exponent)) << 56U) |
           (mantissa & 0x00ffffffffffffffULL);
}

std::vector<std::uint8_t> gdsReal8(std::initializer_list<double> values) {
    std::vector<std::uint8_t> payload;
    for (const auto value : values) {
        const auto bits = gdsReal8Bits(value);
        for (int shift = 56; shift >= 0; shift -= 8) {
            payload.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xffULL));
        }
    }
    return payload;
}

std::vector<std::uint8_t> tinyGds() {
    std::vector<std::uint8_t> bytes;
    appendGdsRecord(bytes, 0x00, 0x02, gdsInt2({600}));
    appendGdsRecord(bytes, 0x01, 0x02, gdsInt2({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    appendGdsRecord(bytes, 0x02, 0x06, gdsString("TINY"));
    appendGdsRecord(bytes, 0x03, 0x05, gdsReal8({1.0e-6, 1.0e-9}));
    appendGdsRecord(bytes, 0x05, 0x02, gdsInt2({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    appendGdsRecord(bytes, 0x06, 0x06, gdsString("LEAF"));
    appendGdsRecord(bytes, 0x08, 0x00);
    appendGdsRecord(bytes, 0x0d, 0x02, gdsInt2({1}));
    appendGdsRecord(bytes, 0x0e, 0x02, gdsInt2({0}));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({0, 0, 10, 0, 10, 10, 0, 10, 0, 0}));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x09, 0x00);
    appendGdsRecord(bytes, 0x0d, 0x02, gdsInt2({2}));
    appendGdsRecord(bytes, 0x0e, 0x02, gdsInt2({1}));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({0, 20, 20, 20}));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x0c, 0x00);
    appendGdsRecord(bytes, 0x0d, 0x02, gdsInt2({3}));
    appendGdsRecord(bytes, 0x16, 0x02, gdsInt2({7}));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({5, 5}));
    appendGdsRecord(bytes, 0x19, 0x06, gdsString("label"));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x07, 0x00);
    appendGdsRecord(bytes, 0x05, 0x02, gdsInt2({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    appendGdsRecord(bytes, 0x06, 0x06, gdsString("TOP"));
    appendGdsRecord(bytes, 0x0a, 0x00);
    appendGdsRecord(bytes, 0x12, 0x06, gdsString("LEAF"));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({100, 200}));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x0b, 0x00);
    appendGdsRecord(bytes, 0x12, 0x06, gdsString("LEAF"));
    appendGdsRecord(bytes, 0x13, 0x02, gdsInt2({2, 1}));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({200, 300, 250, 300, 200, 350}));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x07, 0x00);
    appendGdsRecord(bytes, 0x04, 0x00);
    return bytes;
}

std::vector<std::uint8_t> missingTargetGds() {
    std::vector<std::uint8_t> bytes;
    appendGdsRecord(bytes, 0x00, 0x02, gdsInt2({600}));
    appendGdsRecord(bytes, 0x01, 0x02, gdsInt2({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    appendGdsRecord(bytes, 0x02, 0x06, gdsString("BROKEN"));
    appendGdsRecord(bytes, 0x03, 0x05, gdsReal8({1.0e-6, 1.0e-9}));
    appendGdsRecord(bytes, 0x05, 0x02, gdsInt2({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    appendGdsRecord(bytes, 0x06, 0x06, gdsString("TOP"));
    appendGdsRecord(bytes, 0x0a, 0x00);
    appendGdsRecord(bytes, 0x12, 0x06, gdsString("MISSING"));
    appendGdsRecord(bytes, 0x10, 0x03, gdsInt4({0, 0}));
    appendGdsRecord(bytes, 0x11, 0x00);
    appendGdsRecord(bytes, 0x07, 0x00);
    appendGdsRecord(bytes, 0x04, 0x00);
    return bytes;
}

} // namespace

TEST_CASE("LEF parser captures layers macros pins vias and geometry", "[layout][lef]") {
    auto result = parseLef(kTinyLef, "tiny.lef");

    CHECK(result.value.version == "5.8");
    CHECK(result.value.units_per_micron == 2000);
    REQUIRE(result.value.layers.size() == 1);
    CHECK(result.value.layers[0].name == "M1");
    CHECK(result.value.layers[0].kind == LayoutLayerKind::Routing);
    REQUIRE(result.value.vias.size() == 1);
    CHECK(result.value.vias[0].shapes.size() == 1);
    REQUIRE(result.value.macros.size() == 1);
    CHECK(result.value.macros[0].name == "invx1");
    CHECK(result.value.macros[0].pins.size() == 3);
    CHECK(result.value.macros[0].pins[0].direction == LayoutPinDirection::Input);
    CHECK(result.value.macros[0].pins[1].name == "VDD");
    CHECK(result.value.macros[0].pins[1].use == "POWER");
    CHECK(result.value.macros[0].pins[2].name == "VSS");
    CHECK(result.value.macros[0].pins[2].use == "GROUND");
    REQUIRE(result.value.macros[0].pins[0].ports.size() == 1);
    CHECK(result.value.macros[0].pins[0].ports[0].shapes.size() == 2);
    CHECK(result.value.macros[0].obstructions.size() == 1);
}

TEST_CASE("DEF parser captures components pins nets blockages and units", "[layout][def]") {
    auto result = parseDef(kTinyDef, "tiny.def");

    CHECK(result.value.version == "5.8");
    CHECK(result.value.design_name == "top");
    CHECK(result.value.units_per_micron == 2000);
    REQUIRE(result.value.die_area.has_value());
    CHECK(result.value.die_area->x1 == 4000);
    REQUIRE(result.value.components.size() == 1);
    CHECK(result.value.components[0].name == "U1");
    CHECK(result.value.components[0].status == LayoutPlacementStatus::Placed);
    REQUIRE(result.value.pins.size() == 1);
    CHECK(result.value.pins[0].shapes.size() == 1);
    REQUIRE(result.value.nets.size() == 1);
    CHECK(result.value.nets[0].connections.size() == 1);
    CHECK(result.value.nets[0].shapes.size() == 1);
    CHECK(result.value.blockages.size() == 1);
}

TEST_CASE("GDS parser captures hierarchy elements references and source v3 catalog",
          "[layout][gds][protocol]") {
    const auto gds_path = std::filesystem::temp_directory_path() / "pristine-layout-tiny.gds";
    {
        std::ofstream output(gds_path, std::ios::binary);
        const auto bytes = tinyGds();
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    auto parsed = parseGdsFile(gds_path);
    CHECK(parsed.value.version == 600);
    CHECK(parsed.value.name == "TINY");
    CHECK(parsed.value.units_per_micron == 1000);
    REQUIRE(parsed.value.cells.size() == 2);
    CHECK(parsed.value.cells[0].name == "LEAF");
    CHECK(parsed.value.cells[1].name == "TOP");
    CHECK(parsed.value.top_cell_index == 1);
    CHECK(parsed.value.cells[1].is_top);
    CHECK(parsed.value.elements.size() == 5);
    CHECK(parsed.value.parse_metrics.record_count > 0);
    CHECK(parsed.value.parse_metrics.xy_point_count > 0);
    CHECK(parsed.value.parse_metrics.string_count > 0);
    CHECK(parsed.value.parse_metrics.cell_count == 2);
    CHECK(parsed.value.parse_metrics.reference_count == 2);
    CHECK(parsed.value.parse_metrics.element_count == 5);
    REQUIRE(parsed.value.references.size() == 2);
    CHECK(parsed.value.references[0].target_cell_index == 0);
    CHECK(parsed.value.references[1].kind == LayoutGdsElementKind::Aref);
    CHECK(parsed.value.references[1].columns == 2);

    auto source = openGdsLayoutSource(gds_path, "file:///tiny.gds", "tiny-gds");
    CHECK(source->sourceKind() == "gds");
    CHECK(source->protocolVersion() == kLayoutProtocolVersion);
    const auto& data = source->dataSet();
    CHECK(data.title == "tiny-gds");
    REQUIRE(data.gds.has_value());
    CHECK(data.gds->cells.size() == 2);
    CHECK(data.shapes.empty());
    REQUIRE(data.bounds.has_value());
    CHECK(data.bounds->x0 == 100);
    CHECK(data.bounds->y0 == 200);
    CHECK(data.bounds->x1 == 270);
    CHECK(data.bounds->y1 == 320);
    REQUIRE(data.gds->cells[1].bounds.has_value());
    CHECK(data.gds->cells[1].bounds->x0 == data.bounds->x0);
    CHECK(data.gds->cells[1].bounds->y0 == data.bounds->y0);
    CHECK(data.gds->cells[1].bounds->x1 == data.bounds->x1);
    CHECK(data.gds->cells[1].bounds->y1 == data.bounds->y1);

    const auto frame = encodeFrame(LayoutFrame{.message_type = LayoutMessageType::Hello,
                                               .request_id = 7,
                                               .flags = 0,
                                               .version = kLayoutProtocolVersion});
    const auto decoded = decodeFrame(frame);
    CHECK(decoded.version == kLayoutProtocolVersion);
    CHECK(decoded.message_type == LayoutMessageType::Hello);

    const auto hello = source->encodeHelloResponse();
    CHECK(readU16(hello.data(), hello.size(), 0) == kLayoutProtocolVersion);
    CHECK(readU32(hello.data(), hello.size(), 12) == 2);

    const auto catalog = source->encodeCatalogResponse();
    CHECK(std::string(reinterpret_cast<const char*>(catalog.data()), 4) == "PLCT");
    CHECK(readU16(catalog.data(), catalog.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(catalog.data(), catalog.size(), 6) == 136);
    CHECK(readU32(catalog.data(), catalog.size(), 12) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), 16) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 36) == 3);
    CHECK(readU32(catalog.data(), catalog.size(), 44) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 52) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 76) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 92) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), 100) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), 108) == 5);
    const auto cell_offset = readU32(catalog.data(), catalog.size(), 96);
    const auto string_offset = readU32(catalog.data(), catalog.size(), 28);
    CHECK(readTableString(catalog,
                          string_offset,
                          readU32(catalog.data(), catalog.size(), cell_offset)) == "LEAF");
    constexpr std::uint32_t kGdsCellStride = 56;
    CHECK(readTableString(catalog,
                          string_offset,
                          readU32(catalog.data(), catalog.size(), cell_offset + kGdsCellStride)) ==
          "TOP");
    CHECK(readU32(catalog.data(), catalog.size(), cell_offset + kGdsCellStride + 20) == 1);

    const auto catalog_summary = source->encodeCatalogSummaryResponse();
    CHECK(std::string(reinterpret_cast<const char*>(catalog_summary.data()), 4) == "PLCS");
    CHECK(readU16(catalog_summary.data(), catalog_summary.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(catalog_summary.data(), catalog_summary.size(), 6) == 152);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 12) == 2);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 16) == 0);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 20) == 1);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 24) == 1);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 60) == 3);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 88) == 2);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 92) == 2);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 96) == 5);
    CHECK(readU32(catalog_summary.data(), catalog_summary.size(), 100) > 0);
    const auto summary_string_offset = readU32(catalog_summary.data(), catalog_summary.size(), 112);
    const auto summary_layer_offset = readU32(catalog_summary.data(), catalog_summary.size(), 64);
    CHECK(readTableString(catalog_summary,
                          summary_string_offset,
                          readU32(catalog_summary.data(), catalog_summary.size(), summary_layer_offset)) ==
          "GDS:1/0");

    const auto first_cell_page = source->encodeCatalogPageResponse(
        decodeCatalogPageRequestPayload(catalogPageRequest(2, 0, 1)));
    CHECK(std::string(reinterpret_cast<const char*>(first_cell_page.data()), 4) == "PLCP");
    CHECK(readU16(first_cell_page.data(), first_cell_page.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(first_cell_page.data(), first_cell_page.size(), 6) == 40);
    CHECK(readU32(first_cell_page.data(), first_cell_page.size(), 8) == 2);
    CHECK(readU32(first_cell_page.data(), first_cell_page.size(), 12) == 0);
    CHECK(readU32(first_cell_page.data(), first_cell_page.size(), 16) == 1);
    CHECK(readU32(first_cell_page.data(), first_cell_page.size(), 20) == 2);
    CHECK(readU32(first_cell_page.data(), first_cell_page.size(), 24) == 1);
    const auto page_string_offset = readU32(first_cell_page.data(), first_cell_page.size(), 28);
    CHECK(readTableString(first_cell_page,
                          page_string_offset,
                          readU32(first_cell_page.data(), first_cell_page.size(), 40)) == "LEAF");

    const auto top_cell_page = source->encodeCatalogPageResponse(
        decodeCatalogPageRequestPayload(catalogPageRequest(2, 1, 1)));
    CHECK(readU32(top_cell_page.data(), top_cell_page.size(), 12) == 1);
    CHECK(readU32(top_cell_page.data(), top_cell_page.size(), 16) == 1);
    CHECK(readU32(top_cell_page.data(), top_cell_page.size(), 24) == kNoLayoutIndex);
    const auto top_page_string_offset = readU32(top_cell_page.data(), top_cell_page.size(), 28);
    CHECK(readTableString(top_cell_page,
                          top_page_string_offset,
                          readU32(top_cell_page.data(), top_cell_page.size(), 40)) == "TOP");

    const auto layer_page = source->encodeCatalogPageResponse(
        decodeCatalogPageRequestPayload(catalogPageRequest(1, 0, 2)));
    CHECK(readU32(layer_page.data(), layer_page.size(), 8) == 1);
    CHECK(readU32(layer_page.data(), layer_page.size(), 16) == 2);
    CHECK(readU32(layer_page.data(), layer_page.size(), 20) == 3);
    CHECK(readU32(layer_page.data(), layer_page.size(), 24) == 2);

    const auto geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        geometryRequest(2)));
    CHECK(readU16(geometry.data(), geometry.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU32(geometry.data(), geometry.size(), 12) == 2);
    CHECK(readU32(geometry.data(), geometry.size(), 20) == kLayoutFrameFlagTruncated);

    const auto leaf_geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        ownerFilteredGeometryRequest(0, {}, {0})));
    CHECK(readU16(leaf_geometry.data(), leaf_geometry.size(), 4) == kLayoutProtocolVersion);
    CHECK(geometryShapeCount(leaf_geometry) == 3);
    auto leaf_shape_offset = geometryShapeTableOffset(leaf_geometry);
    constexpr std::uint32_t kShapeTableStride = 28;
    for (std::uint32_t shape_index = 0; shape_index < geometryShapeCount(leaf_geometry);
         ++shape_index) {
        CHECK(readU16(leaf_geometry.data(),
                      leaf_geometry.size(),
                      leaf_shape_offset + (shape_index * kShapeTableStride) + 6) ==
              static_cast<std::uint16_t>(LayoutOwnerKind::GdsElement));
        CHECK(readU32(leaf_geometry.data(),
                      leaf_geometry.size(),
                      leaf_shape_offset + (shape_index * kShapeTableStride) + 12) == 0);
    }

    const auto top_geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        ownerFilteredGeometryRequest(0, {}, {1})));
    CHECK(readU16(top_geometry.data(), top_geometry.size(), 4) == kLayoutProtocolVersion);
    CHECK(geometryShapeCount(top_geometry) == 9);
    const auto top_shape_offset = geometryShapeTableOffset(top_geometry);
    bool saw_leaf_source_cell = false;
    for (std::uint32_t shape_index = 0; shape_index < geometryShapeCount(top_geometry);
         ++shape_index) {
        saw_leaf_source_cell =
            saw_leaf_source_cell ||
            readU32(top_geometry.data(),
                    top_geometry.size(),
                    top_shape_offset + (shape_index * kShapeTableStride) + 12) == 0;
    }
    CHECK(saw_leaf_source_cell);
    CHECK_THROWS_WITH(source->encodeGeometryResponse(decodeGeometryRequestPayload(
                          ownerFilteredGeometryRequest(0, {}, {42}))),
                      Catch::Matchers::ContainsSubstring("GDS cell index is out of range"));

    std::error_code error;
    std::filesystem::remove(gds_path, error);
}

TEST_CASE("GDS spatial index serves tile hit inspect and selection payloads",
          "[layout][gds][spatial][protocol]") {
    const auto gds_path = std::filesystem::temp_directory_path() / "pristine-layout-spatial.gds";
    {
        std::ofstream output(gds_path, std::ios::binary);
        const auto bytes = tinyGds();
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    auto source = openGdsLayoutSource(gds_path, "file:///spatial.gds", "spatial-gds");
    const auto tile_request = decodeTileGeometryRequestPayload(tileGeometryRequest(
        1,
        LayoutRect{.x0 = 90, .y0 = 190, .x1 = 275, .y1 = 360},
        2));
    CHECK(tile_request.root_cell_index == 1);
    CHECK(tile_request.max_shapes == 2);
    auto tile = source->encodeTileGeometryResponse(tile_request);
    CHECK(std::string(reinterpret_cast<const char*>(tile.data()), 4) == "PLTG");
    CHECK(readU16(tile.data(), tile.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(tile.data(), tile.size(), 6) >= 84);
    CHECK(readU32(tile.data(), tile.size(), 8) == kLayoutFrameFlagTruncated);
    CHECK(tileShapeCount(tile) == 2);
    CHECK(readU32(tile.data(), tile.size(), 56) >= 1);
    CHECK(tileLodShapeCount(tile) == 2);
    CHECK(tileCacheHitCount(tile) == 0);
    CHECK(tileCacheMissCount(tile) == 1);
    auto tile_geometry = tileGeometryPayload(tile);
    CHECK(std::string(reinterpret_cast<const char*>(tile_geometry.data()), 4) == "PLGE");
    CHECK(geometryShapeCount(tile_geometry) == 2);

    const auto cached_tile = source->encodeTileGeometryResponse(tile_request);
    CHECK(tileCacheHitCount(cached_tile) == 1);
    CHECK(tileCacheMissCount(cached_tile) == 0);

    const auto precise_tile_request = decodeTileGeometryRequestPayload(tileGeometryRequest(
        1,
        LayoutRect{.x0 = 90, .y0 = 190, .x1 = 275, .y1 = 360},
        0,
        0,
        0,
        0,
        0));
    const auto precise_tile = source->encodeTileGeometryResponse(precise_tile_request);
    auto precise_geometry = tileGeometryPayload(precise_tile);
    CHECK(geometryShapeCount(precise_geometry) == 9);
    CHECK(tileLodShapeCount(precise_tile) == 0);

    const auto medium_tile_request = decodeTileGeometryRequestPayload(tileGeometryRequest(
        1,
        LayoutRect{.x0 = 90, .y0 = 190, .x1 = 275, .y1 = 360},
        0,
        0,
        0,
        0,
        1));
    const auto medium_tile = source->encodeTileGeometryResponse(medium_tile_request);
    auto medium_geometry = tileGeometryPayload(medium_tile);
    CHECK(geometryShapeCount(medium_geometry) == 9);
    CHECK(tileLodShapeCount(medium_tile) == 9);

    const auto page1_request = decodeTileGeometryRequestPayload(tileGeometryRequest(
        1,
        LayoutRect{.x0 = 90, .y0 = 190, .x1 = 275, .y1 = 360},
        1,
        0,
        0,
        0,
        0));
    const auto page1 = source->encodeTileGeometryResponse(page1_request);
    CHECK(readU32(page1.data(), page1.size(), 8) == kLayoutFrameFlagTruncated);
    REQUIRE(tileNextToken(page1) != 0);
    const auto page2_request = decodeTileGeometryRequestPayload(tileGeometryRequest(
        1,
        LayoutRect{.x0 = 90, .y0 = 190, .x1 = 275, .y1 = 360},
        1,
        0,
        0,
        tileNextToken(page1),
        0));
    const auto page2 = source->encodeTileGeometryResponse(page2_request);
    CHECK(tileShapeCount(page2) == 1);
    CHECK_THROWS_WITH(source->encodeTileGeometryResponse(decodeTileGeometryRequestPayload(
                          tileGeometryRequest(1,
                                              LayoutRect{.x0 = 90,
                                                         .y0 = 190,
                                                         .x1 = 275,
                                                         .y1 = 360},
                                              1,
                                              0,
                                              0,
                                              0x00abcdef,
                                              0))),
                      Catch::Matchers::ContainsSubstring("continuation token"));

    const auto hit_request = decodeHitTestRequestPayload(
        hitTestRequest(1, LayoutPoint{.x = 105, .y = 205}, 5));
    const auto hit = source->encodeHitTestResponse(hit_request);
    CHECK(std::string(reinterpret_cast<const char*>(hit.data()), 4) == "PLHT");
    CHECK(readU16(hit.data(), hit.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(hit.data(), hit.size(), 6) >= 64);
    REQUIRE(readU32(hit.data(), hit.size(), 8) >= 1);
    const auto hit_row_offset = readU32(hit.data(), hit.size(), 12);
    CHECK(readU32(hit.data(), hit.size(), 16) == 80);
    CHECK(readU32(hit.data(), hit.size(), 56) >= readU32(hit.data(), hit.size(), 8));
    CHECK(readU16(hit.data(), hit.size(), hit_row_offset) ==
          static_cast<std::uint16_t>(LayoutSpatialObjectKind::Element));
    const auto hit_element_index = readU32(hit.data(), hit.size(), hit_row_offset + 12);
    const auto hit_layer_index = readU32(hit.data(), hit.size(), hit_row_offset + 16);
    const auto hit_datatype = readU32(hit.data(), hit.size(), hit_row_offset + 20);
    const auto hit_instance_hash = readU64(hit.data(), hit.size(), hit_row_offset + 32);
    CHECK(hit_instance_hash != 0);

    const auto inspect_payload = source->encodeInspectResponse(decodeInspectRequestPayload(
        inspectRequest(LayoutSpatialObjectKind::Element,
                       kNoLayoutIndex,
                       kNoLayoutIndex,
                       hit_element_index,
                       hit_layer_index,
                       hit_datatype,
                       hit_instance_hash)));
    CHECK(std::string(reinterpret_cast<const char*>(inspect_payload.data()), 4) == "PLIN");
    CHECK(readU16(inspect_payload.data(), inspect_payload.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(inspect_payload.data(), inspect_payload.size(), 6) >= 144);
    CHECK(readU32(inspect_payload.data(), inspect_payload.size(), 8) ==
          static_cast<std::uint32_t>(LayoutSpatialObjectKind::Element));
    CHECK(readU32(inspect_payload.data(), inspect_payload.size(), 20) == hit_element_index);
    CHECK(readU64(inspect_payload.data(), inspect_payload.size(), 32) == hit_instance_hash);
    CHECK(readU32(inspect_payload.data(), inspect_payload.size(), 116) ==
          static_cast<std::uint32_t>(LayoutInspectClass::Shape));
    CHECK(readU32(inspect_payload.data(), inspect_payload.size(), 120) == 0);
    const auto inspect_string_offset = readU32(inspect_payload.data(), inspect_payload.size(), 68);
    const auto inspect_instance_offset =
        readU32(inspect_payload.data(), inspect_payload.size(), 124);
    CHECK_FALSE(readTableString(inspect_payload, inspect_string_offset, inspect_instance_offset)
                    .empty());

    const auto text_inspect = source->encodeInspectResponse(decodeInspectRequestPayload(
        inspectRequest(LayoutSpatialObjectKind::Element, kNoLayoutIndex, kNoLayoutIndex, 2)));
    CHECK(readU32(text_inspect.data(), text_inspect.size(), 116) ==
          static_cast<std::uint32_t>(LayoutInspectClass::Label));
    const auto path_inspect = source->encodeInspectResponse(decodeInspectRequestPayload(
        inspectRequest(LayoutSpatialObjectKind::Element, kNoLayoutIndex, kNoLayoutIndex, 1)));
    CHECK(readU32(path_inspect.data(), path_inspect.size(), 116) ==
          static_cast<std::uint32_t>(LayoutInspectClass::Wire));

    const auto selection = source->encodeSelectionGeometryResponse(
        decodeSelectionGeometryRequestPayload(inspectRequest(LayoutSpatialObjectKind::Element,
                                                             kNoLayoutIndex,
                                                             kNoLayoutIndex,
                                                             hit_element_index,
                                                             hit_layer_index,
                                                             hit_datatype,
                                                             hit_instance_hash)));
    CHECK(std::string(reinterpret_cast<const char*>(selection.data()), 4) == "PLGE");
    CHECK(geometryShapeCount(selection) == 1);

    constexpr std::uint32_t kSearchKindCell = 1U << 0U;
    constexpr std::uint32_t kSearchKindText = 1U << 2U;
    constexpr std::uint32_t kSearchKindLayer = 1U << 3U;
    const auto cell_search = source->encodeSearchResponse(
        decodeSearchRequestPayload(searchRequest("TOP", 4, kSearchKindCell, 1)));
    CHECK(std::string(reinterpret_cast<const char*>(cell_search.data()), 4) == "PLSR");
    CHECK(readU16(cell_search.data(), cell_search.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(cell_search.data(), cell_search.size(), 6) == 56);
    REQUIRE(searchResultCount(cell_search) >= 1);
    CHECK(searchRowStride(cell_search) == 88);
    const auto cell_row = searchRowOffset(cell_search);
    const auto cell_strings = searchStringOffset(cell_search);
    CHECK(readU16(cell_search.data(), cell_search.size(), cell_row) ==
          static_cast<std::uint16_t>(LayoutSpatialObjectKind::Cell));
    CHECK(readU16(cell_search.data(), cell_search.size(), cell_row + 2) ==
          static_cast<std::uint16_t>(LayoutInspectClass::Cell));
    CHECK(readU32(cell_search.data(), cell_search.size(), cell_row + 4) == 1);
    CHECK(readTableString(cell_search,
                          cell_strings,
                          readU32(cell_search.data(), cell_search.size(), cell_row + 24)) == "TOP");

    const auto text_search = source->encodeSearchResponse(
        decodeSearchRequestPayload(searchRequest("label", 4, kSearchKindText, 1)));
    REQUIRE(searchResultCount(text_search) >= 1);
    const auto text_row = searchRowOffset(text_search);
    CHECK(readU16(text_search.data(), text_search.size(), text_row) ==
          static_cast<std::uint16_t>(LayoutSpatialObjectKind::Element));
    CHECK(readU16(text_search.data(), text_search.size(), text_row + 2) ==
          static_cast<std::uint16_t>(LayoutInspectClass::Label));
    const auto text_search_element = readU32(text_search.data(), text_search.size(), text_row + 12);
    const auto text_search_layer = readU32(text_search.data(), text_search.size(), text_row + 16);
    const auto text_search_datatype = readU32(text_search.data(), text_search.size(), text_row + 20);
    const auto text_search_inspect = source->encodeInspectResponse(decodeInspectRequestPayload(
        inspectRequest(LayoutSpatialObjectKind::Element,
                       kNoLayoutIndex,
                       kNoLayoutIndex,
                       text_search_element,
                       text_search_layer,
                       text_search_datatype)));
    CHECK(readU32(text_search_inspect.data(), text_search_inspect.size(), 116) ==
          static_cast<std::uint32_t>(LayoutInspectClass::Label));
    const auto text_search_selection = source->encodeSelectionGeometryResponse(
        decodeSelectionGeometryRequestPayload(inspectRequest(LayoutSpatialObjectKind::Element,
                                                             kNoLayoutIndex,
                                                             kNoLayoutIndex,
                                                             text_search_element,
                                                             text_search_layer,
                                                             text_search_datatype)));
    CHECK(geometryShapeCount(text_search_selection) == 1);

    const auto layer_search = source->encodeSearchResponse(
        decodeSearchRequestPayload(searchRequest("GDS:1", 4, kSearchKindLayer, 1)));
    REQUIRE(searchResultCount(layer_search) >= 1);
    CHECK(readU16(layer_search.data(), layer_search.size(), searchRowOffset(layer_search)) ==
          static_cast<std::uint16_t>(LayoutSpatialObjectKind::Element));
    CHECK_THROWS_WITH(source->encodeSearchResponse(
                          decodeSearchRequestPayload(searchRequest("TOP", 4, kSearchKindCell, 42))),
                      Catch::Matchers::ContainsSubstring("root cell index is out of range"));

    CHECK_THROWS_WITH(source->encodeTileGeometryResponse(decodeTileGeometryRequestPayload(
                          tileGeometryRequest(42,
                                              LayoutRect{.x0 = 0, .y0 = 0, .x1 = 1, .y1 = 1}))),
                      Catch::Matchers::ContainsSubstring("root cell index is out of range"));

    auto unsupported = tileGeometryRequest(1, LayoutRect{.x0 = 0, .y0 = 0, .x1 = 1, .y1 = 1});
    unsupported[0] = 4;
    CHECK_THROWS_WITH(decodeTileGeometryRequestPayload(unsupported),
                      Catch::Matchers::ContainsSubstring("unsupported flags"));
    unsupported = tileGeometryRequest(1, LayoutRect{.x0 = 0, .y0 = 0, .x1 = 1, .y1 = 1});
    unsupported.push_back(0);
    CHECK_THROWS_WITH(decodeTileGeometryRequestPayload(unsupported),
                      Catch::Matchers::ContainsSubstring("trailing bytes"));

    std::error_code error;
    std::filesystem::remove(gds_path, error);
}

TEST_CASE("GDS parser reports recoverable and hard malformed input", "[layout][gds]") {
    auto parsed = parseGds(missingTargetGds(), "broken.gds");
    CHECK_FALSE(parsed.value.cells.empty());
    REQUIRE_FALSE(parsed.value.diagnostics.empty());
    CHECK(parsed.value.diagnostics.front().message.find("MISSING") != std::string::npos);

    const std::vector<std::uint8_t> truncated{0, 8, 0, 2, 0};
    auto failed = parseGds(truncated, "truncated.gds");
    REQUIRE_FALSE(failed.value.diagnostics.empty());
    CHECK(failed.value.diagnostics.front().severity == LayoutDiagnosticSeverity::Error);
}

TEST_CASE("Layout source aggregates LEF DEF data and encodes catalog geometry", "[layout][protocol]") {
    const auto lef_path = writeFixture("pristine-layout-tiny.lef", kTinyLef);
    const auto def_path = writeFixture("pristine-layout-tiny.def", kTinyDef);

    auto source = openLefDefLayoutSource({lef_path},
                                         {"file:///tiny.lef"},
                                         def_path,
                                         std::string("file:///tiny.def"),
                                         "tiny");
    const auto& data = source->dataSet();
    CHECK(data.title == "tiny");
    CHECK(data.units_per_micron == 2000);
    CHECK(data.layers.size() == 1);
    CHECK(data.macros.size() == 1);
    CHECK(data.macros[0].pins.size() == 3);
    CHECK(data.components.size() == 1);
    CHECK(data.nets.size() == 1);
    CHECK(data.bounds.has_value());
    CHECK(data.shapes.size() >= 6);

    const auto frame = encodeFrame(LayoutFrame{.message_type = LayoutMessageType::CatalogRequest,
                                               .request_id = 42,
                                               .flags = 0,
                                               .payload = {1, 2, 3}});
    const auto decoded = decodeFrame(frame);
    CHECK(decoded.message_type == LayoutMessageType::CatalogRequest);
    CHECK(decoded.request_id == 42);
    CHECK(decoded.payload.size() == 3);

    const auto hello = source->encodeHelloResponse();
    CHECK(readU16(hello.data(), hello.size(), 0) == kLayoutProtocolVersion);
    CHECK(readU32(hello.data(), hello.size(), 4) == 2000);

    const auto catalog = source->encodeCatalogResponse();
    REQUIRE(catalog.size() > 136);
    CHECK(std::string(reinterpret_cast<const char*>(catalog.data()), 4) == "PLCT");
    CHECK(readU16(catalog.data(), catalog.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(catalog.data(), catalog.size(), 6) == 136);
    CHECK(readU32(catalog.data(), catalog.size(), 8) == 2000);
    CHECK(readU32(catalog.data(), catalog.size(), 12) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 36) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 44) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 60) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 68) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 84) == 1);
    CHECK(readU32(catalog.data(), catalog.size(), 92) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 100) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 108) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), 116) == 0);
    const auto pin_count = readU32(catalog.data(), catalog.size(), 52);
    const auto pin_table_offset = readU32(catalog.data(), catalog.size(), 56);
    const auto def_pin_count = readU32(catalog.data(), catalog.size(), 76);
    const auto def_pin_table_offset = readU32(catalog.data(), catalog.size(), 80);
    const auto string_table_offset = readU32(catalog.data(), catalog.size(), 28);
    REQUIRE(pin_count == 3);
    REQUIRE(pin_table_offset > 0);
    REQUIRE(def_pin_count == 1);
    REQUIRE(def_pin_table_offset > 0);
    constexpr std::uint32_t kPinTableStride = 28;
    constexpr std::uint32_t kDefPinTableStride = 40;
    REQUIRE(pin_table_offset + (pin_count * kPinTableStride) <= catalog.size());
    REQUIRE(def_pin_table_offset + (def_pin_count * kDefPinTableStride) <= catalog.size());
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + 0) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + 4) == 0);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(), catalog.size(), pin_table_offset + 8)) == "A");
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(), catalog.size(), pin_table_offset + 12)) ==
          "SIGNAL");
    CHECK(readU16(catalog.data(), catalog.size(), pin_table_offset + 16) ==
          static_cast<std::uint16_t>(LayoutPinDirection::Input));
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + 20) == 0);
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + 24) == 2);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset + kPinTableStride + 8)) == "VDD");
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset + kPinTableStride + 12)) == "POWER");
    CHECK(readU16(catalog.data(), catalog.size(), pin_table_offset + kPinTableStride + 16) ==
          static_cast<std::uint16_t>(LayoutPinDirection::Inout));
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + kPinTableStride + 20) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + kPinTableStride + 24) == 1);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset + (2U * kPinTableStride) + 8)) == "VSS");
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset + (2U * kPinTableStride) + 12)) == "GROUND");
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + (2U * kPinTableStride) + 20) ==
          3);
    CHECK(readU32(catalog.data(), catalog.size(), pin_table_offset + (2U * kPinTableStride) + 24) ==
          1);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(), catalog.size(), def_pin_table_offset)) == "IN");
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(), catalog.size(), def_pin_table_offset + 4)) == "n1");
    CHECK(readU32(catalog.data(), catalog.size(), def_pin_table_offset + 32) == 6);
    CHECK(readU32(catalog.data(), catalog.size(), def_pin_table_offset + 36) == 1);

    const auto geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        geometryRequest(3)));
    REQUIRE(geometry.size() > 96);
    CHECK(std::string(reinterpret_cast<const char*>(geometry.data()), 4) == "PLGE");
    CHECK(readU16(geometry.data(), geometry.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU32(geometry.data(), geometry.size(), 12) == 3);
    CHECK(readU32(geometry.data(), geometry.size(), 20) == kLayoutFrameFlagTruncated);
    const auto shape_table_offset = readU32(geometry.data(), geometry.size(), 24);
    constexpr std::uint32_t kShapeTableStride = 28;
    REQUIRE(shape_table_offset + (3U * kShapeTableStride) <= geometry.size());
    CHECK(readU16(geometry.data(), geometry.size(), shape_table_offset + 6) ==
          static_cast<std::uint16_t>(LayoutOwnerKind::Pin));
    CHECK(readU32(geometry.data(), geometry.size(), shape_table_offset + 8) == 0);
    CHECK(readU32(geometry.data(), geometry.size(), shape_table_offset + 12) == 0);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset +
                                      (readU32(geometry.data(), geometry.size(), shape_table_offset + 8) *
                                       kPinTableStride) +
                                      8)) == "A");
    CHECK(readU16(geometry.data(), geometry.size(), shape_table_offset + kShapeTableStride + 6) ==
          static_cast<std::uint16_t>(LayoutOwnerKind::Pin));
    CHECK(readU32(geometry.data(), geometry.size(), shape_table_offset + kShapeTableStride + 12) ==
          0);
    CHECK(readU16(geometry.data(), geometry.size(), shape_table_offset + (2U * kShapeTableStride) + 6) ==
          static_cast<std::uint16_t>(LayoutOwnerKind::Pin));
    CHECK(readU32(geometry.data(),
                  geometry.size(),
                  shape_table_offset + (2U * kShapeTableStride) + 8) == 1);
    CHECK(readU32(geometry.data(),
                  geometry.size(),
                  shape_table_offset + (2U * kShapeTableStride) + 12) == 0);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset +
                                      (readU32(geometry.data(),
                                               geometry.size(),
                                               shape_table_offset + (2U * kShapeTableStride) + 8) *
                                       kPinTableStride) +
                                      8)) == "VDD");

    const auto macro_geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        ownerFilteredGeometryRequest(0, {0}, {})));
    CHECK(readU16(macro_geometry.data(), macro_geometry.size(), 4) == kLayoutProtocolVersion);
    REQUIRE(geometryShapeCount(macro_geometry) == 5);
    const auto macro_shape_offset = geometryShapeTableOffset(macro_geometry);
    for (std::uint32_t shape_index = 0; shape_index < geometryShapeCount(macro_geometry);
         ++shape_index) {
        CHECK(readU32(macro_geometry.data(),
                      macro_geometry.size(),
                      macro_shape_offset + (shape_index * kShapeTableStride) + 12) == 0);
    }
    CHECK_THROWS_WITH(source->encodeGeometryResponse(decodeGeometryRequestPayload(
                          ownerFilteredGeometryRequest(0, {99}, {}))),
                      Catch::Matchers::ContainsSubstring("macro index is out of range"));
    CHECK_THROWS_WITH(source->encodeGeometryResponse(decodeGeometryRequestPayload(
                          ownerFilteredGeometryRequest(0, {}, {0}))),
                      Catch::Matchers::ContainsSubstring("requires a GDS layout source"));

    std::error_code error;
    std::filesystem::remove(lef_path, error);
    std::filesystem::remove(def_path, error);
}

TEST_CASE("Layout binary protocol rejects non-v3 frames", "[layout][protocol]") {
    auto legacyFrame = [](std::uint16_t version) {
        std::vector<std::uint8_t> frame;
        frame.insert(frame.end(), {'P', 'L', 'D', '1'});
        appendU16(frame, version);
        appendU16(frame, static_cast<std::uint16_t>(LayoutMessageType::Hello));
        appendU32(frame, 1);
        appendU32(frame, 0);
        appendU32(frame, 0);
        appendU32(frame, 0);
        return frame;
    };

    CHECK_THROWS_WITH(decodeFrame(legacyFrame(1)),
                      Catch::Matchers::ContainsSubstring("Unsupported layout frame version"));
    CHECK_THROWS_WITH(decodeFrame(legacyFrame(2)),
                      Catch::Matchers::ContainsSubstring("Unsupported layout frame version"));
}

TEST_CASE("Layout geometry request rejects trailing bytes", "[layout][protocol]") {
    auto payload = geometryRequest(0);
    payload.push_back(0);
    CHECK_THROWS_WITH(decodeGeometryRequestPayload(payload),
                      Catch::Matchers::ContainsSubstring("trailing bytes"));
}

TEST_CASE("Layout geometry request decodes owner filter extension", "[layout][protocol]") {
    const auto request = decodeGeometryRequestPayload(ownerFilteredGeometryRequest(17, {3, 5}, {7}));
    CHECK_FALSE(request.has_bbox);
    CHECK(request.max_shapes == 17);
    REQUIRE(request.macro_indices.size() == 2);
    CHECK(request.macro_indices[0] == 3);
    CHECK(request.macro_indices[1] == 5);
    REQUIRE(request.gds_root_cell_indices.size() == 1);
    CHECK(request.gds_root_cell_indices[0] == 7);

    auto unsupported = geometryRequest(0);
    unsupported[0] = 4;
    CHECK_THROWS_WITH(decodeGeometryRequestPayload(unsupported),
                      Catch::Matchers::ContainsSubstring("unsupported flags"));

    auto truncated = ownerFilteredGeometryRequest(0, {0}, {1});
    truncated.pop_back();
    CHECK_THROWS_WITH(decodeGeometryRequestPayload(truncated),
                      Catch::Matchers::ContainsSubstring("truncated"));
}

TEST_CASE("Layout search request decodes strict v3 payload", "[layout][protocol]") {
    const auto request = decodeSearchRequestPayload(searchRequest("leaf",
                                                                  7,
                                                                  3,
                                                                  5,
                                                                  LayoutRect{.x0 = 1,
                                                                             .y0 = 2,
                                                                             .x1 = 3,
                                                                             .y1 = 4}));
    CHECK(request.has_bbox);
    CHECK(request.max_results == 7);
    CHECK(request.kind_mask == 3);
    CHECK(request.root_cell_index == 5);
    CHECK(request.bbox.x0 == 1);
    CHECK(request.bbox.y1 == 4);
    CHECK(request.query == "leaf");

    auto unsupported = searchRequest("leaf");
    unsupported[0] = 2;
    CHECK_THROWS_WITH(decodeSearchRequestPayload(unsupported),
                      Catch::Matchers::ContainsSubstring("unsupported flags"));

    auto trailing = searchRequest("leaf");
    trailing.push_back(0);
    CHECK_THROWS_WITH(decodeSearchRequestPayload(trailing),
                      Catch::Matchers::ContainsSubstring("trailing bytes"));

    auto oversized = searchRequest("leaf");
    const auto query_size_offset = oversized.size() - 4U - std::string_view("leaf").size();
    oversized[query_size_offset] = 0x01;
    oversized[query_size_offset + 1U] = 0x00;
    oversized[query_size_offset + 2U] = 0x01;
    oversized[query_size_offset + 3U] = 0x00;
    CHECK_THROWS_WITH(decodeSearchRequestPayload(oversized),
                      Catch::Matchers::ContainsSubstring("query is too large"));
}

TEST_CASE("Layout catalog page request decodes strict v3 payload", "[layout][protocol]") {
    const auto request = decodeCatalogPageRequestPayload(catalogPageRequest(2, 3, 5, 4096));
    CHECK(request.table_kind == LayoutCatalogPageTableKind::Cells);
    CHECK(request.offset == 3);
    CHECK(request.limit == 5);
    CHECK(request.max_bytes == 4096);

    CHECK_THROWS_WITH(decodeCatalogPageRequestPayload(catalogPageRequest(99)),
                      Catch::Matchers::ContainsSubstring("table kind is unsupported"));
    CHECK_THROWS_WITH(decodeCatalogPageRequestPayload(catalogPageRequest(1, 0, 0, 0, 1)),
                      Catch::Matchers::ContainsSubstring("unsupported flags"));

    auto trailing = catalogPageRequest(1);
    trailing.push_back(0);
    CHECK_THROWS_WITH(decodeCatalogPageRequestPayload(trailing),
                      Catch::Matchers::ContainsSubstring("trailing bytes"));

    auto truncated = catalogPageRequest(1);
    truncated.pop_back();
    CHECK_THROWS_WITH(decodeCatalogPageRequestPayload(truncated),
                      Catch::Matchers::ContainsSubstring("truncated"));
}

} // namespace pristine::layout
