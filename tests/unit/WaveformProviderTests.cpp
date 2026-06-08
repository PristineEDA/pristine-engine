#include "pristine/waveform/WaveformBinaryProtocol.h"
#include "pristine/waveform/FstWaveformSource.h"
#include "pristine/waveform/WaveformMockGenerator.h"
#include "pristine/waveform/WaveformViewportEncoder.h"
#include "pristine/waveform/fst/FstReader.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fastlz.h>
#include <lz4.h>
#include <zlib.h>

namespace pristine::waveform {
namespace {

namespace fs = std::filesystem;

void appendU64Be(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendFstVarint(std::vector<std::uint8_t>& output, std::uint64_t value) {
    while (value >> 7U) {
        output.push_back(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }
    output.push_back(static_cast<std::uint8_t>(value & 0x7fU));
}

void appendFstSignedVarint(std::vector<std::uint8_t>& output, std::int64_t value) {
    bool more = true;
    while (more) {
        auto byte = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        const auto sign_bit = (byte & 0x40U) != 0U;
        if ((value == 0 && !sign_bit) || (value == -1 && sign_bit)) {
            more = false;
        }
        else {
            byte |= 0x80U;
        }
        output.push_back(byte);
    }
}

void appendBytes(std::vector<std::uint8_t>& output, std::string_view value) {
    output.insert(output.end(), value.begin(), value.end());
}

void appendNullString(std::vector<std::uint8_t>& output, std::string_view value) {
    appendBytes(output, value);
    output.push_back(0);
}

void appendFstBlock(std::vector<std::uint8_t>& output,
                    std::uint8_t type,
                    const std::vector<std::uint8_t>& payload) {
    output.push_back(type);
    appendU64Be(output, payload.size() + 8U);
    output.insert(output.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> compressZlib(const std::vector<std::uint8_t>& input) {
    auto bound = compressBound(static_cast<uLong>(input.size()));
    std::vector<std::uint8_t> output(static_cast<std::size_t>(bound));
    auto output_size = bound;
    REQUIRE(compress2(output.data(),
                      &output_size,
                      input.data(),
                      static_cast<uLong>(input.size()),
                      4) == Z_OK);
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

std::vector<std::uint8_t> compressGzip(const std::vector<std::uint8_t>& input) {
    z_stream stream{};
    REQUIRE(deflateInit2(&stream,
                         4,
                         Z_DEFLATED,
                         16 + MAX_WBITS,
                         8,
                         Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<std::uint8_t> output(compressBound(static_cast<uLong>(input.size())) + 32U);
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const auto result = deflate(&stream, Z_FINISH);
    REQUIRE(deflateEnd(&stream) == Z_OK);
    REQUIRE(result == Z_STREAM_END);
    output.resize(static_cast<std::size_t>(stream.total_out));
    return output;
}

std::vector<std::uint8_t> compressLz4(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> output(static_cast<std::size_t>(LZ4_compressBound(
        static_cast<int>(input.size()))));
    const auto output_size = LZ4_compress_default(
        reinterpret_cast<const char*>(input.data()),
        reinterpret_cast<char*>(output.data()),
        static_cast<int>(input.size()),
        static_cast<int>(output.size()));
    REQUIRE(output_size > 0);
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

std::vector<std::uint8_t> compressFastLz(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> output((input.size() * 2U) + 66U);
    const auto output_size = fastlz_compress(input.data(),
                                             static_cast<int>(input.size()),
                                             output.data());
    REQUIRE(output_size > 0);
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

fs::path uniqueTempDir(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() /
                ("pristine-engine-" + std::string(name) + "-" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

enum class TinyFstCompression {
    None,
    Zlib,
    Lz4,
    FastLz,
    DynamicAlias2,
};

enum class TinyFstHierarchyCompression {
    None,
    Gzip,
    Lz4,
    Lz4Duo,
};

fs::path writeTinyFstFixture(const fs::path& directory,
                             TinyFstCompression compression = TinyFstCompression::None,
                             TinyFstHierarchyCompression hierarchy_compression =
                                 TinyFstHierarchyCompression::None) {
    fs::create_directories(directory);
    std::vector<std::uint8_t> bytes;

    bytes.push_back(0);
    appendU64Be(bytes, 329);
    appendU64Be(bytes, 0);
    appendU64Be(bytes, 40);
    const double endian_test = 2.7182818284590452354;
    const auto old_size = bytes.size();
    bytes.resize(old_size + sizeof(double));
    std::memcpy(bytes.data() + old_size, &endian_test, sizeof(double));
    appendU64Be(bytes, 0);
    appendU64Be(bytes, 1);
    appendU64Be(bytes, 2);
    appendU64Be(bytes, 2);
    appendU64Be(bytes, 1);
    bytes.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(-9)));
    bytes.resize(bytes.size() + 128, 0);
    bytes.resize(bytes.size() + 119, 0);
    bytes.push_back(0);
    appendU64Be(bytes, 0);
    REQUIRE(bytes.size() == 330);

    std::vector<std::uint8_t> hierarchy;
    hierarchy.push_back(254);
    hierarchy.push_back(0);
    appendNullString(hierarchy, "tb");
    appendNullString(hierarchy, "");
    hierarchy.push_back(16);
    hierarchy.push_back(0);
    appendNullString(hierarchy, "clk");
    appendFstVarint(hierarchy, 1);
    appendFstVarint(hierarchy, 0);
    hierarchy.push_back(16);
    hierarchy.push_back(0);
    appendNullString(hierarchy, "data");
    appendFstVarint(hierarchy, 4);
    appendFstVarint(hierarchy, 0);
    hierarchy.push_back(255);
    if (hierarchy_compression == TinyFstHierarchyCompression::None) {
        appendFstBlock(bytes, 4, hierarchy);
    }
    else if (hierarchy_compression == TinyFstHierarchyCompression::Gzip) {
        std::vector<std::uint8_t> hierarchy_block;
        appendU64Be(hierarchy_block, hierarchy.size());
        auto encoded_hierarchy = compressGzip(hierarchy);
        hierarchy_block.insert(hierarchy_block.end(),
                               encoded_hierarchy.begin(),
                               encoded_hierarchy.end());
        appendFstBlock(bytes, 4, hierarchy_block);
    }
    else {
        auto encoded_hierarchy = compressLz4(hierarchy);
        auto block_type = std::uint8_t{6};
        if (hierarchy_compression == TinyFstHierarchyCompression::Lz4Duo) {
            std::vector<std::uint8_t> duo_payload;
            appendFstVarint(duo_payload, encoded_hierarchy.size());
            auto second_pass = compressLz4(encoded_hierarchy);
            duo_payload.insert(duo_payload.end(), second_pass.begin(), second_pass.end());
            encoded_hierarchy = std::move(duo_payload);
            block_type = 7;
        }
        std::vector<std::uint8_t> hierarchy_block;
        appendU64Be(hierarchy_block, hierarchy.size());
        hierarchy_block.insert(hierarchy_block.end(),
                               encoded_hierarchy.begin(),
                               encoded_hierarchy.end());
        appendFstBlock(bytes, block_type, hierarchy_block);
    }

    std::vector<std::uint8_t> geometry_data;
    appendFstVarint(geometry_data, 1);
    appendFstVarint(geometry_data, 4);
    auto geometry_payload = geometry_data;
    if (compression == TinyFstCompression::Zlib) {
        geometry_payload = compressZlib(geometry_data);
    }
    std::vector<std::uint8_t> geometry;
    appendU64Be(geometry, geometry_data.size());
    appendU64Be(geometry, 2);
    geometry.insert(geometry.end(), geometry_payload.begin(), geometry_payload.end());
    appendFstBlock(bytes, 3, geometry);

    std::vector<std::uint8_t> value_payload;
    appendU64Be(value_payload, 0);
    appendU64Be(value_payload, 40);
    appendU64Be(value_payload, 0);

    std::vector<std::uint8_t> initial_frame;
    appendBytes(initial_frame, "0");
    appendBytes(initial_frame, "xxxx");
    auto encoded_initial_frame = initial_frame;
    if (compression == TinyFstCompression::Zlib) {
        encoded_initial_frame = compressZlib(initial_frame);
    }
    appendFstVarint(value_payload, initial_frame.size());
    appendFstVarint(value_payload, encoded_initial_frame.size());
    appendFstVarint(value_payload, 2);
    value_payload.insert(value_payload.end(),
                         encoded_initial_frame.begin(),
                         encoded_initial_frame.end());

    appendFstVarint(value_payload, 2);
    const auto pack_type = compression == TinyFstCompression::Lz4 ? '4'
                           : compression == TinyFstCompression::FastLz ? 'F'
                                                                       : 'Z';
    value_payload.push_back(static_cast<std::uint8_t>(pack_type));

    auto encode_chain = [compression](std::vector<std::uint8_t> raw) {
        std::vector<std::uint8_t> chain;
        if (compression == TinyFstCompression::None) {
            appendFstVarint(chain, 0);
            chain.insert(chain.end(), raw.begin(), raw.end());
            return chain;
        }
        appendFstVarint(chain, raw.size());
        if (compression == TinyFstCompression::Zlib ||
            compression == TinyFstCompression::DynamicAlias2) {
            raw = compressZlib(raw);
        }
        else if (compression == TinyFstCompression::Lz4) {
            raw = compressLz4(raw);
        }
        else {
            raw = compressFastLz(raw);
        }
        chain.insert(chain.end(), raw.begin(), raw.end());
        return chain;
    };

    std::vector<std::uint8_t> handle1_raw_chain;
    appendFstVarint(handle1_raw_chain, 6);
    appendFstVarint(handle1_raw_chain, 8);

    std::vector<std::uint8_t> handle2_raw_chain;
    appendFstVarint(handle2_raw_chain, 4);
    handle2_raw_chain.push_back(0x30);
    appendFstVarint(handle2_raw_chain, 5);
    appendBytes(handle2_raw_chain, "zzzz");

    auto handle1_chain = encode_chain(std::move(handle1_raw_chain));
    auto handle2_chain = encode_chain(std::move(handle2_raw_chain));

    value_payload.insert(value_payload.end(), handle1_chain.begin(), handle1_chain.end());
    value_payload.insert(value_payload.end(), handle2_chain.begin(), handle2_chain.end());

    std::vector<std::uint8_t> chain_index;
    const auto handle1_offset = std::uint64_t{1};
    const auto handle2_offset = handle1_offset + handle1_chain.size();
    if (compression == TinyFstCompression::DynamicAlias2) {
        appendFstSignedVarint(chain_index, static_cast<std::int64_t>((handle1_offset << 1U) | 1U));
        appendFstSignedVarint(
            chain_index,
            static_cast<std::int64_t>(((handle2_offset - handle1_offset) << 1U) | 1U));
    }
    else {
        appendFstVarint(chain_index, (handle1_offset << 1U) | 1U);
        appendFstVarint(chain_index, ((handle2_offset - handle1_offset) << 1U) | 1U);
    }
    value_payload.insert(value_payload.end(), chain_index.begin(), chain_index.end());
    appendU64Be(value_payload, chain_index.size());

    std::vector<std::uint8_t> time_table;
    appendFstVarint(time_table, 0);
    appendFstVarint(time_table, 10);
    appendFstVarint(time_table, 5);
    appendFstVarint(time_table, 5);
    appendFstVarint(time_table, 10);
    auto encoded_time_table = time_table;
    if (compression == TinyFstCompression::Zlib) {
        encoded_time_table = compressZlib(time_table);
    }
    value_payload.insert(value_payload.end(),
                         encoded_time_table.begin(),
                         encoded_time_table.end());
    appendU64Be(value_payload, time_table.size());
    appendU64Be(value_payload, encoded_time_table.size());
    appendU64Be(value_payload, 5);
    appendFstBlock(bytes, compression == TinyFstCompression::DynamicAlias2 ? 8 : 1, value_payload);

    const auto path = directory / "tiny.fst";
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
    return path;
}

void checkTinyFstTransitions(const fst::FstData& data) {
    REQUIRE(data.transitions.size() == 6);
    CHECK(data.transitions.at(0).handle == 1);
    CHECK(data.transitions.at(0).value == "0");
    CHECK(data.transitions.at(1).handle == 2);
    CHECK(data.transitions.at(1).value == "xxxx");
    CHECK(data.transitions.at(2).handle == 1);
    CHECK(data.transitions.at(2).time == 10);
    CHECK(data.transitions.at(2).value == "1");
    CHECK(data.transitions.at(3).handle == 2);
    CHECK(data.transitions.at(3).time == 15);
    CHECK(data.transitions.at(3).value == "0011");
    CHECK(data.transitions.at(5).value == "zzzz");
}

std::string readTableString(const std::vector<std::uint8_t>& table, std::uint32_t offset) {
    const auto length = readU32(table.data(), table.size(), offset);
    const auto begin = table.begin() + static_cast<std::ptrdiff_t>(offset + 4U);
    return std::string(begin, begin + static_cast<std::ptrdiff_t>(length));
}

WaveformViewportRequest smallViewport(std::uint32_t max_segments = 0) {
    return WaveformViewportRequest{.start_time = 0.0,
                                   .end_time = 100.0,
                                   .viewport_pixel_width = 500.0F,
                                   .lane_height = 24.0F,
                                   .header_height = 22.0F,
                                   .max_segments = max_segments,
                                   .signal_ids = {"tb_top_module1-clk", "u_top_module1-counting"}};
}

WaveformViewportRequestV2 preparedViewport(std::uint32_t max_segments = 0) {
    return WaveformViewportRequestV2{.prepared_start_time = 0.0,
                                     .prepared_end_time = 200.0,
                                     .viewport_start_time = 40.0,
                                     .viewport_end_time = 100.0,
                                     .viewport_pixel_width = 500.0F,
                                     .lane_height = 24.0F,
                                     .header_height = 22.0F,
                                     .max_segments = max_segments,
                                     .signal_ids = {"tb_top_module1-clk",
                                                    "u_top_module1-counting"}};
}

std::vector<std::uint8_t> encodeViewportFrameRequestPayloadV2ForTest(
    const WaveformViewportRequestV2& request) {
    std::vector<std::uint8_t> payload;
    appendF64(payload, request.prepared_start_time);
    appendF64(payload, request.prepared_end_time);
    appendF64(payload, request.viewport_start_time);
    appendF64(payload, request.viewport_end_time);
    appendF32(payload, request.viewport_pixel_width);
    appendF32(payload, request.lane_height);
    appendF32(payload, request.header_height);
    appendU32(payload, request.max_segments);
    appendU32(payload, static_cast<std::uint32_t>(request.signal_ids.size()));
    for (const auto& signal_id : request.signal_ids) {
        appendString(payload, signal_id);
    }
    return payload;
}

std::uint32_t readSignalSegmentCount(const std::vector<std::uint8_t>& payload,
                                     std::uint32_t signal_table_offset,
                                     std::size_t table_index) {
    const auto entry_offset = signal_table_offset + static_cast<std::uint32_t>(table_index) * 16U + 8U;
    return readU32(payload.data(), payload.size(), entry_offset);
}

} // namespace

TEST_CASE("Waveform binary frame roundtrips and rejects invalid magic", "[waveform][protocol]") {
    WaveformFrame frame{.message_type = WaveformMessageType::CatalogRequest,
                        .request_id = 42,
                        .flags = 7,
                        .payload = {1, 2, 3, 4}};

    const auto encoded = encodeFrame(frame);
    const auto decoded = decodeFrame(encoded);

    CHECK(decoded.message_type == WaveformMessageType::CatalogRequest);
    CHECK(decoded.request_id == 42);
    CHECK(decoded.flags == 7);
    CHECK(decoded.payload == std::vector<std::uint8_t>{1, 2, 3, 4});

    auto invalid = encoded;
    invalid[0] = 0;
    CHECK_THROWS_AS(decodeFrame(invalid), std::runtime_error);
}

TEST_CASE("Waveform mock generator is deterministic", "[waveform][mock]") {
    const auto data = makeMockWaveformDataSet();

    CHECK(data.id == "counter-waveform-mock");
    CHECK(data.title == "counter_tb");
    CHECK(data.timescale_unit == "ns");
    CHECK(data.duration == 200.0);
    CHECK(data.groups.size() == 3);
    CHECK(data.signals.size() == 168);
    CHECK(data.signals.front().id == "tb_top_module1-clk");
    CHECK(data.signals.back().id == "dense-signal-160");
}

TEST_CASE("Waveform catalog encodes string and signal tables without transitions",
          "[waveform][catalog]") {
    const auto data = makeMockWaveformDataSet();
    const auto payload = encodeCatalogResponsePayload(data);

    const auto group_count = readU32(payload.data(), payload.size(), 0);
    const auto signal_count = readU32(payload.data(), payload.size(), 4);
    const auto group_table_size = readU32(payload.data(), payload.size(), 8);
    const auto signal_table_size = readU32(payload.data(), payload.size(), 12);
    const auto string_table_size = readU32(payload.data(), payload.size(), 16);
    const auto group_table_offset = 20U;
    const auto signal_table_offset = group_table_offset + group_table_size;
    const auto string_table_offset = signal_table_offset + signal_table_size;

    REQUIRE(group_count == 3);
    REQUIRE(signal_count == 168);
    CHECK(group_table_size == group_count * 8U);
    CHECK(signal_table_size == signal_count * 28U);
    CHECK(string_table_offset + string_table_size == payload.size());

    const auto first_group_id_offset = readU32(payload.data(), payload.size(), group_table_offset);
    const std::vector<std::uint8_t> strings(payload.begin() +
                                               static_cast<std::ptrdiff_t>(string_table_offset),
                                           payload.end());
    CHECK(readTableString(strings, first_group_id_offset) == "tb_top_module1");

    const auto first_signal_id_offset = readU32(payload.data(), payload.size(), signal_table_offset);
    const auto first_signal_kind = payload.at(signal_table_offset + 16U);
    CHECK(readTableString(strings, first_signal_id_offset) == "tb_top_module1-clk");
    CHECK(first_signal_kind == static_cast<std::uint8_t>(WaveformSignalKind::Clock));
}

TEST_CASE("Waveform viewport frame uses typed-array compatible column offsets",
          "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    const auto payload = encodeViewportFramePayload(data, smallViewport());

    REQUIRE(payload.size() > 56);
    CHECK(payload[0] == 'P');
    CHECK(payload[1] == 'W');
    CHECK(payload[2] == 'V');
    CHECK(payload[3] == 'F');
    CHECK(readU16(payload.data(), payload.size(), 4) == 1);
    CHECK(readU32(payload.data(), payload.size(), 8) == 2);
    const auto segment_count = readU32(payload.data(), payload.size(), 12);
    REQUIRE(segment_count > 0);

    const auto signal_table_offset = readU32(payload.data(), payload.size(), 16);
    const auto x0_offset = readU32(payload.data(), payload.size(), 20);
    const auto x1_offset = readU32(payload.data(), payload.size(), 24);
    const auto lane_y_offset = readU32(payload.data(), payload.size(), 28);
    const auto value_kind_offset = readU32(payload.data(), payload.size(), 32);
    const auto label_index_offset = readU32(payload.data(), payload.size(), 36);
    const auto label_bytes_offset = readU32(payload.data(), payload.size(), 40);
    const auto label_bytes_length = readU32(payload.data(), payload.size(), 44);

    CHECK(signal_table_offset % 4U == 0);
    CHECK(x0_offset % 4U == 0);
    CHECK(x1_offset % 4U == 0);
    CHECK(lane_y_offset % 4U == 0);
    CHECK(label_index_offset % 4U == 0);
    CHECK(value_kind_offset < label_index_offset);
    CHECK(label_bytes_offset + label_bytes_length == payload.size());

    CHECK(readU32(payload.data(), payload.size(), signal_table_offset) == 0);
    CHECK(readU32(payload.data(), payload.size(), signal_table_offset + 16U) == 7);
    CHECK(readF32(payload.data(), payload.size(), x0_offset) == 0.0F);
    CHECK(readF32(payload.data(), payload.size(), lane_y_offset) == 34.0F);
    CHECK(payload.at(value_kind_offset) == static_cast<std::uint8_t>(WaveformValueKind::Low));
}

TEST_CASE("Waveform viewport v2 request decodes prepared and display ranges",
          "[waveform][viewport]") {
    const auto request = preparedViewport();
    const auto decoded = decodeViewportFrameRequestPayloadV2(
        encodeViewportFrameRequestPayloadV2ForTest(request));

    CHECK(decoded.prepared_start_time == 0.0);
    CHECK(decoded.prepared_end_time == 200.0);
    CHECK(decoded.viewport_start_time == 40.0);
    CHECK(decoded.viewport_end_time == 100.0);
    CHECK(decoded.viewport_pixel_width == 500.0F);
    CHECK(decoded.lane_height == 24.0F);
    CHECK(decoded.header_height == 22.0F);
    CHECK(decoded.max_segments == 0);
    CHECK(decoded.signal_ids == request.signal_ids);
}

TEST_CASE("Waveform viewport v2 frame includes prepared range and time columns",
          "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    const auto request = preparedViewport();
    const auto payload = encodeViewportFramePayloadV2(data, request);

    REQUIRE(payload.size() > 96);
    CHECK(payload[0] == 'P');
    CHECK(payload[1] == 'W');
    CHECK(payload[2] == 'V');
    CHECK(payload[3] == 'F');
    CHECK(readU16(payload.data(), payload.size(), 4) == 2);
    CHECK(readU16(payload.data(), payload.size(), 6) == 96);
    CHECK(readU32(payload.data(), payload.size(), 8) == 2);
    const auto segment_count = readU32(payload.data(), payload.size(), 12);
    REQUIRE(segment_count > 0);

    const auto signal_table_offset = readU32(payload.data(), payload.size(), 16);
    const auto time0_offset = readU32(payload.data(), payload.size(), 56);
    const auto time1_offset = readU32(payload.data(), payload.size(), 60);

    CHECK(time0_offset % 8U == 0);
    CHECK(time1_offset % 8U == 0);
    CHECK(readF64(payload.data(), payload.size(), 64) == request.prepared_start_time);
    CHECK(readF64(payload.data(), payload.size(), 72) == request.prepared_end_time);
    CHECK(readF64(payload.data(), payload.size(), 80) == request.viewport_start_time);
    CHECK(readF64(payload.data(), payload.size(), 88) == request.viewport_end_time);
    CHECK(readU32(payload.data(), payload.size(), 48) == 0);
    CHECK(readSignalSegmentCount(payload, signal_table_offset, 0) > 0);
    CHECK(readF64(payload.data(), payload.size(), time0_offset) == request.prepared_start_time);
    CHECK(readF64(payload.data(), payload.size(), time1_offset) > request.prepared_start_time);
}

TEST_CASE("Waveform viewport frame reports max segment truncation", "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    const auto payload = encodeViewportFramePayload(data, smallViewport(2));

    CHECK(readU32(payload.data(), payload.size(), 48) == kWaveformFrameFlagTruncated);
    CHECK(readU32(payload.data(), payload.size(), 52) == 2);
    CHECK(readU32(payload.data(), payload.size(), 12) == 2);
}

TEST_CASE("Waveform dense viewport v2 returns every requested signal without truncation",
          "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    auto request = preparedViewport();
    request.prepared_start_time = 0.0;
    request.prepared_end_time = 200.0;
    request.viewport_start_time = 0.0;
    request.viewport_end_time = 200.0;
    request.viewport_pixel_width = 1800.0F;
    request.max_segments = 0;
    request.signal_ids = {"dense-signal-107",
                          "dense-signal-108",
                          "dense-signal-109",
                          "dense-signal-110",
                          "dense-signal-111",
                          "dense-signal-112",
                          "dense-signal-113",
                          "dense-signal-114",
                          "dense-signal-115",
                          "dense-signal-116",
                          "dense-signal-117",
                          "dense-signal-118",
                          "dense-signal-119"};

    const auto payload = encodeViewportFramePayloadV2(data, request);
    const auto signal_count = readU32(payload.data(), payload.size(), 8);
    const auto segment_count = readU32(payload.data(), payload.size(), 12);
    const auto signal_table_offset = readU32(payload.data(), payload.size(), 16);
    const auto requested_signal_count = static_cast<std::uint32_t>(request.signal_ids.size());

    REQUIRE(signal_count == requested_signal_count);
    CHECK(readU32(payload.data(), payload.size(), 48) == 0);
    CHECK(segment_count > requested_signal_count);

    for (std::size_t index = 0; index < request.signal_ids.size(); ++index) {
        CHECK(readSignalSegmentCount(payload, signal_table_offset, index) > 0);
    }
}

TEST_CASE("Waveform dense viewport returns every requested signal without truncation",
          "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    auto request = smallViewport();
    request.start_time = 0.0;
    request.end_time = 200.0;
    request.viewport_pixel_width = 1800.0F;
    request.max_segments = 0;
    request.signal_ids = {"dense-signal-107",
                          "dense-signal-108",
                          "dense-signal-109",
                          "dense-signal-110",
                          "dense-signal-111",
                          "dense-signal-112",
                          "dense-signal-113",
                          "dense-signal-114",
                          "dense-signal-115",
                          "dense-signal-116",
                          "dense-signal-117",
                          "dense-signal-118",
                          "dense-signal-119"};

    const auto payload = encodeViewportFramePayload(data, request);
    const auto signal_count = readU32(payload.data(), payload.size(), 8);
    const auto segment_count = readU32(payload.data(), payload.size(), 12);
    const auto signal_table_offset = readU32(payload.data(), payload.size(), 16);
    const auto requested_signal_count = static_cast<std::uint32_t>(request.signal_ids.size());

    REQUIRE(signal_count == requested_signal_count);
    CHECK(readU32(payload.data(), payload.size(), 48) == 0);
    CHECK(segment_count > requested_signal_count);

    for (std::size_t index = 0; index < request.signal_ids.size(); ++index) {
        CHECK(readSignalSegmentCount(payload, signal_table_offset, index) > 0);
    }
}

TEST_CASE("FST reader restores header hierarchy and value-block index", "[waveform][fst]") {
    const auto workspace = uniqueTempDir("fst-reader");
    const auto fst_path = writeTinyFstFixture(workspace);

    const auto data = fst::readFstFile(fst_path, fst::FstReadOptions{.workspace_root = workspace});

    CHECK(data.header.start_time == 0);
    CHECK(data.header.end_time == 40);
    CHECK(data.header.timescale == -9);
    REQUIRE(data.scopes.size() == 1);
    CHECK(data.scopes.front().path == "tb");
    REQUIRE(data.signals.size() == 2);
    CHECK(data.signals.at(0).handle == 1);
    CHECK(data.signals.at(0).path == "tb.clk");
    CHECK(data.signals.at(1).width == 4);
    REQUIRE(data.value_blocks.size() == 1);
    CHECK(data.value_blocks.front().begin_time == 0);
    CHECK(data.value_blocks.front().end_time == 40);
    checkTinyFstTransitions(data);
}

TEST_CASE("FST reader inflates zlib LZ4 and FastLZ value blocks", "[waveform][fst]") {
    const auto workspace = uniqueTempDir("fst-compressed-reader");

    for (const auto compression : {TinyFstCompression::Zlib,
                                   TinyFstCompression::Lz4,
                                   TinyFstCompression::FastLz,
                                   TinyFstCompression::DynamicAlias2}) {
        const auto fst_path = writeTinyFstFixture(workspace, compression);
        const auto data = fst::readFstFile(
            fst_path,
            fst::FstReadOptions{.workspace_root = workspace});

        REQUIRE(data.signals.size() == 2);
        CHECK(data.signals.at(1).width == 4);
        checkTinyFstTransitions(data);
    }

    for (const auto hierarchy_compression : {TinyFstHierarchyCompression::Gzip,
                                             TinyFstHierarchyCompression::Lz4,
                                             TinyFstHierarchyCompression::Lz4Duo}) {
        const auto fst_path = writeTinyFstFixture(workspace,
                                                  TinyFstCompression::None,
                                                  hierarchy_compression);
        const auto data = fst::readFstFile(
            fst_path,
            fst::FstReadOptions{.workspace_root = workspace});

        REQUIRE(data.scopes.size() == 1);
        CHECK(data.scopes.front().path == "tb");
        REQUIRE(data.signals.size() == 2);
        CHECK(data.signals.at(1).path == "tb.data");
        checkTinyFstTransitions(data);
    }
}

TEST_CASE("FST waveform source returns columnar viewport frames", "[waveform][fst]") {
    const auto workspace = uniqueTempDir("fst-source");
    const auto fst_path = writeTinyFstFixture(workspace);
    const auto source = openFstWaveformSource(fst_path, "file:///tiny.fst", workspace);
    const auto& data = source->dataSet();

    REQUIRE(source->sourceKind() == std::string_view("fst"));
    CHECK(data.title == "tiny");
    CHECK(data.timescale_unit == "ns");
    CHECK(data.duration == 40.0);
    REQUIRE(data.groups.size() == 1);
    REQUIRE(data.signals.size() == 2);
    CHECK(data.signals.at(0).id == "fst:1");
    CHECK(data.signals.at(1).kind == WaveformSignalKind::Bus);

    const WaveformViewportRequestV2 request{.prepared_start_time = 0.0,
                                            .prepared_end_time = 40.0,
                                            .viewport_start_time = 0.0,
                                            .viewport_end_time = 40.0,
                                            .viewport_pixel_width = 400.0F,
                                            .lane_height = 20.0F,
                                            .header_height = 10.0F,
                                            .max_segments = 0,
                                            .signal_ids = {"fst:1", "fst:2"}};
    const auto payload = source->encodeViewportFrameV2(request);

    CHECK(payload[0] == 'P');
    CHECK(payload[1] == 'W');
    CHECK(payload[2] == 'V');
    CHECK(payload[3] == 'F');
    CHECK(readU16(payload.data(), payload.size(), 4) == 2);
    CHECK(readU32(payload.data(), payload.size(), 8) == 2);
    CHECK(readU32(payload.data(), payload.size(), 12) == 6);
    const auto value_kind_offset = readU32(payload.data(), payload.size(), 32);
    const auto label_bytes_offset = readU32(payload.data(), payload.size(), 40);
    const auto label_bytes_length = readU32(payload.data(), payload.size(), 44);
    CHECK(payload.at(value_kind_offset) == static_cast<std::uint8_t>(WaveformValueKind::Low));
    CHECK(label_bytes_offset + label_bytes_length == payload.size());
    CHECK(label_bytes_length > 0);
}

TEST_CASE("FST reader rejects workspace external waveform paths", "[waveform][fst]") {
    const auto workspace = uniqueTempDir("fst-workspace");
    const auto outside = uniqueTempDir("fst-outside");
    const auto fst_path = writeTinyFstFixture(outside);

    CHECK_THROWS_WITH(fst::readFstFile(fst_path,
                                       fst::FstReadOptions{.workspace_root = workspace}),
                      Catch::Matchers::ContainsSubstring("workspace root"));
}

} // namespace pristine::waveform
