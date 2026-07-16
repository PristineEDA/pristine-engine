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
    const SemanticModuleSignature child_signature{
        .definition = child,
        .schematic = ModuleSchematic{.name = "child",
                                     .range = child.range,
                                     .selection_range = child.selection_range,
                                     .ports = child.port_details,
                                     .cells = {}},
        .uri = "file:///workspace/child.sv"};
    const SemanticModuleSignature top_signature{
        .definition = top,
        .schematic = ModuleSchematic{.name = "top",
                                     .range = top.range,
                                     .selection_range = top.selection_range,
                                     .ports = {},
                                     .cells = {SchematicCell{
                                         .id = "u_child",
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
                                                                              .range = rangeAt(3, 28, 34)}}}}},
        .uri = "file:///workspace/top.sv"};

    DesignGraphContext context{.generation = 9,
                               .snapshot_available = true,
                               .top_modules = {"top"},
                               .modules_by_name = {{"child", child}, {"top", top}},
                               .module_uris_by_name = {{"child", "file:///workspace/child.sv"},
                                                       {"top", "file:///workspace/top.sv"}},
                               .module_signatures_by_name = {{"child", child_signature},
                                                             {"top", top_signature}},
                               .module_entries = {DesignGraphModuleEntry{.uri = "file:///workspace/child.sv",
                                                                          .definition = child},
                                                  DesignGraphModuleEntry{.uri = "file:///workspace/top.sv",
                                                                         .definition = top}}};
    auto& call_index = context.module_call_edge_index;
    call_index.items_by_id.emplace(
        "module|child",
        SnapshotModuleCallHierarchyItem{.id = "module|child",
                                        .name = "child",
                                        .kind = "module",
                                        .uri = "file:///workspace/child.sv",
                                        .range = child.range,
                                        .selection_range = child.selection_range});
    call_index.items_by_id.emplace(
        "module|top",
        SnapshotModuleCallHierarchyItem{.id = "module|top",
                                        .name = "top",
                                        .kind = "module",
                                        .uri = "file:///workspace/top.sv",
                                        .range = top.range,
                                        .selection_range = top.selection_range});
    call_index.edges.push_back(SnapshotModuleCallEdge{.caller_item_id = "module|top",
                                                      .callee_item_id = "module|child",
                                                      .instance_id = "instance|u_child",
                                                      .uri = "file:///workspace/top.sv",
                                                      .range = top.instances.front().range,
                                                      .selection_range =
                                                          top.instances.front().module_selection_range});
    call_index.edges_by_caller_item_id["module|top"] = {0};
    call_index.edges_by_callee_item_id["module|child"] = {0};
    call_index.items_by_uri["file:///workspace/child.sv"] = {
        SnapshotModuleCallHierarchyRange{.range = child.range, .item_id = "module|child"}};
    call_index.items_by_uri["file:///workspace/top.sv"] = {
        SnapshotModuleCallHierarchyRange{.range = top.range, .item_id = "module|top"},
        SnapshotModuleCallHierarchyRange{.range = top.instances.front().selection_range,
                                         .item_id = "module|child"},
        SnapshotModuleCallHierarchyRange{.range = top.instances.front().module_selection_range,
                                         .item_id = "module|child"}};
    return context;
}

SemanticSymbolIdentity symbol(std::string stable_id, std::string name, int line, int start, int end) {
    return SemanticSymbolIdentity{.stable_id = std::move(stable_id),
                                  .name = std::move(name),
                                  .kind = "Variable",
                                  .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                               .range = rangeAt(line, start, end)}};
}

