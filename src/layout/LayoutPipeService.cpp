#include "pristine/layout/LayoutPipeService.h"

#include "pristine/layout/LayoutBinaryProtocol.h"
#include "pristine/layout/LayoutSource.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <cstring>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace pristine::layout {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t kFrameHeaderSize = 24;
constexpr std::size_t kMaxPayloadSize = 128U * 1024U * 1024U;

std::uint32_t currentProcessId() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::string endpointKind() {
#if defined(_WIN32)
    return "namedPipe";
#else
    return "unixSocket";
#endif
}

std::string endpointPath(std::string_view session_id) {
#if defined(_WIN32)
    return std::string("\\\\.\\pipe\\pristine-engine-layout-") +
           std::to_string(currentProcessId()) + "-" + std::string(session_id);
#else
    return (fs::temp_directory_path() /
            ("pristine-engine-layout-" + std::to_string(currentProcessId()) + "-" +
             std::string(session_id) + ".sock"))
        .generic_string();
#endif
}

std::vector<std::uint8_t> handleRequest(const LayoutSource& source, const LayoutFrame& request) {
    try {
        switch (request.message_type) {
            case LayoutMessageType::Hello:
                return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::HelloResponse,
                                               .request_id = request.request_id,
                                               .flags = 0,
                                               .version = source.protocolVersion(),
                                               .payload = source.encodeHelloResponse()});
            case LayoutMessageType::CatalogRequest:
                try {
                    return encodeFrame(LayoutFrame{
                        .message_type = LayoutMessageType::CatalogResponse,
                        .request_id = request.request_id,
                        .flags = 0,
                        .version = source.protocolVersion(),
                        .payload = source.encodeCatalogResponse()});
                }
                catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()).find("payload is too large") !=
                        std::string_view::npos) {
                        throw std::runtime_error(
                            "Layout catalog payload is too large; use paged catalog requests");
                    }
                    throw;
                }
            case LayoutMessageType::CatalogSummaryRequest:
                return encodeFrame(LayoutFrame{
                    .message_type = LayoutMessageType::CatalogSummaryResponse,
                    .request_id = request.request_id,
                    .flags = 0,
                    .version = source.protocolVersion(),
                    .payload = source.encodeCatalogSummaryResponse()});
            case LayoutMessageType::CatalogPageRequest: {
                const auto catalog_request = decodeCatalogPageRequestPayload(request.payload);
                return encodeFrame(LayoutFrame{
                    .message_type = LayoutMessageType::CatalogPageResponse,
                    .request_id = request.request_id,
                    .flags = 0,
                    .version = source.protocolVersion(),
                    .payload = source.encodeCatalogPageResponse(catalog_request)});
            }
            case LayoutMessageType::GeometryRequest: {
                const auto geometry_request = decodeGeometryRequestPayload(request.payload);
                const auto payload = source.encodeGeometryResponse(geometry_request);
                const auto flags = readU32(payload.data(), payload.size(), 20);
                return encodeFrame(LayoutFrame{.message_type =
                                                   LayoutMessageType::GeometryResponse,
                                               .request_id = request.request_id,
                                               .flags = flags,
                                               .version = source.protocolVersion(),
                                               .payload = payload});
            }
            case LayoutMessageType::TileGeometryRequest: {
                const auto tile_request = decodeTileGeometryRequestPayload(request.payload);
                const auto payload = source.encodeTileGeometryResponse(tile_request);
                const auto flags = readU32(payload.data(), payload.size(), 8);
                return encodeFrame(LayoutFrame{.message_type =
                                                   LayoutMessageType::TileGeometryResponse,
                                               .request_id = request.request_id,
                                               .flags = flags,
                                               .version = source.protocolVersion(),
                                               .payload = payload});
            }
            case LayoutMessageType::HitTestRequest: {
                const auto hit_request = decodeHitTestRequestPayload(request.payload);
                return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::HitTestResponse,
                                               .request_id = request.request_id,
                                               .flags = 0,
                                               .version = source.protocolVersion(),
                                               .payload = source.encodeHitTestResponse(hit_request)});
            }
            case LayoutMessageType::InspectRequest: {
                const auto inspect_request = decodeInspectRequestPayload(request.payload);
                return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::InspectResponse,
                                               .request_id = request.request_id,
                                               .flags = 0,
                                               .version = source.protocolVersion(),
                                               .payload =
                                                   source.encodeInspectResponse(inspect_request)});
            }
            case LayoutMessageType::SelectionGeometryRequest: {
                const auto selection_request =
                    decodeSelectionGeometryRequestPayload(request.payload);
                const auto payload = source.encodeSelectionGeometryResponse(selection_request);
                const auto flags = readU32(payload.data(), payload.size(), 20);
                return encodeFrame(LayoutFrame{.message_type =
                                                   LayoutMessageType::SelectionGeometryResponse,
                                               .request_id = request.request_id,
                                               .flags = flags,
                                               .version = source.protocolVersion(),
                                               .payload = payload});
            }
            case LayoutMessageType::SearchRequest: {
                const auto search_request = decodeSearchRequestPayload(request.payload);
                return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::SearchResponse,
                                               .request_id = request.request_id,
                                               .flags = 0,
                                               .version = source.protocolVersion(),
                                               .payload = source.encodeSearchResponse(search_request)});
            }
            case LayoutMessageType::Close:
                return {};
            default:
                return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::ErrorResponse,
                                               .request_id = request.request_id,
                                               .flags = 0,
                                               .version = source.protocolVersion(),
                                               .payload = encodeErrorPayload(
                                                   LayoutErrorCode::UnknownMessage,
                                                   "Unknown layout message type")});
        }
    }
    catch (const std::exception& error) {
        return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::ErrorResponse,
                                       .request_id = request.request_id,
                                       .flags = 0,
                                       .version = source.protocolVersion(),
                                       .payload = encodeErrorPayload(LayoutErrorCode::InvalidRequest,
                                                                     error.what())});
    }
}

