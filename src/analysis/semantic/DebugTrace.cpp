#include "DebugTrace.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#if defined(_WIN32) && !defined(NDEBUG)
#include <cstdlib>
#include <crtdbg.h>
#endif

namespace pristine::analysis::semantic {
namespace {

using Clock = std::chrono::steady_clock;

struct DebugTraceState {
    bool enabled = false;
    std::string path;
};

std::string envValue(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value, value_size == 0 ? 0 : value_size - 1);
    std::free(value);
    return result;
#else
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return {};
#endif
}

bool truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" ||
           value == "YES" || value == "on" || value == "ON";
}

DebugTraceState readTraceState() {
    DebugTraceState state;
    state.enabled = truthy(envValue("PRISTINE_DEBUG_TRACE"));
    state.path = envValue("PRISTINE_DEBUG_TRACE_FILE");

#if defined(_WIN32) && !defined(NDEBUG)
    if (truthy(envValue("PRISTINE_DEBUG_SUPPRESS_ABORT_DIALOG"))) {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif

    return state;
}

const DebugTraceState& traceState() {
    static const DebugTraceState state = readTraceState();
    return state;
}

std::mutex& traceMutex() {
    static std::mutex mutex;
    return mutex;
}

std::ofstream& traceFile() {
    static std::ofstream stream;
    static bool opened = false;
    if (!opened) {
        opened = true;
        const auto& state = traceState();
        if (!state.path.empty()) {
            stream.open(state.path, std::ios::binary | std::ios::app);
        }
    }
    return stream;
}

std::uint64_t nowMicros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

std::string jsonString(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20U) {
                    out << "\\u";
                    constexpr char hex[] = "0123456789abcdef";
                    out << '0' << '0' << hex[(ch >> 4U) & 0x0fU] << hex[ch & 0x0fU];
                }
                else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

void writeEvent(std::string_view event,
                std::string_view phase,
                std::string_view detail,
                std::uint64_t elapsed_micros = 0) {
    if (!debugTraceEnabled()) {
        return;
    }

    std::ostringstream line;
    line << "{\"timeMicros\":" << nowMicros()
         << ",\"event\":" << jsonString(event)
         << ",\"phase\":" << jsonString(phase);
    if (!detail.empty()) {
        line << ",\"detail\":" << jsonString(detail);
    }
    if (elapsed_micros != 0) {
        line << ",\"elapsedMicros\":" << elapsed_micros;
    }
    line << "}\n";

    std::lock_guard lock(traceMutex());
    auto& file = traceFile();
    if (file.is_open()) {
        file << line.str();
        file.flush();
        return;
    }
    std::cerr << line.str();
}

} // namespace

bool debugTraceEnabled() {
    return traceState().enabled;
}

void debugTraceInstant(std::string_view phase, std::string_view detail) {
    writeEvent("instant", phase, detail);
}

DebugTraceScope::DebugTraceScope(std::string phase, std::string detail)
    : phase_(std::move(phase))
    , detail_(std::move(detail))
    , start_micros_(nowMicros())
    , enabled_(debugTraceEnabled()) {
    if (enabled_) {
        writeEvent("begin", phase_, detail_);
    }
}

DebugTraceScope::~DebugTraceScope() {
    if (!enabled_) {
        return;
    }
    const auto end_micros = nowMicros();
    writeEvent("end",
               phase_,
               detail_,
               end_micros > start_micros_ ? end_micros - start_micros_ : 0);
}

} // namespace pristine::analysis::semantic
