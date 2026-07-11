#define NDEBUG
#include "../../src/analysis/semantic/DebugTrace.h"
#undef NDEBUG

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace pristine::analysis::semantic {

TEST_CASE("Release debug trace detail factories are compiled but not evaluated",
          "[analysis][debug-trace][release]") {
    int evaluations = 0;
    PRISTINE_DEBUG_TRACE_SCOPE_LAZY("release.compile", [&] {
        ++evaluations;
        return std::string("detail");
    });
    CHECK(evaluations == 0);
}

} // namespace pristine::analysis::semantic
