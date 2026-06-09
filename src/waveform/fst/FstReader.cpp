#include "pristine/waveform/fst/FstReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <fastlz.h>
#include <lz4.h>
#include <zlib.h>

namespace pristine::waveform::fst {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kFstHeaderLength = 330;
constexpr std::uint64_t kFstHeaderSectionLength = 329;
constexpr std::uint8_t kScopeTag = 254;
constexpr std::uint8_t kUpscopeTag = 255;
constexpr std::uint8_t kAttrBeginTag = 252;
constexpr std::uint8_t kAttrEndTag = 253;
constexpr std::uint8_t kMaxScopeType = 22;
constexpr std::uint8_t kMaxVarType = 29;
constexpr std::uint8_t kVarTypeReal = 3;
constexpr std::uint8_t kVarTypeRealParameter = 4;
constexpr std::uint8_t kVarTypePort = 18;
constexpr std::uint8_t kVarTypeRealTime = 20;
constexpr std::uint8_t kVarTypeGenericString = 21;
constexpr std::uint8_t kVarTypeShortReal = 29;
constexpr double kFstDoubleEndianTest = 2.7182818284590452354;
constexpr std::string_view kExtraValueChars = "xzhuwl-?";

struct FstFileBytes {
    std::filesystem::path path;
    std::vector<std::uint8_t> bytes;
};

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedMicros(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

class ScopedMetric {
public:
    explicit ScopedMetric(std::uint64_t* target) : target_(target) {
        if (target_ != nullptr) {
            start_ = Clock::now();
        }
    }

    ScopedMetric(const ScopedMetric&) = delete;
    ScopedMetric& operator=(const ScopedMetric&) = delete;

    ~ScopedMetric() {
        if (target_ != nullptr) {
            *target_ += elapsedMicros(start_, Clock::now());
        }
    }

private:
    std::uint64_t* target_ = nullptr;
    Clock::time_point start_{};
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string("FST parse error: ") + std::string(message));
}

bool isWithinRoot(const fs::path& root, const fs::path& path) {
    std::error_code error;
    const auto canonical_root = fs::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const auto canonical_path = fs::weakly_canonical(path, error);
    if (error) {
        return false;
    }

    auto root_it = canonical_root.begin();
    auto path_it = canonical_path.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++path_it) {
        if (path_it == canonical_path.end() || *root_it != *path_it) {
            return false;
        }
    }
    return true;
}

void validateWorkspacePath(const fs::path& path, const FstReadOptions& options, std::string_view label) {
    if (!options.workspace_root.has_value()) {
        return;
    }
    if (!isWithinRoot(*options.workspace_root, path)) {
        throw std::runtime_error(std::string(label) + " must be inside the workspace root");
    }
}

std::uint64_t readU64Be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
        fail("truncated uint64");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

std::uint64_t readU64Be(std::istream& input) {
    std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        fail("truncated uint64");
    }
    std::uint64_t value = 0;
    for (const auto byte : bytes) {
        value = (value << 8U) | static_cast<std::uint64_t>(byte);
    }
    return value;
}

std::uint32_t checkedU32(std::uint64_t value, std::string_view field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(value);
}

std::string fixedString(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset,
                        std::size_t length) {
    if (offset > bytes.size() || bytes.size() - offset < length) {
        fail("truncated fixed string");
    }
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    auto nul = std::find(begin, end, std::uint8_t{0});
    return std::string(begin, nul);
}

bool headerFloatEndianMatchesHost(const std::vector<std::uint8_t>& header) {
    std::array<std::uint8_t, sizeof(double)> bytes{};
    std::copy_n(header.begin() + 25, bytes.size(), bytes.begin());

    double direct = 0.0;
    std::memcpy(&direct, bytes.data(), sizeof(direct));
    if (direct == kFstDoubleEndianTest) {
        return true;
    }

    std::reverse(bytes.begin(), bytes.end());
    double reversed = 0.0;
    std::memcpy(&reversed, bytes.data(), sizeof(reversed));
    if (reversed == kFstDoubleEndianTest) {
        return false;
    }
    fail("bad endian test");
}

FstHeader parseHeader(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kFstHeaderLength) {
        fail("truncated header");
    }
    if (bytes[0] != static_cast<std::uint8_t>(FstBlockType::Header)) {
        fail("bad header magic");
    }
    const auto section_length = readU64Be(bytes, 1);
    if (section_length < kFstHeaderSectionLength) {
        fail("unsupported short header");
    }
    FstHeader header{};
    header.float_endian_matches_host = headerFloatEndianMatchesHost(bytes);
    header.start_time = readU64Be(bytes, 9);
    header.end_time = readU64Be(bytes, 17);
    header.memory_used_by_writer = readU64Be(bytes, 33);
    header.scope_count = readU64Be(bytes, 41);
    header.variable_count = readU64Be(bytes, 49);
    header.max_handle = readU64Be(bytes, 57);
    header.value_change_section_count = readU64Be(bytes, 65);
    header.timescale = static_cast<std::int8_t>(bytes[73]);
    header.version = fixedString(bytes, 74, 128);
    header.date = fixedString(bytes, 202, 119);
    header.file_type = bytes[321];
    header.timezero = readU64Be(bytes, 322);
    if (header.end_time < header.start_time) {
        fail("header end time precedes start time");
    }
    return header;
}