void setConeEdges(DesignGraphContext& context, std::vector<SnapshotAssignmentEdge> edges) {
    auto& adjacency = context.cone_adjacency_index;
    adjacency = {};
    adjacency.edges = std::move(edges);
    for (size_t index = 0; index < adjacency.edges.size(); ++index) {
        const auto& edge = adjacency.edges[index];
        adjacency.edges_by_from_symbol_id[edge.from_symbol_id].push_back(index);
        adjacency.edges_by_to_symbol_id[edge.to_symbol_id].push_back(index);
    }
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

TEST_CASE("DesignGraphProvider module hierarchy preserves Pristine tree fields and partial messages",
          "[analysis][semantic][design-graph-provider][hierarchy][pristine]") {
    auto context = simpleDesignContext();
    auto& top = context.modules_by_name.at("top");
    const ModuleDefinition bus_if{.name = "bus_if",
                                  .kind = "interface",
                                  .range = rangeAt(7, 0, 30),
                                  .selection_range = rangeAt(7, 10, 16),
                                  .ports = {},
                                  .port_details = {},
                                  .instances = {}};
    context.modules_by_name.emplace("bus_if", bus_if);
    context.module_uris_by_name.emplace("bus_if", "file:///workspace/bus_if.sv");
    context.module_entries.push_back(DesignGraphModuleEntry{.uri = "file:///workspace/bus_if.sv",
                                                            .definition = bus_if});
    top.instances.push_back(ModuleInstantiation{.module_name = "child",
                                                .instance_name = "u_child_b",
                                                .range = rangeAt(4, 2, 37),
                                                .selection_range = rangeAt(4, 8, 17),
                                                .module_selection_range = rangeAt(4, 2, 7)});
    top.instances.push_back(ModuleInstantiation{.module_name = "bus_if",
                                                .instance_name = "if0",
                                                .range = rangeAt(5, 2, 20),
                                                .selection_range = rangeAt(5, 9, 12),
                                                .module_selection_range = rangeAt(5, 2, 8)});
    top.instances.push_back(ModuleInstantiation{.module_name = "missing_child",
                                                .instance_name = "u_missing",
                                                .range = rangeAt(6, 2, 43),
                                                .selection_range = rangeAt(6, 16, 25),
                                                .module_selection_range = rangeAt(6, 2, 15)});

    const auto hierarchy = moduleHierarchy(context, std::string_view("top"), 8);

    REQUIRE_FALSE(hierarchy.unresolved);
    CHECK(hierarchy.partial);
    REQUIRE(hierarchy.roots.size() == 1);
    const auto& root = hierarchy.roots.front();
    CHECK(root.module_name == "top");
    CHECK(root.kind == "module");
    REQUIRE(root.children.size() == 4);
    CHECK(root.children[0].module_name == "child");
    CHECK(root.children[0].instance_name == "u_child");
    CHECK(root.children[0].instance_range.has_value());
    CHECK(root.children[1].module_name == "child");
    CHECK(root.children[1].instance_name == "u_child_b");
    CHECK(root.children[2].module_name == "bus_if");
    CHECK(root.children[2].kind == "interface");
    CHECK(root.children[2].instance_name == "if0");
    CHECK(root.children[3].module_name == "missing_child");
    CHECK(root.children[3].instance_name == "u_missing");
    CHECK(root.children[3].unresolved);
    CHECK(std::any_of(hierarchy.messages.begin(), hierarchy.messages.end(), [](const auto& message) {
        return message.find("missing_child") != std::string::npos;
    }));
}

TEST_CASE("DesignGraphProvider deduplicates repeated unresolved hierarchy and schematic messages",
          "[analysis][semantic][design-graph-provider][hierarchy][schematic][messages]") {
    auto context = simpleDesignContext();
    context.modules_by_name.at("top").instances = {
        ModuleInstantiation{.module_name = "missing_child",
                            .instance_name = "u_missing_a",
                            .range = rangeAt(8, 2, 32),
                            .selection_range = rangeAt(8, 16, 27),
                            .module_selection_range = rangeAt(8, 2, 15)},
        ModuleInstantiation{.module_name = "missing_child",
                            .instance_name = "u_missing_b",
                            .range = rangeAt(9, 2, 32),
                            .selection_range = rangeAt(9, 16, 27),
                            .module_selection_range = rangeAt(9, 2, 15)}};
    context.module_signatures_by_name.at("top").definition.instances = context.modules_by_name.at("top").instances;
    context.module_signatures_by_name.at("top").schematic.cells = {
        SchematicCell{.id = "u_missing_a",
                      .name = "u_missing_a",
                      .type = "missing_child",
                      .kind = "module",
                      .range = rangeAt(8, 2, 32),
                      .selection_range = rangeAt(8, 16, 27)},
        SchematicCell{.id = "u_missing_b",
                      .name = "u_missing_b",
                      .type = "missing_child",
                      .kind = "module",
                      .range = rangeAt(9, 2, 32),
                      .selection_range = rangeAt(9, 16, 27)}};

    const auto hierarchy = moduleHierarchy(context, std::string_view("top"), 8);
    REQUIRE_FALSE(hierarchy.unresolved);
    CHECK(hierarchy.partial);
    REQUIRE(hierarchy.roots.size() == 1);
    REQUIRE(hierarchy.roots.front().children.size() == 2);
    CHECK(hierarchy.roots.front().children[0].unresolved);
    CHECK(hierarchy.roots.front().children[1].unresolved);
    CHECK(std::count_if(hierarchy.messages.begin(), hierarchy.messages.end(), [](const auto& message) {
              return message.find("Unresolved module 'missing_child'") != std::string::npos;
          }) == 1);

    const auto graph = schematic(context, std::string_view("top"), 8);
    REQUIRE_FALSE(graph.unresolved);
    CHECK(graph.partial);
    CHECK(std::count_if(graph.messages.begin(), graph.messages.end(), [](const auto& message) {
              return message.find("No schematic data found for module 'missing_child'") != std::string::npos;
          }) == 1);
}

TEST_CASE("DesignGraphProvider memoizes repeated hierarchy subtrees without merging instances",
          "[analysis][semantic][design-graph-provider][hierarchy][cache]") {
    auto context = simpleDesignContext();
    ModuleDefinition leaf{.name = "leaf",
                          .kind = "module",
                          .range = rangeAt(10, 0, 20),
                          .selection_range = rangeAt(10, 7, 11),
                          .ports = {},
                          .port_details = {},
                          .instances = {}};
    ModuleDefinition wrapper{.name = "wrapper",
                             .kind = "module",
                             .range = rangeAt(12, 0, 40),
                             .selection_range = rangeAt(12, 7, 14),
                             .ports = {},
                             .port_details = {},
                             .instances = {ModuleInstantiation{.module_name = "leaf",
                                                               .instance_name = "u_leaf",
                                                               .range = rangeAt(13, 2, 18),
                                                               .selection_range = rangeAt(13, 7, 13),
                                                               .module_selection_range = rangeAt(13, 2, 6)}}};
    auto& top = context.modules_by_name.at("top");
    top.instances = {ModuleInstantiation{.module_name = "wrapper",
                                         .instance_name = "u_wrap_a",
                                         .range = rangeAt(20, 2, 24),
                                         .selection_range = rangeAt(20, 10, 18),
                                         .module_selection_range = rangeAt(20, 2, 9)},
                     ModuleInstantiation{.module_name = "wrapper",
                                         .instance_name = "u_wrap_b",
                                         .range = rangeAt(21, 2, 24),
                                         .selection_range = rangeAt(21, 10, 18),
                                         .module_selection_range = rangeAt(21, 2, 9)}};
    context.modules_by_name.emplace("leaf", leaf);
    context.modules_by_name.emplace("wrapper", wrapper);
    context.module_uris_by_name.emplace("leaf", "file:///workspace/leaf.sv");
    context.module_uris_by_name.emplace("wrapper", "file:///workspace/wrapper.sv");

    const auto hierarchy = moduleHierarchy(context, std::string_view("top"), 8);

    REQUIRE_FALSE(hierarchy.unresolved);
    REQUIRE_FALSE(hierarchy.partial);
    REQUIRE(hierarchy.roots.size() == 1);
    const auto& children = hierarchy.roots.front().children;
    REQUIRE(children.size() == 2);
    CHECK(children[0].module_name == "wrapper");
    CHECK(children[0].instance_name == "u_wrap_a");
    CHECK(children[0].instance_range.has_value());
    CHECK(children[0].instance_range->start_line == 20);
    CHECK(children[1].module_name == "wrapper");
    CHECK(children[1].instance_name == "u_wrap_b");
    CHECK(children[1].instance_range.has_value());
    CHECK(children[1].instance_range->start_line == 21);
    REQUIRE(children[0].children.size() == 1);
    REQUIRE(children[1].children.size() == 1);
    CHECK(children[0].children.front().module_name == "leaf");
    CHECK(children[1].children.front().module_name == "leaf");
    CHECK(children[0].children.front().instance_name == "u_leaf");
    CHECK(children[1].children.front().instance_name == "u_leaf");
}

TEST_CASE("DesignGraphProvider schematic emits Pristine-compatible ports cells and net endpoints",
          "[analysis][semantic][design-graph-provider][schematic][pristine]") {
    auto context = simpleDesignContext();
    auto& top_signature = context.module_signatures_by_name.at("top");
    top_signature.schematic.ports = {
        SchematicPort{.name = "clk",
                      .direction = "input",
                      .width_text = "logic",
                      .range = rangeAt(2, 11, 26),
                      .selection_range = rangeAt(2, 23, 26)},
        SchematicPort{.name = "ready",
                      .direction = "output",
                      .width_text = "logic",
                      .range = rangeAt(2, 28, 46),
                      .selection_range = rangeAt(2, 41, 46)}};
    top_signature.schematic.cells.push_back(
        SchematicCell{.id = "u_ordered",
                      .name = "u_ordered",
                      .type = "child",
                      .kind = "module",
                      .range = rangeAt(4, 2, 30),
                      .selection_range = rangeAt(4, 8, 17),
                      .connections = {SchematicConnection{.port_name = "",
                                                           .port_index = 0,
                                                           .signal = "clk",
                                                           .range = rangeAt(4, 18, 21)},
                                      SchematicConnection{.port_name = "",
                                                           .port_index = 1,
                                                           .signal = "ordered_out",
                                                           .range = rangeAt(4, 23, 29)}}});

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
    CHECK(std::any_of(top_view->module.ports.begin(),
                      top_view->module.ports.end(),
                      [](const SchematicPort& port) {
                          return port.name == "clk" && port.direction == "input";
                      }));
    CHECK(std::any_of(top_view->module.cells.begin(),
                      top_view->module.cells.end(),
                      [](const SchematicCell& cell) {
                          return cell.id == "u_ordered" && cell.type == "child";
                      }));

    const auto clk_net = std::find_if(top_view->nets.begin(),
                                      top_view->nets.end(),
                                      [](const SemanticSchematicNet& net) {
                                          return net.name == "clk";
                                      });
    REQUIRE(clk_net != top_view->nets.end());
    CHECK(std::any_of(clk_net->drivers.begin(),
                      clk_net->drivers.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "$port:clk" && endpoint.port_name == "clk";
                      }));
    CHECK(std::any_of(clk_net->loads.begin(),
                      clk_net->loads.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "u_child" && endpoint.port_name == "clk";
                      }));
    CHECK(std::any_of(clk_net->loads.begin(),
                      clk_net->loads.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "u_ordered" && endpoint.port_name == "clk";
                      }));

    const auto ready_net = std::find_if(top_view->nets.begin(),
                                        top_view->nets.end(),
                                        [](const SemanticSchematicNet& net) {
                                            return net.name == "ready";
                                        });
    REQUIRE(ready_net != top_view->nets.end());
    CHECK(std::any_of(ready_net->loads.begin(),
                      ready_net->loads.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "$port:ready" && endpoint.port_name == "ready";
                      }));
    CHECK(std::any_of(ready_net->drivers.begin(),
                      ready_net->drivers.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "u_child" && endpoint.port_name == "out";
                      }));
}

