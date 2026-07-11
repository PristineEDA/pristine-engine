#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pristine::analysis::semantic {

class DebugTraceScope {
public:
    DebugTraceScope(std::string phase, std::string detail = {});
    ~DebugTraceScope();

    DebugTraceScope(const DebugTraceScope&) = delete;
    DebugTraceScope& operator=(const DebugTraceScope&) = delete;
    DebugTraceScope(DebugTraceScope&&) = delete;
    DebugTraceScope& operator=(DebugTraceScope&&) = delete;

private:
    std::string phase_;
    std::string detail_;
    std::uint64_t start_micros_ = 0;
    bool enabled_ = false;
};

[[nodiscard]] bool debugTraceEnabled();
void debugTraceInstant(std::string_view phase, std::string_view detail = {});

} // namespace pristine::analysis::semantic

#ifndef NDEBUG
#define PRISTINE_DEBUG_TRACE_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define PRISTINE_DEBUG_TRACE_CONCAT(lhs, rhs) PRISTINE_DEBUG_TRACE_CONCAT_IMPL(lhs, rhs)
#define PRISTINE_DEBUG_TRACE_SCOPE(phase, detail)                                      \
    ::pristine::analysis::semantic::DebugTraceScope PRISTINE_DEBUG_TRACE_CONCAT(       \
        pristine_debug_trace_scope_, __LINE__)(phase, detail)
#define PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE(phase)                                       \
    ::pristine::analysis::semantic::DebugTraceScope PRISTINE_DEBUG_TRACE_CONCAT(       \
        pristine_debug_trace_scope_, __LINE__)(phase)
#define PRISTINE_DEBUG_TRACE_SCOPE_LAZY(phase, detail_factory)                         \
    ::pristine::analysis::semantic::DebugTraceScope PRISTINE_DEBUG_TRACE_CONCAT(       \
        pristine_debug_trace_scope_, __LINE__)(phase, (detail_factory)())
#else
#define PRISTINE_DEBUG_TRACE_SCOPE(phase, detail) ((void)0)
#define PRISTINE_DEBUG_TRACE_SCOPE_SIMPLE(phase) ((void)0)
#define PRISTINE_DEBUG_TRACE_SCOPE_LAZY(phase, detail_factory)                         \
    ((void)sizeof(phase), (void)sizeof(detail_factory))
#endif