std::uint64_t readVarint(const std::vector<std::uint8_t>& bytes,
                         std::size_t& offset,
                         std::size_t max_length = 10) {
    std::uint64_t value = 0;
    std::size_t count = 0;
    while (true) {
        if (offset >= bytes.size()) {
            fail("truncated varint");
        }
        const auto byte = bytes[offset++];
        ++count;
        if (count > max_length) {
            fail("varint is too long");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << (7U * (count - 1U));
        if ((byte & 0x80U) == 0U) {
            return value;
        }
    }
}

std::uint64_t readVarintAt(const std::vector<std::uint8_t>& bytes,
                           std::size_t offset,
                           std::size_t& skip_length,
                           std::size_t max_length = 10) {
    const auto begin = offset;
    const auto result = readVarint(bytes, offset, max_length);
    skip_length = offset - begin;
    return result;
}

std::uint64_t readVarintAtNoSkip(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::size_t skip_length = 0;
    return readVarintAt(bytes, offset, skip_length);
}

std::int64_t readSignedVarint(const std::vector<std::uint8_t>& bytes,
                              std::size_t& offset,
                              std::size_t max_length = 10) {
    std::int64_t value = 0;
    std::size_t count = 0;
    std::uint8_t byte = 0;
    int shift = 0;
    do {
        if (offset >= bytes.size()) {
            fail("truncated signed varint");
        }
        byte = bytes[offset++];
        ++count;
        if (count > max_length) {
            fail("signed varint is too long");
        }
        value |= static_cast<std::int64_t>(byte & 0x7fU) << shift;
        shift += 7;
    } while ((byte & 0x80U) != 0U);

    constexpr int bits = sizeof(std::int64_t) * 8;
    if (shift < bits && (byte & 0x40U) != 0U) {
        value |= -(std::int64_t{1} << shift);
    }
    return value;
}

std::vector<std::uint8_t> inflateGzip(const std::vector<std::uint8_t>& encoded,
                                      std::size_t expected_size,
                                      std::string_view label) {
    if (expected_size > static_cast<std::size_t>(std::numeric_limits<uInt>::max()) ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds gzip inflate range");
    }
    std::vector<std::uint8_t> decoded(expected_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(encoded.data());
    stream.avail_in = static_cast<uInt>(encoded.size());
    stream.next_out = decoded.data();
    stream.avail_out = static_cast<uInt>(decoded.size());
    auto result = inflateInit2(&stream, 16 + MAX_WBITS);
    if (result != Z_OK) {
        throw std::runtime_error(std::string("FST gzip inflate init failed for ") +
                                 std::string(label));
    }
    result = inflate(&stream, Z_FINISH);
    const auto ended = inflateEnd(&stream);
    if (result != Z_STREAM_END || ended != Z_OK || stream.total_out != decoded.size()) {
        throw std::runtime_error(std::string("FST gzip inflate failed for ") +
                                 std::string(label));
    }
    return decoded;
}

std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& encoded,
                                      std::size_t expected_size,
                                      std::string_view label) {
    if (expected_size > static_cast<std::size_t>(std::numeric_limits<uLongf>::max()) ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds zlib range");
    }
    std::vector<std::uint8_t> decoded(expected_size);
    auto destination_size = static_cast<uLongf>(decoded.size());
    const auto result = uncompress(decoded.data(),
                                   &destination_size,
                                   encoded.data(),
                                   static_cast<uLong>(encoded.size()));
    if (result != Z_OK || destination_size != decoded.size()) {
        throw std::runtime_error(std::string("FST zlib inflate failed for ") +
                                 std::string(label));
    }
    return decoded;
}

std::vector<std::uint8_t> inflateRawDeflate(const std::vector<std::uint8_t>& encoded,
                                            std::size_t expected_size,
                                            std::string_view label) {
    if (expected_size > static_cast<std::size_t>(std::numeric_limits<uInt>::max()) ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds raw deflate range");
    }
    std::vector<std::uint8_t> decoded(expected_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(encoded.data());
    stream.avail_in = static_cast<uInt>(encoded.size());
    stream.next_out = decoded.data();
    stream.avail_out = static_cast<uInt>(decoded.size());
    auto result = inflateInit2(&stream, -MAX_WBITS);
    if (result != Z_OK) {
        throw std::runtime_error(std::string("FST raw deflate init failed for ") +
                                 std::string(label));
    }
    result = inflate(&stream, Z_FINISH);
    const auto ended = inflateEnd(&stream);
    if (result != Z_STREAM_END || ended != Z_OK || stream.total_out != decoded.size()) {
        throw std::runtime_error(std::string("FST raw deflate failed for ") +
                                 std::string(label));
    }
    return decoded;
}

std::vector<std::uint8_t> inflateLz4(const std::vector<std::uint8_t>& encoded,
                                     std::size_t expected_size,
                                     std::string_view label) {
    if (expected_size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds LZ4 range");
    }
    std::vector<std::uint8_t> decoded(expected_size);
    const auto result = LZ4_decompress_safe_partial(
        reinterpret_cast<const char*>(encoded.data()),
        reinterpret_cast<char*>(decoded.data()),
        static_cast<int>(encoded.size()),
        static_cast<int>(decoded.size()),
        static_cast<int>(decoded.size()));
    if (result != static_cast<int>(decoded.size())) {
        throw std::runtime_error(std::string("FST LZ4 inflate failed for ") +
                                 std::string(label));
    }
    return decoded;
}

std::vector<std::uint8_t> inflateFastLz(const std::vector<std::uint8_t>& encoded,
                                        std::size_t expected_size,
                                        std::string_view label) {
    if (expected_size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds FastLZ range");
    }
    std::vector<std::uint8_t> decoded(expected_size);
    const auto result = fastlz_decompress(encoded.data(),
                                          static_cast<int>(encoded.size()),
                                          decoded.data(),
                                          static_cast<int>(decoded.size()));
    if (result != static_cast<int>(decoded.size())) {
        throw std::runtime_error(std::string("FST FastLZ inflate failed for ") +
                                 std::string(label));
    }
    return decoded;
}

FstFileBytes readFstBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open FST file: " + path.string());
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        fail("file is shorter than FST header");
    }

    if (bytes.front() != static_cast<std::uint8_t>(FstBlockType::ZWrapper)) {
        return FstFileBytes{.path = path, .bytes = std::move(bytes)};
    }
    if (bytes.size() < 27U) {
        fail("truncated gzip-wrapped FST file");
    }
    const auto section_length = readU64Be(bytes, 1);
    const auto uncompressed_length = readU64Be(bytes, 9);
    if (section_length == 0U) {
        fail("gzip-wrapped FST file is not finished");
    }
    if (section_length > bytes.size() - 1U) {
        fail("truncated gzip-wrapped FST block");
    }
    if (bytes[17] != 0x1fU || bytes[18] != 0x8bU || bytes[19] != 8U) {
        fail("bad gzip wrapper header");
    }
    if (bytes[20] != 0U) {
        fail("unsupported gzip wrapper flags");
    }
    const auto deflate_begin = std::size_t{27};
    const auto deflate_end = 1U + static_cast<std::size_t>(section_length);
    if (deflate_end < deflate_begin || deflate_end > bytes.size()) {
        fail("truncated gzip wrapper body");
    }
    std::vector<std::uint8_t> encoded(bytes.begin() + static_cast<std::ptrdiff_t>(deflate_begin),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(deflate_end));
    auto decoded = inflateRawDeflate(encoded,
                                     static_cast<std::size_t>(uncompressed_length),
                                     "FST gzip wrapper");
    return FstFileBytes{.path = path, .bytes = std::move(decoded)};
}

