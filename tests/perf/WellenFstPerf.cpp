#include "pristine/waveform/fst/FstReader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fst = pristine::waveform::fst;

constexpr std::string_view kInvalidFixture = "sigrok/libsigrok.vcd.fst";
const auto kStatusStart = std::chrono::steady_clock::now();

double elapsedSeconds() {
    const auto elapsed = std::chrono::steady_clock::now() - kStatusStart;
    return std::chrono::duration<double>(elapsed).count();
}

void emitStatus(std::string_view phase, std::string_view detail = {}) {
    std::cerr << "[pristine-test] test=pristine_wellen_fst_perf phase=" << phase
              << " elapsed=" << elapsedSeconds() << "s";
    if (!detail.empty()) {
        std::cerr << " detail=" << detail;
    }
    std::cerr << '\n';
}

std::string jsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    return result;
}

std::vector<fs::path> collectFixtures(const fs::path& inputs_dir) {
    if (!fs::is_directory(inputs_dir)) {
        throw std::runtime_error("missing wellen FST inputs directory: " + inputs_dir.string());
    }
    std::vector<fs::path> fixtures;
    for (const auto& entry : fs::recursive_directory_iterator(inputs_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fst") {
            fixtures.push_back(entry.path());
        }
    }
    std::ranges::sort(fixtures);
    if (fixtures.size() != 61U) {
        std::ostringstream message;
        message << "expected exactly 61 wellen FST fixtures, found " << fixtures.size();
        throw std::runtime_error(message.str());
    }
    return fixtures;
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double pct) {
    if (values.empty()) {
        return 0;
    }
    std::ranges::sort(values);
    const auto index =
        std::min<std::size_t>(values.size() - 1U,
                              static_cast<std::size_t>((values.size() - 1U) * pct));
    return values[index];
}

struct FixturePerf {
    std::string relative_path;
    std::string status;
    std::uint64_t file_bytes = 0;
    std::size_t signal_count = 0;
    std::size_t value_block_count = 0;
    std::size_t transition_count = 0;
    std::uint64_t index_micros = 0;
    std::uint64_t full_decode_micros = 0;
    std::uint64_t value_decode_micros = 0;
    std::uint64_t header_parse_micros = 0;
    std::uint64_t hierarchy_parse_micros = 0;
    std::uint64_t geometry_parse_micros = 0;
    std::uint64_t value_block_index_micros = 0;
    std::uint64_t block_scan_micros = 0;
    std::string error;
};

void writeFixtureJson(std::ostream& output, const FixturePerf& perf) {
    output << "{\"kind\":\"fixture\""
           << ",\"relativePath\":\"" << jsonEscape(perf.relative_path) << "\""
           << ",\"status\":\"" << jsonEscape(perf.status) << "\""
           << ",\"fileBytes\":" << perf.file_bytes
           << ",\"signalCount\":" << perf.signal_count
           << ",\"valueBlockCount\":" << perf.value_block_count
           << ",\"transitionCount\":" << perf.transition_count
           << ",\"indexMicros\":" << perf.index_micros
           << ",\"fullDecodeMicros\":" << perf.full_decode_micros
           << ",\"valueDecodeMicros\":" << perf.value_decode_micros
           << ",\"headerParseMicros\":" << perf.header_parse_micros
           << ",\"hierarchyParseMicros\":" << perf.hierarchy_parse_micros
           << ",\"geometryParseMicros\":" << perf.geometry_parse_micros
           << ",\"valueBlockIndexMicros\":" << perf.value_block_index_micros
           << ",\"blockScanMicros\":" << perf.block_scan_micros;
    if (!perf.error.empty()) {
        output << ",\"error\":\"" << jsonEscape(perf.error) << "\"";
    }
    output << "}\n";
}

void writeSummaryJson(std::ostream& output, const std::vector<FixturePerf>& results) {
    auto valid_count = std::size_t{0};
    auto invalid_count = std::size_t{0};
    std::vector<std::uint64_t> index_times;
    std::vector<std::uint64_t> full_times;
    for (const auto& result : results) {
        if (result.status == "valid") {
            ++valid_count;
            index_times.push_back(result.index_micros);
            full_times.push_back(result.full_decode_micros);
        }
        else {
            ++invalid_count;
        }
    }

    auto slow = results;
    std::ranges::sort(slow, [](const auto& lhs, const auto& rhs) {
        return lhs.full_decode_micros > rhs.full_decode_micros;
    });

    output << "{\"kind\":\"summary\""
           << ",\"valid\":" << valid_count
           << ",\"invalid\":" << invalid_count
           << ",\"indexP50Micros\":" << percentile(index_times, 0.50)
           << ",\"indexP95Micros\":" << percentile(index_times, 0.95)
           << ",\"indexMaxMicros\":" << (index_times.empty() ? 0 : *std::ranges::max_element(index_times))
           << ",\"fullDecodeP50Micros\":" << percentile(full_times, 0.50)
           << ",\"fullDecodeP95Micros\":" << percentile(full_times, 0.95)
           << ",\"fullDecodeMaxMicros\":" << (full_times.empty() ? 0 : *std::ranges::max_element(full_times))
           << ",\"topSlowFixtures\":[";
    auto emitted = std::size_t{0};
    for (const auto& result : slow) {
        if (result.status != "valid") {
            continue;
        }
        if (emitted != 0U) {
            output << ",";
        }
        output << "{\"relativePath\":\"" << jsonEscape(result.relative_path) << "\""
               << ",\"fullDecodeMicros\":" << result.full_decode_micros
               << ",\"indexMicros\":" << result.index_micros
               << ",\"valueDecodeMicros\":" << result.value_decode_micros
               << ",\"transitionCount\":" << result.transition_count << "}";
        ++emitted;
        if (emitted == 10U) {
            break;
        }
    }
    output << "]}\n";
}