std::vector<std::uint8_t> encodeProtocolErrorResponse(std::uint32_t request_id,
                                                      LayoutErrorCode code,
                                                      std::string_view message) {
    return encodeFrame(LayoutFrame{.message_type = LayoutMessageType::ErrorResponse,
                                   .request_id = request_id,
                                   .flags = 0,
                                   .version = kLayoutProtocolVersion,
                                   .payload = encodeErrorPayload(code, message)});
}

#if defined(_WIN32)

using NativeHandle = HANDLE;
const NativeHandle kInvalidNativeHandle = INVALID_HANDLE_VALUE;

bool readExact(NativeHandle handle,
               std::uint8_t* output,
               std::size_t length,
               const LayoutPipeService& service,
               std::string_view session_id) {
    std::size_t read_total = 0;
    while (read_total < length) {
        if (service.shouldStop(session_id)) {
            return false;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0) {
            Sleep(5);
            continue;
        }
        const auto remaining = length - read_total;
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, std::min<std::size_t>(available, 64U * 1024U)));
        DWORD bytes_read = 0;
        if (!ReadFile(handle, output + read_total, chunk, &bytes_read, nullptr) ||
            bytes_read == 0) {
            return false;
        }
        read_total += bytes_read;
    }
    return true;
}

bool writeExact(NativeHandle handle, const std::vector<std::uint8_t>& bytes) {
    std::size_t written_total = 0;
    while (written_total < bytes.size()) {
        const auto remaining = bytes.size() - written_total;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64U * 1024U));
        DWORD bytes_written = 0;
        if (!WriteFile(handle, bytes.data() + written_total, chunk, &bytes_written, nullptr)) {
            return false;
        }
        written_total += bytes_written;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readFrameBytes(NativeHandle handle,
                                                        const LayoutPipeService& service,
                                                        std::string_view session_id) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    if (!readExact(handle, header.data(), header.size(), service, session_id)) {
        return std::nullopt;
    }
    const auto payload_size = readU32(header.data(), header.size(), 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Layout payload is too large");
    }
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.resize(kFrameHeaderSize + payload_size);
    if (payload_size != 0 &&
        !readExact(handle, bytes.data() + kFrameHeaderSize, payload_size, service, session_id)) {
        return std::nullopt;
    }
    return bytes;
}

void closeNative(NativeHandle handle) {
    if (handle != kInvalidNativeHandle) {
        FlushFileBuffers(handle);
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
    }
}