std::vector<std::uint8_t> readRange(const FstFileBytes& file,
                                    std::uint64_t offset,
                                    std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fail("block payload is too large");
    }
    if (offset > file.bytes.size() || size > file.bytes.size() - offset) {
        fail("truncated block payload");
    }
    return std::vector<std::uint8_t>(
        file.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        file.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

std::string readNullString(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset >= bytes.size()) {
        fail("truncated string");
    }
    const auto begin = offset;
    while (offset < bytes.size() && bytes[offset] != 0) {
        ++offset;
    }
    if (offset >= bytes.size()) {
        fail("unterminated string");
    }
    std::string result(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    ++offset;
    return result;
}

std::string joinPath(const std::vector<std::string>& scopes, std::string_view leaf = {}) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& scope : scopes) {
        if (scope.empty()) {
            continue;
        }
        if (!first) {
            stream << '.';
        }
        stream << scope;
        first = false;
    }
    if (!leaf.empty()) {
        if (!first) {
            stream << '.';
        }
        stream << leaf;
    }
    return stream.str();
}

void skipAttribute(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    // libfst hierarchy attributes carry optional source, enum, VHDL, pack, and array metadata.
    // The waveform catalog only needs scopes, signal names, handles, and widths, so consume the
    // record exactly and leave richer metadata for a later catalog expansion.
    if (offset >= bytes.size()) {
        fail("truncated attribute");
    }
    const auto attribute_type = bytes[offset++];
    if (offset >= bytes.size()) {
        fail("truncated attribute subtype");
    }
    const auto subtype = bytes[offset++];
    if (attribute_type == 0U && (subtype == 4U || subtype == 5U)) {
        (void)readVarint(bytes, offset);
        if (offset >= bytes.size() || bytes[offset] != 0U) {
            fail("unterminated source attribute");
        }
        ++offset;
    }
    else {
        (void)readNullString(bytes, offset);
    }
    (void)readVarint(bytes, offset);
}

void parseHierarchyPayload(const std::vector<std::uint8_t>& payload, FstData& data) {
    std::vector<std::string> scope_names;
    std::vector<std::uint32_t> scope_indices;
    std::size_t offset = 0;
    std::uint32_t next_handle = 1;
    std::uint32_t unnamed_scope = 0;

    while (offset < payload.size()) {
        const auto tag = payload[offset++];
        if (tag == kScopeTag) {
            if (offset >= payload.size()) {
                fail("truncated scope type");
            }
            auto scope_type = payload[offset++];
            if (scope_type > kMaxScopeType) {
                scope_type = 0;
            }
            auto name = readNullString(payload, offset);
            auto component = readNullString(payload, offset);
            if (name.empty()) {
                name = "$unnamed_scope_" + std::to_string(unnamed_scope++);
            }
            const auto parent = scope_indices.empty() ? 0U : scope_indices.back();
            scope_names.push_back(name);
            const auto scope_index = static_cast<std::uint32_t>(data.scopes.size());
            scope_indices.push_back(scope_index);
            data.scopes.push_back(FstScope{.index = scope_index,
                                           .parent_index = parent,
                                           .type = scope_type,
                                           .name = name,
                                           .component = component,
                                           .path = joinPath(scope_names)});
            continue;
        }
        if (tag == kUpscopeTag) {
            if (!scope_names.empty()) {
                scope_names.pop_back();
                scope_indices.pop_back();
            }
            continue;
        }
        if (tag == kAttrBeginTag) {
            skipAttribute(payload, offset);
            continue;
        }
        if (tag == kAttrEndTag) {
            continue;
        }
        if (tag > kMaxVarType) {
            fail("unknown hierarchy record tag");
        }

        if (offset >= payload.size()) {
            fail("truncated variable direction");
        }
        const auto direction = payload[offset++];
        auto name = readNullString(payload, offset);
        auto width = checkedU32(readVarint(payload, offset), "FST signal width");
        const auto alias = checkedU32(readVarint(payload, offset), "FST signal alias");
        if (tag == kVarTypePort) {
            width = width >= 2U ? (width - 2U) / 3U : 0U;
        }
        if (tag == kVarTypeReal || tag == kVarTypeRealParameter || tag == kVarTypeRealTime ||
            tag == kVarTypeShortReal) {
            width = 8;
        }
        if (tag == kVarTypeGenericString) {
            width = 0;
        }
        if (width == 0 || width == std::numeric_limits<std::uint32_t>::max()) {
            width = 1;
        }

        const auto handle = alias == 0 ? next_handle++ : alias;
        const auto scope_index = scope_indices.empty() ? 0U : scope_indices.back();
        auto path = joinPath(scope_names, name);
        data.signals.push_back(FstSignal{.handle = handle,
                                         .alias_handle = alias,
                                         .var_type = tag,
                                         .direction = direction,
                                         .width = width,
                                         .scope_index = scope_index,
                                         .name = std::move(name),
                                         .path = std::move(path)});
    }
}

void parseGeometryPayload(const std::vector<std::uint8_t>& payload, FstData& data) {
    if (payload.size() < 16) {
        fail("truncated geometry block");
    }
    const auto uncompressed_length = readU64Be(payload, 0);
    const auto max_handle = readU64Be(payload, 8);
    const auto encoded_length = payload.size() - 16U;
    if (max_handle > data.header.max_handle && data.header.max_handle != 0) {
        fail("geometry max handle exceeds header max handle");
    }

    std::vector<std::uint8_t> geometry(payload.begin() + 16, payload.end());
    if (uncompressed_length != encoded_length) {
        geometry = inflateZlib(geometry,
                               static_cast<std::size_t>(uncompressed_length),
                               "FST geometry");
    }

    std::size_t offset = 0;
    data.signal_widths_by_handle.clear();
    data.signal_geometry_by_handle.clear();
    data.signal_widths_by_handle.reserve(static_cast<std::size_t>(max_handle));
    data.signal_geometry_by_handle.reserve(static_cast<std::size_t>(max_handle));
    for (std::uint64_t handle = 1; handle <= max_handle; ++handle) {
        if (offset >= geometry.size()) {
            break;
        }
        const auto encoded = checkedU32(readVarint(geometry, offset), "FST geometry width");
        auto signal_geometry = FstSignalGeometry{};
        if (encoded == 0U) {
            signal_geometry.length = 8U;
            signal_geometry.is_real = true;
        }
        else if (encoded == std::numeric_limits<std::uint32_t>::max()) {
            signal_geometry.length = 0U;
        }
        else {
            signal_geometry.length = encoded;
        }
        data.signal_geometry_by_handle.push_back(signal_geometry);
        data.signal_widths_by_handle.push_back(std::max(1U, signal_geometry.length));
    }
    for (auto& signal : data.signals) {
        if (signal.handle == 0U ||
            signal.handle > data.signal_widths_by_handle.size()) {
            continue;
        }
        signal.width = std::max(1U, data.signal_widths_by_handle[signal.handle - 1U]);
    }
}

