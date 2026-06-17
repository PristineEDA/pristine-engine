#pragma once

#include "pristine/layout/LayoutData.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace pristine::layout {

inline constexpr std::string_view kLayoutProtocolName = "pristine-layout-columnar-v3";
inline constexpr std::uint16_t kLayoutProtocolVersion = 3;
inline constexpr std::uint32_t kLayoutFrameFlagTruncated = 1U;

enum class LayoutMessageType : std::uint16_t {
    Hello = 1,
    HelloResponse = 2,
    CatalogRequest = 3,
    CatalogResponse = 4,
    GeometryRequest = 5,
    GeometryResponse = 6,
    ErrorResponse = 7,
    Close = 8,
    TileGeometryRequest = 9,
    TileGeometryResponse = 10,
    HitTestRequest = 11,
    HitTestResponse = 12,
    InspectRequest = 13,
    InspectResponse = 14,
    SelectionGeometryRequest = 15,
    SelectionGeometryResponse = 16,
    SearchRequest = 17,
    SearchResponse = 18,
    CatalogSummaryRequest = 19,
    CatalogSummaryResponse = 20,
    CatalogPageRequest = 21,
    CatalogPageResponse = 22,
};

enum class LayoutErrorCode : std::uint32_t {
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    UnknownMessage = 3,
    InternalError = 4,
};

struct LayoutFrame {
    LayoutMessageType message_type = LayoutMessageType::Hello;
    std::uint32_t request_id = 0;
    std::uint32_t flags = 0;
    std::uint16_t version = kLayoutProtocolVersion;
    std::vector<std::uint8_t> payload{};
};

[[nodiscard]] std::vector<std::uint8_t> encodeFrame(const LayoutFrame& frame);
[[nodiscard]] LayoutFrame decodeFrame(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] LayoutFrame decodeFrame(const std::uint8_t* bytes, std::size_t size);

[[nodiscard]] std::vector<std::uint8_t> encodeHelloResponsePayload(const LayoutDataSet& data);
[[nodiscard]] std::vector<std::uint8_t> encodeCatalogResponsePayload(const LayoutDataSet& data);
[[nodiscard]] std::vector<std::uint8_t> encodeCatalogSummaryResponsePayload(const LayoutDataSet& data);
[[nodiscard]] std::vector<std::uint8_t> encodeCatalogPageResponsePayload(
    const LayoutDataSet& data,
    const LayoutCatalogPageRequest& request);
[[nodiscard]] std::vector<std::uint8_t> encodeGeometryResponsePayload(
    const LayoutDataSet& data,
    const LayoutGeometryRequest& request);
[[nodiscard]] std::vector<std::uint8_t> encodeGeometryResponsePayload(
    const LayoutDataSet& data,
    const LayoutGeometryRequest& request,
    const std::vector<LayoutShape>& shapes);
[[nodiscard]] std::vector<std::uint8_t> encodeGeometryResponsePayload(
    const LayoutDataSet& data,
    const LayoutGeometryRequest& request,
    const std::vector<LayoutShape>& shapes,
    bool truncated);
[[nodiscard]] std::vector<std::uint8_t> encodeTileGeometryResponsePayload(
    const LayoutDataSet& data,
    const LayoutTileGeometryRequest& request,
    const LayoutTileGeometryResult& result);
[[nodiscard]] std::vector<std::uint8_t> encodeHitTestResponsePayload(
    const LayoutDataSet& data,
    const LayoutHitTestResponse& response);
[[nodiscard]] std::vector<std::uint8_t> encodeInspectResponsePayload(
    const LayoutDataSet& data,
    const LayoutInspectResult& result);
[[nodiscard]] std::vector<std::uint8_t> encodeSearchResponsePayload(
    const LayoutDataSet& data,
    const LayoutSearchResponse& response);
[[nodiscard]] std::vector<std::uint8_t> encodeErrorPayload(LayoutErrorCode code,
                                                         std::string_view message);
[[nodiscard]] LayoutGeometryRequest decodeGeometryRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutTileGeometryRequest decodeTileGeometryRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutHitTestRequest decodeHitTestRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutInspectRequest decodeInspectRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutSelectionGeometryRequest decodeSelectionGeometryRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutSearchRequest decodeSearchRequestPayload(
    const std::vector<std::uint8_t>& payload);
[[nodiscard]] LayoutCatalogPageRequest decodeCatalogPageRequestPayload(
    const std::vector<std::uint8_t>& payload);

void appendString(std::vector<std::uint8_t>& output, std::string_view value);
void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value);
void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value);
void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value);
void appendF64(std::vector<std::uint8_t>& output, double value);
void alignTo(std::vector<std::uint8_t>& output, std::size_t alignment);

[[nodiscard]] std::uint16_t readU16(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] std::uint32_t readU32(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] std::uint64_t readU64(const std::uint8_t* bytes, std::size_t size, std::size_t offset);
[[nodiscard]] double readF64(const std::uint8_t* bytes, std::size_t size, std::size_t offset);

} // namespace pristine::layout