TEST_CASE("DesignGraphProvider schematic connects interface cells to same-name nets",
          "[analysis][semantic][design-graph-provider][schematic][interface][modport]") {
    auto context = simpleDesignContext();
    ModuleDefinition bus_if{.name = "bus_if",
                            .kind = "interface",
                            .range = rangeAt(7, 0, 40),
                            .selection_range = rangeAt(7, 10, 16),
                            .ports = {},
                            .port_details = {},
                            .instances = {}};
    ModuleDefinition consumer{.name = "consumer",
                              .kind = "module",
                              .range = rangeAt(9, 0, 40),
                              .selection_range = rangeAt(9, 7, 15),
                              .ports = {},
                              .port_details = {SchematicPort{.name = "bus",
                                                             .direction = "interface",
                                                             .width_text = "bus_if.master",
                                                             .range = rangeAt(9, 16, 33),
                                                             .selection_range = rangeAt(9, 30, 33)}},
                              .instances = {}};
    ModuleDefinition top{.name = "top",
                         .kind = "module",
                         .range = rangeAt(12, 0, 80),
                         .selection_range = rangeAt(12, 7, 10),
                         .ports = {},
                         .port_details = {},
                         .instances = {ModuleInstantiation{.module_name = "bus_if",
                                                           .instance_name = "bus",
                                                           .range = rangeAt(13, 2, 15),
                                                           .selection_range = rangeAt(13, 9, 12),
                                                           .module_selection_range = rangeAt(13, 2, 8)},
                                       ModuleInstantiation{.module_name = "consumer",
                                                           .instance_name = "u_consumer",
                                                           .range = rangeAt(14, 2, 26),
                                                           .selection_range = rangeAt(14, 11, 21),
                                                           .module_selection_range = rangeAt(14, 2, 10)}}};
    context.modules_by_name = {{"bus_if", bus_if}, {"consumer", consumer}, {"top", top}};
    context.module_uris_by_name = {{"bus_if", "file:///workspace/bus_if.sv"},
                                   {"consumer", "file:///workspace/consumer.sv"},
                                   {"top", "file:///workspace/top.sv"}};
    context.module_signatures_by_name = {
        {"bus_if",
         SemanticModuleSignature{.definition = bus_if,
                                 .schematic = ModuleSchematic{.name = "bus_if",
                                                              .range = bus_if.range,
                                                              .selection_range = bus_if.selection_range,
                                                              .ports = {},
                                                              .cells = {}},
                                 .uri = "file:///workspace/bus_if.sv"}},
        {"consumer",
         SemanticModuleSignature{.definition = consumer,
                                 .schematic = ModuleSchematic{.name = "consumer",
                                                              .range = consumer.range,
                                                              .selection_range = consumer.selection_range,
                                                              .ports = consumer.port_details,
                                                              .cells = {}},
                                 .uri = "file:///workspace/consumer.sv"}},
        {"top",
         SemanticModuleSignature{
             .definition = top,
             .schematic = ModuleSchematic{.name = "top",
                                          .range = top.range,
                                          .selection_range = top.selection_range,
                                          .ports = {},
                                          .cells = {SchematicCell{.id = "bus",
                                                                  .name = "bus",
                                                                  .type = "bus_if",
                                                                  .kind = "interface",
                                                                  .range = rangeAt(13, 2, 15),
                                                                  .selection_range = rangeAt(13, 9, 12)},
                                                    SchematicCell{.id = "u_consumer",
                                                                  .name = "u_consumer",
                                                                  .type = "consumer",
                                                                  .kind = "module",
                                                                  .range = rangeAt(14, 2, 26),
                                                                  .selection_range = rangeAt(14, 11, 21),
                                                                  .connections = {SchematicConnection{
                                                                      .port_name = "bus",
                                                                      .signal = "bus",
                                                                      .range = rangeAt(14, 22, 25)}}}}},
             .uri = "file:///workspace/top.sv"}}};
    context.module_entries = {DesignGraphModuleEntry{.uri = "file:///workspace/bus_if.sv",
                                                     .definition = bus_if},
                              DesignGraphModuleEntry{.uri = "file:///workspace/consumer.sv",
                                                     .definition = consumer},
                              DesignGraphModuleEntry{.uri = "file:///workspace/top.sv",
                                                     .definition = top}};

    const auto graph = schematic(context, std::string_view("top"), 8);

    REQUIRE_FALSE(graph.unresolved);
    const auto top_view = std::find_if(graph.modules.begin(),
                                       graph.modules.end(),
                                       [](const SemanticSchematicModuleView& view) {
                                           return view.module.name == "top";
                                       });
    REQUIRE(top_view != graph.modules.end());
    const auto bus_net = std::find_if(top_view->nets.begin(),
                                      top_view->nets.end(),
                                      [](const SemanticSchematicNet& net) {
                                          return net.name == "bus";
                                      });
    REQUIRE(bus_net != top_view->nets.end());
    CHECK(std::any_of(bus_net->drivers.begin(),
                      bus_net->drivers.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "bus" && endpoint.port_name == "interface";
                      }));
    CHECK(std::any_of(bus_net->loads.begin(),
                      bus_net->loads.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "bus" && endpoint.port_name == "interface";
                      }));
    CHECK(std::any_of(bus_net->drivers.begin(),
                      bus_net->drivers.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "u_consumer" && endpoint.port_name == "bus";
                      }));
    CHECK(std::any_of(bus_net->loads.begin(),
                      bus_net->loads.end(),
                      [](const SemanticSchematicEndpoint& endpoint) {
                          return endpoint.node_id == "u_consumer" && endpoint.port_name == "bus";
                      }));
}