std::vector<std::uint64_t> decodeTimeTable(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 24) {
        fail("truncated value-change time table");
    }
    const auto table_meta_offset = payload.size() - 24U;
    const auto uncompressed_length = readU64Be(payload, table_meta_offset);
    const auto encoded_length = readU64Be(payload, table_meta_offset + 8U);
    const auto item_count = readU64Be(payload, table_meta_offset + 16U);
    if (encoded_length > table_meta_offset) {
        fail("value-change time table exceeds block bounds");
    }
    const auto encoded_size = static_cast<std::size_t>(encoded_length);
    const auto table_offset = table_meta_offset - encoded_size;
    std::vector<std::uint8_t> table(payload.begin() + static_cast<std::ptrdiff_t>(table_offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(table_meta_offset));
    if (encoded_length != uncompressed_length) {
        table = inflateZlib(table,
                            static_cast<std::size_t>(uncompressed_length),
                            "FST time table");
    }
    std::vector<std::uint64_t> result;
    result.reserve(static_cast<std::size_t>(item_count));
    std::size_t offset = 0;
    std::uint64_t time = 0;
    for (std::uint64_t index = 0; index < item_count; ++index) {
        time += readVarint(table, offset);
        result.push_back(time);
    }
    if (offset != table.size()) {
        fail("value-change time table has trailing bytes");
    }
    return result;
}

std::string scalarValueFromVli(std::uint64_t vli) {
    char value = '0';
    if ((vli & 1U) == 0U) {
        value = static_cast<char>(((vli >> 1U) & 1U) | static_cast<std::uint64_t>('0'));
    }
    else {
        value = kExtraValueChars[static_cast<std::size_t>((vli >> 1U) & 7U)];
    }
    return std::string(1, value);
}

std::uint32_t signalWidthByIndex(const FstData& data, std::size_t index) {
    if (index < data.signal_widths_by_handle.size()) {
        return data.signal_widths_by_handle[index];
    }
    const auto handle = static_cast<std::uint32_t>(index + 1U);
    const auto it = std::find_if(data.signals.begin(), data.signals.end(), [handle](const auto& signal) {
        return signal.handle == handle;
    });
    if (it == data.signals.end()) {
        return 1;
    }
    return it->width;
}

FstSignalGeometry signalGeometryByIndex(const FstData& data, std::size_t index) {
    if (index < data.signal_geometry_by_handle.size()) {
        return data.signal_geometry_by_handle[index];
    }
    return FstSignalGeometry{.length = signalWidthByIndex(data, index), .is_real = false};
}

bool shouldDecodeSignal(const FstReadOptions& options, std::uint32_t handle) {
    return options.decode_signal_handles.empty() ||
           std::ranges::find(options.decode_signal_handles, handle) !=
               options.decode_signal_handles.end();
}

std::string readRealValue(const std::vector<std::uint8_t>& bytes,
                          std::size_t offset,
                          bool endian_matches_host) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(double)) {
        fail("truncated FST real value");
    }
    std::array<std::uint8_t, sizeof(double)> raw{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), raw.size(), raw.begin());
    if (!endian_matches_host) {
        std::reverse(raw.begin(), raw.end());
    }
    double value = 0.0;
    std::memcpy(&value, raw.data(), sizeof(value));
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

std::string readInitialFrameValue(const std::vector<std::uint8_t>& bytes,
                                  std::size_t& offset,
                                  const FstSignalGeometry& geometry,
                                  bool endian_matches_host) {
    if (geometry.is_real) {
        auto value = readRealValue(bytes, offset, endian_matches_host);
        offset += sizeof(double);
        return value;
    }
    if (geometry.length == 0U) {
        const auto length = checkedU32(readVarint(bytes, offset, 5),
                                       "FST initial variable-length value length");
        if (offset > bytes.size() || bytes.size() - offset < length) {
            fail("truncated FST initial variable-length signal value");
        }
        std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        return value;
    }
    if (offset > bytes.size() || bytes.size() - offset < geometry.length) {
        fail("truncated FST initial frame signal value");
    }
    std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + geometry.length));
    offset += geometry.length;
    return value;
}

std::vector<std::uint8_t> decodeFrameBytes(const std::vector<std::uint8_t>& payload,
                                           std::size_t& offset) {
    const auto frame_uncompressed_length = checkedU32(readVarint(payload, offset), "FST frame uncompressed length");
    const auto frame_encoded_length = checkedU32(readVarint(payload, offset), "FST frame encoded length");
    (void)checkedU32(readVarint(payload, offset), "FST frame max handle");
    if (offset > payload.size() || payload.size() - offset < frame_encoded_length) {
        fail("truncated FST initial frame");
    }
    std::vector<std::uint8_t> frame(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(offset + frame_encoded_length));
    offset += frame_encoded_length;
    if (frame_uncompressed_length != frame_encoded_length) {
        frame = inflateZlib(frame,
                            frame_uncompressed_length,
                            "FST initial frame");
    }
    return frame;
}

std::vector<std::uint8_t> decodeChainIndexBytes(const std::vector<std::uint8_t>& payload,
                                                std::size_t table_meta_offset,
                                                std::uint64_t encoded_time_table_length) {
    if (encoded_time_table_length > table_meta_offset || table_meta_offset < encoded_time_table_length + 8U) {
        fail("FST value-chain index exceeds block bounds");
    }
    const auto chain_length_offset =
        table_meta_offset - static_cast<std::size_t>(encoded_time_table_length) - 8U;
    const auto chain_length = readU64Be(payload, chain_length_offset);
    if (chain_length > chain_length_offset) {
        fail("FST value-chain index exceeds block bounds");
    }
    const auto chain_offset = chain_length_offset - static_cast<std::size_t>(chain_length);
    return std::vector<std::uint8_t>(
        payload.begin() + static_cast<std::ptrdiff_t>(chain_offset),
        payload.begin() + static_cast<std::ptrdiff_t>(chain_length_offset));
}

enum class ValueChainLocationKind : std::uint8_t {
    None,
    Alias,
    Offset,
};