#else

using NativeHandle = int;
constexpr NativeHandle kInvalidNativeHandle = -1;

bool readExact(NativeHandle fd,
               std::uint8_t* output,
               std::size_t length,
               const LayoutPipeService& service,
               std::string_view session_id) {
    std::size_t read_total = 0;
    while (read_total < length) {
        if (service.shouldStop(session_id)) {
            return false;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);
        timeval timeout{.tv_sec = 0, .tv_usec = 5000};
        const auto ready = select(fd + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready < 0) {
            return false;
        }
        if (ready == 0) {
            continue;
        }
        const auto result = ::read(fd, output + read_total, length - read_total);
        if (result <= 0) {
            return false;
        }
        read_total += static_cast<std::size_t>(result);
    }
    return true;
}

bool writeExact(NativeHandle fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t written_total = 0;
    while (written_total < bytes.size()) {
        const auto result = ::write(fd, bytes.data() + written_total, bytes.size() - written_total);
        if (result <= 0) {
            return false;
        }
        written_total += static_cast<std::size_t>(result);
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readFrameBytes(NativeHandle fd,
                                                        const LayoutPipeService& service,
                                                        std::string_view session_id) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    if (!readExact(fd, header.data(), header.size(), service, session_id)) {
        return std::nullopt;
    }
    const auto payload_size = readU32(header.data(), header.size(), 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Layout payload is too large");
    }
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.resize(kFrameHeaderSize + payload_size);
    if (payload_size != 0 &&
        !readExact(fd, bytes.data() + kFrameHeaderSize, payload_size, service, session_id)) {
        return std::nullopt;
    }
    return bytes;
}

void closeNative(NativeHandle fd) {
    if (fd != kInvalidNativeHandle) {
        (void)::close(fd);
    }
}

#endif

void serveConnection(NativeHandle connection,
                     const LayoutSource& source,
                     const LayoutPipeService& service,
                     std::string_view session_id) {
    while (!service.shouldStop(session_id)) {
        auto frame_bytes = readFrameBytes(connection, service, session_id);
        if (!frame_bytes.has_value()) {
            return;
        }
        LayoutFrame request;
        try {
            request = decodeFrame(*frame_bytes);
        }
        catch (const std::exception& error) {
            const auto response = encodeProtocolErrorResponse(0,
                                                              LayoutErrorCode::UnsupportedVersion,
                                                              error.what());
            (void)writeExact(connection, response);
            return;
        }
        if (request.message_type == LayoutMessageType::Close) {
            return;
        }
        auto response = handleRequest(source, request);
        if (response.empty() || !writeExact(connection, response)) {
            return;
        }
    }
}

} // namespace

LayoutPipeService::~LayoutPipeService() {
    stop();
}

LayoutSessionInfo LayoutPipeService::openSession(std::shared_ptr<LayoutSource> source,
                                                 std::size_t lef_count,
                                                 bool def_present) {
    if (!source) {
        throw std::runtime_error("Layout source is null");
    }
    stop();

    const auto& data = source->dataSet();
    LayoutSessionInfo info{.session_id = std::to_string(next_session_number_++),
                           .protocol = std::string(source->protocolName()),
                           .endpoint = LayoutPipeEndpoint{},
                           .title = data.title,
                           .source = std::string(source->sourceKind()),
                           .lef_count = lef_count,
                           .def_present = def_present,
                           .units_per_micron = data.units_per_micron,
                           .bounds = data.bounds,
                           .layer_count = data.layers.size(),
                           .macro_count = data.macros.size(),
                           .component_count = data.components.size(),
                           .net_count = data.nets.size(),
                           .cell_count = data.gds.has_value() ? data.gds->cells.size() : 0U,
                           .reference_count = data.gds.has_value() ? data.gds->references.size() : 0U,
                           .element_count = data.gds.has_value() ? data.gds->elements.size() : 0U,
                           .diagnostic_count = data.diagnostics.size(),
                           .gds_open_metrics = data.gds_open_metrics,
                           .file_uris = data.file_uris};
    info.endpoint.kind = endpointKind();
    info.endpoint.path = endpointPath(info.session_id);
    for (const auto& diagnostic : data.diagnostics) {
        info.messages.push_back(diagnostic.message);
    }

    {
        std::lock_guard lock(mutex_);
        stop_requested_ = false;
        active_session_ = info;
    }
    thread_ = std::thread(&LayoutPipeService::runSession,
                          this,
                          info.session_id,
                          info.endpoint.path,
                          std::move(source));
    return info;
}