TEST_CASE("DesignGraphProvider traces backward cone through continuous assignments",
          "[analysis][semantic][design-graph-provider][cone]") {
    auto context = simpleDesignContext();
    const auto a = symbol("symbol|a", "a", 1, 8, 9);
    const auto b = symbol("symbol|b", "b", 2, 8, 9);
    const auto mid = symbol("symbol|mid", "mid", 3, 8, 11);
    const auto out = symbol("symbol|out", "out", 4, 8, 11);
    context.symbols_by_id = {{"symbol|a", DesignGraphSymbol{.identity = a}},
                             {"symbol|b", DesignGraphSymbol{.identity = b}},
                             {"symbol|mid", DesignGraphSymbol{.identity = mid}},
                             {"symbol|out", DesignGraphSymbol{.identity = out}}};
    setConeEdges(context, {
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|mid",
                               .to_symbol_id = "symbol|a",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(5, 2, 20)},
                               .expression = "a & b"},
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|mid",
                               .to_symbol_id = "symbol|b",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(5, 2, 20)},
                               .expression = "a & b"},
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|out",
                               .to_symbol_id = "symbol|mid",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(6, 2, 18)},
                               .expression = "mid"}});
    const SemanticLookupResult lookup{.generation = 9,
                                      .symbol = out,
                                      .unresolved = false};

    const auto trace = backwardCone(context, "file:///workspace/cone.sv", lookup, 2000);

    REQUIRE_FALSE(trace.unresolved);
    REQUIRE(trace.root_symbol_id.has_value());
    CHECK(*trace.root_symbol_id == "symbol|out");
    CHECK(trace.nodes.size() == 4);
    CHECK(trace.edges.size() == 3);
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "symbol|out" && edge.to_symbol_id == "symbol|mid";
    }));
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "symbol|mid" && edge.to_symbol_id == "symbol|a";
    }));
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "symbol|mid" && edge.to_symbol_id == "symbol|b";
    }));
}