struct ValueChainLocation {
    ValueChainLocationKind kind = ValueChainLocationKind::None;
    std::uint32_t alias_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

void assignPreviousChainLength(std::vector<ValueChainLocation>& locations,
                               std::uint32_t previous_index,
                               std::uint32_t next_offset) {
    if (previous_index == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    auto& previous = locations.at(previous_index);
    if (previous.kind != ValueChainLocationKind::Offset) {
        fail("FST value-chain previous entry is not an offset");
    }
    if (next_offset < previous.offset) {
        fail("FST value-chain offset table is not monotonic");
    }
    previous.length = next_offset - previous.offset;
}

void finalizeChainLocations(std::vector<ValueChainLocation>& locations,
                            std::uint32_t previous_index,
                            std::size_t chain_end_offset,
                            std::string_view label) {
    const auto chain_end = checkedU32(chain_end_offset, "FST value-chain end offset");
    assignPreviousChainLength(locations, previous_index, chain_end);

    for (std::size_t index = 0; index < locations.size(); ++index) {
        auto& location = locations[index];
        if (location.kind != ValueChainLocationKind::Alias) {
            continue;
        }
        if (location.alias_index >= locations.size()) {
            fail(std::string(label) + " alias exceeds max handle");
        }
        const auto& target = locations[location.alias_index];
        if (target.kind != ValueChainLocationKind::Offset) {
            fail(std::string(label) + " alias does not target a value-chain offset");
        }
        location.kind = ValueChainLocationKind::Offset;
        location.offset = target.offset;
        location.length = target.length;
    }
}

std::vector<ValueChainLocation> decodePlainChainLocations(const std::vector<std::uint8_t>& chain_index,
                                                          std::uint32_t max_handle,
                                                          std::size_t chain_end_offset) {
    std::vector<ValueChainLocation> locations(max_handle);
    std::size_t offset = 0;
    std::uint32_t index = 0;
    std::uint64_t previous_value = 0;
    std::uint32_t previous_index = std::numeric_limits<std::uint32_t>::max();

    while (offset < chain_index.size()) {
        if (index >= max_handle) {
            fail("FST value-chain table exceeds max handle");
        }
        auto value = readVarint(chain_index, offset, 5);
        if (value == 0U) {
            value = readVarint(chain_index, offset, 5);
            if (value == 0U) {
                fail("FST value-chain alias index is zero");
            }
            locations[index].kind = ValueChainLocationKind::Alias;
            locations[index].alias_index =
                checkedU32(value - 1U, "FST dynamic alias index");
            ++index;
        }
        else if ((value & 1U) != 0U) {
            previous_value += value >> 1U;
            const auto chain_offset = checkedU32(previous_value, "FST value-chain offset");
            assignPreviousChainLength(locations, previous_index, chain_offset);
            locations[index].kind = ValueChainLocationKind::Offset;
            locations[index].offset = chain_offset;
            previous_index = index++;
        }
        else {
            const auto loop_count = checkedU32(value >> 1U, "FST value-chain zero run");
            if (loop_count > max_handle || index > max_handle - loop_count) {
                fail("FST value-chain zero run exceeds max handle");
            }
            index += loop_count;
        }
    }

    finalizeChainLocations(locations, previous_index, chain_end_offset, "FST value-chain");
    return locations;
}

std::vector<ValueChainLocation> decodeDynamicAlias2ChainLocations(
    const std::vector<std::uint8_t>& chain_index,
    std::uint32_t max_handle,
    std::size_t chain_end_offset) {
    std::vector<ValueChainLocation> locations(max_handle);
    std::size_t offset = 0;
    std::uint32_t index = 0;
    std::uint64_t previous_value = 0;
    std::uint32_t previous_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t previous_alias = 0;
    bool has_previous_alias = false;

    while (offset < chain_index.size()) {
        if (index >= max_handle) {
            fail("FST alias2 value-chain table exceeds max handle");
        }
        if ((chain_index[offset] & 0x01U) != 0U) {
            const auto shifted_value = readSignedVarint(chain_index, offset, 10) >> 1;
            if (shifted_value > 0) {
                previous_value += static_cast<std::uint64_t>(shifted_value);
                const auto chain_offset = checkedU32(previous_value, "FST alias2 value-chain offset");
                assignPreviousChainLength(locations, previous_index, chain_offset);
                locations[index].kind = ValueChainLocationKind::Offset;
                locations[index].offset = chain_offset;
                previous_index = index++;
            }
            else if (shifted_value < 0) {
                const auto alias_value =
                    static_cast<std::uint64_t>(-shifted_value - 1);
                previous_alias = checkedU32(alias_value, "FST alias2 value-chain alias index");
                has_previous_alias = true;
                locations[index].kind = ValueChainLocationKind::Alias;
                locations[index].alias_index = previous_alias;
                ++index;
            }
            else {
                if (!has_previous_alias) {
                    fail("FST alias2 repeats alias before defining one");
                }
                locations[index].kind = ValueChainLocationKind::Alias;
                locations[index].alias_index = previous_alias;
                ++index;
            }
        }
        else {
            const auto value = readVarint(chain_index, offset, 5);
            const auto loop_count = checkedU32(value >> 1U, "FST alias2 value-chain zero run");
            if (loop_count > max_handle || index > max_handle - loop_count) {
                fail("FST alias2 value-chain zero run exceeds max handle");
            }
            index += loop_count;
        }
    }

    finalizeChainLocations(locations, previous_index, chain_end_offset, "FST alias2 value-chain");
    return locations;
}

void decodeLibfstValueChangePayload(const std::vector<std::uint8_t>& payload,
                                    const FstBlockIndexEntry& entry,
                                    std::size_t value_block_index,
                                    const std::vector<std::uint64_t>& time_table,
                                    FstData& data,
                                    const FstReadOptions& options) {
    const auto table_meta_offset = payload.size() - 24U;
    const auto encoded_time_table_length = readU64Be(payload, table_meta_offset + 8U);

    std::size_t offset = 24;
    const auto initial_frame = decodeFrameBytes(payload, offset);
    const auto max_handle = checkedU32(readVarint(payload, offset), "FST value-chain max handle");
    if (offset >= payload.size()) {
        fail("truncated FST value-chain pack type");
    }
    const auto pack_type_offset = offset;
    const auto pack_type = static_cast<char>(payload[offset++]);
    if (pack_type != 'Z' && pack_type != '4' && pack_type != 'F') {
        fail("unsupported FST value-chain pack type");
    }

    const auto chain_index = decodeChainIndexBytes(payload, table_meta_offset, encoded_time_table_length);
    const auto chain_index_end_offset =
        table_meta_offset - static_cast<std::size_t>(encoded_time_table_length) - 8U;
    if (chain_index.size() > chain_index_end_offset) {
        fail("FST value-chain index exceeds block bounds");
    }
    const auto chain_index_begin_offset = chain_index_end_offset - chain_index.size();
    const auto chain_end_offset = chain_index_begin_offset - pack_type_offset;
    const auto chain_locations = entry.type == FstBlockType::ValueChangeDataDynamicAlias2
                                     ? decodeDynamicAlias2ChainLocations(chain_index,
                                                                         max_handle,
                                                                         chain_end_offset)
                                     : decodePlainChainLocations(chain_index,
                                                                 max_handle,
                                                                 chain_end_offset);

    std::vector<std::uint32_t> tc_head(time_table.size() == 0U ? 1U : time_table.size(), 0);
    std::vector<std::uint32_t> scatter(max_handle + 1U, 0);
    std::vector<std::size_t> heads(max_handle + 1U, 0);
    std::vector<std::size_t> remaining(max_handle + 1U, 0);
    std::vector<std::vector<std::uint8_t>> chains(max_handle + 1U);

    if (value_block_index == 0U && (time_table.empty() || time_table.front() > entry.begin_time)) {
        std::size_t initial_offset = 0;
        for (std::uint32_t index = 0; index < max_handle; ++index) {
            const auto geometry = signalGeometryByIndex(data, index);
            auto value = readInitialFrameValue(initial_frame,
                                               initial_offset,
                                               geometry,
                                               data.header.float_endian_matches_host);
            if (shouldDecodeSignal(options, index + 1U)) {
                data.transitions.push_back(FstTransition{.handle = index + 1U,
                                                         .time = entry.begin_time,
                                                         .value = std::move(value)});
            }
        }
    }

    for (std::uint32_t index = 0; index < max_handle; ++index) {
        if (!shouldDecodeSignal(options, index + 1U)) {
            continue;
        }
        const auto location = chain_locations[index];
        if (location.kind != ValueChainLocationKind::Offset || location.length == 0U) {
            continue;
        }
        const auto chain_file_begin = pack_type_offset + location.offset;
        const auto chain_file_end = chain_file_begin + location.length;
        if (chain_file_begin > payload.size() || chain_file_end > payload.size()) {
            fail("FST value-chain offset exceeds payload bounds");
        }
        auto cursor = chain_file_begin;
        if (cursor > payload.size()) {
            fail("FST value-chain offset exceeds payload bounds");
        }
        const auto decoded_length = checkedU32(readVarint(payload, cursor, 5),
                                               "FST decoded value-chain length");
        const auto chain_begin = cursor;
        if (chain_begin > chain_file_end) {
            fail("FST value-chain length exceeds payload bounds");
        }
        if (decoded_length != 0U) {
            if (pack_type == 'Z') {
                const auto zlib_end = chain_begin + location.length;
                if (zlib_end > payload.size()) {
                    fail("FST zlib value-chain length exceeds payload bounds");
                }
                chains[index].assign(payload.begin() + static_cast<std::ptrdiff_t>(chain_begin),
                                     payload.begin() + static_cast<std::ptrdiff_t>(zlib_end));
                chains[index] = inflateZlib(chains[index],
                                            decoded_length,
                                            "FST value-chain");
            }
            else if (pack_type == '4') {
                chains[index].assign(payload.begin() + static_cast<std::ptrdiff_t>(chain_begin),
                                     payload.begin() + static_cast<std::ptrdiff_t>(chain_file_end));
                chains[index] = inflateLz4(chains[index],
                                           decoded_length,
                                           "FST LZ4 value-chain");
            }
            else {
                chains[index].assign(payload.begin() + static_cast<std::ptrdiff_t>(chain_begin),
                                     payload.begin() + static_cast<std::ptrdiff_t>(chain_file_end));
                chains[index] = inflateFastLz(chains[index],
                                              decoded_length,
                                              "FST FastLZ value-chain");
            }
        }
        else {
            // libfst can store uncompressed chains even when the section pack type is LZ4/FastLZ
            // if compression would not reduce the data.
            chains[index].assign(payload.begin() + static_cast<std::ptrdiff_t>(chain_begin),
                                 payload.begin() + static_cast<std::ptrdiff_t>(chain_file_end));
            chains[index].shrink_to_fit();
        }
        heads[index] = 0;
        remaining[index] = chains[index].size();
        if (remaining[index] == 0U) {
            continue;
        }

        const auto geometry = signalGeometryByIndex(data, index);
        const auto vli = readVarintAtNoSkip(chains[index], 0);
        const auto tdelta = geometry.length == 1U ? vli >> (2U << (vli & 1U))
                                                  : vli >> 1U;
        if (tdelta >= tc_head.size()) {
            fail("FST value-chain first time delta exceeds time table");
        }
        scatter[index] = tc_head[static_cast<std::size_t>(tdelta)];
        tc_head[static_cast<std::size_t>(tdelta)] = index + 1U;
    }

    for (std::size_t time_index = 0; time_index < time_table.size(); ++time_index) {
        while (tc_head[time_index] != 0U) {
            const auto index = tc_head[time_index] - 1U;
            std::size_t skip = 0;
            const auto vli = readVarintAt(chains[index], heads[index], skip, 5);
            const auto geometry = signalGeometryByIndex(data, index);

            std::string value;
            if (geometry.length == 1U) {
                value = scalarValueFromVli(vli);
            }
            else if (geometry.length == 0U) {
                std::size_t value_offset = heads[index] + skip;
                const auto length = checkedU32(readVarint(chains[index], value_offset, 5),
                                               "FST variable-length value length");
                if (value_offset > chains[index].size() ||
                    chains[index].size() - value_offset < length) {
                    fail("truncated FST variable-length value");
                }
                value.assign(chains[index].begin() + static_cast<std::ptrdiff_t>(value_offset),
                             chains[index].begin() +
                                 static_cast<std::ptrdiff_t>(value_offset + length));
                skip = value_offset + length - heads[index];
            }
            else if (geometry.is_real) {
                if ((vli & 1U) == 0U) {
                    fail("unsupported packed FST real value");
                }
                const auto value_offset = heads[index] + skip;
                value = readRealValue(chains[index],
                                      value_offset,
                                      data.header.float_endian_matches_host);
                skip += sizeof(double);
            }
            else {
                std::size_t value_offset = heads[index] + skip;
                if ((vli & 1U) == 0U) {
                    const auto byte_count = (geometry.length + 7U) / 8U;
                    if (value_offset > chains[index].size() ||
                        chains[index].size() - value_offset < byte_count) {
                        fail("truncated packed FST vector value");
                    }
                    value.reserve(geometry.length);
                    for (std::uint32_t bit = 0; bit < geometry.length; ++bit) {
                        const auto byte = chains[index][value_offset + bit / 8U];
                        const auto mask = static_cast<std::uint8_t>(1U << (7U - (bit & 7U)));
                        value.push_back((byte & mask) != 0U ? '1' : '0');
                    }
                    skip += byte_count;
                }
                else {
                    if (value_offset > chains[index].size() ||
                        chains[index].size() - value_offset < geometry.length) {
                        fail("truncated FST vector value");
                    }
                    value.assign(chains[index].begin() + static_cast<std::ptrdiff_t>(value_offset),
                                 chains[index].begin() +
                                     static_cast<std::ptrdiff_t>(value_offset + geometry.length));
                    skip += geometry.length;
                }
            }

            data.transitions.push_back(FstTransition{.handle = index + 1U,
                                                     .time = time_table[time_index],
                                                     .value = std::move(value)});
            if (skip > remaining[index]) {
                fail("FST value-chain skip exceeds remaining data");
            }
            heads[index] += skip;
            remaining[index] -= skip;

            tc_head[time_index] = scatter[index];
            scatter[index] = 0;
            if (remaining[index] != 0U) {
                const auto next_vli = readVarintAtNoSkip(chains[index], heads[index]);
                const auto tdelta = geometry.length == 1U
                                        ? next_vli >> (2U << (next_vli & 1U))
                                        : next_vli >> 1U;
                if (time_index + tdelta >= tc_head.size()) {
                    fail("FST value-chain next time delta exceeds time table");
                }
                scatter[index] = tc_head[time_index + static_cast<std::size_t>(tdelta)];
                tc_head[time_index + static_cast<std::size_t>(tdelta)] = index + 1U;
            }
        }
    }
}

void parseSimplePristineTransitionPayload(const std::vector<std::uint8_t>& payload, FstData& data) {
    if (payload.size() < 40) {
        return;
    }
    // Private test-only extension after the libfst VCDATA block header. Real libfst blocks are
    // still indexed above; this lets unit/e2e fixtures exercise the source path without a writer.
    if (!(payload[24] == 'P' && payload[25] == 'S' && payload[26] == 'T' && payload[27] == 'V')) {
        return;
    }
    std::size_t offset = 28;
    const auto count = checkedU32(readVarint(payload, offset), "FST transition count");
    data.transitions.reserve(data.transitions.size() + count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto handle = checkedU32(readVarint(payload, offset), "FST transition handle");
        const auto time = readVarint(payload, offset);
        const auto length = checkedU32(readVarint(payload, offset), "FST transition value length");
        if (offset > payload.size() || payload.size() - offset < length) {
            fail("truncated transition value");
        }
        std::string value(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                          payload.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        data.transitions.push_back(FstTransition{.handle = handle, .time = time, .value = value});
    }
}

void parseValueBlockIndexPayload(const std::vector<std::uint8_t>& payload,
                                 FstBlockIndexEntry& entry) {
    if (payload.size() < 24) {
        fail("truncated value-change block");
    }
    entry.begin_time = readU64Be(payload, 0);
    entry.end_time = readU64Be(payload, 8);
    entry.memory_required_for_traversal = readU64Be(payload, 16);
    entry.compression = FstCompressionKind::Unknown;
    if (entry.end_time < entry.begin_time) {
        fail("value-change block has reversed time range");
    }
}

bool valueBlockIntersectsDecodeWindow(const FstBlockIndexEntry& entry,
                                      const FstReadOptions& options) {
    if (!options.decode_transitions) {
        return false;
    }
    if (options.decode_end_time.has_value() &&
        entry.begin_time > *options.decode_end_time) {
        return false;
    }
    if (options.decode_start_time.has_value() &&
        entry.end_time < *options.decode_start_time) {
        return false;
    }
    return true;
}

void decodeValueBlockPayload(const std::vector<std::uint8_t>& payload,
                             const FstBlockIndexEntry& entry,
                             FstData& data,
                             const FstReadOptions& options,
                             std::size_t relevant_value_block_index) {
    if (!valueBlockIntersectsDecodeWindow(entry, options)) {
        return;
    }
    // Decode the libfst time table and value chains. DEFLATE geometry/time/frame data and
    // DEFLATE/LZ4/FastLZ value chains are supported; LZ4 hierarchy blocks are handled
    // separately from this value-change path.
    const auto time_table = decodeTimeTable(payload);
    if (payload.size() >= 28 &&
        !(payload[24] == 'P' &&
          payload[25] == 'S' &&
          payload[26] == 'T' &&
          payload[27] == 'V')) {
        decodeLibfstValueChangePayload(payload,
                                       entry,
                                       relevant_value_block_index,
                                       time_table,
                                       data,
                                       options);
        return;
    }
    parseSimplePristineTransitionPayload(payload, data);
}

void parseLz4HierarchyPayload(const std::vector<std::uint8_t>& payload,
                              FstData& data,
                              bool duo);

void parseGzipHierarchyPayload(const std::vector<std::uint8_t>& payload,
                               FstData& data);

void parseBlock(const FstFileBytes& file,
                FstBlockIndexEntry& entry,
                FstData& data,
                FstReadMetrics* metrics) {
    const auto payload = readRange(file, entry.payload_offset, entry.payload_size);
    switch (entry.type) {
        case FstBlockType::Hierarchy: {
            ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->hierarchy_parse_micros);
            parseGzipHierarchyPayload(payload, data);
            break;
        }
        case FstBlockType::Geometry: {
            ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->geometry_parse_micros);
            parseGeometryPayload(payload, data);
            break;
        }
        case FstBlockType::ValueChangeData:
        case FstBlockType::ValueChangeDataDynamicAlias:
        case FstBlockType::ValueChangeDataDynamicAlias2: {
            ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->value_block_index_micros);
            parseValueBlockIndexPayload(payload, entry);
            data.value_blocks.push_back(entry);
            break;
        }
        case FstBlockType::HierarchyLz4: {
            ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->hierarchy_parse_micros);
            parseLz4HierarchyPayload(payload, data, false);
            break;
        }
        case FstBlockType::HierarchyLz4Duo: {
            ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->hierarchy_parse_micros);
            parseLz4HierarchyPayload(payload, data, true);
            break;
        }
        default:
            break;
    }
}