bool LayoutPipeService::closeSession(std::string_view session_id) {
    std::optional<LayoutSessionInfo> active;
    {
        std::lock_guard lock(mutex_);
        active = active_session_;
    }
    if (!active.has_value() || active->session_id != session_id) {
        return false;
    }
    stop();
    return true;
}

void LayoutPipeService::stop() {
    std::optional<std::string> endpoint_path;
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        if (active_session_.has_value()) {
            endpoint_path = active_session_->endpoint.path;
        }
    }
    if (endpoint_path.has_value()) {
        wakeEndpoint(*endpoint_path);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard lock(mutex_);
        active_session_.reset();
        stop_requested_ = false;
    }
}

bool LayoutPipeService::shouldStop(std::string_view session_id) const {
    std::lock_guard lock(mutex_);
    return stop_requested_ || !active_session_.has_value() ||
           active_session_->session_id != session_id;
}

#if defined(_WIN32)

void LayoutPipeService::runSession(std::string session_id,
                                   std::string endpoint_path,
                                   std::shared_ptr<LayoutSource> source) {
    while (!shouldStop(session_id)) {
        NativeHandle pipe = CreateNamedPipeA(endpoint_path.c_str(),
                                             PIPE_ACCESS_DUPLEX,
                                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                             1,
                                             1024 * 1024,
                                             1024 * 1024,
                                             0,
                                             nullptr);
        if (pipe == kInvalidNativeHandle) {
            return;
        }
        BOOL connected = FALSE;
        while (!shouldStop(session_id)) {
            if (ConnectNamedPipe(pipe, nullptr) != 0) {
                connected = TRUE;
                break;
            }
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
                break;
            }
            if (error != ERROR_PIPE_LISTENING) {
                break;
            }
            Sleep(5);
        }
        if (!connected || shouldStop(session_id)) {
            closeNative(pipe);
            continue;
        }
        serveConnection(pipe, *source, *this, session_id);
        closeNative(pipe);
    }
}

void LayoutPipeService::wakeEndpoint(const std::string& endpoint_path) {
    NativeHandle client = CreateFileA(endpoint_path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      0,
                                      nullptr);
    if (client != kInvalidNativeHandle) {
        CloseHandle(client);
    }
}

#else

void LayoutPipeService::runSession(std::string session_id,
                                   std::string endpoint_path,
                                   std::shared_ptr<LayoutSource> source) {
    std::error_code remove_error;
    fs::remove(endpoint_path, remove_error);

    const auto server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server == kInvalidNativeHandle) {
        return;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint_path.size() >= sizeof(address.sun_path)) {
        closeNative(server);
        return;
    }
    std::strncpy(address.sun_path, endpoint_path.c_str(), sizeof(address.sun_path) - 1);
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(server, 1) != 0) {
        closeNative(server);
        fs::remove(endpoint_path, remove_error);
        return;
    }

    while (!shouldStop(session_id)) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(server, &read_set);
        timeval timeout{.tv_sec = 0, .tv_usec = 5000};
        const auto ready = select(server + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }
        const auto client = ::accept(server, nullptr, nullptr);
        if (client == kInvalidNativeHandle) {
            continue;
        }
        if (!shouldStop(session_id)) {
            serveConnection(client, *source, *this, session_id);
        }
        closeNative(client);
    }

    closeNative(server);
    fs::remove(endpoint_path, remove_error);
}

void LayoutPipeService::wakeEndpoint(const std::string& endpoint_path) {
    const auto client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client == kInvalidNativeHandle) {
        return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint_path.size() < sizeof(address.sun_path)) {
        std::strncpy(address.sun_path, endpoint_path.c_str(), sizeof(address.sun_path) - 1);
        (void)::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    closeNative(client);
}

#endif

} // namespace pristine::layout
