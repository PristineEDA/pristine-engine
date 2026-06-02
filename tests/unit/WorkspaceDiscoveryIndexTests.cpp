#include "pristine/analysis/SemanticEngine.h"
#include "../../src/analysis/semantic/WorkspaceDiscoveryIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using namespace pristine::analysis;

namespace {

bool hasDeclaration(const SemanticWorkspaceDiscoverySnapshot& discovery,
                    std::string_view name,
                    std::string_view kind) {
    return std::any_of(discovery.declarations.begin(),
                       discovery.declarations.end(),
                       [&](const SemanticDiscoverySymbol& symbol) {
                           return symbol.name == name && symbol.kind == kind;
                       });
}

} // namespace

TEST_CASE("WorkspaceDiscoveryIndex discovers top-level design candidates deterministically",
          "[analysis][semantic][discovery]") {
    auto index = semantic::buildWorkspaceDiscoveryIndex(
        7,
        {semantic::DiscoveryDocumentInput{.uri = "file:///workspace/b.sv",
                                          .text = R"(
package defs;
endpackage

module child(input logic clk);
endmodule
)"},
         semantic::DiscoveryDocumentInput{.uri = "file:///workspace/a.sv",
                                          .text = R"(
`define MAKE_CHILD child

interface bus_if;
endinterface

module top(input logic clk);
  child u_child(.clk(clk));
  import defs::*;
endmodule
)"}});

    CHECK(index.generation == 7);
    CHECK(index.file_count == 2);
    CHECK(index.declaration_count >= 4);
    CHECK(index.macro_count == 1);
    REQUIRE(index.files.size() == 2);
    CHECK(index.files[0].uri == "file:///workspace/a.sv");
    CHECK(index.files[1].uri == "file:///workspace/b.sv");
    REQUIRE(index.declarations_by_name.contains("top"));
    REQUIRE(index.files_by_declaration.contains("child"));
    CHECK(index.files_by_declaration.at("child") == std::vector<std::string>{"file:///workspace/b.sv"});
    REQUIRE(index.referenced_files_by_name.contains("child"));
    CHECK(index.referenced_files_by_name.at("child") == std::vector<std::string>{"file:///workspace/a.sv"});

    const auto closure = semantic::discoveryDependencyClosure(index, std::string_view("top"));
    CHECK(closure == std::vector<std::string>{"file:///workspace/a.sv", "file:///workspace/b.sv"});
}

TEST_CASE("SemanticEngine maintains a lightweight workspace discovery snapshot",
          "[analysis][semantic-engine][discovery]") {
    SemanticEngine engine;
    engine.updateDocument("file:///workspace/top.sv",
                          R"(
module child;
endmodule

module top;
  child u_child();
endmodule
)",
                          SemanticEngineDocumentState{.version = 1, .is_open = false, .dirty = false});

    auto discovery = engine.workspaceDiscovery();
    CHECK(discovery.generation == engine.generation());
    CHECK(discovery.file_count == 1);
    CHECK(hasDeclaration(discovery, "top", "module"));
    CHECK(hasDeclaration(discovery, "child", "module"));
    CHECK(discovery.reference_count >= 1);

    engine.updateDocument("file:///workspace/pkg.sv",
                          R"(
package defs;
  typedef logic value_t;
endpackage
)",
                          SemanticEngineDocumentState{.version = 1, .is_open = false, .dirty = false});
    auto updated = engine.workspaceDiscovery();
    CHECK(updated.generation == engine.generation());
    CHECK(updated.file_count == 2);
    CHECK(hasDeclaration(updated, "defs", "package"));

    engine.removeDocument("file:///workspace/top.sv");
    auto removed = engine.workspaceDiscovery();
    CHECK(removed.generation == engine.generation());
    CHECK(removed.file_count == 1);
    CHECK_FALSE(hasDeclaration(removed, "top", "module"));
}