void scanBlocks(const FstFileBytes& file, FstData& data, FstReadMetrics* metrics) {
    ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->block_scan_micros);
    const auto file_size = static_cast<std::uint64_t>(file.bytes.size());
    if (file_size < kFstHeaderLength) {
        fail("file is shorter than FST header");
    }

    std::uint64_t offset = kFstHeaderLength;
    while (offset < file_size) {
        const auto type = static_cast<FstBlockType>(file.bytes[static_cast<std::size_t>(offset)]);
        if (type == FstBlockType::Skip) {
            break;
        }
        const auto section_length = readU64Be(file.bytes, static_cast<std::size_t>(offset + 1U));
        if (section_length < 8) {
            fail("invalid block length");
        }
        if (section_length > file_size - offset - 1U) {
            fail("truncated block");
        }

        FstBlockIndexEntry entry{.type = type,
                                 .file_offset = offset,
                                 .section_length = section_length,
                                 .payload_offset = offset + 9U,
                                 .payload_size = section_length - 8U};
        parseBlock(file, entry, data, metrics);
        offset += 1U + section_length;
    }
}

void decodeValueBlocks(const FstFileBytes& file,
                       FstData& data,
                       const FstReadOptions& options,
                       FstReadMetrics* metrics) {
    if (!options.decode_transitions) {
        return;
    }
    ScopedMetric metric(metrics == nullptr ? nullptr : &metrics->value_decode_micros);
    std::size_t relevant_value_block_index = 0;
    for (const auto& entry : data.value_blocks) {
        if (!valueBlockIntersectsDecodeWindow(entry, options)) {
            continue;
        }
        const auto payload = readRange(file, entry.payload_offset, entry.payload_size);
        decodeValueBlockPayload(payload,
                                entry,
                                data,
                                options,
                                relevant_value_block_index++);
    }
}