TEST_CASE("DesignGraphProvider traces backward cone through cross-module instance port edges",
          "[analysis][semantic][design-graph-provider][cone][instance][no-fallback]") {
    auto context = simpleDesignContext();
    auto& child_signature = context.module_signatures_by_name.at("child");
    child_signature.definition.range = rangeAt(0, 0, 96);
    child_signature.definition.port_details = {
        SchematicPort{.name = "in",
                      .direction = "input",
                      .width_text = "logic [WIDTH-1:0]",
                      .range = rangeAt(0, 42, 67),
                      .selection_range = rangeAt(0, 60, 62)},
        SchematicPort{.name = "out",
                      .direction = "output",
                      .width_text = "logic [WIDTH-1:0]",
                      .range = rangeAt(0, 69, 95),
                      .selection_range = rangeAt(0, 88, 91)}};
    child_signature.schematic.ports = child_signature.definition.port_details;
    child_signature.schematic.range = child_signature.definition.range;
    context.modules_by_name.at("child").range = child_signature.definition.range;
    context.modules_by_name.at("child").port_details = child_signature.definition.port_details;
    auto& top_signature = context.module_signatures_by_name.at("top");
    top_signature.schematic.cells.front().connections = {
        SchematicConnection{.port_name = "in",
                            .port_index = 0,
                            .signal = "a",
                            .range = rangeAt(5, 31, 32)},
        SchematicConnection{.port_name = "out",
                            .port_index = 1,
                            .signal = "y",
                            .range = rangeAt(5, 40, 41)}};

    const auto a = SemanticSymbolIdentity{.stable_id = "top|a",
                                          .name = "a",
                                          .kind = "Variable",
                                          .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(3, 14, 15)}};
    const auto y = SemanticSymbolIdentity{.stable_id = "top|y",
                                          .name = "y",
                                          .kind = "Variable",
                                          .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(4, 14, 15)}};
    const auto child_in = SemanticSymbolIdentity{.stable_id = "child|in",
                                                 .name = "in",
                                                 .kind = "Port",
                                                 .location = SemanticLocation{
                                                     .uri = "file:///workspace/child.sv",
                                                     .range = rangeAt(0, 60, 62)}};
    const auto child_out = SemanticSymbolIdentity{.stable_id = "child|out",
                                                  .name = "out",
                                                  .kind = "Port",
                                                  .location = SemanticLocation{
                                                      .uri = "file:///workspace/child.sv",
                                                      .range = rangeAt(0, 88, 91)}};
    context.symbols_by_id = {{"top|a", DesignGraphSymbol{.identity = a}},
                             {"top|y", DesignGraphSymbol{.identity = y}},
                             {"child|in", DesignGraphSymbol{.identity = child_in}},
                             {"child|out", DesignGraphSymbol{.identity = child_out}}};
    setConeEdges(context, {
        SnapshotAssignmentEdge{.from_symbol_id = "top|y",
                               .to_symbol_id = "child|out",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(5, 40, 41)},
                               .expression = "y"},
        SnapshotAssignmentEdge{.from_symbol_id = "child|out",
                               .to_symbol_id = "child|in",
                               .location = SemanticLocation{.uri = "file:///workspace/child.sv",
                                                            .range = rangeAt(1, 2, 17)},
                               .expression = "in"},
        SnapshotAssignmentEdge{.from_symbol_id = "child|in",
                               .to_symbol_id = "top|a",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(5, 31, 32)},
                               .expression = "a"}});

    const SemanticLookupResult lookup{.generation = 9, .symbol = y, .unresolved = false};

    const auto trace = backwardCone(context, "file:///workspace/top.sv", lookup, 2000);

    REQUIRE_FALSE(trace.unresolved);
    CHECK(trace.nodes.size() == 4);
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "top|y" && edge.to_symbol_id == "child|out";
    }));
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "child|out" && edge.to_symbol_id == "child|in";
    }));
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "child|in" && edge.to_symbol_id == "top|a";
    }));
}

