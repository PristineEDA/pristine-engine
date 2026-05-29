#include "../../src/analysis/semantic/CompletionProvider.h"

#include <catch2/catch_test_macros.hpp>

namespace pristine::analysis::semantic {
namespace {

TEST_CASE("CompletionProvider detects package, member, macro, and module contexts",
          "[analysis][semantic][completion-provider]") {
    {
        const auto context = detectCompletionContext("module top;\n  defs::thi\nendmodule\n",
                                                     1,
                                                     11,
                                                     "thi");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.package_qualifier == "defs");
        CHECK_FALSE(context.member_access);
        CHECK_FALSE(context.macro_invocation);
    }

    {
        const auto context = detectCompletionContext("module top;\n  u_child.da\nendmodule\n",
                                                     1,
                                                     12,
                                                     "da");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.member_access);
        CHECK(context.member_qualifier == "u_child");
    }

    {
        const auto context = detectCompletionContext("module top;\n  `AD\nendmodule\n",
                                                     1,
                                                     5,
                                                     "AD");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.macro_invocation);
    }

    {
        const auto context = detectCompletionContext("module top;\n  chi\nendmodule\n",
                                                     1,
                                                     5,
                                                     "chi");
        REQUIRE(context.prefix_start.has_value());
        CHECK(context.module_instantiation_position);
    }
}

TEST_CASE("CompletionProvider maps prefix start with UTF-16 positions",
          "[analysis][semantic][completion-provider][utf16]") {
    const std::string text = "module top;\n  logic smile_😀;\n  smile_😀\nendmodule\n";

    const auto start = completionPrefixStartOffset(text, 2, 8, "smile_");

    REQUIRE(start.has_value());
    CHECK(text.substr(*start, 6) == "smile_");
}

} // namespace
} // namespace pristine::analysis::semantic