std::optional<fs::path> sidecarPath(const fs::path& fst_path, const FstReadOptions& options) {
    auto candidate = fst_path;
    candidate += ".hier";
    std::error_code error;
    if (!fs::exists(candidate, error)) {
        return std::nullopt;
    }
    validateWorkspacePath(candidate, options, "FST hierarchy sidecar");
    const auto canonical_candidate = fs::weakly_canonical(candidate, error);
    if (error) {
        throw std::runtime_error("Unable to resolve FST hierarchy sidecar: " + candidate.string());
    }
    return canonical_candidate;
}

void parseSidecarHierarchy(const fs::path& sidecar, FstData& data) {
    std::ifstream input(sidecar, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open FST hierarchy sidecar: " + sidecar.string());
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
    if (!bytes.empty()) {
        parseHierarchyPayload(bytes, data);
    }
}

void parseGzipHierarchyPayload(const std::vector<std::uint8_t>& payload,
                               FstData& data) {
    if (payload.empty()) {
        return;
    }
    if (payload.size() < 10 || payload[8] != 0x1fU || payload[9] != 0x8bU) {
        parseHierarchyPayload(payload, data);
        return;
    }
    const auto uncompressed_length = readU64Be(payload, 0);
    std::vector<std::uint8_t> encoded(payload.begin() + 8, payload.end());
    auto hierarchy = inflateGzip(encoded,
                                 static_cast<std::size_t>(uncompressed_length),
                                 "FST gzip hierarchy");
    parseHierarchyPayload(hierarchy, data);
}

void parseLz4HierarchyPayload(const std::vector<std::uint8_t>& payload,
                              FstData& data,
                              bool duo) {
    if (payload.size() < 8) {
        fail("truncated compressed FST hierarchy");
    }
    const auto uncompressed_length = readU64Be(payload, 0);
    std::vector<std::uint8_t> encoded(payload.begin() + 8, payload.end());
    if (duo) {
        std::size_t offset = 0;
        const auto intermediate_length = checkedU32(readVarint(encoded, offset),
                                                    "FST LZ4 hierarchy intermediate length");
        std::vector<std::uint8_t> second_pass(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                                              encoded.end());
        encoded = inflateLz4(second_pass,
                             intermediate_length,
                             "FST LZ4 duo hierarchy first pass");
    }
    auto hierarchy = inflateLz4(encoded,
                                static_cast<std::size_t>(uncompressed_length),
                                "FST LZ4 hierarchy");
    parseHierarchyPayload(hierarchy, data);
}

} // namespace