TEST_CASE("DesignGraphProvider traces instance port edges when generated connection ranges are broad",
          "[analysis][semantic][design-graph-provider][cone][generated][instance][no-text-fallback]") {
    auto context = simpleDesignContext();
    auto& child_signature = context.module_signatures_by_name.at("child");
    child_signature.definition.range = ParseRange{.start_line = 0,
                                                  .start_character = 0,
                                                  .end_line = 2,
                                                  .end_character = 20};
    child_signature.definition.port_details = {
        SchematicPort{.name = "in",
                      .direction = "input",
                      .width_text = "logic",
                      .range = rangeAt(0, 13, 21),
                      .selection_range = rangeAt(0, 19, 21)},
        SchematicPort{.name = "out",
                      .direction = "output",
                      .width_text = "logic",
                      .range = rangeAt(0, 23, 33),
                      .selection_range = rangeAt(0, 30, 33)}};
    child_signature.schematic.ports = child_signature.definition.port_details;
    child_signature.schematic.range = child_signature.definition.range;

    auto& top_signature = context.module_signatures_by_name.at("top");
    top_signature.definition.range = ParseRange{.start_line = 2,
                                                .start_character = 0,
                                                .end_line = 8,
                                                .end_character = 40};
    top_signature.schematic.cells.front().connections = {
        SchematicConnection{.port_name = "in",
                            .port_index = 0,
                            .signal = "a",
                            .range = rangeAt(6, 12, 20)},
        SchematicConnection{.port_name = "out",
                            .port_index = 1,
                            .signal = "y",
                            .range = rangeAt(6, 22, 31)}};

    const auto a = SemanticSymbolIdentity{.stable_id = "top|a",
                                          .name = "a",
                                          .kind = "Variable",
                                          .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(3, 14, 15)}};
    const auto y = SemanticSymbolIdentity{.stable_id = "top|y",
                                          .name = "y",
                                          .kind = "Variable",
                                          .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(4, 14, 15)}};
    const auto child_in = SemanticSymbolIdentity{.stable_id = "child|in",
                                                 .name = "in",
                                                 .kind = "Port",
                                                 .location = SemanticLocation{
                                                     .uri = "file:///workspace/child.sv",
                                                     .range = rangeAt(0, 19, 21)}};
    const auto child_out = SemanticSymbolIdentity{.stable_id = "child|out",
                                                  .name = "out",
                                                  .kind = "Port",
                                                  .location = SemanticLocation{
                                                      .uri = "file:///workspace/child.sv",
                                                      .range = rangeAt(0, 30, 33)}};
    context.symbols_by_id = {{"top|a", DesignGraphSymbol{.identity = a}},
                             {"top|y", DesignGraphSymbol{.identity = y}},
                             {"child|in", DesignGraphSymbol{.identity = child_in}},
                             {"child|out", DesignGraphSymbol{.identity = child_out}}};
    setConeEdges(context, {
        SnapshotAssignmentEdge{.from_symbol_id = "top|y",
                               .to_symbol_id = "child|out",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(6, 22, 31)},
                               .expression = "y"},
        SnapshotAssignmentEdge{.from_symbol_id = "child|out",
                               .to_symbol_id = "child|in",
                               .location = SemanticLocation{.uri = "file:///workspace/child.sv",
                                                            .range = rangeAt(1, 2, 17)},
                               .expression = "in"},
        SnapshotAssignmentEdge{.from_symbol_id = "child|in",
                               .to_symbol_id = "top|a",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(6, 12, 20)},
                               .expression = "a"}});
    const SemanticLookupResult lookup{.generation = 9, .symbol = y, .unresolved = false};

    const auto trace = backwardCone(context, "file:///workspace/top.sv", lookup, 2000);

    REQUIRE_FALSE(trace.unresolved);
    CHECK(trace.nodes.size() == 4);
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "top|y" && edge.to_symbol_id == "child|out";
    }));
    CHECK(std::any_of(trace.edges.begin(), trace.edges.end(), [](const SemanticConeEdge& edge) {
        return edge.from_symbol_id == "child|in" && edge.to_symbol_id == "top|a";
    }));
}

TEST_CASE("DesignGraphProvider truncates backward cone at the requested result cap",
          "[analysis][semantic][design-graph-provider][cone][truncated][no-fallback]") {
    auto context = simpleDesignContext();
    const auto a = symbol("symbol|a", "a", 1, 8, 9);
    const auto b = symbol("symbol|b", "b", 2, 8, 9);
    const auto c = symbol("symbol|c", "c", 3, 8, 9);
    const auto out = symbol("symbol|out", "out", 4, 8, 11);
    context.symbols_by_id = {{"symbol|a", DesignGraphSymbol{.identity = a}},
                             {"symbol|b", DesignGraphSymbol{.identity = b}},
                             {"symbol|c", DesignGraphSymbol{.identity = c}},
                             {"symbol|out", DesignGraphSymbol{.identity = out}}};
    setConeEdges(context, {
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|out",
                               .to_symbol_id = "symbol|a",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(5, 2, 15)},
                               .expression = "a"},
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|out",
                               .to_symbol_id = "symbol|b",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(6, 2, 15)},
                               .expression = "b"},
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|out",
                               .to_symbol_id = "symbol|c",
                               .location = SemanticLocation{.uri = "file:///workspace/cone.sv",
                                                            .range = rangeAt(7, 2, 15)},
                               .expression = "c"}});
    const SemanticLookupResult lookup{.generation = 9,
                                      .symbol = out,
                                      .unresolved = false};

    const auto trace = backwardCone(context, "file:///workspace/cone.sv", lookup, 2);

    REQUIRE_FALSE(trace.unresolved);
    CHECK(trace.partial);
    CHECK(trace.truncated);
    REQUIRE(trace.messages.size() == 1);
    CHECK(trace.messages.front().find("result cap") != std::string::npos);
    CHECK(trace.nodes.size() <= 2);
    CHECK(trace.edges.size() <= 2);
}

