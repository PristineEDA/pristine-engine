#include "../../src/analysis/semantic/DesignGraphProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

DesignGraphContext simpleDesignContext() {
    ModuleDefinition child{.name = "child",
                           .kind = "module",
                           .range = rangeAt(0, 0, 42),
                           .selection_range = rangeAt(0, 7, 12),
                           .ports = {},
                           .port_details = {SchematicPort{.name = "clk",
                                                          .direction = "input",
                                                          .width_text = "logic",
                                                          .range = rangeAt(0, 13, 28),
                                                          .selection_range = rangeAt(0, 25, 28)},
                                            SchematicPort{.name = "out",
                                                          .direction = "output",
                                                          .width_text = "logic",
                                                          .range = rangeAt(0, 30, 46),
                                                          .selection_range = rangeAt(0, 43, 46)}},
                           .instances = {}};
    ModuleDefinition top{.name = "top",
                         .kind = "module",
                         .range = rangeAt(2, 0, 64),
                         .selection_range = rangeAt(2, 7, 10),
                         .ports = {},
                         .port_details = {},
                         .instances = {ModuleInstantiation{.module_name = "child",
                                                           .instance_name = "u_child",
                                                           .range = rangeAt(3, 2, 35),
                                                           .selection_range = rangeAt(3, 8, 15),
                                                           .module_selection_range = rangeAt(3, 2, 7)}}};
    const ModuleSchematic child_schematic{.name = "child",
                                          .range = child.range,
                                          .selection_range = child.selection_range,
                                          .ports = child.port_details,
                                          .cells = {}};
    const ModuleSchematic top_schematic{.name = "top",
                                        .range = top.range,
                                        .selection_range = top.selection_range,
                                        .ports = {},
                                        .cells = {SchematicCell{.id = "u_child",
                                                               .name = "u_child",
                                                               .type = "child",
                                                               .kind = "module",
                                                               .range = rangeAt(3, 2, 35),
                                                               .selection_range = rangeAt(3, 8, 15),
                                                               .connections = {SchematicConnection{.port_name = "clk",
                                                                                                    .signal = "clk",
                                                                                                    .range = rangeAt(3, 17, 26)},
                                                                               SchematicConnection{.port_name = "out",
                                                                                                    .signal = "ready",
                                                                                                    .range = rangeAt(3, 28, 34)}}}}};

    return DesignGraphContext{.generation = 9,
                              .snapshot_available = true,
                              .top_modules = {"top"},
                              .modules_by_name = {{"child", child}, {"top", top}},
                              .module_uris_by_name = {{"child", "file:///workspace/child.sv"},
                                                      {"top", "file:///workspace/top.sv"}},
                              .schematics_by_name = {{"child", child_schematic}, {"top", top_schematic}},
                              .schematic_uris_by_name = {{"child", "file:///workspace/child.sv"},
                                                         {"top", "file:///workspace/top.sv"}},
                              .module_entries = {DesignGraphModuleEntry{.uri = "file:///workspace/child.sv",
                                                                         .definition = child},
                                                 DesignGraphModuleEntry{.uri = "file:///workspace/top.sv",
                                                                        .definition = top}}};
}

TEST_CASE("DesignGraphProvider builds module hierarchy and schematic from design context",
          "[analysis][semantic][design-graph-provider]") {
    const auto context = simpleDesignContext();

    const auto hierarchy = moduleHierarchy(context, std::nullopt, 8);
    REQUIRE_FALSE(hierarchy.unresolved);
    REQUIRE(hierarchy.roots.size() == 1);
    CHECK(hierarchy.roots.front().module_name == "top");
    REQUIRE(hierarchy.roots.front().children.size() == 1);
    CHECK(hierarchy.roots.front().children.front().module_name == "child");
    CHECK(hierarchy.roots.front().children.front().instance_name == "u_child");

    const auto graph = schematic(context, std::string_view("top"), 8);
    REQUIRE_FALSE(graph.unresolved);
    REQUIRE(graph.root_module_id.has_value());
    CHECK(*graph.root_module_id == "top");
    const auto top_view = std::find_if(graph.modules.begin(),
                                       graph.modules.end(),
                                       [](const SemanticSchematicModuleView& view) {
                                           return view.module.name == "top";
                                       });
    REQUIRE(top_view != graph.modules.end());
    const auto ready_net = std::find_if(top_view->nets.begin(),
                                        top_view->nets.end(),
                                        [](const SemanticSchematicNet& net) {
                                            return net.name == "ready";
                                        });
    REQUIRE(ready_net != top_view->nets.end());
    REQUIRE(ready_net->drivers.size() == 1);
    CHECK(ready_net->drivers.front().node_id == "u_child");
}

TEST_CASE("DesignGraphProvider prepares incoming and outgoing call hierarchy",
          "[analysis][semantic][design-graph-provider][call-hierarchy]") {
    const auto context = simpleDesignContext();

    const auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 3, 4);
    REQUIRE_FALSE(prepared.unresolved);
    REQUIRE(prepared.items.size() == 1);
    CHECK(prepared.items.front().name == "child");

    const auto incoming = incomingCalls(context, prepared.items.front());
    REQUIRE_FALSE(incoming.unresolved);
    REQUIRE(incoming.calls.size() == 1);
    CHECK(incoming.calls.front().item.name == "top");
    CHECK(incoming.calls.front().from_ranges.front().start_line == 3);

    const auto top_prepare = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE_FALSE(top_prepare.unresolved);
    REQUIRE(top_prepare.items.size() == 1);
    const auto outgoing = outgoingCalls(context, top_prepare.items.front());
    REQUIRE_FALSE(outgoing.unresolved);
    REQUIRE(outgoing.calls.size() == 1);
    CHECK(outgoing.calls.front().item.name == "child");
}

TEST_CASE("DesignGraphProvider reports unavailable snapshot",
          "[analysis][semantic][design-graph-provider][unresolved]") {
    const DesignGraphContext context{.generation = 12, .snapshot_available = false};

    const auto hierarchy = moduleHierarchy(context, std::nullopt, 8);
    CHECK(hierarchy.unresolved);
    REQUIRE_FALSE(hierarchy.messages.empty());
    CHECK(hierarchy.messages.front().find("snapshot is unavailable") != std::string::npos);

    const auto graph = schematic(context, std::nullopt, 8);
    CHECK(graph.unresolved);

    const auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 0, 0);
    CHECK(prepared.unresolved);
}

} // namespace
} // namespace pristine::analysis::semantic
