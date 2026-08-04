#include "pristine/analysis/SourceUtil.h"

#include <catch2/catch_test_macros.hpp>

namespace pristine::analysis {

TEST_CASE("SourceUtil normalizes and joins file URIs", "[analysis][source-util]") {
    CHECK(normalizeFileUri("file:///workspace/rtl/../top.sv") == "file:///workspace/top.sv");
    CHECK(withoutTrailingSlash("file:///workspace/") == "file:///workspace");
    CHECK(uriDirectory("file:///workspace/rtl/top.sv") == "file:///workspace/rtl");
    CHECK(joinFileUri("file:///workspace/rtl", "../include/defs.svh") ==
          "file:///workspace/include/defs.svh");
}

TEST_CASE("SourceUtil converts file URI paths portably", "[analysis][source-util]") {
    const auto virtual_path = fileUriToPath("file:///workspace/top.sv");
    CHECK(virtual_path.find("workspace") != std::string::npos);
    CHECK(pathToFileUri(virtual_path) == "file:///workspace/top.sv");
    CHECK(pathToFileUri("/workspace/top.sv") == "file:///workspace/top.sv");
}

} // namespace pristine::analysis