TEST_CASE("DesignGraphProvider reports missing cone assignments",
          "[analysis][semantic][design-graph-provider][cone]") {
    auto context = simpleDesignContext();
    const auto out = symbol("symbol|out", "out", 4, 8, 11);
    context.symbols_by_id = {{"symbol|out", DesignGraphSymbol{.identity = out}}};
    const SemanticLookupResult lookup{.generation = 9,
                                      .symbol = out,
                                      .unresolved = false};

    const auto trace = backwardCone(context, "file:///workspace/cone.sv", lookup, 2000);

    CHECK_FALSE(trace.unresolved);
    CHECK(trace.nodes.empty());
    REQUIRE_FALSE(trace.messages.empty());
    CHECK(trace.messages.front().find("No AST assignment edges") != std::string::npos);
}

TEST_CASE("DesignGraphProvider reports unresolved configured hierarchy roots",
          "[analysis][semantic][design-graph-provider][hierarchy][partial]") {
    auto context = simpleDesignContext();
    context.top_modules = {"top", "missing_top"};

    const auto hierarchy = moduleHierarchy(context, std::nullopt, 8);

    REQUIRE_FALSE(hierarchy.unresolved);
    CHECK(hierarchy.partial);
    REQUIRE(hierarchy.roots.size() == 2);
    CHECK(hierarchy.roots[1].module_name == "missing_top");
    CHECK(hierarchy.roots[1].unresolved);
    CHECK(std::any_of(hierarchy.messages.begin(), hierarchy.messages.end(), [](const auto& message) {
        return message.find("Unresolved module 'missing_top'") != std::string::npos;
    }));
}

TEST_CASE("DesignGraphProvider infers all uninstantiated hierarchy roots",
          "[analysis][semantic][design-graph-provider][hierarchy][multi-root]") {
    auto context = simpleDesignContext();
    context.top_modules.clear();
    ModuleDefinition top_b{.name = "top_b",
                           .kind = "module",
                           .range = rangeAt(8, 0, 20),
                           .selection_range = rangeAt(8, 7, 12),
                           .ports = {},
                           .port_details = {},
                           .instances = {ModuleInstantiation{.module_name = "child",
                                                             .instance_name = "u_child_b",
                                                             .range = rangeAt(9, 2, 26),
                                                             .selection_range = rangeAt(9, 8, 17),
                                                             .module_selection_range = rangeAt(9, 2, 7)}}};
    context.modules_by_name.emplace("top_b", top_b);
    context.module_uris_by_name.emplace("top_b", "file:///workspace/top_b.sv");

    const auto hierarchy = moduleHierarchy(context, std::nullopt, 8);

    REQUIRE_FALSE(hierarchy.unresolved);
    REQUIRE(hierarchy.roots.size() == 2);
    CHECK(hierarchy.roots[0].module_name == "top");
    CHECK(hierarchy.roots[1].module_name == "top_b");
}

TEST_CASE("DesignGraphProvider falls back to all hierarchy roots for cyclic designs",
          "[analysis][semantic][design-graph-provider][hierarchy][multi-root][cycle]") {
    auto context = simpleDesignContext();
    context.top_modules.clear();
    context.modules_by_name.at("child").instances = {ModuleInstantiation{.module_name = "top",
                                                                         .instance_name = "u_top",
                                                                         .range = rangeAt(1, 2, 20),
                                                                         .selection_range = rangeAt(1, 8, 13),
                                                                         .module_selection_range = rangeAt(1, 2, 5)}};

    const auto hierarchy = moduleHierarchy(context, std::nullopt, 8);

    REQUIRE_FALSE(hierarchy.unresolved);
    REQUIRE(hierarchy.roots.size() == 2);
    CHECK(hierarchy.roots[0].module_name == "child");
    CHECK(hierarchy.roots[1].module_name == "top");
}

TEST_CASE("DesignGraphProvider reports missing schematic signatures without syntax fallback",
          "[analysis][semantic][design-graph-provider][schematic][partial][no-fallback]") {
    auto context = simpleDesignContext();
    context.module_signatures_by_name.erase("child");

    const auto graph = schematic(context, std::string_view("top"), 8);

    REQUIRE_FALSE(graph.unresolved);
    CHECK(graph.partial);
    CHECK(std::any_of(graph.modules.begin(), graph.modules.end(), [](const auto& view) {
        return view.module.name == "top";
    }));
    CHECK(std::none_of(graph.modules.begin(), graph.modules.end(), [](const auto& view) {
        return view.module.name == "child";
    }));
    CHECK(std::any_of(graph.messages.begin(), graph.messages.end(), [](const auto& message) {
        return message.find("No schematic data found for module 'child'") != std::string::npos;
    }));
}

