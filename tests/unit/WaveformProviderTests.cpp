#include "pristine/waveform/WaveformBinaryProtocol.h"
#include "pristine/waveform/WaveformMockGenerator.h"
#include "pristine/waveform/WaveformViewportEncoder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pristine::waveform {
namespace {

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

TEST_CASE("Waveform viewport frame reports max segment truncation", "[waveform][viewport]") {
    const auto data = makeMockWaveformDataSet();
    const auto payload = encodeViewportFramePayload(data, smallViewport(2));

    CHECK(readU32(payload.data(), payload.size(), 48) == kWaveformFrameFlagTruncated);
    CHECK(readU32(payload.data(), payload.size(), 52) == 2);
    CHECK(readU32(payload.data(), payload.size(), 12) == 2);
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

} // namespace pristine::waveform