FixturePerf measureFixture(const fs::path& inputs_dir, const fs::path& fixture) {
    auto result = FixturePerf{};
    result.relative_path = fixture.lexically_relative(inputs_dir).generic_string();
    try {
        const auto index_data =
            fst::readFstFile(fixture,
                             fst::FstReadOptions{.workspace_root = inputs_dir,
                                                 .decode_transitions = false,
                                                 .collect_metrics = true});
        if (!index_data.metrics.has_value()) {
            throw std::runtime_error("missing index metrics");
        }

        if (result.relative_path == kInvalidFixture) {
            result.status = "unexpected-valid";
            result.file_bytes = index_data.metrics->file_bytes;
            result.signal_count = index_data.signals.size();
            result.value_block_count = index_data.value_blocks.size();
            result.index_micros = index_data.metrics->total_micros;
            return result;
        }

        const auto full_data =
            fst::readFstFile(fixture,
                             fst::FstReadOptions{.workspace_root = inputs_dir,
                                                 .decode_transitions = true,
                                                 .collect_metrics = true});
        if (!full_data.metrics.has_value()) {
            throw std::runtime_error("missing full decode metrics");
        }
        result.status = "valid";
        result.file_bytes = full_data.metrics->file_bytes;
        result.signal_count = full_data.signals.size();
        result.value_block_count = full_data.value_blocks.size();
        result.transition_count = full_data.transitions.size();
        result.index_micros = index_data.metrics->total_micros;
        result.full_decode_micros = full_data.metrics->total_micros;
        result.value_decode_micros = full_data.metrics->value_decode_micros;
        result.header_parse_micros = full_data.metrics->header_parse_micros;
        result.hierarchy_parse_micros = full_data.metrics->hierarchy_parse_micros;
        result.geometry_parse_micros = full_data.metrics->geometry_parse_micros;
        result.value_block_index_micros = full_data.metrics->value_block_index_micros;
        result.block_scan_micros = full_data.metrics->block_scan_micros;
    }
    catch (const std::exception& error) {
        result.error = error.what();
        if (result.relative_path == kInvalidFixture &&
            result.error.find("FST hierarchy did not define any signals") != std::string::npos) {
            result.status = "expected-invalid";
        }
        else {
            result.status = "error";
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto inputs_dir = argc > 1 ? fs::path{argv[1]}
                                         : fs::path{".deps/src/wellen/wellen/inputs"};
        auto output_path = std::optional<fs::path>{};
        if (const auto* env_path = std::getenv("PRISTINE_WELLEN_FST_PERF_LOG");
            env_path != nullptr && *env_path != '\0') {
            output_path = fs::path{env_path};
        }

        std::ofstream file_output;
        std::ostream* output = &std::cout;
        if (output_path.has_value()) {
            file_output.open(*output_path, std::ios::trunc);
            if (!file_output) {
                throw std::runtime_error("unable to open perf log: " + output_path->string());
            }
            output = &file_output;
        }

        const auto fixtures = collectFixtures(inputs_dir);
        emitStatus("begin", "fixtures=" + std::to_string(fixtures.size()));
        std::vector<FixturePerf> results;
        results.reserve(fixtures.size());
        for (size_t index = 0; index < fixtures.size(); ++index) {
            const auto& fixture = fixtures[index];
            emitStatus("fixture",
                       std::to_string(index + 1) + "/" + std::to_string(fixtures.size()) + " " +
                           fixture.lexically_relative(inputs_dir).generic_string());
            auto result = measureFixture(inputs_dir, fixture);
            writeFixtureJson(*output, result);
            results.push_back(std::move(result));
        }
        writeSummaryJson(*output, results);

        const auto bad = std::ranges::find_if(results, [](const auto& result) {
            return result.status == "error" || result.status == "unexpected-valid";
        });
        emitStatus("summary", bad == results.end() ? "status=passed" : "status=failed");
        return bad == results.end() ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
