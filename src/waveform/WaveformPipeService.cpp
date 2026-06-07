#include "pristine/waveform/WaveformPipeService.h"

#include "pristine/waveform/WaveformBinaryProtocol.h"
#include "pristine/waveform/WaveformMockGenerator.h"
#include "pristine/waveform/WaveformViewportEncoder.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
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
#    include <cerrno>
#    include <cstring>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace pristine::waveform {
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
    return std::string("\\\\.\\pipe\\pristine-engine-waveform-") +
           std::to_string(currentProcessId()) + "-" + std::string(session_id);
#else
    return (fs::temp_directory_path() /
            ("pristine-engine-waveform-" + std::to_string(currentProcessId()) + "-" +
             std::string(session_id) + ".sock"))
        .generic_string();
#endif
}

std::vector<std::uint8_t> handleRequest(const WaveformDataSet& data, const WaveformFrame& request) {
    try {
        switch (request.message_type) {
            case WaveformMessageType::Hello:
                return encodeFrame(WaveformFrame{.message_type = WaveformMessageType::HelloResponse,
                                                 .request_id = request.request_id,
                                                 .flags = 0,
                                                 .payload = encodeHelloResponsePayload(data)});
            case WaveformMessageType::CatalogRequest:
                return encodeFrame(WaveformFrame{.message_type =
                                                     WaveformMessageType::CatalogResponse,
                                                 .request_id = request.request_id,
                                                 .flags = 0,
                                                 .payload = encodeCatalogResponsePayload(data)});
            case WaveformMessageType::ViewportFrameRequest: {
                const auto viewport_request = decodeViewportFrameRequestPayload(request.payload);
                const auto payload = encodeViewportFramePayload(data, viewport_request);
                const auto flags = readU32(payload.data(), payload.size(), 48);
                return encodeFrame(WaveformFrame{.message_type =
                                                     WaveformMessageType::ViewportFrameResponse,
                                                 .request_id = request.request_id,
                                                 .flags = flags,
                                                 .payload = payload});
            }
            case WaveformMessageType::ViewportFrameRequestV2: {
                const auto viewport_request = decodeViewportFrameRequestPayloadV2(request.payload);
                const auto payload = encodeViewportFramePayloadV2(data, viewport_request);
                const auto flags = readU32(payload.data(), payload.size(), 48);
                return encodeFrame(WaveformFrame{.message_type =
                                                     WaveformMessageType::ViewportFrameResponseV2,
                                                 .request_id = request.request_id,
                                                 .flags = flags,
                                                 .payload = payload});
            }
            case WaveformMessageType::Close:
                return {};
            default:
                return encodeFrame(WaveformFrame{.message_type = WaveformMessageType::ErrorResponse,
                                                 .request_id = request.request_id,
                                                 .flags = 0,
                                                 .payload = encodeErrorPayload(
                                                     WaveformErrorCode::UnknownMessage,
                                                     "Unknown waveform message type")});
        }
    }
    catch (const std::exception& error) {
        return encodeFrame(WaveformFrame{.message_type = WaveformMessageType::ErrorResponse,
                                         .request_id = request.request_id,
                                         .flags = 0,
                                         .payload = encodeErrorPayload(
                                             WaveformErrorCode::InvalidRequest,
                                             error.what())});
    }
}

#if defined(_WIN32)

using NativeHandle = HANDLE;
const NativeHandle kInvalidNativeHandle = INVALID_HANDLE_VALUE;

bool readExact(NativeHandle handle,
               std::uint8_t* output,
               std::size_t length,
               const WaveformPipeService& service,
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
                                                        const WaveformPipeService& service,
                                                        std::string_view session_id) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    if (!readExact(handle, header.data(), header.size(), service, session_id)) {
        return std::nullopt;
    }
    const auto payload_size = readU32(header.data(), header.size(), 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Waveform payload is too large");
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
               const WaveformPipeService& service,
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
                                                        const WaveformPipeService& service,
                                                        std::string_view session_id) {
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    if (!readExact(fd, header.data(), header.size(), service, session_id)) {
        return std::nullopt;
    }
    const auto payload_size = readU32(header.data(), header.size(), 16);
    if (payload_size > kMaxPayloadSize) {
        throw std::runtime_error("Waveform payload is too large");
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
                     const WaveformDataSet& data,
                     const WaveformPipeService& service,
                     std::string_view session_id) {
    while (!service.shouldStop(session_id)) {
        auto frame_bytes = readFrameBytes(connection, service, session_id);
        if (!frame_bytes.has_value()) {
            return;
        }
        const auto request = decodeFrame(*frame_bytes);
        if (request.message_type == WaveformMessageType::Close) {
            return;
        }
        auto response = handleRequest(data, request);
        if (response.empty() || !writeExact(connection, response)) {
            return;
        }
    }
}

} // namespace

WaveformPipeService::~WaveformPipeService() {
    stop();
}

WaveformSessionInfo WaveformPipeService::openMockSession() {
    stop();

    auto data = makeMockWaveformDataSet();
    WaveformSessionInfo info{
        .session_id = std::to_string(next_session_number_++),
        .protocol = std::string(kWaveformProtocolName),
        .endpoint = WaveformPipeEndpoint{},
        .title = data.title,
        .duration = data.duration,
        .timescale_unit = data.timescale_unit,
        .group_count = data.groups.size(),
        .signal_count = data.signals.size()};
    info.endpoint.kind = endpointKind();
    info.endpoint.path = endpointPath(info.session_id);

    {
        std::lock_guard lock(mutex_);
        stop_requested_ = false;
        active_session_ = info;
    }
    thread_ = std::thread(&WaveformPipeService::runSession,
                          this,
                          info.session_id,
                          info.endpoint.path,
                          std::move(data));
    return info;
}

bool WaveformPipeService::closeSession(std::string_view session_id) {
    std::optional<WaveformSessionInfo> active;
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

void WaveformPipeService::stop() {
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

bool WaveformPipeService::shouldStop(std::string_view session_id) const {
    std::lock_guard lock(mutex_);
    return stop_requested_ || !active_session_.has_value() || active_session_->session_id != session_id;
}

#if defined(_WIN32)

void WaveformPipeService::runSession(std::string session_id,
                                     std::string endpoint_path,
                                     WaveformDataSet data) {
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
        serveConnection(pipe, data, *this, session_id);
        closeNative(pipe);
    }
}

void WaveformPipeService::wakeEndpoint(const std::string& endpoint_path) {
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

void WaveformPipeService::runSession(std::string session_id,
                                     std::string endpoint_path,
                                     WaveformDataSet data) {
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
            serveConnection(client, data, *this, session_id);
        }
        closeNative(client);
    }

    closeNative(server);
    fs::remove(endpoint_path, remove_error);
}

void WaveformPipeService::wakeEndpoint(const std::string& endpoint_path) {
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

} // namespace pristine::waveform
