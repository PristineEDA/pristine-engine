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
    REQUIRE(parsed.value.references.size() == 2);
    CHECK(parsed.value.references[0].target_cell_index == 0);
    CHECK(parsed.value.references[1].kind == LayoutGdsElementKind::Aref);
    CHECK(parsed.value.references[1].columns == 2);

    auto source = openGdsLayoutSource(gds_path, "file:///tiny.gds", "tiny-gds");
    CHECK(source->sourceKind() == "gds");
    CHECK(source->protocolVersion() == kLayoutProtocolVersionV3);
    const auto& data = source->dataSet();
    CHECK(data.title == "tiny-gds");
    REQUIRE(data.gds.has_value());
    CHECK(data.gds->cells.size() == 2);
    CHECK(data.shapes.size() >= 6);
    REQUIRE(data.bounds.has_value());
    CHECK(data.bounds->x0 == 0);
    CHECK(data.bounds->y0 == 0);
    CHECK(data.bounds->x1 >= 260);
    CHECK(data.bounds->y1 >= 310);

    const auto frame = encodeFrame(LayoutFrame{.message_type = LayoutMessageType::Hello,
                                               .request_id = 7,
                                               .flags = 0,
                                               .version = kLayoutProtocolVersionV3});
    const auto decoded = decodeFrame(frame);
    CHECK(decoded.version == kLayoutProtocolVersionV3);
    CHECK(decoded.message_type == LayoutMessageType::Hello);

    const auto hello = source->encodeHelloResponse();
    CHECK(readU16(hello.data(), hello.size(), 0) == kLayoutProtocolVersionV3);
    CHECK(readU32(hello.data(), hello.size(), 12) == 2);

    const auto catalog = source->encodeCatalogResponse();
    CHECK(std::string(reinterpret_cast<const char*>(catalog.data()), 4) == "PLCT");
    CHECK(readU16(catalog.data(), catalog.size(), 4) == kLayoutProtocolVersionV3);
    CHECK(readU16(catalog.data(), catalog.size(), 6) == 128);
    CHECK(readU32(catalog.data(), catalog.size(), 12) == 3);
    CHECK(readU32(catalog.data(), catalog.size(), 16) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), 20) == 2);
    CHECK(readU32(catalog.data(), catalog.size(), 24) == 5);
    const auto cell_offset = readU32(catalog.data(), catalog.size(), 36);
    const auto string_offset = readU32(catalog.data(), catalog.size(), 56);
    CHECK(readTableString(catalog,
                          string_offset,
                          readU32(catalog.data(), catalog.size(), cell_offset)) == "LEAF");
    constexpr std::uint32_t kGdsCellStride = 56;
    CHECK(readTableString(catalog,
                          string_offset,
                          readU32(catalog.data(), catalog.size(), cell_offset + kGdsCellStride)) ==
          "TOP");
    CHECK(readU32(catalog.data(), catalog.size(), cell_offset + kGdsCellStride + 20) == 1);

    const auto geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        geometryRequest(2)));
    CHECK(readU16(geometry.data(), geometry.size(), 4) == kLayoutProtocolVersionV3);
    CHECK(readU32(geometry.data(), geometry.size(), 12) == 2);
    CHECK(readU32(geometry.data(), geometry.size(), 20) == kLayoutFrameFlagTruncated);

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
    REQUIRE(catalog.size() > 80);
    CHECK(std::string(reinterpret_cast<const char*>(catalog.data()), 4) == "PLCT");
    CHECK(readU16(catalog.data(), catalog.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU16(catalog.data(), catalog.size(), 6) == 80);
    CHECK(readU32(catalog.data(), catalog.size(), 8) == 2000);
    CHECK(readU32(catalog.data(), catalog.size(), 12) == 1);
    const auto pin_count = readU32(catalog.data(), catalog.size(), 72);
    const auto pin_table_offset = readU32(catalog.data(), catalog.size(), 76);
    const auto string_table_offset = readU32(catalog.data(), catalog.size(), 60);
    REQUIRE(pin_count == 3);
    REQUIRE(pin_table_offset > 0);
    constexpr std::uint32_t kPinTableStride = 28;
    REQUIRE(pin_table_offset + (pin_count * kPinTableStride) <= catalog.size());
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

    const auto geometry = source->encodeGeometryResponse(decodeGeometryRequestPayload(
        geometryRequest(3)));
    REQUIRE(geometry.size() > 96);
    CHECK(std::string(reinterpret_cast<const char*>(geometry.data()), 4) == "PLGE");
    CHECK(readU16(geometry.data(), geometry.size(), 4) == kLayoutProtocolVersion);
    CHECK(readU32(geometry.data(), geometry.size(), 12) == 3);
    CHECK(readU32(geometry.data(), geometry.size(), 20) == kLayoutFrameFlagTruncated);
    const auto shape_table_offset = readU32(geometry.data(), geometry.size(), 24);
    constexpr std::uint32_t kShapeTableStrideV2 = 28;
    REQUIRE(shape_table_offset + (3U * kShapeTableStrideV2) <= geometry.size());
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
    CHECK(readU16(geometry.data(), geometry.size(), shape_table_offset + kShapeTableStrideV2 + 6) ==
          static_cast<std::uint16_t>(LayoutOwnerKind::Pin));
    CHECK(readU32(geometry.data(), geometry.size(), shape_table_offset + kShapeTableStrideV2 + 12) ==
          0);
    CHECK(readU16(geometry.data(), geometry.size(), shape_table_offset + (2U * kShapeTableStrideV2) + 6) ==
          static_cast<std::uint16_t>(LayoutOwnerKind::Pin));
    CHECK(readU32(geometry.data(),
                  geometry.size(),
                  shape_table_offset + (2U * kShapeTableStrideV2) + 8) == 1);
    CHECK(readU32(geometry.data(),
                  geometry.size(),
                  shape_table_offset + (2U * kShapeTableStrideV2) + 12) == 0);
    CHECK(readTableString(catalog,
                          string_table_offset,
                          readU32(catalog.data(),
                                  catalog.size(),
                                  pin_table_offset +
                                      (readU32(geometry.data(),
                                               geometry.size(),
                                               shape_table_offset + (2U * kShapeTableStrideV2) + 8) *
                                       kPinTableStride) +
                                      8)) == "VDD");

    std::error_code error;
    std::filesystem::remove(lef_path, error);
    std::filesystem::remove(def_path, error);
}

TEST_CASE("Layout binary protocol rejects legacy v1 frames", "[layout][protocol]") {
    std::vector<std::uint8_t> frame;
    frame.insert(frame.end(), {'P', 'L', 'D', '1'});
    appendU16(frame, 1);
    appendU16(frame, static_cast<std::uint16_t>(LayoutMessageType::Hello));
    appendU32(frame, 1);
    appendU32(frame, 0);
    appendU32(frame, 0);
    appendU32(frame, 0);

    CHECK_THROWS_WITH(decodeFrame(frame),
                      Catch::Matchers::ContainsSubstring("Unsupported layout frame version"));
}

TEST_CASE("Layout geometry request rejects trailing bytes", "[layout][protocol]") {
    auto payload = geometryRequest(0);
    payload.push_back(0);
    CHECK_THROWS_WITH(decodeGeometryRequestPayload(payload),
                      Catch::Matchers::ContainsSubstring("trailing bytes"));
}

} // namespace pristine::layout