FstData readFstFile(const std::filesystem::path& path, const FstReadOptions& options) {
    auto metrics = FstReadMetrics{};
    auto* metrics_ptr = options.collect_metrics ? &metrics : nullptr;
    const auto total_start = options.collect_metrics ? Clock::now() : Clock::time_point{};
    validateWorkspacePath(path, options, "FST file");

    std::error_code error;
    const auto canonical_path = fs::weakly_canonical(path, error);
    if (error) {
        throw std::runtime_error("Unable to resolve FST file: " + path.string());
    }

    const auto file_read_start = options.collect_metrics ? Clock::now() : Clock::time_point{};
    auto file = readFstBytes(canonical_path);
    if (metrics_ptr != nullptr) {
        metrics_ptr->file_read_micros = elapsedMicros(file_read_start, Clock::now());
        metrics_ptr->file_bytes = static_cast<std::uint64_t>(file.bytes.size());
    }
    if (file.bytes.size() < kFstHeaderLength) {
        fail("truncated header");
    }
    std::vector<std::uint8_t> header(file.bytes.begin(),
                                    file.bytes.begin() +
                                        static_cast<std::ptrdiff_t>(kFstHeaderLength));

    FstData data;
    data.file_path = canonical_path;
    const auto header_parse_start = options.collect_metrics ? Clock::now() : Clock::time_point{};
    data.header = parseHeader(header);
    if (metrics_ptr != nullptr) {
        metrics_ptr->header_parse_micros = elapsedMicros(header_parse_start, Clock::now());
    }

    const auto sidecar = sidecarPath(canonical_path, options);
    if (sidecar.has_value()) {
        data.hierarchy_sidecar_path = sidecar;
        const auto sidecar_parse_start = options.collect_metrics ? Clock::now() : Clock::time_point{};
        parseSidecarHierarchy(*sidecar, data);
        if (metrics_ptr != nullptr) {
            metrics_ptr->sidecar_hierarchy_parse_micros =
                elapsedMicros(sidecar_parse_start, Clock::now());
        }
    }
    scanBlocks(file, data, metrics_ptr);
    if (data.header.start_time == 0U && data.header.end_time == 0U &&
        !data.value_blocks.empty()) {
        data.header.start_time = data.value_blocks.front().begin_time;
        data.header.end_time = data.value_blocks.front().end_time;
        for (const auto& block : data.value_blocks) {
            data.header.start_time = std::min(data.header.start_time, block.begin_time);
            data.header.end_time = std::max(data.header.end_time, block.end_time);
        }
    }

    if (data.signals.empty() && data.header.variable_count != 0) {
        fail("FST hierarchy did not define any signals");
    }
    decodeValueBlocks(file, data, options, metrics_ptr);
    if (metrics_ptr != nullptr) {
        metrics_ptr->total_micros = elapsedMicros(total_start, Clock::now());
        data.metrics = metrics;
    }
    return data;
}

} // namespace pristine::waveform::fst