TEST_CASE("DesignGraphProvider prepares incoming and outgoing call hierarchy",
          "[analysis][semantic][design-graph-provider][call-hierarchy]") {
    const auto context = simpleDesignContext();

    const auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 3, 4);
    REQUIRE_FALSE(prepared.unresolved);
    REQUIRE(prepared.items.size() == 1);
    CHECK(prepared.items.front().name == "child");
    CHECK_FALSE(prepared.items.front().opaque_id.empty());
    CHECK(prepared.items.front().generation == context.generation);
    CHECK(prepared.scanned_module_count == 0);

    const auto incoming = incomingCalls(context, prepared.items.front());
    REQUIRE_FALSE(incoming.unresolved);
    REQUIRE(incoming.calls.size() == 1);
    CHECK(incoming.calls.front().item.name == "top");
    CHECK(incoming.calls.front().from_ranges.front().start_line == 3);
    CHECK(incoming.scanned_edge_count == 1);
    CHECK(incoming.scanned_module_count == 0);

    const auto top_prepare = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE_FALSE(top_prepare.unresolved);
    REQUIRE(top_prepare.items.size() == 1);
    const auto outgoing = outgoingCalls(context, top_prepare.items.front());
    REQUIRE_FALSE(outgoing.unresolved);
    REQUIRE(outgoing.calls.size() == 1);
    CHECK(outgoing.calls.front().item.name == "child");
    CHECK(outgoing.scanned_edge_count == 1);
    CHECK(outgoing.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider rejects stale call hierarchy item generations",
          "[analysis][semantic][design-graph-provider][call-hierarchy][stale]") {
    const auto context = simpleDesignContext();
    auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE(prepared.items.size() == 1);
    --prepared.items.front().generation;

    const auto outgoing = outgoingCalls(context, prepared.items.front());
    CHECK(outgoing.unresolved);
    CHECK(outgoing.calls.empty());
    CHECK(outgoing.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider rejects forged call hierarchy opaque ids",
          "[analysis][semantic][design-graph-provider][call-hierarchy][forged]") {
    const auto context = simpleDesignContext();
    auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE(prepared.items.size() == 1);
    prepared.items.front().opaque_id = "module|forged";

    const auto outgoing = outgoingCalls(context, prepared.items.front());
    CHECK(outgoing.unresolved);
    CHECK(outgoing.calls.empty());
    CHECK(outgoing.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider returns empty indexed calls for an uninstantiated module",
          "[analysis][semantic][design-graph-provider][call-hierarchy][leaf]") {
    const auto context = simpleDesignContext();
    const auto prepared = prepareCallHierarchy(context, "file:///workspace/child.sv", 0, 8);
    REQUIRE(prepared.items.size() == 1);

    const auto outgoing = outgoingCalls(context, prepared.items.front());
    CHECK_FALSE(outgoing.unresolved);
    CHECK(outgoing.calls.empty());
    CHECK(outgoing.scanned_edge_count == 0);
}

TEST_CASE("DesignGraphProvider does not scan modules for a missing call hierarchy URI",
          "[analysis][semantic][design-graph-provider][call-hierarchy][missing]") {
    const auto context = simpleDesignContext();
    const auto prepared = prepareCallHierarchy(context, "file:///workspace/missing.sv", 0, 0);

    CHECK(prepared.unresolved);
    CHECK(prepared.items.empty());
    CHECK(prepared.scanned_edge_count == 0);
    CHECK(prepared.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider rejects call hierarchy items without opaque identity",
          "[analysis][semantic][design-graph-provider][call-hierarchy][missing-id]") {
    const auto context = simpleDesignContext();
    auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE(prepared.items.size() == 1);
    prepared.items.front().opaque_id.clear();

    const auto outgoing = outgoingCalls(context, prepared.items.front());
    CHECK(outgoing.unresolved);
    CHECK(outgoing.calls.empty());
    CHECK(outgoing.scanned_edge_count == 0);
}

TEST_CASE("DesignGraphProvider prepares a module definition from its indexed range",
          "[analysis][semantic][design-graph-provider][call-hierarchy][definition-range]") {
    const auto context = simpleDesignContext();
    const auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);

    REQUIRE_FALSE(prepared.unresolved);
    REQUIRE(prepared.items.size() == 1);
    CHECK(prepared.items.front().opaque_id == "module|top");
    CHECK(prepared.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider prepares a callee from an indexed instance range",
          "[analysis][semantic][design-graph-provider][call-hierarchy][instance-range]") {
    const auto context = simpleDesignContext();
    const auto prepared = prepareCallHierarchy(context, "file:///workspace/top.sv", 3, 10);

    REQUIRE_FALSE(prepared.unresolved);
    REQUIRE(prepared.items.size() == 1);
    CHECK(prepared.items.front().opaque_id == "module|child");
    CHECK(prepared.scanned_module_count == 0);
}

TEST_CASE("DesignGraphProvider preserves repeated instance call edges",
          "[analysis][semantic][design-graph-provider][call-hierarchy][repeated]") {
    auto context = simpleDesignContext();
    context.module_call_edge_index.edges.push_back(
        SnapshotModuleCallEdge{.caller_item_id = "module|top",
                               .callee_item_id = "module|child",
                               .instance_id = "instance|u_child_second",
                               .uri = "file:///workspace/top.sv",
                               .range = rangeAt(4, 2, 35),
                               .selection_range = rangeAt(4, 2, 7)});
    context.module_call_edge_index.edges_by_caller_item_id["module|top"] = {0, 1};
    context.module_call_edge_index.edges_by_callee_item_id["module|child"] = {0, 1};

    const auto top = prepareCallHierarchy(context, "file:///workspace/top.sv", 2, 8);
    REQUIRE(top.items.size() == 1);
    const auto outgoing = outgoingCalls(context, top.items.front());
    REQUIRE(outgoing.calls.size() == 2);
    CHECK(outgoing.calls[0].from_ranges.front().start_line == 3);
    CHECK(outgoing.calls[1].from_ranges.front().start_line == 4);
    CHECK(outgoing.scanned_edge_count == 2);
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
