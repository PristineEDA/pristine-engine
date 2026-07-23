#include "../../src/analysis/semantic/AstIndex.h"
#include "../../src/analysis/semantic/SnapshotBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <tuple>

namespace pristine::analysis::semantic {
namespace {

ParseRange rangeAt(int line, int start, int end) {
    return ParseRange{.start_line = line,
                      .start_character = start,
                      .end_line = line,
                      .end_character = end};
}

AstIndexSymbol symbol(std::string stable_id,
                      std::string name,
                      std::string kind,
                      std::string uri,
                      int line) {
    return AstIndexSymbol{.stable_id = std::move(stable_id),
                          .identity = SemanticSymbolIdentity{
                              .stable_id = stable_id,
                              .name = std::move(name),
                              .kind = std::move(kind),
                              .location = SemanticLocation{.uri = std::move(uri),
                                                           .range = rangeAt(line, 2, 12)}}};
}

auto buildReferenceCallDesign(std::string top_source =
                                  "module top;\n"
                                  "  logic ready;\n"
                                  "  assign ready = ready;\n"
                                  "  child u_child();\n"
                                  "endmodule\n") {
    SnapshotBuildInput input{.generation = 91,
                             .documents = {{"file:///workspace/child.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                                                   .text = "module child; endmodule\n",
                                                                   .version = 1}},
                                           {"file:///workspace/top.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                                                   .text = std::move(top_source),
                                                                   .version = 1,
                                                                   .is_open = true}}}};
    return SnapshotBuilder{}.build(std::move(input));
}

auto buildGraphSource(std::string source,
                      std::uint64_t generation = 91,
                      std::vector<std::string> top_modules = {}) {
    SnapshotBuildInput input{.generation = generation,
                             .config = SemanticEngineConfig{.top_modules = std::move(top_modules)},
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                                                   .text = std::move(source),
                                                                   .version = 1,
                                                                   .is_open = true}}}};
    return SnapshotBuilder{}.build(std::move(input));
}

const SnapshotConeAdjacencyEdge* coneDataEdge(const SnapshotConeAdjacencyIndex& index,
                                              std::string_view expression) {
    const auto it = std::find_if(index.edges.begin(), index.edges.end(), [&](const auto& candidate) {
        return candidate.expression == expression && candidate.source_role == SnapshotConeSourceRole::Data;
    });
    return it == index.edges.end() ? nullptr : &*it;
}

bool graphHasDataSlicePrecision(std::string source, SnapshotConeSlicePrecision precision) {
    auto output = buildGraphSource(std::move(source));
    if (output.data == nullptr) return false;
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    return std::any_of(view.cone_adjacency_index.edges.begin(),
                       view.cone_adjacency_index.edges.end(),
                       [&](const SnapshotConeAdjacencyEdge& edge) {
                           return edge.source_role == SnapshotConeSourceRole::Data &&
                                  edge.source_slice.precision == precision;
                       });
}

std::vector<const SnapshotResolvedConnectionSliceFact*> parameterOverrideFacts(const SnapshotData& data) {
    std::vector<const SnapshotResolvedConnectionSliceFact*> facts;
    for (const auto& [_, values] : data.resolved_connection_slices_by_instance_id) {
        for (const auto& value : values) {
            if (value.kind == SnapshotConeEdgeKind::ParameterOverride) facts.push_back(&value);
        }
    }
    std::sort(facts.begin(), facts.end(), [](const auto* left, const auto* right) {
        return std::tie(left->instance_stable_id, left->endpoint_index, left->endpoint_stable_id) <
               std::tie(right->instance_stable_id, right->endpoint_index, right->endpoint_stable_id);
    });
    return facts;
}

bool hasIndexedParameterEndpoints(const std::vector<const SnapshotResolvedConnectionSliceFact*>& facts) {
    return std::all_of(facts.begin(), facts.end(), [](const auto* fact) {
        return !fact->instance_stable_id.empty() && !fact->endpoint_stable_id.empty() &&
               fact->endpoint_index >= 0;
    });
}

bool hasDirectParameterEndpoints(const std::vector<const SnapshotResolvedConnectionSliceFact*>& facts) {
    return hasIndexedParameterEndpoints(facts);
}

std::vector<const SnapshotSchematicConnectionFact*> schematicConnectionFacts(const SnapshotData& data) {
    std::vector<const SnapshotSchematicConnectionFact*> facts;
    for (const auto& [_, values] : data.design_graph_binding_index.schematic_connections_by_module) {
        for (const auto& value : values) {
            facts.push_back(&value);
        }
    }
    std::sort(facts.begin(), facts.end(), [](const auto* left, const auto* right) {
        return std::tie(left->caller_module_name,
                        left->instance_stable_id,
                        left->kind,
                        left->endpoint_index,
                        left->endpoint_stable_id) <
               std::tie(right->caller_module_name,
                        right->instance_stable_id,
                        right->kind,
                        right->endpoint_index,
                        right->endpoint_stable_id);
    });
    return facts;
}

const SchematicCell* schematicCell(const AstIndexView& view,
                                   std::string_view module,
                                   std::string_view instance) {
    const auto signature = view.module_signatures_by_name.find(std::string(module));
    if (signature == view.module_signatures_by_name.end()) return nullptr;
    const auto cell = std::find_if(signature->second.schematic.cells.begin(),
                                   signature->second.schematic.cells.end(),
                                   [&](const auto& candidate) { return candidate.name == instance; });
    return cell == signature->second.schematic.cells.end() ? nullptr : &*cell;
}

TEST_CASE("AstIndex filters, sorts, and maps workspace symbols",
          "[analysis][semantic][ast-index][workspace-symbol]") {
    const AstIndexContext context{
        .generation = 5,
        .snapshot_available = true,
        .symbols = {symbol("symbol|ready", "ready", "Variable", "file:///workspace/top.sv", 2),
                    symbol("symbol|pkg", "control_pkg", "Package", "file:///workspace/pkg.sv", 0),
                    symbol("symbol|type", "nibble_t", "TypeAlias", "file:///workspace/pkg.sv", 1)}};

    const auto packages = workspaceSymbols(context, "ct", 100);
    REQUIRE_FALSE(packages.unresolved);
    REQUIRE(packages.symbols.size() == 1);
    CHECK(packages.symbols.front().name == "control_pkg");
    CHECK(packages.symbols.front().kind == 4);
    CHECK(packages.symbols.front().stable_id == "symbol|pkg");

    const auto typedefs = workspaceSymbols(context, "nt", 100);
    CHECK(std::any_of(typedefs.symbols.begin(), typedefs.symbols.end(), [](const SemanticWorkspaceSymbol& symbol) {
        return symbol.name == "nibble_t" && symbol.kind == 26;
    }));

    const auto truncated = workspaceSymbols(context, "", 1);
    CHECK(truncated.truncated);
    REQUIRE(truncated.symbols.size() == 1);
    CHECK(truncated.messages.front().find("truncated") != std::string::npos);
}

TEST_CASE("AstIndex reports unavailable snapshot for workspace symbols",
          "[analysis][semantic][ast-index][workspace-symbol][unresolved]") {
    const AstIndexContext context{.generation = 8, .snapshot_available = false};

    const auto result = workspaceSymbols(context, "", 100);

    CHECK(result.unresolved);
    CHECK(result.generation == 8);
    REQUIRE_FALSE(result.messages.empty());
    CHECK(result.messages.front().find("snapshot is unavailable") != std::string::npos);
}

TEST_CASE("AstIndex builds provider-facing symbol and reference views",
          "[analysis][semantic][ast-index][provider-view]") {
    SnapshotData data;
    const auto ready_identity = SemanticSymbolIdentity{
        .stable_id = "symbol|ready",
        .name = "ready",
        .kind = "Variable",
        .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                     .range = rangeAt(2, 8, 13)}};
    data.symbols_by_id.emplace("symbol|ready",
                               SnapshotIndexedSymbol{.identity = ready_identity,
                                                     .type_display = "logic"});
    data.references.push_back(SnapshotIndexedReference{
        .stable_id = "symbol|ready",
        .name = "ready",
        .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                     .range = rangeAt(4, 9, 14)},
        .is_declaration = false});
    data.references.push_back(SnapshotIndexedReference{
        .stable_id = "symbol|ready",
        .name = "ready",
        .location = ready_identity.location,
        .is_declaration = true});
    data.navigation_targets_by_id.emplace(
        "symbol|ready",
        SnapshotNavigationTargetFact{.identity = ready_identity, .type_display = "logic"});
    data.navigation_occurrences_by_uri["file:///workspace/top.sv"].occurrences = {
        SnapshotNavigationOccurrence{.stable_id = "symbol|ready",
                                     .location = data.references[0].location,
                                     .has_type_display = true},
        SnapshotNavigationOccurrence{.stable_id = "symbol|ready",
                                     .location = ready_identity.location,
                                     .is_declaration = true,
                                     .has_type_display = true}};
    data.navigation_occurrences_by_symbol["symbol|ready"] =
        data.navigation_occurrences_by_uri["file:///workspace/top.sv"].occurrences;
    data.design_graph_binding_index.symbol_ids_by_uri_range.emplace(
        "file:///workspace/top.sv\x1f" "1:2:1:7", "symbol|ready");

    const auto view = buildAstIndexView(&data, 42);

    CHECK(view.generation == 42);
    CHECK(view.snapshot_available);
    REQUIRE(view.symbols.size() == 1);
    CHECK(view.symbols.front().stable_id == "symbol|ready");
    REQUIRE(view.navigation_targets_by_id.contains("symbol|ready"));
    CHECK(view.navigation_targets_by_id.at("symbol|ready").identity.name == "ready");
    REQUIRE(view.diagnostic_symbols_by_id.contains("symbol|ready"));
    CHECK(view.diagnostic_symbols_by_id.at("symbol|ready").type_display == "logic");
    REQUIRE(view.design_graph_symbols_by_id.contains("symbol|ready"));
    REQUIRE(view.navigation_occurrences_by_uri.contains("file:///workspace/top.sv"));
    CHECK(std::any_of(view.navigation_occurrences_by_uri.at("file:///workspace/top.sv").occurrences.begin(),
                      view.navigation_occurrences_by_uri.at("file:///workspace/top.sv").occurrences.end(),
                      [](const SnapshotNavigationOccurrence& occurrence) {
                          return occurrence.is_declaration;
                      }));
    REQUIRE(view.design_graph_binding_index.symbol_ids_by_uri_range.contains(
        "file:///workspace/top.sv\x1f" "1:2:1:7"));
    CHECK(view.design_graph_binding_index.symbol_ids_by_uri_range.at(
              "file:///workspace/top.sv\x1f" "1:2:1:7") == "symbol|ready");
}

TEST_CASE("AstIndex prefers typed same-range symbol references deterministically",
          "[analysis][semantic][ast-index][lookup][types]") {
    SnapshotData data;
    const auto location = SemanticLocation{.uri = "file:///workspace/typed.sv",
                                           .range = rangeAt(1, 26, 30)};
    const auto untyped_identity = SemanticSymbolIdentity{.stable_id = "symbol|data|port",
                                                         .name = "data",
                                                         .kind = "Port",
                                                         .location = location};
    const auto typed_identity = SemanticSymbolIdentity{.stable_id = "symbol|data|internal",
                                                       .name = "data",
                                                       .kind = "Port",
                                                       .location = location};

    data.symbols_by_id.emplace(untyped_identity.stable_id,
                               SnapshotIndexedSymbol{.identity = untyped_identity,
                                                     .symbol = nullptr,
                                                     .type_display = {}});
    data.symbols_by_id.emplace(typed_identity.stable_id,
                               SnapshotIndexedSymbol{.identity = typed_identity,
                                                     .symbol = nullptr,
                                                     .type_display = "logic [WIDTH-1:0]"});
    data.references.push_back(SnapshotIndexedReference{.stable_id = untyped_identity.stable_id,
                                                       .name = "data",
                                                       .location = location,
                                                       .is_declaration = true});
    data.references.push_back(SnapshotIndexedReference{.stable_id = typed_identity.stable_id,
                                                       .name = "data",
                                                       .location = location,
                                                       .is_declaration = true});
    data.reference_occurrences_by_uri[location.uri] =
        SnapshotReferenceOccurrenceIndex{.reference_indexes = {0, 1},
                                         .prefix_max_end_ranges = {location.range, location.range}};

    const auto id = symbolIdAtLocation(data, "file:///workspace/typed.sv", 1, 28);

    REQUIRE(id.has_value());
    CHECK(*id == typed_identity.stable_id);
}

TEST_CASE("AstIndex builds provider-facing graph, diagnostic, and signature views",
          "[analysis][semantic][ast-index][provider-view]") {
    SnapshotData data;
    data.modules_by_name.emplace("child",
                                 ModuleDefinition{.name = "child",
                                                  .kind = "module",
                                                  .range = rangeAt(0, 0, 48),
                                                  .selection_range = rangeAt(0, 7, 12),
                                                  .port_details = {SchematicPort{.name = "clk",
                                                                                 .direction = "input",
                                                                                 .width_text = "logic",
                                                                                 .range = rangeAt(0, 13, 28),
                                                                                 .selection_range = rangeAt(0, 25, 28)}}});
    data.module_uris_by_name.emplace("child", "file:///workspace/child.sv");
    data.module_entries.push_back(SnapshotModuleEntry{.uri = "file:///workspace/child.sv",
                                                      .definition = data.modules_by_name.at("child")});
    data.assignment_edges_by_uri["file:///workspace/top.sv"] = {
        SnapshotAssignmentEdge{.from_symbol_id = "symbol|ready",
                               .to_symbol_id = "symbol|clk",
                               .location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                            .range = rangeAt(3, 2, 20)},
                               .expression_location = SemanticLocation{.uri = "file:///workspace/top.sv",
                                                                       .range = rangeAt(3, 17, 20)},
                               .expression = "clk"}};
    data.package_imports_by_uri["file:///workspace/top.sv"] = {
        PackageImport{.package_name = "defs",
                      .package_range = rangeAt(1, 9, 13),
                      .range = rangeAt(1, 2, 16)}};
    data.macros_by_uri["file:///workspace/top.sv"] = {
        MacroDefinition{.name = "READY",
                        .body = "1",
                        .range = rangeAt(0, 0, 15),
                        .selection_range = rangeAt(0, 8, 13)}};
    data.module_instances_by_uri["file:///workspace/top.sv"] = {
        SnapshotModuleInstance{.module_name = "child",
                               .instance_name = "u_child",
                               .instance_stable_id = {},
                               .uri = "file:///workspace/top.sv",
                               .range = rangeAt(4, 2, 26),
                               .selection_range = rangeAt(4, 8, 15),
                               .module_selection_range = rangeAt(4, 2, 7)}};

    const auto view = buildAstIndexView(&data, 77);

    CHECK(view.modules_by_name.contains("child"));
    CHECK(view.module_uris_by_name.at("child") == "file:///workspace/child.sv");
    REQUIRE(view.design_graph_module_entries.size() == 1);
    CHECK(view.assignment_edges_by_uri.at("file:///workspace/top.sv").size() == 1);
    CHECK(view.package_imports_by_uri.at("file:///workspace/top.sv").front().package_name == "defs");
    CHECK(view.macros_by_uri.at("file:///workspace/top.sv").front().name == "READY");
    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    CHECK(view.signature_module_instances_by_uri.at("file:///workspace/top.sv").front().module_name == "child");
}

TEST_CASE("AstIndex builds AST symbol, reference, and module instance indexes",
          "[analysis][semantic][ast-index][build]") {
    SnapshotBuildInput input{.generation = 21,
                             .documents = {{"file:///workspace/child.sv",
                                            SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                                                   .text = "module child; endmodule\n",
                                                                   .version = 1}},
                                           {"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  logic ready;\n"
                                                        "  assign ready = ready;\n"
                                                        "  child u_child();\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto& data = *output.data;
    const auto ready_id = symbolIdAtLocation(data, "file:///workspace/top.sv", 1, 9);
    REQUIRE(ready_id.has_value());
    bool ready_truncated = false;
    const auto ready_locations = locationsForSymbol(data, *ready_id, true, 100, ready_truncated);
    REQUIRE_FALSE(ready_truncated);
    CHECK(ready_locations.size() == 3);

    const auto child_id = findDefinitionSymbolId(data, "child");
    REQUIRE(child_id.has_value());
    REQUIRE(data.symbols_by_id.contains(*child_id));
    CHECK(data.symbols_by_id.at(*child_id).identity.location.uri == "file:///workspace/child.sv");

    const auto edges_it = data.implementation_edge_index.edges_by_target_stable_id.find(*child_id);
    REQUIRE(edges_it != data.implementation_edge_index.edges_by_target_stable_id.end());
    REQUIRE(edges_it->second.size() == 1);
    const auto& edge = data.implementation_edge_index.edges[edges_it->second.front()];
    CHECK(edge.target_stable_id == *child_id);
    CHECK(edge.location.uri == "file:///workspace/top.sv");
}

TEST_CASE("AstIndex builds URI-local reference occurrence ranges",
          "[analysis][semantic][ast-index][reference-occurrence]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto& index = output.data->reference_occurrences_by_uri;
    REQUIRE(index.contains("file:///workspace/top.sv"));
    REQUIRE(index.contains("file:///workspace/child.sv"));
    const auto& top = index.at("file:///workspace/top.sv");
    CHECK(top.reference_indexes.size() == top.prefix_max_end_ranges.size());
    CHECK_FALSE(top.reference_indexes.empty());
}

TEST_CASE("AstIndex reference occurrence lookup returns the exact token range",
          "[analysis][semantic][ast-index][reference-occurrence][range]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/top.sv",
                                                      2,
                                                      17);
    REQUIRE(lookup.has_value());
    CHECK(lookup->location.range.start_line == 2);
    CHECK(lookup->location.range.start_character == 17);
    CHECK(lookup->location.range.end_character == 22);
}

TEST_CASE("AstIndex reference occurrence lookup does not inspect unrelated URIs",
          "[analysis][semantic][ast-index][reference-occurrence][uri-local]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/missing.sv",
                                                      0,
                                                      0);
    CHECK_FALSE(lookup.has_value());
}

TEST_CASE("AstIndex reference occurrence lookup scans fewer than snapshot-wide references",
          "[analysis][semantic][ast-index][reference-occurrence][zero-global-scan]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/top.sv",
                                                      1,
                                                      9);
    REQUIRE(lookup.has_value());
    CHECK(lookup->scanned_occurrence_count > 0);
    CHECK(lookup->scanned_occurrence_count < output.data->references.size());
}

TEST_CASE("AstIndex classifies declaration reference occurrences",
          "[analysis][semantic][ast-index][reference-occurrence][role][declaration]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/top.sv",
                                                      1,
                                                      9);
    REQUIRE(lookup.has_value());
    CHECK(lookup->role == SemanticReferenceRole::Declaration);
}

TEST_CASE("AstIndex classifies assignment reads and writes",
          "[analysis][semantic][ast-index][reference-occurrence][role][read-write]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto write = referenceOccurrenceAtLocation(*output.data,
                                                     "file:///workspace/top.sv",
                                                     2,
                                                     10);
    const auto read = referenceOccurrenceAtLocation(*output.data,
                                                    "file:///workspace/top.sv",
                                                    2,
                                                    18);
    REQUIRE(write.has_value());
    REQUIRE(read.has_value());
    CHECK(write->role == SemanticReferenceRole::Write);
    CHECK(read->role == SemanticReferenceRole::Read);
}

TEST_CASE("AstIndex classifies module instantiation references",
          "[analysis][semantic][ast-index][reference-occurrence][role][instance]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/top.sv",
                                                      3,
                                                      3);
    REQUIRE(lookup.has_value());
    CHECK(lookup->role == SemanticReferenceRole::Instance);
}

TEST_CASE("AstIndex classifies declared type references",
          "[analysis][semantic][ast-index][reference-occurrence][role][type]") {
    SnapshotBuildInput input{
        .generation = 93,
        .documents = {{"file:///workspace/types.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/types.sv",
                                              .text = "package defs; typedef logic word_t; endpackage\n"
                                                      "module top; defs::word_t value; endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto lookup = referenceOccurrenceAtLocation(*output.data,
                                                      "file:///workspace/types.sv",
                                                      1,
                                                      20);
    REQUIRE(lookup.has_value());
    CHECK(lookup->role == SemanticReferenceRole::Type);
}

TEST_CASE("AstIndex builds direct caller and callee module edge maps",
          "[analysis][semantic][ast-index][module-call-edge]") {
    auto output = buildReferenceCallDesign();
    REQUIRE(output.data != nullptr);

    const auto& index = output.data->module_call_edge_index;
    REQUIRE(index.edges.size() == 1);
    const auto& edge = index.edges.front();
    CHECK(index.items_by_id.contains(edge.caller_item_id));
    CHECK(index.items_by_id.contains(edge.callee_item_id));
    REQUIRE(index.edges_by_caller_item_id.contains(edge.caller_item_id));
    REQUIRE(index.edges_by_callee_item_id.contains(edge.callee_item_id));
    CHECK(index.edges_by_caller_item_id.at(edge.caller_item_id) == std::vector<size_t>{0});
    CHECK(index.edges_by_callee_item_id.at(edge.callee_item_id) == std::vector<size_t>{0});
}

TEST_CASE("AstIndex keeps repeated module call edges independently addressable",
          "[analysis][semantic][ast-index][module-call-edge][repeated]") {
    auto output = buildReferenceCallDesign("module top;\n"
                                           "  child u_first();\n"
                                           "  child u_second();\n"
                                           "endmodule\n");
    REQUIRE(output.data != nullptr);

    const auto& edges = output.data->module_call_edge_index.edges;
    REQUIRE(edges.size() == 2);
    CHECK(edges[0].instance_id != edges[1].instance_id);
    CHECK(edges[0].selection_range.start_line != edges[1].selection_range.start_line);
}

TEST_CASE("AstIndex derives module signatures and schematic views from slang AST",
          "[analysis][semantic][ast-index][signature][schematic]") {
    SnapshotBuildInput input{
        .generation = 33,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                              .text = "module child(input logic clk, output logic [3:0] data);\n"
                                                      "endmodule\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  logic [3:0] data;\n"
                                                      "  child u_child(.clk(clk), .data(data));\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child = view.module_signatures_by_name.at("child");
    CHECK(child.definition.name == "child");
    REQUIRE(child.definition.port_details.size() == 2);
    CHECK(child.definition.port_details[0].name == "clk");
    CHECK(child.definition.port_details[0].direction == "input");
    CHECK(child.definition.port_details[1].name == "data");
    CHECK(child.definition.port_details[1].direction == "output");
    CHECK(child.definition.port_details[1].width_text.find("logic") != std::string::npos);

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top_schematic = view.module_signatures_by_name.at("top").schematic;
    REQUIRE(top_schematic.cells.size() == 1);
    CHECK(top_schematic.cells.front().name == "u_child");
    CHECK(top_schematic.cells.front().type == "child");
    REQUIRE(top_schematic.cells.front().connections.size() == 2);
    CHECK(top_schematic.cells.front().connections[0].port_name == "clk");
    CHECK(top_schematic.cells.front().connections[1].port_name == "data");
}

TEST_CASE("AstIndex derives module instances from slang AST rather than syntax model",
          "[analysis][semantic][ast-index][module-instance][no-fallback]") {
    SnapshotBuildInput input{
        .generation = 36,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                              .text = "module child(input logic clk, output logic ready);\n"
                                                      "endmodule\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  logic ready;\n"
                                                      "  child #(.WIDTH(4)) u_child(\n"
                                                      "    .clk(clk),\n"
                                                      "    .ready(ready)\n"
                                                      "  );\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto instances_it = view.module_instances_by_uri.find("file:///workspace/top.sv");
    REQUIRE(instances_it != view.module_instances_by_uri.end());
    const auto instance_it = std::find_if(instances_it->second.begin(),
                                          instances_it->second.end(),
                                          [](const SnapshotModuleInstance& candidate) {
                                              return candidate.module_name == "child" &&
                                                     candidate.instance_name == "u_child" &&
                                                     !candidate.target_stable_id.empty();
                                          });
    REQUIRE(instance_it != instances_it->second.end());
    const auto& instance = *instance_it;
    CHECK(instance.module_name == "child");
    CHECK(instance.instance_name == "u_child");
    CHECK_FALSE(instance.target_stable_id.empty());
    CHECK(instance.range.start_line == 3);
    CHECK(instance.range.end_line == 6);
    CHECK(instance.module_selection_range.start_line == 3);
    CHECK(instance.module_selection_range.start_character == 2);
    CHECK(instance.selection_range.start_line == 3);
    CHECK(instance.selection_range.start_character > instance.module_selection_range.start_character);

    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    REQUIRE(view.signature_module_instances_by_uri.at("file:///workspace/top.sv").size() == 1);
    const auto& inlay_instance = view.signature_module_instances_by_uri.at("file:///workspace/top.sv").front();
    REQUIRE(inlay_instance.connections.size() == 2);
    CHECK(inlay_instance.connections[0].port_name == "clk");
    CHECK(inlay_instance.connections[1].port_name == "ready");
}

TEST_CASE("AstIndex records modport-qualified interface port type display",
          "[analysis][semantic][ast-index][inlay][interface][modport]") {
    SnapshotBuildInput input{
        .generation = 37,
        .documents = {{"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "interface bus_if;\n"
                                                      "  logic ready;\n"
                                                      "  modport master(input ready);\n"
                                                      "endinterface\n"
                                                      "module top(bus_if.master bus);\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto& data = *output.data;
    const auto port_it = std::find_if(data.symbols_by_id.begin(),
                                      data.symbols_by_id.end(),
                                      [](const auto& entry) {
                                          const auto& indexed = entry.second;
                                          return indexed.identity.kind == "InterfacePort" &&
                                                 indexed.identity.name == "bus";
                                      });
    REQUIRE(port_it != data.symbols_by_id.end());
    CHECK(port_it->second.type_display == "bus_if.master");
}

TEST_CASE("AstIndex derives parameter override inlay facts from AST module parameters",
          "[analysis][semantic][ast-index][inlay][parameter]") {
    SnapshotBuildInput input{
        .generation = 38,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/child.sv",
                           .text = "module child #(parameter int WIDTH = 8, parameter int DEPTH = 4) "
                                   "(input logic clk);\n"
                                   "endmodule\n",
                           .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  logic clk;\n"
                                                      "  child #(.WIDTH(16), .DEPTH(2)) u_child(.clk(clk));\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_definition = view.module_signatures_by_name.at("child").definition;
    CHECK(std::any_of(child_definition.parameter_details.begin(),
                      child_definition.parameter_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "WIDTH" && port.direction == "parameter" &&
                                 port.width_text == "int";
                      }));
    CHECK(std::any_of(child_definition.parameter_details.begin(),
                      child_definition.parameter_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "DEPTH" && port.direction == "parameter" &&
                                 port.width_text == "int";
                      }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top_cells = view.module_signatures_by_name.at("top").schematic.cells;
    REQUIRE(top_cells.size() == 1);
    REQUIRE(top_cells.front().connections.size() == 1);
    CHECK(top_cells.front().connections.front().port_name == "clk");

    REQUIRE(view.signature_module_instances_by_uri.contains("file:///workspace/top.sv"));
    const auto& inlay_instances = view.signature_module_instances_by_uri.at("file:///workspace/top.sv");
    REQUIRE(inlay_instances.size() == 1);
    const auto& inlay_connections = inlay_instances.front().connections;
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "WIDTH" && connection.signal == "16" &&
                                 connection.range.start_line == 2;
                      }));
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "DEPTH" && connection.signal == "2" &&
                                 connection.range.start_line == 2;
                      }));
    CHECK(std::any_of(inlay_connections.begin(),
                      inlay_connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "clk" && connection.signal == "clk";
                      }));
}

TEST_CASE("AstIndex exposes parameterized cross-module cone inputs to design graph",
          "[analysis][semantic][ast-index][cone][parameterized][cross-module]") {
    SnapshotBuildInput input{
        .generation = 41,
        .documents = {{"file:///workspace/cone.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/cone.sv",
                           .text =
                               "module child #(parameter int WIDTH = 8) "
                               "(input logic [WIDTH-1:0] in, output logic [WIDTH-1:0] out);\n"
                               "  assign out = in;\n"
                               "endmodule\n"
                               "module top;\n"
                               "  logic [3:0] a;\n"
                               "  logic [3:0] y;\n"
                               "  child #(.WIDTH(4)) u_child(.in(a), .out(y));\n"
                               "endmodule\n",
                           .version = 1,
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_ports = view.module_signatures_by_name.at("child").definition.port_details;
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "in" && port.direction == "input";
    }));
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "out" && port.direction == "output";
    }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    REQUIRE(cells.size() == 1);
    CHECK(std::any_of(cells.front().connections.begin(),
                      cells.front().connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "a";
                      }));
    CHECK(std::any_of(cells.front().connections.begin(),
                      cells.front().connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "y";
                      }));

    REQUIRE(view.assignment_edges_by_uri.contains("file:///workspace/cone.sv"));
    CHECK(std::any_of(view.assignment_edges_by_uri.at("file:///workspace/cone.sv").begin(),
                      view.assignment_edges_by_uri.at("file:///workspace/cone.sv").end(),
                      [&](const SnapshotAssignmentEdge& edge) {
                          const auto from = view.design_graph_symbols_by_id.at(edge.from_symbol_id)
                                                .identity.name;
                          const auto to = view.design_graph_symbols_by_id.at(edge.to_symbol_id)
                                              .identity.name;
                          return from == "out" && to == "in";
                      }));
}

TEST_CASE("AstIndex attaches self-cycle and unresolved instance candidates to module definitions",
          "[analysis][semantic][ast-index][module-instance][hierarchy][no-fallback]") {
    SnapshotBuildInput input{.generation = 37,
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  top u_self();\n"
                                                        "  missing u_missing();\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.modules_by_name.contains("top"));
    const auto& top = view.modules_by_name.at("top");
    REQUIRE(top.instances.size() == 2);
    CHECK(top.instances[0].module_name == "top");
    CHECK(top.instances[0].instance_name == "u_self");
    CHECK(top.instances[1].module_name == "missing");
    CHECK(top.instances[1].instance_name == "u_missing");
}

TEST_CASE("AstIndex promotes generated module instances into schematic cells",
          "[analysis][semantic][ast-index][schematic][generate][no-fallback]") {
    SnapshotBuildInput input{.generation = 38,
                             .config = SemanticEngineConfig{.top_modules = {"top"}},
                             .documents = {{"file:///workspace/generated.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated.sv",
                                                .text = "module leaf(input logic clk);\n"
                                                        "endmodule\n"
                                                        "module top;\n"
                                                        "  logic clk;\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      leaf u_leaf(.clk(clk));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("leaf"));
    CHECK(std::any_of(view.module_signatures_by_name.at("leaf").definition.port_details.begin(),
                      view.module_signatures_by_name.at("leaf").definition.port_details.end(),
                      [](const SchematicPort& port) {
                          return port.name == "clk" && port.direction == "input";
                      }));
    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top = view.module_signatures_by_name.at("top");
    CHECK(std::any_of(top.definition.instances.begin(),
                      top.definition.instances.end(),
                      [](const ModuleInstantiation& instance) {
                          return instance.module_name == "leaf" && instance.instance_name == "u_leaf";
                      }));
    CHECK(std::any_of(top.schematic.cells.begin(),
                      top.schematic.cells.end(),
                      [](const SchematicCell& cell) {
                          return cell.name == "u_leaf" && cell.type == "leaf" && cell.kind == "module";
                      }));
    CHECK(std::any_of(top.schematic.cells.begin(),
                      top.schematic.cells.end(),
                      [](const SchematicCell& cell) {
                          return cell.name == "u_leaf" &&
                                 std::any_of(cell.connections.begin(),
                                             cell.connections.end(),
                                             [](const SchematicConnection& connection) {
                                                 return connection.port_name == "clk" &&
                                                        connection.signal == "clk";
                                             });
                      }));
}

TEST_CASE("AstIndex preserves elaborated generated instances as distinct call edges",
          "[analysis][semantic][ast-index][module-call-edge][generate]") {
    SnapshotBuildInput input{.generation = 92,
                             .config = SemanticEngineConfig{.top_modules = {"top"}},
                             .documents = {{"file:///workspace/generated-call.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated-call.sv",
                                                .text = "module child; endmodule\n"
                                                        "module top;\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "  for (i = 0; i < 2; i = i + 1) begin : g\n"
                                                        "    child u_child();\n"
                                                        "  end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto& instances = output.data->module_instances_by_uri.at(
        "file:///workspace/generated-call.sv");
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].instance_stable_id != instances[1].instance_stable_id);
    const auto& edges = output.data->module_call_edge_index.edges;
    REQUIRE(edges.size() == 2);
    CHECK(edges[0].instance_id != edges[1].instance_id);
}

TEST_CASE("AstIndex preserves generated schematic port width and connection facts",
          "[analysis][semantic][ast-index][schematic][generate][port-net][no-fallback]") {
    SnapshotBuildInput input{.generation = 40,
                             .config = SemanticEngineConfig{.top_modules = {"top"}},
                             .documents = {{"file:///workspace/generated-port-net.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated-port-net.sv",
                                                .text = "module child(input logic [3:0] in,\n"
                                                        "             output logic [3:0] out);\n"
                                                        "endmodule\n"
                                                        "module top(input logic [3:0] data_i,\n"
                                                        "           output logic [3:0] data_o);\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      child u_child(.in(data_i), .out(data_o));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("child"));
    const auto& child_ports = view.module_signatures_by_name.at("child").definition.port_details;
    std::string child_port_dump;
    for (const auto& port : child_ports) {
        child_port_dump += port.name + ":" + port.direction + ":" + port.width_text + ";";
    }
    INFO(child_port_dump);
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "in" && port.direction == "input" && port.width_text == "logic [3:0]";
    }));
    CHECK(std::any_of(child_ports.begin(), child_ports.end(), [](const SchematicPort& port) {
        return port.name == "out" && port.direction == "output" && port.width_text == "logic [3:0]";
    }));

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& top = view.module_signatures_by_name.at("top");
    CHECK(std::any_of(top.schematic.ports.begin(), top.schematic.ports.end(), [](const SchematicPort& port) {
        return port.name == "data_i" && port.direction == "input" && port.width_text == "logic [3:0]";
    }));
    CHECK(std::any_of(top.schematic.ports.begin(), top.schematic.ports.end(), [](const SchematicPort& port) {
        return port.name == "data_o" && port.direction == "output" && port.width_text == "logic [3:0]";
    }));

    const auto cell_it = std::find_if(top.schematic.cells.begin(),
                                      top.schematic.cells.end(),
                                      [](const SchematicCell& cell) {
                                          return cell.name == "u_child" && cell.type == "child";
                                      });
    REQUIRE(cell_it != top.schematic.cells.end());
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "data_i";
                      }));
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "data_o";
                      }));
}

TEST_CASE("AstIndex exposes generated-only instance connection facts for backward cone",
          "[analysis][semantic][ast-index][cone][generate][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .config = SemanticEngineConfig{.top_modules = {"top"}},
                             .documents = {{"file:///workspace/generated-cone.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/generated-cone.sv",
                                                .text = "module child(input logic in, output logic out);\n"
                                                        "  assign out = in;\n"
                                                        "endmodule\n"
                                                        "module top;\n"
                                                        "  logic a;\n"
                                                        "  logic y;\n"
                                                        "  genvar i;\n"
                                                        "  generate\n"
                                                        "    for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                        "      child u_child(.in(a), .out(y));\n"
                                                        "    end\n"
                                                        "  endgenerate\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    const auto cell_it = std::find_if(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_child" && cell.type == "child";
    });
    REQUIRE(cell_it != cells.end());
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "in" && connection.signal == "a";
                      }));
    CHECK(std::any_of(cell_it->connections.begin(),
                      cell_it->connections.end(),
                      [](const SchematicConnection& connection) {
                          return connection.port_name == "out" && connection.signal == "y";
                      }));

    const auto seed = std::find_if(output.data->assignment_edge_seeds.begin(),
                                   output.data->assignment_edge_seeds.end(),
                                   [](const SnapshotAssignmentEdgeSeed& candidate) {
                                       return candidate.left_expression == "out" &&
                                              candidate.left_symbol_names == std::vector<std::string>{"out"};
                                   });
    REQUIRE(seed != output.data->assignment_edge_seeds.end());
    REQUIRE(seed->data_sources.size() == 1);
    CHECK(seed->data_sources.front().source_symbol_names == std::vector<std::string>{"in"});

    const auto edge_it = view.assignment_edges_by_uri.find("file:///workspace/generated-cone.sv");
    REQUIRE(edge_it != view.assignment_edges_by_uri.end());
    CHECK(std::any_of(edge_it->second.begin(), edge_it->second.end(), [&](const SnapshotAssignmentEdge& edge) {
        const auto from_it = view.design_graph_symbols_by_id.find(edge.from_symbol_id);
        const auto to_it = view.design_graph_symbols_by_id.find(edge.to_symbol_id);
        return from_it != view.design_graph_symbols_by_id.end() &&
               to_it != view.design_graph_symbols_by_id.end() &&
               from_it->second.identity.name == "out" && to_it->second.identity.name == "in";
    }));
}

TEST_CASE("AstIndex derives primitive and assignment schematic cells from slang AST",
          "[analysis][semantic][ast-index][schematic][no-fallback]") {
    SnapshotBuildInput input{.generation = 34,
                             .documents = {{"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top(input logic a, input logic b, input logic sel, output logic y);\n"
                                                        "  logic n1;\n"
                                                        "  and u_and(n1, a, b);\n"
                                                        "  assign y = sel ? n1 : (a | b);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    REQUIRE(view.module_signatures_by_name.contains("top"));
    const auto& cells = view.module_signatures_by_name.at("top").schematic.cells;
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.name == "u_and" && cell.kind == "and";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "or";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "mux";
    }));
    CHECK(std::any_of(cells.begin(), cells.end(), [](const SchematicCell& cell) {
        return cell.kind == "buf";
    }));

    const auto pins_it = output.data->design_graph_binding_index.schematic_cell_pins_by_module.find("top");
    REQUIRE(pins_it != output.data->design_graph_binding_index.schematic_cell_pins_by_module.end());
    CHECK(std::any_of(pins_it->second.begin(), pins_it->second.end(), [](const auto& fact) {
        return fact.cell_id == "u_and" && fact.pin_name == "P0" &&
               fact.pin_direction == SnapshotSchematicCellPinDirection::Output &&
               fact.display_label == "n1" && !fact.unresolved;
    }));
    CHECK(std::any_of(pins_it->second.begin(), pins_it->second.end(), [](const auto& fact) {
        return fact.cell_id == "u_and" && fact.pin_name == "P1" &&
               fact.pin_direction == SnapshotSchematicCellPinDirection::Input &&
               fact.display_label == "a" && !fact.unresolved;
    }));
    CHECK(std::any_of(pins_it->second.begin(), pins_it->second.end(), [](const auto& fact) {
        return fact.cell_kind == SnapshotSchematicCellKind::Assignment &&
               fact.pin_direction == SnapshotSchematicCellPinDirection::Control &&
               fact.pin_name == "S" && fact.display_label == "sel" && !fact.unresolved;
    }));
    const auto edges_it = view.assignment_edges_by_uri.find("file:///workspace/top.sv");
    REQUIRE(edges_it != view.assignment_edges_by_uri.end());
    const auto edgeTargetNames = [&](std::string_view expected,
                                     SnapshotConeEdgeKind kind,
                                     SnapshotConeSourceRole role,
                                     std::string_view expression) {
        return std::any_of(edges_it->second.begin(),
                           edges_it->second.end(),
                           [&](const SnapshotAssignmentEdge& edge) {
                               const auto from_it = view.design_graph_symbols_by_id.find(edge.from_symbol_id);
                               const auto to_it = view.design_graph_symbols_by_id.find(edge.to_symbol_id);
                               return from_it != view.design_graph_symbols_by_id.end() &&
                                      to_it != view.design_graph_symbols_by_id.end() &&
                                      from_it->second.identity.name == "y" &&
                                      to_it->second.identity.name == expected && edge.kind == kind &&
                                      edge.source_role == role && edge.expression == expression;
                           });
    };
    CHECK(edgeTargetNames("sel",
                          SnapshotConeEdgeKind::ControlDependency,
                          SnapshotConeSourceRole::Control,
                          "sel"));
    CHECK(edgeTargetNames("n1",
                          SnapshotConeEdgeKind::Assignment,
                          SnapshotConeSourceRole::Data,
                          "n1"));
    CHECK(edgeTargetNames("a",
                          SnapshotConeEdgeKind::Assignment,
                          SnapshotConeSourceRole::Data,
                          "a | b"));
    CHECK(edgeTargetNames("b",
                          SnapshotConeEdgeKind::Assignment,
                          SnapshotConeSourceRole::Data,
                          "a | b"));
}

TEST_CASE("AstIndex derives typed N-input and bidirectional primitive pins",
          "[analysis][semantic][ast-index][schematic][primitive][direction][no-fallback]") {
    SnapshotBuildInput input{.generation = 340,
                             .documents = {{"file:///workspace/primitive-directions.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/primitive-directions.sv",
                                                .text = "module top(input wire a, input wire b, output wire y);\n"
                                                        "  or u_or(y, a, b);\n"
                                                        "  tran u_tran(a, b);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto pins = output.data->design_graph_binding_index.schematic_cell_pins_by_module.at("top");
    CHECK(std::any_of(pins.begin(), pins.end(), [](const auto& fact) {
        return fact.cell_id == "u_or" && fact.pin_name == "P0" &&
               fact.pin_direction == SnapshotSchematicCellPinDirection::Output && fact.display_label == "y";
    }));
    CHECK(std::any_of(pins.begin(), pins.end(), [](const auto& fact) {
        return fact.cell_id == "u_or" && fact.pin_name == "P1" &&
               fact.pin_direction == SnapshotSchematicCellPinDirection::Input && fact.display_label == "a";
    }));
    CHECK(std::count_if(pins.begin(), pins.end(), [](const auto& fact) {
              return fact.cell_id == "u_tran" &&
                     fact.pin_direction == SnapshotSchematicCellPinDirection::Inout;
          }) == 2);
}
TEST_CASE("AstIndex preserves literal primitive pins as explicit partial facts",
          "[analysis][semantic][ast-index][schematic][primitive][literal][partial][no-fallback]") {
    SnapshotBuildInput input{.generation = 341,
                             .documents = {{"file:///workspace/primitive-literal.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/primitive-literal.sv",
                                                .text = "module top(input logic a, output logic y);\n"
                                                        "  and u_and(y, a, 1);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto pins = output.data->design_graph_binding_index.schematic_cell_pins_by_module.at("top");
    CHECK(std::any_of(pins.begin(), pins.end(), [](const auto& fact) {
        return fact.cell_id == "u_and" && fact.pin_name == "P2" && fact.literal &&
               fact.net_symbol_id.empty() && !fact.unresolved;
    }));
}
TEST_CASE("AstIndex derives declared type references from slang AST",
          "[analysis][semantic][ast-index][type-definition][no-fallback]") {
    SnapshotBuildInput input{.generation = 35,
                             .documents = {{"file:///workspace/types.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/types.sv",
                                                .text = "module top;\n"
                                                        "  typedef logic [3:0] nibble_t;\n"
                                                        "  nibble_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    auto locations = typeDefinitionLocationsAt(view, "file:///workspace/types.sv", 2, 3);

    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == "file:///workspace/types.sv");
    CHECK(locations.front().range.start_line == 1);
    CHECK(locations.front().range.start_character == 22);
    CHECK(locations.front().range.end_character == 30);
}

TEST_CASE("AstIndex resolves typedef alias chain type references by indexed type facts",
          "[analysis][semantic][ast-index][type-definition][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 36,
                             .documents = {{"file:///workspace/alias.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/alias.sv",
                                                .text = "package defs;\n"
                                                        "  typedef logic [7:0] base_t;\n"
                                                        "  typedef base_t alias_t;\n"
                                                        "endpackage\n"
                                                        "module top;\n"
                                                        "  import defs::*;\n"
                                                        "  alias_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto alias_locations = typeDefinitionLocationsAt(view, "file:///workspace/alias.sv", 6, 3);
    REQUIRE(alias_locations.size() == 1);
    CHECK(alias_locations.front().uri == "file:///workspace/alias.sv");
    CHECK(alias_locations.front().range.start_line == 2);
    CHECK(alias_locations.front().range.start_character == 17);
    CHECK(alias_locations.front().range.end_character == 24);

    const auto base_locations = typeDefinitionLocationsAt(view, "file:///workspace/alias.sv", 2, 10);
    REQUIRE(base_locations.size() == 1);
    CHECK(base_locations.front().range.start_line == 1);
    CHECK(base_locations.front().range.start_character == 22);
    CHECK(base_locations.front().range.end_character == 28);
}

TEST_CASE("AstIndex alias lookup does not resolve unimported package members by name",
          "[analysis][semantic][ast-index][type-definition][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 37,
                             .documents = {{"file:///workspace/defs.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/defs.sv",
                                                .text = "package defs;\n"
                                                        "  typedef logic exported_t;\n"
                                                        "endpackage\n",
                                                .version = 1}},
                                           {"file:///workspace/top.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/top.sv",
                                                .text = "module top;\n"
                                                        "  exported_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto locations = typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 3);

    CHECK(locations.empty());
}

TEST_CASE("AstIndex resolves package-qualified alias chains without leaking same-file packages",
          "[analysis][semantic][ast-index][type-definition][package][alias][no-fallback]") {
    SnapshotBuildInput input{.generation = 38,
                             .documents = {{"file:///workspace/packages.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/packages.sv",
                                                .text = "package other;\n"
                                                        "  typedef logic [3:0] alias_t;\n"
                                                        "endpackage\n"
                                                        "package defs;\n"
                                                        "  typedef logic [7:0] base_t;\n"
                                                        "  typedef base_t alias_t;\n"
                                                        "endpackage\n"
                                                        "module top;\n"
                                                        "  defs::alias_t value;\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto package_locations = typeDefinitionLocationsAt(view, "file:///workspace/packages.sv", 8, 10);

    REQUIRE(package_locations.size() == 1);
    CHECK(package_locations.front().uri == "file:///workspace/packages.sv");
    CHECK(package_locations.front().range.start_line == 5);
    CHECK(package_locations.front().range.start_character == 17);
    CHECK(package_locations.front().range.end_character == 24);
}

TEST_CASE("AstIndex resolves interface modport type references from interface ports",
          "[analysis][semantic][ast-index][type-definition][interface][modport][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/if-port.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/if-port.sv",
                                                .text = "interface bus_if;\n"
                                                        "  logic ready;\n"
                                                        "  modport master(input ready);\n"
                                                        "endinterface\n"
                                                        "module top(bus_if.master bus);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto interface_locations = typeDefinitionLocationsAt(view, "file:///workspace/if-port.sv", 4, 12);
    REQUIRE(interface_locations.size() == 1);
    CHECK(interface_locations.front().range.start_line == 0);
    CHECK(interface_locations.front().range.start_character == 10);
    CHECK(interface_locations.front().range.end_character == 16);

    const auto modport_locations = typeDefinitionLocationsAt(view, "file:///workspace/if-port.sv", 4, 18);
    REQUIRE(modport_locations.size() == 1);
    CHECK(modport_locations.front().range.start_line == 2);
    CHECK(modport_locations.front().range.start_character == 10);
    CHECK(modport_locations.front().range.end_character == 16);
}

TEST_CASE("AstIndex builds deterministic interface modport member bindings",
          "[analysis][semantic][ast-index][interface][modport][binding][direction]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/if-binding.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/if-binding.sv",
                                                .text = "interface bus_if;\n"
                                                        "  logic ready;\n"
                                                        "  logic valid;\n"
                                                        "  modport master(input ready, output valid);\n"
                                                        "endinterface\n"
                                                        "module top(bus_if.master bus);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.interface_modport_binding_index.modports_by_stable_id.size() == 1);
    REQUIRE(view.interface_modport_binding_index.ports_by_stable_id.size() == 1);
    CHECK(view.interface_modport_binding_index.member_count == 2);
    CHECK(view.interface_modport_binding_index.resolved_port_binding_count == 1);

    const auto& port = view.interface_modport_binding_index.ports_by_stable_id.begin()->second;
    REQUIRE(port.resolved);
    REQUIRE_FALSE(port.modport_stable_id.empty());
    CHECK(port.interface_type_location.range.start_line == 5);
    CHECK(port.interface_type_location.range.start_character == 11);
    CHECK(port.modport_location.range.start_line == 5);
    CHECK(port.modport_location.range.start_character == 18);

    const auto& members = view.interface_modport_binding_index.members_by_modport_stable_id.at(port.modport_stable_id);
    REQUIRE(members.size() == 2);
    CHECK(members[0].name == "ready");
    CHECK(members[0].direction == SnapshotGraphPortDirection::Input);
    CHECK(members[1].name == "valid");
    CHECK(members[1].direction == SnapshotGraphPortDirection::Output);
}

TEST_CASE("AstIndex rejects unresolved interface modports without a type-definition fallback",
          "[analysis][semantic][ast-index][type-definition][interface][modport][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/if-unresolved.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/if-unresolved.sv",
                                                .text = "interface bus_if;\n"
                                                        "  modport master;\n"
                                                        "endinterface\n"
                                                        "module top(bus_if.unknown bus);\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.interface_modport_binding_index.ports_by_stable_id.size() == 1);
    CHECK_FALSE(view.interface_modport_binding_index.ports_by_stable_id.begin()->second.resolved);
    CHECK(typeDefinitionLocationsAt(view, "file:///workspace/if-unresolved.sv", 3, 18).empty());
}

TEST_CASE("AstIndex derives function and task signature calls from slang AST",
          "[analysis][semantic][ast-index][signature][function][task][no-fallback]") {
    SnapshotBuildInput input{.generation = 39,
                             .documents = {{"file:///workspace/calls.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/calls.sv",
                                                .text = "module top;\n"
                                                        "  function automatic int add(input int lhs, input int rhs);\n"
                                                        "    return lhs + rhs;\n"
                                                        "  endfunction\n"
                                                        "  task automatic emit(input logic ready);\n"
                                                        "  endtask\n"
                                                        "  initial begin\n"
                                                        "    int value = add(1, 2);\n"
                                                        "    emit(1'b1);\n"
                                                        "  end\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    REQUIRE(view.callable_invocations_by_uri.contains("file:///workspace/calls.sv"));
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/calls.sv");

    const auto add_call = std::find_if(calls.begin(), calls.end(), [](const CallableInvocationFact& call) {
        return call.name == "add";
    });
    REQUIRE(add_call != calls.end());
    CHECK(add_call->kind == "function");
    CHECK(add_call->return_type == "int");
    CHECK(add_call->range.start_line == 7);
    CHECK(add_call->range.start_character == 16);
    CHECK(add_call->selection_range.start_line == 7);
    CHECK(add_call->selection_range.start_character == 16);
    CHECK(add_call->selection_range.end_character == 19);
    REQUIRE(add_call->parameters.size() == 2);
    CHECK(add_call->parameters[0] == "input int lhs");
    CHECK(add_call->parameters[1] == "input int rhs");

    const auto emit_call = std::find_if(calls.begin(), calls.end(), [](const CallableInvocationFact& call) {
        return call.name == "emit";
    });
    REQUIRE(emit_call != calls.end());
    CHECK(emit_call->kind == "task");
    CHECK(emit_call->range.start_line == 8);
    CHECK(emit_call->range.start_character == 4);
    CHECK(emit_call->selection_range.start_line == 8);
    CHECK(emit_call->selection_range.start_character == 4);
    CHECK(emit_call->selection_range.end_character == 8);
    REQUIRE(emit_call->parameters.size() == 1);
    CHECK(emit_call->parameters[0] == "input logic ready");
}

TEST_CASE("AstIndex scope visibility exposes parent context and URI-local inlay facts",
          "[analysis][semantic][ast-index][scope-visibility][inlay]") {
    SnapshotBuildInput input{.generation = 40,
                             .documents = {{"file:///workspace/scopes.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/scopes.sv",
                                                .text = "module top;\n"
                                                        "  logic ready;\n"
                                                        "  initial begin : outer_scope\n"
                                                        "    logic local_ready;\n"
                                                        "    begin : inner_scope\n"
                                                        "      local_ready = ready;\n"
                                                        "    end\n"
                                                        "  end\n"
                                                        "endmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto scopes_it = view.scope_visibility_by_uri.find("file:///workspace/scopes.sv");
    REQUIRE(scopes_it != view.scope_visibility_by_uri.end());
    CHECK(std::all_of(scopes_it->second.begin(), scopes_it->second.end(), [](const auto& scope) {
        return !scope.context_kind.empty();
    }));
    CHECK(std::any_of(scopes_it->second.begin(), scopes_it->second.end(), [](const auto& scope) {
        return !scope.parent_stable_id.empty();
    }));

    const auto inlay_it = view.inlay_symbols_by_uri.find("file:///workspace/scopes.sv");
    REQUIRE(inlay_it != view.inlay_symbols_by_uri.end());
    CHECK(std::all_of(inlay_it->second.begin(), inlay_it->second.end(), [](const auto& symbol) {
        return symbol.identity.location.uri == "file:///workspace/scopes.sv";
    }));
}

TEST_CASE("AstIndex workspace visibility indexes class declarations for prefix completion",
          "[analysis][semantic][ast-index][completion][prefix-index]") {
    SnapshotBuildInput input{.generation = 41,
                             .documents = {{"file:///workspace/class-prefix.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/class-prefix.sv",
                                                .text = "class packet;\nendclass\nmodule top;\n  pac\nendmodule\n",
                                                .version = 1,
                                                .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::any_of(view.workspace_visibility.begin(),
                      view.workspace_visibility.end(),
                      [](const auto& candidate) {
                          return candidate.identity.name == "packet" &&
                                 candidate.identity.kind == "ClassType";
                      }));
}
TEST_CASE("AstIndex document visibility retains AST explicit imports during recovery",
          "[analysis][semantic][ast-index][completion][import][recovery]") {
    SnapshotBuildInput input{.generation = 42,
                             .documents = {
                                 {"file:///workspace/defs.sv",
                                  SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                                         .text = "package defs; typedef logic token_t; endpackage\n",
                                                         .version = 1}},
                                 {"file:///workspace/user.sv",
                                  SemanticEngineDocument{.uri = "file:///workspace/user.sv",
                                                         .text = "module top;\n"
                                                                 "  import defs::token_t;\n"
                                                                 "  tok\n"
                                                                 "endmodule\n",
                                                         .version = 1,
                                                         .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto candidates = view.document_visibility_by_uri.find("file:///workspace/user.sv");
    REQUIRE(candidates != view.document_visibility_by_uri.end());
    CHECK(std::any_of(candidates->second.begin(), candidates->second.end(), [](const auto& candidate) {
        return candidate.identity.name == "token_t" &&
               candidate.origin == SnapshotVisibilityOrigin::ExplicitImport;
    }));
}
TEST_CASE("AstIndex derives array-of-struct member completion facts from declared types",
          "[analysis][semantic][ast-index][completion][member][array]") {
    SnapshotBuildInput input{
        .generation = 40,
        .documents = {{"file:///workspace/array-struct.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/array-struct.sv",
                                              .text = "typedef struct packed {\n"
                                                      "  logic status_valid;\n"
                                                      "  logic status_ready;\n"
                                                      "  logic payload;\n"
                                                      "} packet_t;\n"
                                                      "module top;\n"
                                                      "  packet_t lanes [2];\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/array-struct.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view name) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == "lanes" &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == "Field";
                           });
    };

    CHECK(has_member("status_valid"));
    CHECK(has_member("status_ready"));
    CHECK(has_member("payload"));
}

TEST_CASE("AstIndex derives class property and method member completion facts from declared types",
          "[analysis][semantic][ast-index][completion][member][class]") {
    SnapshotBuildInput input{
        .generation = 41,
        .documents = {{"file:///workspace/class-member.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/class-member.sv",
                                              .text = "class packet;\n"
                                                      "  int size_bytes;\n"
                                                      "  int size_words;\n"
                                                      "  function int size_sum();\n"
                                                      "    return size_bytes + size_words;\n"
                                                      "  endfunction\n"
                                                      "endclass\n"
                                                      "module top;\n"
                                                      "  packet pkt;\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/class-member.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view name, std::string_view kind) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == "pkt" &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == kind;
                           });
    };

    CHECK(has_member("size_bytes", "Field"));
    CHECK(has_member("size_words", "Field"));
    CHECK(has_member("size_sum", "Subroutine"));
}

TEST_CASE("AstIndex derives interface instance and modport member completion facts",
          "[analysis][semantic][ast-index][completion][member][interface][modport]") {
    SnapshotBuildInput input{
        .generation = 42,
        .documents = {{"file:///workspace/interface-member.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/interface-member.sv",
                                              .text = "interface bus_if;\n"
                                                      "  logic status_valid;\n"
                                                      "  logic status_ready;\n"
                                                      "  logic payload;\n"
                                                      "  modport master(input status_ready, output status_valid);\n"
                                                      "endinterface\n"
                                                      "module consumer(bus_if.master master_bus);\n"
                                                      "endmodule\n"
                                                      "module top;\n"
                                                      "  bus_if bus();\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto completions_it = view.member_completions_by_uri.find("file:///workspace/interface-member.sv");
    REQUIRE(completions_it != view.member_completions_by_uri.end());

    const auto has_member = [&](std::string_view qualifier,
                                std::string_view name,
                                std::string_view kind) {
        return std::any_of(completions_it->second.begin(),
                           completions_it->second.end(),
                           [&](const SnapshotMemberCompletion& completion) {
                               return completion.qualifier == qualifier &&
                                      completion.identity.name == name &&
                                      completion.identity.kind == kind;
                           });
    };

    CHECK(has_member("bus", "status_valid", "Variable"));
    CHECK(has_member("bus", "status_ready", "Variable"));
    CHECK(has_member("bus", "payload", "Variable"));
    CHECK(has_member("master_bus", "status_valid", "Field"));
    CHECK(has_member("master_bus", "status_ready", "Field"));
    CHECK_FALSE(has_member("master_bus", "payload", "Net"));
    CHECK_FALSE(has_member("master_bus", "payload", "Field"));
}

TEST_CASE("AstIndex narrows package-qualified type references to the AST type token",
          "[analysis][semantic][ast-index][type-definition][package][no-fallback]") {
    SnapshotBuildInput input{
        .generation = 38,
        .documents = {{"file:///workspace/defs.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                              .text = "package defs;\n"
                                                      "  typedef logic [7:0] word_t;\n"
                                                      "endpackage\n",
                                              .version = 1}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n"
                                                      "  defs::word_t value;\n"
                                                      "endmodule\n",
                                              .version = 1,
                                              .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);

    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 3).empty());

    auto locations = typeDefinitionLocationsAt(view, "file:///workspace/top.sv", 1, 10);
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == "file:///workspace/defs.sv");
    CHECK(locations.front().range.start_line == 1);
    CHECK(locations.front().range.start_character == 22);
    CHECK(locations.front().range.end_character == 28);
}

TEST_CASE("AstIndex builds package re-export and wildcard scope visibility facts",
          "[analysis][semantic][ast-index][visibility][package-export]") {
    SnapshotBuildInput input{
        .generation = 61,
        .documents = {
            {"file:///workspace/defs.sv",
             SemanticEngineDocument{.uri = "file:///workspace/defs.sv",
                                    .text = "package defs; parameter int WIDTH = 8; typedef logic [7:0] word_t; endpackage\n"}},
            {"file:///workspace/api.sv",
             SemanticEngineDocument{.uri = "file:///workspace/api.sv",
                                    .text = "package api; import defs::*; export defs::*; endpackage\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "module top; import api::*; localparam int W = WIDTH; endmodule\n",
                                    .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto api = view.package_visibility_by_name.find("api");
    REQUIRE(api != view.package_visibility_by_name.end());
    CHECK(std::find(api->second.exported_packages.begin(),
                    api->second.exported_packages.end(),
                    "defs") != api->second.exported_packages.end());
    CHECK(std::any_of(api->second.candidates.begin(),
                      api->second.candidates.end(),
                      [](const auto& candidate) { return candidate.identity.name == "WIDTH"; }));
    CHECK(std::any_of(api->second.candidates.begin(),
                      api->second.candidates.end(),
                      [](const auto& candidate) { return candidate.identity.name == "word_t"; }));
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/defs.sv",
              AffectedDependencyEdgeKind::SemanticExport) ==
          std::vector<std::string>{"file:///workspace/api.sv"});
}

TEST_CASE("AstIndex indexes nested typed member qualifiers without global candidates",
          "[analysis][semantic][ast-index][visibility][member][nested]") {
    SnapshotBuildInput input{
        .generation = 62,
        .documents = {{"file:///workspace/nested.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/nested.sv",
                           .text = "typedef struct packed { logic ready; logic error; } master_t;\n"
                                   "typedef struct packed { master_t master; } bus_t;\n"
                                   "module top; bus_t bus; initial bus.master.ready = 1'b1; endmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto members = view.member_completions_by_uri.find("file:///workspace/nested.sv");
    REQUIRE(members != view.member_completions_by_uri.end());
    CHECK(std::any_of(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "bus.master" && member.identity.name == "ready";
    }));
    CHECK(std::any_of(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "bus.master" && member.identity.name == "error";
    }));
}

TEST_CASE("AstIndex indexes ordinary module instance members for hierarchical completion",
          "[analysis][semantic][ast-index][visibility][member][instance]") {
    SnapshotBuildInput input{
        .generation = 63,
        .documents = {{"file:///workspace/instance.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/instance.sv",
                           .text = "module child(input logic data_i, output logic data_o); logic state; endmodule\n"
                                   "module top; child u_child(); endmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto members = view.member_completions_by_uri.find("file:///workspace/instance.sv");
    REQUIRE(members != view.member_completions_by_uri.end());
    for (const auto name : {"data_i", "data_o", "state"}) {
        CAPTURE(name);
        CHECK(std::any_of(members->second.begin(), members->second.end(), [&](const auto& member) {
            return member.qualifier == "u_child" && member.identity.name == name;
        }));
    }
    const auto data_i = std::find_if(members->second.begin(), members->second.end(), [](const auto& member) {
        return member.qualifier == "u_child" && member.identity.name == "data_i";
    });
    REQUIRE(data_i != members->second.end());
    const auto resolve = view.completion_resolve_by_id.find(data_i->identity.stable_id);
    REQUIRE(resolve != view.completion_resolve_by_id.end());
    CHECK(resolve->second.kind == SnapshotCompletionResolveKind::Member);
    CHECK(resolve->second.type_display == "logic");
}

TEST_CASE("AstIndex callable invocation facts preserve AST argument ranges",
          "[analysis][semantic][ast-index][callable][arguments]") {
    SnapshotBuildInput input{.generation = 70,
                             .documents = {{"file:///workspace/args.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/args.sv",
                                                .text = "module top; function int add(input int lhs, input int rhs); return lhs + rhs; endfunction int value = add(10, 20); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/args.sv");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.front().argument_ranges.size() == 2);
    CHECK(calls.front().argument_ranges[0].start_character <
          calls.front().argument_ranges[1].start_character);
    CHECK(calls.front().resolved);
    CHECK_FALSE(calls.front().target_stable_id.empty());
}

TEST_CASE("AstIndex macro invocation resolves the preceding definition",
          "[analysis][semantic][ast-index][macro][definition]") {
    SnapshotBuildInput input{.generation = 71,
                             .documents = {{"file:///workspace/macro.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/macro.sv",
                                                .text = "`define ADD(a, b) ((a) + (b))\nmodule top; int value = `ADD(1, 2); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/macro.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().definition.parameters == std::vector<std::string>{"a", "b"});
    CHECK(macros.front().expansion_text == "((1) + (2))");
    const auto visible = view.visible_macros_by_uri.find("file:///workspace/macro.sv");
    REQUIRE(visible != view.visible_macros_by_uri.end());
    REQUIRE_FALSE(visible->second.empty());
    const auto resolve_id = macroCompletionResolveId(visible->second.front());
    CAPTURE(resolve_id, view.completion_resolve_by_id.size());
    CHECK(view.completion_resolve_by_id.contains(resolve_id));
}

TEST_CASE("AstIndex indexes disabled conditional regions and excludes inactive macro invocations",
          "[analysis][semantic][ast-index][macro][inactive-region][no-fallback]") {
    SnapshotBuildInput input{.generation = 711,
                             .documents = {{"file:///workspace/inactive.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/inactive.sv",
                                                .text = "`define VALUE 1\n"
                                                        "`ifdef DISABLED\n"
                                                        "  int disabled_value = `VALUE;\n"
                                                        "`else\n"
                                                        "  int active_value = `VALUE;\n"
                                                        "`endif\n"
                                                        "module top; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto regions = inactiveRegionsForUri(view, "file:///workspace/inactive.sv");
    REQUIRE_FALSE(regions.empty());
    CHECK(regions.front().start_line == 2);
    CHECK(view.inactive_region_count >= 1);
    CHECK(view.inactive_region_build_micros >= 0);
    const auto macros = view.macro_invocations_by_uri.find("file:///workspace/inactive.sv");
    REQUIRE(macros != view.macro_invocations_by_uri.end());
    REQUIRE(macros->second.size() == 1);
    CHECK(macros->second.front().resolved);
    CHECK(macros->second.front().range.start_line == 4);
}

TEST_CASE("AstIndex macro invocation does not see a later definition",
          "[analysis][semantic][ast-index][macro][ordering][no-fallback]") {
    SnapshotBuildInput input{.generation = 72,
                             .documents = {{"file:///workspace/later.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/later.sv",
                                                .text = "module top; int value = `LATER; endmodule\n`define LATER 1\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/later.sv");
    REQUIRE(macros.size() == 1);
    CHECK_FALSE(macros.front().resolved);
    CHECK(macros.front().definition_uri.empty());
}

TEST_CASE("AstIndex macro invocation selects the latest preceding redefine",
          "[analysis][semantic][ast-index][macro][redefine]") {
    SnapshotBuildInput input{.generation = 73,
                             .documents = {{"file:///workspace/redefine.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/redefine.sv",
                                                .text = "`define VALUE 1\n`define VALUE 2\nmodule top; int value = `VALUE; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/redefine.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().expansion_text == "2");
}

TEST_CASE("AstIndex macro invocation rejects a definition after undef",
          "[analysis][semantic][ast-index][macro][undef][no-fallback]") {
    SnapshotBuildInput input{.generation = 74,
                             .documents = {{"file:///workspace/undef.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/undef.sv",
                                                .text = "`define VALUE 1\n`undef VALUE\nmodule top; int value = `VALUE; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/undef.sv");
    REQUIRE(macros.size() == 1);
    CHECK_FALSE(macros.front().resolved);
    CHECK(output.data->macro_undefs_by_uri.at("file:///workspace/undef.sv").size() == 1);
}

TEST_CASE("AstIndex resolves included macros and records macro include invalidation",
          "[analysis][semantic][ast-index][macro][include][affected]") {
    SnapshotBuildInput input{
        .generation = 75,
        .documents = {
            {"file:///workspace/defs.svh",
             SemanticEngineDocument{.uri = "file:///workspace/defs.svh", .text = "`define FLAG 1\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "`include \"defs.svh\"\nmodule top; int value = `FLAG; endmodule\n",
                                    .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/top.sv");
    REQUIRE(macros.size() == 1);
    CHECK(macros.front().resolved);
    CHECK(macros.front().definition_uri == "file:///workspace/defs.svh");
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/defs.svh", AffectedDependencyEdgeKind::MacroInclude) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("AstIndex records cross-file callable type invalidation edges",
          "[analysis][semantic][ast-index][callable][affected]") {
    SnapshotBuildInput input{
        .generation = 76,
        .documents = {
            {"file:///workspace/api.sv",
             SemanticEngineDocument{.uri = "file:///workspace/api.sv",
                                    .text = "package api; function int add(input int value); return value; endfunction endpackage\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "module top; import api::*; int value = add(1); endmodule\n",
                                    .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/api.sv", AffectedDependencyEdgeKind::CallableType) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("AstIndex records cross-file interface modport invalidation edges",
          "[analysis][semantic][ast-index][interface][modport][affected]") {
    SnapshotBuildInput input{
        .generation = 76,
        .documents = {
            {"file:///workspace/bus_if.sv",
             SemanticEngineDocument{.uri = "file:///workspace/bus_if.sv",
                                    .text = "interface bus_if; logic ready; modport master(input ready); endinterface\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "module top(bus_if.master bus); endmodule\n",
                                    .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    CHECK(output.affected_dependencies.dependentUris(
              "file:///workspace/bus_if.sv", AffectedDependencyEdgeKind::InterfaceModport) ==
          std::vector<std::string>{"file:///workspace/top.sv"});
}

TEST_CASE("AstIndex orders nested callable invocations by source range deterministically",
          "[analysis][semantic][ast-index][callable][nested][deterministic]") {
    SnapshotBuildInput input{.generation = 77,
                             .documents = {{"file:///workspace/nested-calls.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/nested-calls.sv",
                                                .text = "module top; function int id(input int value); return value; endfunction int value = id(id(1)); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& calls = view.callable_invocations_by_uri.at("file:///workspace/nested-calls.sv");
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].range.start_character <= calls[1].range.start_character);
    CHECK(calls[0].target_stable_id == calls[1].target_stable_id);
}

TEST_CASE("AstIndex expands nested function-like macros during index construction",
          "[analysis][semantic][ast-index][macro][nested-expansion]") {
    SnapshotBuildInput input{.generation = 78,
                             .documents = {{"file:///workspace/nested-macro.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/nested-macro.sv",
                                                .text = "`define ADD(a, b) ((a) + (b))\n`define TWICE(x) `ADD(x, x)\nmodule top; int value = `TWICE(3); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/nested-macro.sv");
    const auto twice = std::find_if(macros.begin(), macros.end(), [](const auto& macro) {
        return macro.name == "TWICE";
    });
    REQUIRE(twice != macros.end());
    CHECK(twice->resolved);
    CHECK(twice->expansion_text == "((3) + (3))");
}

TEST_CASE("AstIndex bounds recursive macro expansion cycles",
          "[analysis][semantic][ast-index][macro][expansion-cycle]") {
    SnapshotBuildInput input{.generation = 79,
                             .documents = {{"file:///workspace/macro-cycle.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/macro-cycle.sv",
                                                .text = "`define A `B\n`define B `A\nmodule top; int value = `A; endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& macros = view.macro_invocations_by_uri.at("file:///workspace/macro-cycle.sv");
    const auto invocation = std::find_if(macros.begin(), macros.end(), [](const auto& macro) {
        return macro.name == "A" && macro.range.start_line == 2;
    });
    REQUIRE(invocation != macros.end());
    CHECK(invocation->resolved);
    CHECK(invocation->expansion_text.size() <= 2);
}

TEST_CASE("AstIndex preserves empty named-port ranges for signature active parameter",
          "[analysis][semantic][ast-index][signature][module][named-port]") {
    SnapshotBuildInput input{.generation = 80,
                             .documents = {{"file:///workspace/empty-ports.sv",
                                            SemanticEngineDocument{
                                                .uri = "file:///workspace/empty-ports.sv",
                                                .text = "module child(input logic clk, output logic rst_n); endmodule\nmodule top; child child_i(.clk(), .rst_n()); endmodule\n",
                                                .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto& instances =
        view.signature_module_instances_by_uri.at("file:///workspace/empty-ports.sv");
    REQUIRE(instances.size() == 1);
    REQUIRE(instances.front().connections.size() == 2);
    CHECK(instances.front().connections[0].port_name == "clk");
    CHECK(instances.front().connections[1].port_name == "rst_n");
    CHECK(instances.front().connections[0].range.start_character <
          instances.front().connections[1].range.start_character);
}

TEST_CASE("SnapshotBuilder limits macro visibility to document include closure",
          "[analysis][semantic][ast-index][visibility][macro]") {
    SnapshotBuildInput input{
        .generation = 64,
        .documents = {
            {"file:///workspace/included.svh",
             SemanticEngineDocument{.uri = "file:///workspace/included.svh",
                                    .text = "`define INCLUDED_FLAG 1\n"}},
            {"file:///workspace/unrelated.svh",
             SemanticEngineDocument{.uri = "file:///workspace/unrelated.svh",
                                    .text = "`define UNRELATED_FLAG 1\n"}},
            {"file:///workspace/top.sv",
             SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                    .text = "`include \"included.svh\"\nmodule top; endmodule\n",
                                    .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto macros = output.data->visible_macros_by_uri.find("file:///workspace/top.sv");
    REQUIRE(macros != output.data->visible_macros_by_uri.end());
    CHECK(std::any_of(macros->second.begin(), macros->second.end(), [](const auto& macro) {
        return macro.definition.name == "INCLUDED_FLAG";
    }));
    CHECK(std::none_of(macros->second.begin(), macros->second.end(), [](const auto& macro) {
        return macro.definition.name == "UNRELATED_FLAG";
    }));
}

TEST_CASE("AstIndex scope visibility metrics and ordering are deterministic",
          "[analysis][semantic][ast-index][visibility][deterministic]") {
    const auto build = [](std::uint64_t generation) {
        SnapshotBuildInput input{
            .generation = generation,
            .documents = {{"file:///workspace/order.sv",
                           SemanticEngineDocument{.uri = "file:///workspace/order.sv",
                                                  .text = "package defs; parameter int B = 2; parameter int A = 1; endpackage\n"
                                                          "module top; import defs::*; logic z; logic a; endmodule\n",
                                                  .is_open = true}}}};
        return SnapshotBuilder{}.build(std::move(input));
    };
    auto first = build(65);
    auto second = build(66);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    const auto first_view = buildAstIndexView(first.data.get(), first.snapshot.generation);
    const auto second_view = buildAstIndexView(second.data.get(), second.snapshot.generation);
    REQUIRE(first_view.package_visibility_by_name.contains("defs"));
    REQUIRE(second_view.package_visibility_by_name.contains("defs"));
    const auto names = [](const auto& candidates) {
        std::vector<std::string> result;
        for (const auto& candidate : candidates) {
            result.push_back(candidate.identity.name);
        }
        return result;
    };
    CHECK(names(first_view.package_visibility_by_name.at("defs").candidates) ==
          names(second_view.package_visibility_by_name.at("defs").candidates));
    CHECK(first_view.scope_visibility_count > 0);
    CHECK(first_view.package_visibility_count == 1);
    CHECK(first_view.scope_visibility_build_micros >= 0);
}

TEST_CASE("AstIndex indexes class method calls as callable visibility facts",
          "[analysis][semantic][ast-index][visibility][callable][class]") {
    SnapshotBuildInput input{
        .generation = 66,
        .documents = {{"file:///workspace/class-call.sv",
                       SemanticEngineDocument{
                           .uri = "file:///workspace/class-call.sv",
                           .text = "class packet;\n"
                                   "  function int sum(input int lhs, input int rhs); return lhs + rhs; endfunction\n"
                                   "endclass\nmodule top;\n  packet pkt;\n"
                                   "  initial begin\n"
                                   "    int value;\n"
                                   "    pkt = new;\n"
                                   "    value = pkt.sum(1, 2);\n"
                                   "  end\nendmodule\n",
                           .is_open = true}}}};

    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto calls = view.callable_invocations_by_uri.find("file:///workspace/class-call.sv");
    REQUIRE(calls != view.callable_invocations_by_uri.end());
    REQUIRE(calls->second.size() == 1);
    const auto& call = calls->second.front();
    CHECK(call.name == "sum");
    CHECK(call.parameters.size() == 2);
    CHECK(call.range.start_line == 8);
    CHECK(call.range.start_character == 12);
    CHECK(call.range.end_line == 8);
    CHECK(call.range.end_character == 25);
    CHECK(call.selection_range.start_character == 16);
    CHECK(call.selection_range.end_character == 19);
}

TEST_CASE("AstIndex builds direct graph bindings and cone adjacency from module connections",
          "[analysis][semantic][ast-index][design-graph-binding][cone]") {
    SnapshotBuildInput input{
        .generation = 71,
        .documents = {{"file:///workspace/child.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/child.sv",
                                              .text = "module child(input logic in, output logic out);\n"
                                                      "  assign out = in;\nendmodule\n"}},
                      {"file:///workspace/top.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/top.sv",
                                              .text = "module top;\n  logic a; logic y;\n"
                                                      "  child u_child(.in(a), .out(y));\nendmodule\n",
                                              .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK_FALSE(view.design_graph_binding_index.symbol_ids_by_uri_range.empty());
    CHECK(view.design_graph_binding_index.port_symbol_ids_by_module_port.contains("child\x1f" "in"));
    CHECK(view.design_graph_binding_index.port_symbol_ids_by_module_port.contains("child\x1f" "out"));
    const auto input_endpoint = view.design_graph_binding_index.endpoints_by_module_member.find("child\x1f" "in");
    REQUIRE(input_endpoint != view.design_graph_binding_index.endpoints_by_module_member.end());
    CHECK(input_endpoint->second.kind == SnapshotGraphEndpointKind::Port);
    CHECK(input_endpoint->second.direction == SnapshotGraphPortDirection::Input);
    const auto output_endpoint = view.design_graph_binding_index.endpoints_by_module_member.find("child\x1f" "out");
    REQUIRE(output_endpoint != view.design_graph_binding_index.endpoints_by_module_member.end());
    CHECK(output_endpoint->second.direction == SnapshotGraphPortDirection::Output);
    REQUIRE(view.cone_adjacency_index.edges.size() == 3);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.kind == SnapshotConeEdgeKind::Assignment;
                        }) == 1);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.kind == SnapshotConeEdgeKind::InstancePort;
                        }) == 2);
    CHECK(view.cone_adjacency_index.edges_by_from_symbol_id.size() == 3);
    CHECK(view.cone_adjacency_index.edges_by_to_symbol_id.size() == 3);
}

TEST_CASE("AstIndex binds assigned module ports to the resolved assignment identities",
          "[analysis][semantic][ast-index][design-graph-binding][port]") {
    auto output = buildReferenceCallDesign("module child(input logic in, output logic out);\n"
                                           "  assign out = in;\nendmodule\n"
                                           "module top; logic a; logic y; child u(.in(a), .out(y)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto out = view.design_graph_binding_index.port_symbol_ids_by_module_port.find("child\x1f" "out");
    REQUIRE(out != view.design_graph_binding_index.port_symbol_ids_by_module_port.end());
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [&](const SnapshotConeAdjacencyEdge& edge) { return edge.from_symbol_id == out->second; }));
}

TEST_CASE("AstIndex stores generated instance bindings independently",
          "[analysis][semantic][ast-index][design-graph-binding][generated]") {
    SnapshotBuildInput input{
        .generation = 72,
        .config = SemanticEngineConfig{.top_modules = {"top"}},
        .documents = {{"file:///workspace/generated.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/generated.sv",
                                              .text = "module child(input logic in, output logic out); assign out = in; endmodule\n"
                                                      "module top; logic a; logic y; genvar i; generate\n"
                                                      "  for (i = 0; i < 1; i = i + 1) begin : g\n"
                                                      "    child u(.in(a), .out(y));\n  end\nendgenerate endmodule\n",
                                              .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(view.design_graph_binding_index.instance_ids_by_uri_range.size() >= 2);
    CHECK(view.cone_adjacency_index.edges.size() == 3);
    CHECK(std::all_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.kind == SnapshotConeEdgeKind::Assignment ||
                                 !edge.generated_instance_id.empty();
                      }));
}

TEST_CASE("AstIndex keeps cone adjacency deterministic across equivalent builds",
          "[analysis][semantic][ast-index][cone][deterministic]") {
    const auto build = [](std::uint64_t generation) {
        SnapshotBuildInput input{
            .generation = generation,
            .documents = {{"file:///workspace/chain.sv",
                           SemanticEngineDocument{.uri = "file:///workspace/chain.sv",
                                                  .text = "module top; logic a; logic mid; logic y;\n"
                                                          "assign mid = a; assign y = mid; endmodule\n",
                                                  .is_open = true}}}};
        return SnapshotBuilder{}.build(std::move(input));
    };
    auto first = build(73);
    auto second = build(74);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    CHECK(first.data->cone_adjacency_index.edges.size() == second.data->cone_adjacency_index.edges.size());
    REQUIRE(first.data->cone_adjacency_index.edges.size() == 2);
    for (size_t index = 0; index < first.data->cone_adjacency_index.edges.size(); ++index) {
        CHECK(first.data->cone_adjacency_index.edges[index].from_symbol_id ==
              second.data->cone_adjacency_index.edges[index].from_symbol_id);
        CHECK(first.data->cone_adjacency_index.edges[index].to_symbol_id ==
              second.data->cone_adjacency_index.edges[index].to_symbol_id);
    }
}

TEST_CASE("AstIndex indexes if control dependencies for procedural assignments",
          "[analysis][semantic][ast-index][cone][control][if]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, output logic y);\n"
        "  always_comb begin if (select) y = a; else y = b; end\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto control_edges = std::count_if(view.cone_adjacency_index.edges.begin(),
                                             view.cone_adjacency_index.edges.end(),
                                             [](const SnapshotConeAdjacencyEdge& edge) {
                                                 return edge.kind == SnapshotConeEdgeKind::ControlDependency &&
                                                        edge.source_role == SnapshotConeSourceRole::Control;
                                             });
    CHECK(control_edges >= 1);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.kind == SnapshotConeEdgeKind::Assignment &&
                                 edge.source_role == SnapshotConeSourceRole::Data;
                      }));
}

TEST_CASE("AstIndex indexes case selectors as control dependencies",
          "[analysis][semantic][ast-index][cone][control][case]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, output logic y);\n"
        "  always_comb begin case (select) 1'b0: y = a; default: y = b; endcase end\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.kind == SnapshotConeEdgeKind::ControlDependency &&
                                 edge.source_role == SnapshotConeSourceRole::Control;
                      }));
}

TEST_CASE("AstIndex indexes conditional-expression controls without text lookup",
          "[analysis][semantic][ast-index][cone][control][ternary]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, output logic y);\n"
        "  assign y = select ? a : b;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.kind == SnapshotConeEdgeKind::ControlDependency &&
                                 edge.source_role == SnapshotConeSourceRole::Control &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.expression == "select" &&
                                   edge.source_role == SnapshotConeSourceRole::Data;
                        }) == 0);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.kind == SnapshotConeEdgeKind::Assignment &&
                                   edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "a" || edge.expression == "b");
                        }) == 2);
}

TEST_CASE("AstIndex indexes nested ternary selectors separately from branch data",
          "[analysis][semantic][ast-index][cone][control][ternary][nested]") {
    auto output = buildGraphSource(
        "module top(input logic first, input logic second, input logic a, input logic b, input logic c, "
        "output logic y);\n"
        "  assign y = first ? a : second ? b : c;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto ternary_controls = std::count_if(view.cone_adjacency_index.edges.begin(),
                                                view.cone_adjacency_index.edges.end(),
                                                [](const SnapshotConeAdjacencyEdge& edge) {
                                                    return edge.control_origin ==
                                                               SnapshotConeControlOrigin::TernaryCondition &&
                                                           edge.source_role == SnapshotConeSourceRole::Control;
                                                });
    CHECK(ternary_controls == 2);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "first" || edge.expression == "second");
                        }) == 0);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "a" || edge.expression == "b" || edge.expression == "c");
                        }) == 3);
}

TEST_CASE("AstIndex finds ternary controls nested inside binary expressions",
          "[analysis][semantic][ast-index][cone][control][ternary][binary]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, input logic c, output logic y);\n"
        "  assign y = (select ? a : b) ^ c;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "a" || edge.expression == "b" || edge.expression == "c");
                        }) == 3);
}

TEST_CASE("AstIndex preserves concatenation slices around ternary branches",
          "[analysis][semantic][ast-index][cone][control][ternary][concatenation]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, input logic c, output logic [1:0] y);\n"
        "  assign y = {select ? a : b, c};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   edge.slice_kind == SnapshotConeSliceKind::Concatenation;
                        }) == 3);
}

TEST_CASE("AstIndex preserves static and dynamic select facts in ternary branches",
          "[analysis][semantic][ast-index][cone][control][ternary][slice]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic [3:0] data, input logic [1:0] index, output logic [2:0] y);\n"
        "  assign y = select ? data[3:1] : data[index];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.slice_kind == SnapshotConeSliceKind::RangeSelect;
                      }));
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.slice_kind == SnapshotConeSliceKind::DynamicSelect;
                      }));
    CHECK(std::none_of(view.cone_adjacency_index.edges.begin(),
                       view.cone_adjacency_index.edges.end(),
                       [](const SnapshotConeAdjacencyEdge& edge) {
                           return edge.expression == "select" &&
                                  edge.source_role == SnapshotConeSourceRole::Data;
                       }));
}

TEST_CASE("AstIndex records exact source and sink slices for static cone selects",
          "[analysis][semantic][ast-index][cone][slice][exact]") {
    auto output = buildGraphSource(
        "module top(input logic [7:0] data, output logic [3:0] y);\n"
        "  assign y[3:0] = data[7:4];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto edge = std::find_if(view.cone_adjacency_index.edges.begin(),
                                   view.cone_adjacency_index.edges.end(),
                                   [](const SnapshotConeAdjacencyEdge& candidate) {
                                       return candidate.expression == "data[7:4]" &&
                                              candidate.source_role == SnapshotConeSourceRole::Data;
                                   });
    REQUIRE(edge != view.cone_adjacency_index.edges.end());
    CHECK(edge->source_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(edge->source_slice.msb == 7);
    CHECK(edge->source_slice.lsb == 4);
    CHECK(edge->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(edge->sink_slice.msb == 3);
    CHECK(edge->sink_slice.lsb == 0);
}

TEST_CASE("AstIndex maps concatenation operands onto exact descending sink slices",
          "[analysis][semantic][ast-index][cone][slice][concatenation][exact]") {
    auto output = buildGraphSource(
        "module top(input logic [3:0] upper, input logic [3:0] lower, output logic [7:0] y);\n"
        "  assign y[7:0] = {upper[3:0], lower[3:0]};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto exact_sink_for = [&](std::string_view expression) {
        const auto edge = std::find_if(view.cone_adjacency_index.edges.begin(),
                                       view.cone_adjacency_index.edges.end(),
                                       [&](const SnapshotConeAdjacencyEdge& candidate) {
                                           return candidate.expression == expression &&
                                                  candidate.source_role == SnapshotConeSourceRole::Data;
                                       });
        REQUIRE(edge != view.cone_adjacency_index.edges.end());
        CHECK(edge->slice_kind == SnapshotConeSliceKind::Concatenation);
        CHECK(edge->source_slice.precision == SnapshotConeSlicePrecision::Exact);
        CHECK(edge->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
        return edge->sink_slice;
    };

    const auto upper_sink = exact_sink_for("upper[3:0]");
    CHECK(upper_sink.msb == 7);
    CHECK(upper_sink.lsb == 4);
    const auto lower_sink = exact_sink_for("lower[3:0]");
    CHECK(lower_sink.msb == 3);
    CHECK(lower_sink.lsb == 0);
}

TEST_CASE("AstIndex maps concatenation operands onto exact ascending sink slices",
          "[analysis][semantic][ast-index][cone][slice][concatenation][ascending]") {
    auto output = buildGraphSource(
        "module top(input logic [3:0] upper, input logic [3:0] lower, output logic [0:7] y);\n"
        "  assign y[0:7] = {upper[3:0], lower[3:0]};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto* upper = coneDataEdge(view.cone_adjacency_index, "upper[3:0]");
    const auto* lower = coneDataEdge(view.cone_adjacency_index, "lower[3:0]");
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);
    CHECK(upper->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(upper->sink_slice.msb == 0);
    CHECK(upper->sink_slice.lsb == 3);
    CHECK(lower->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(lower->sink_slice.msb == 4);
    CHECK(lower->sink_slice.lsb == 7);
}

TEST_CASE("AstIndex maps nested concatenation operands onto exact sink slices",
          "[analysis][semantic][ast-index][cone][slice][concatenation][nested]") {
    auto output = buildGraphSource(
        "module top(input logic [1:0] a, input logic [1:0] b, input logic [3:0] c, output logic [7:0] y);\n"
        "  assign y[7:0] = {{a[1:0], b[1:0]}, c[3:0]};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto* a = coneDataEdge(view.cone_adjacency_index, "a[1:0]");
    const auto* b = coneDataEdge(view.cone_adjacency_index, "b[1:0]");
    const auto* c = coneDataEdge(view.cone_adjacency_index, "c[3:0]");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->sink_slice.msb == 7);
    CHECK(a->sink_slice.lsb == 6);
    CHECK(b->sink_slice.msb == 5);
    CHECK(b->sink_slice.lsb == 4);
    CHECK(c->sink_slice.msb == 3);
    CHECK(c->sink_slice.lsb == 0);
}

TEST_CASE("AstIndex shares exact concatenation sink mapping with procedural assignments",
          "[analysis][semantic][ast-index][cone][slice][concatenation][procedural]") {
    auto output = buildGraphSource(
        "module top(input logic [3:0] upper, input logic [3:0] lower, output logic [7:0] y);\n"
        "  always_comb y[7:0] = {upper[3:0], lower[3:0]};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto* upper = coneDataEdge(view.cone_adjacency_index, "upper[3:0]");
    const auto* lower = coneDataEdge(view.cone_adjacency_index, "lower[3:0]");
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);
    CHECK(upper->sink_slice.msb == 7);
    CHECK(upper->sink_slice.lsb == 4);
    CHECK(lower->sink_slice.msb == 3);
    CHECK(lower->sink_slice.lsb == 0);
}

TEST_CASE("AstIndex preserves exact static indexed part-select facts",
          "[analysis][semantic][ast-index][cone][slice][indexed-part-select]") {
    auto output = buildGraphSource(
        "module top(input logic [7:0] data, output logic [2:0] y);\n"
        "  assign y[2:0] = data[2 +: 3];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto edge = std::find_if(view.cone_adjacency_index.edges.begin(),
                                   view.cone_adjacency_index.edges.end(),
                                   [](const auto& candidate) {
                                       return candidate.source_role == SnapshotConeSourceRole::Data &&
                                              candidate.slice_kind == SnapshotConeSliceKind::RangeSelect;
                                   });
    REQUIRE(edge != view.cone_adjacency_index.edges.end());
    CHECK(edge->source_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(edge->source_slice.msb.has_value());
    CHECK(edge->source_slice.lsb.has_value());
    CHECK(edge->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
}

TEST_CASE("AstIndex keeps binary cone sources aggregate instead of fabricating slices",
          "[analysis][semantic][ast-index][cone][slice][aggregate]") {
    auto output = buildGraphSource(
        "module top(input logic [3:0] a, input logic [3:0] b, output logic [3:0] y);\n"
        "  assign y[3:0] = a[3:0] ^ b[3:0];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto edge = std::find_if(view.cone_adjacency_index.edges.begin(),
                                   view.cone_adjacency_index.edges.end(),
                                   [](const auto& candidate) {
                                       return candidate.source_role == SnapshotConeSourceRole::Data &&
                                              candidate.expression == "a[3:0] ^ b[3:0]";
                                   });
    REQUIRE(edge != view.cone_adjacency_index.edges.end());
    CHECK(edge->source_slice.precision == SnapshotConeSlicePrecision::Aggregate);
    CHECK(edge->sink_slice.precision == SnapshotConeSlicePrecision::Exact);
}

TEST_CASE("AstIndex exposes a dynamic-select control without an exact source slice",
          "[analysis][semantic][ast-index][cone][slice][dynamic-control]") {
    auto output = buildGraphSource(
        "module top(input logic [7:0] data, input logic [2:0] index, output logic y);\n"
        "  assign y = data[index];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto* data = coneDataEdge(view.cone_adjacency_index, "data[index]");
    REQUIRE(data != nullptr);
    CHECK(data->source_slice.precision == SnapshotConeSlicePrecision::Dynamic);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const auto& edge) {
                          return edge.kind == SnapshotConeEdgeKind::ControlDependency &&
                                 edge.control_origin == SnapshotConeControlOrigin::DynamicSelect &&
                                 edge.source_role == SnapshotConeSourceRole::Control;
                      }));
}

TEST_CASE("AstIndex treats a low static element select as exact",
          "[analysis][semantic][ast-index][cone][slice][element][low]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, output logic y); assign y = data[0]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex treats a high static element select as exact",
          "[analysis][semantic][ast-index][cone][slice][element][high]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, output logic y); assign y = data[7]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex treats descending static part selects as exact",
          "[analysis][semantic][ast-index][cone][slice][range][descending]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, output logic [3:0] y); assign y[3:0] = data[6:3]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex treats ascending static part selects as exact",
          "[analysis][semantic][ast-index][cone][slice][range][ascending]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [0:7] data, output logic [0:3] y); assign y[0:3] = data[1:4]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex treats static indexed plus part selects as exact",
          "[analysis][semantic][ast-index][cone][slice][range][indexed-plus]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, output logic [2:0] y); assign y[2:0] = data[2 +: 3]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex treats static indexed minus part selects as exact",
          "[analysis][semantic][ast-index][cone][slice][range][indexed-minus]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, output logic [2:0] y); assign y[2:0] = data[5 -: 3]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex marks dynamic element selects dynamic",
          "[analysis][semantic][ast-index][cone][slice][element][dynamic]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, input logic [2:0] index, output logic y); assign y = data[index]; endmodule\n",
        SnapshotConeSlicePrecision::Dynamic));
}

TEST_CASE("AstIndex marks dynamic range selects dynamic",
          "[analysis][semantic][ast-index][cone][slice][range][dynamic]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [7:0] data, input logic [2:0] index, output logic [2:0] y); assign y[2:0] = data[index +: 3]; endmodule\n",
        SnapshotConeSlicePrecision::Dynamic));
}

TEST_CASE("AstIndex keeps concatenated part-select operands exact",
          "[analysis][semantic][ast-index][cone][slice][concatenation][parts]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [3:0] a, input logic [3:0] b, output logic [7:0] y); assign y[7:0] = {a[3:0], b[3:0]}; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex keeps concatenated element-select operands exact",
          "[analysis][semantic][ast-index][cone][slice][concatenation][elements]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [3:0] a, input logic [3:0] b, output logic [1:0] y); assign y[1:0] = {a[1], b[2]}; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex keeps nested concatenation operands exact",
          "[analysis][semantic][ast-index][cone][slice][concatenation][nested-precision]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [1:0] a, input logic [1:0] b, input logic [3:0] c, output logic [7:0] y); assign y[7:0] = {{a[1:0], b[1:0]}, c[3:0]}; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex keeps static ternary branch slices exact",
          "[analysis][semantic][ast-index][cone][slice][ternary][static]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic select, input logic [3:0] a, input logic [3:0] b, output logic [3:0] y); assign y[3:0] = select ? a[3:0] : b[3:0]; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex keeps dynamic ternary branch slices dynamic",
          "[analysis][semantic][ast-index][cone][slice][ternary][dynamic]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic select, input logic [7:0] data, input logic [2:0] index, output logic y); assign y = select ? data[index] : data[0]; endmodule\n",
        SnapshotConeSlicePrecision::Dynamic));
}

TEST_CASE("AstIndex labels binary expression sources aggregate",
          "[analysis][semantic][ast-index][cone][slice][binary][aggregate]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [3:0] a, input logic [3:0] b, output logic [3:0] y); assign y[3:0] = a[3:0] ^ b[3:0]; endmodule\n",
        SnapshotConeSlicePrecision::Aggregate));
}

TEST_CASE("AstIndex preserves exact slices in procedural concatenations",
          "[analysis][semantic][ast-index][cone][slice][procedural][concatenation]") {
    CHECK(graphHasDataSlicePrecision(
        "module top(input logic [3:0] upper, input logic [3:0] lower, output logic [7:0] y); always_comb y[7:0] = {upper[3:0], lower[3:0]}; endmodule\n",
        SnapshotConeSlicePrecision::Exact));
}

TEST_CASE("AstIndex records dynamic select precision without claiming an exact slice",
          "[analysis][semantic][ast-index][cone][slice][dynamic]") {
    auto output = buildGraphSource(
        "module top(input logic [7:0] data, input logic [2:0] index, output logic y);\n"
        "  assign y = data[index];\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto edge = std::find_if(view.cone_adjacency_index.edges.begin(),
                                   view.cone_adjacency_index.edges.end(),
                                   [](const SnapshotConeAdjacencyEdge& candidate) {
                                       return candidate.expression == "data[index]" &&
                                              candidate.source_role == SnapshotConeSourceRole::Data;
                                   });
    REQUIRE(edge != view.cone_adjacency_index.edges.end());
    CHECK(edge->source_slice.precision == SnapshotConeSlicePrecision::Dynamic);
    CHECK_FALSE(edge->source_slice.msb.has_value());
    CHECK_FALSE(edge->source_slice.lsb.has_value());
}

TEST_CASE("AstIndex indexes procedural ternary controls through the shared collector",
          "[analysis][semantic][ast-index][cone][control][ternary][procedural]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, input logic b, output logic y);\n"
        "  always_comb y = select ? a : b;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.kind == SnapshotConeEdgeKind::ControlDependency &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "a" || edge.expression == "b");
                        }) == 2);
}

TEST_CASE("AstIndex keeps literal ternary branches out of unresolved cone facts",
          "[analysis][semantic][ast-index][cone][control][ternary][literal]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, output logic y);\n"
        "  assign y = select ? a : 1'b0;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(view.cone_adjacency_index.unresolved_sources_by_from_symbol_id.empty());
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "a" && edge.source_role == SnapshotConeSourceRole::Data;
                      }));
}

TEST_CASE("AstIndex distinguishes statement and ternary control origins",
          "[analysis][semantic][ast-index][cone][control][ternary][statement]") {
    auto output = buildGraphSource(
        "module top(input logic enable, input logic select, input logic a, input logic b, output logic y);\n"
        "  always_comb if (enable) y = select ? a : b;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "enable" &&
                                 edge.control_origin == SnapshotConeControlOrigin::ConditionalStatement;
                      }));
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
}

TEST_CASE("AstIndex keeps a ternary selector as data only when a branch reads it",
          "[analysis][semantic][ast-index][cone][control][ternary][selector-branch]") {
    auto output = buildGraphSource(
        "module top(input logic select, input logic a, output logic y);\n"
        "  assign y = select ? select : a;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.expression == "select" &&
                                   edge.source_role == SnapshotConeSourceRole::Control &&
                                   edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                        }) == 1);
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.expression == "select" &&
                                   edge.source_role == SnapshotConeSourceRole::Data;
                        }) == 1);
}

TEST_CASE("AstIndex keeps nested ternary control ranges distinct",
          "[analysis][semantic][ast-index][cone][control][ternary][ranges]") {
    auto output = buildGraphSource(
        "module top(input logic first, input logic second, input logic a, input logic b, input logic c, "
        "output logic y);\n"
        "  assign y = first ? (second ? a : b) : c;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    std::vector<ParseRange> ranges;
    for (const auto& edge : view.cone_adjacency_index.edges) {
        if (edge.source_role == SnapshotConeSourceRole::Control &&
            edge.control_origin == SnapshotConeControlOrigin::TernaryCondition) {
            ranges.push_back(edge.expression_location.range);
        }
    }
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].start_character != ranges[1].start_character);
}

TEST_CASE("AstIndex shares ternary source collection with nonblocking assignments",
          "[analysis][semantic][ast-index][cone][control][ternary][nonblocking]") {
    auto output = buildGraphSource(
        "module top(input logic clk, input logic select, input logic a, input logic b, output logic y);\n"
        "  always_ff @(posedge clk) y <= select ? a : b;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.expression == "select" &&
                                 edge.source_role == SnapshotConeSourceRole::Control &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Data &&
                                   (edge.expression == "a" || edge.expression == "b");
                        }) == 2);
}

TEST_CASE("AstIndex classifies a binary ternary condition as control only",
          "[analysis][semantic][ast-index][cone][control][ternary][binary-condition]") {
    auto output = buildGraphSource(
        "module top(input logic left, input logic right, input logic a, input logic b, output logic y);\n"
        "  assign y = (left && right) ? a : b;\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.source_role == SnapshotConeSourceRole::Control &&
                                 edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                      }));
    CHECK(std::none_of(view.cone_adjacency_index.edges.begin(),
                       view.cone_adjacency_index.edges.end(),
                       [](const SnapshotConeAdjacencyEdge& edge) {
                           return edge.source_role == SnapshotConeSourceRole::Data &&
                                  (edge.expression.find("left") != std::string::npos ||
                                   edge.expression.find("right") != std::string::npos);
                       }));
}

TEST_CASE("AstIndex keeps literal nested ternary branches resolved",
          "[analysis][semantic][ast-index][cone][control][ternary][nested-literal]") {
    auto output = buildGraphSource(
        "module top(input logic first, input logic second, input logic a, output logic y);\n"
        "  assign y = first ? 1'b0 : (second ? a : 1'b1);\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    CHECK(view.cone_adjacency_index.unresolved_sources_by_from_symbol_id.empty());
    CHECK(std::count_if(view.cone_adjacency_index.edges.begin(),
                        view.cone_adjacency_index.edges.end(),
                        [](const SnapshotConeAdjacencyEdge& edge) {
                            return edge.source_role == SnapshotConeSourceRole::Control &&
                                   edge.control_origin == SnapshotConeControlOrigin::TernaryCondition;
                        }) == 2);
}

TEST_CASE("AstIndex preserves concatenation and select slice kinds in cone facts",
          "[analysis][semantic][ast-index][cone][slice]") {
    auto output = buildGraphSource(
        "module top(input logic a, input logic b, input logic [3:0] data, input logic [1:0] index, "
        "output logic [5:0] y);\n"
        "  assign y = {a, b, data[3:1], data[index]};\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.slice_kind == SnapshotConeSliceKind::Concatenation;
                      }));
}

TEST_CASE("AstIndex leaves unresolved graph connections out of cone adjacency",
          "[analysis][semantic][ast-index][cone][unresolved][no-fallback]") {
    SnapshotBuildInput input{
        .generation = 75,
        .documents = {{"file:///workspace/missing.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/missing.sv",
                                              .text = "module top; logic y; missing_child u(.out(y)); endmodule\n",
                                              .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto& edges = output.data->cone_adjacency_index.edges;
    CHECK(edges.empty());
}

TEST_CASE("AstIndex indexes named parameter overrides as typed cone edges",
          "[analysis][semantic][ast-index][design-graph-binding][parameter]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(input logic in, output logic out);\n"
        "  assign out = in;\nendmodule\n"
        "module top; localparam int WIDTH = 4; logic a; logic y;\n"
        "  child #(.WIDTH(WIDTH)) u_child(.in(a), .out(y));\nendmodule\n");
    REQUIRE(output.data != nullptr);
    CHECK(output.data->parameter_override_syntax_facts.empty());
    CHECK(output.data->instance_symbols_by_stable_id.empty());
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto resolved = std::find_if(output.data->resolved_connection_slices_by_instance_id.begin(),
                                       output.data->resolved_connection_slices_by_instance_id.end(),
                                       [](const auto& entry) {
                                           return std::any_of(entry.second.begin(),
                                                              entry.second.end(),
                                                              [](const auto& fact) {
                                                                  return fact.kind ==
                                                                         SnapshotConeEdgeKind::ParameterOverride;
                                                              });
                                       });
    REQUIRE(resolved != output.data->resolved_connection_slices_by_instance_id.end());
    const auto parameter_fact = std::find_if(resolved->second.begin(),
                                              resolved->second.end(),
                                              [](const auto& fact) {
                                                  return fact.kind ==
                                                         SnapshotConeEdgeKind::ParameterOverride;
                                              });
    REQUIRE(parameter_fact != resolved->second.end());
    CHECK_FALSE(parameter_fact->endpoint_stable_id.empty());
    REQUIRE(parameter_fact->source_parts.size() == 1);
    CHECK_FALSE(parameter_fact->source_parts.front().source_symbol_id.empty());

    const auto binding = std::find_if(view.design_graph_binding_index.connection_bindings.begin(),
                                      view.design_graph_binding_index.connection_bindings.end(),
                                      [](const SnapshotGraphConnectionBindingFact& value) {
                                          return value.kind == SnapshotConeEdgeKind::ParameterOverride;
                                      });
    REQUIRE(binding != view.design_graph_binding_index.connection_bindings.end());
    CHECK(binding->source_symbol_ids.size() == 1);
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.kind == SnapshotConeEdgeKind::ParameterOverride;
                      }));
}

TEST_CASE("AstIndex indexes ordered parameter overrides with direct endpoint identities",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][ordered]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1, parameter int DEPTH = 2)(); endmodule\n"
        "module top; localparam int W = 4; localparam int D = 8; child #(W, D) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    std::vector<const SnapshotResolvedConnectionSliceFact*> facts;
    for (const auto& [_, values] : output.data->resolved_connection_slices_by_instance_id) {
        for (const auto& fact : values) {
            if (fact.kind == SnapshotConeEdgeKind::ParameterOverride) facts.push_back(&fact);
        }
    }
    REQUIRE(facts.size() == 2);
    CHECK(facts[0]->endpoint_stable_id != facts[1]->endpoint_stable_id);
    CHECK(std::all_of(facts.begin(), facts.end(), [](const auto* fact) {
        return !fact->endpoint_stable_id.empty() && fact->source_parts.size() == 1 &&
               !fact->source_parts.front().source_symbol_id.empty();
    }));
    CHECK(std::count_if(view.design_graph_binding_index.connection_bindings.begin(),
                        view.design_graph_binding_index.connection_bindings.end(),
                        [](const auto& binding) {
                            return binding.kind == SnapshotConeEdgeKind::ParameterOverride;
                        }) == 2);
}

TEST_CASE("AstIndex keeps parameter override identities isolated across repeated instances",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][instance]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int A = 2; localparam int B = 3;\n"
        "  child #(.WIDTH(A)) first(); child #(.WIDTH(B)) second();\nendmodule\n");
    REQUIRE(output.data != nullptr);

    std::vector<const SnapshotResolvedConnectionSliceFact*> facts;
    for (const auto& [_, values] : output.data->resolved_connection_slices_by_instance_id) {
        for (const auto& fact : values) {
            if (fact.kind == SnapshotConeEdgeKind::ParameterOverride) facts.push_back(&fact);
        }
    }
    REQUIRE(facts.size() == 2);
    CHECK(facts[0]->instance_stable_id != facts[1]->instance_stable_id);
    REQUIRE(facts[0]->source_parts.size() == 1);
    REQUIRE(facts[1]->source_parts.size() == 1);
    CHECK(facts[0]->source_parts.front().source_symbol_id !=
          facts[1]->source_parts.front().source_symbol_id);
}

TEST_CASE("AstIndex keeps same-name child parameters bound to their definition identity",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][definition]") {
    auto output = buildGraphSource(
        "module left #(parameter int WIDTH = 1)(); endmodule\n"
        "module right #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; left #(.WIDTH(W)) u_left(); right #(.WIDTH(W)) u_right(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    std::vector<std::string> endpoint_ids;
    for (const auto& binding : view.design_graph_binding_index.connection_bindings) {
        if (binding.kind == SnapshotConeEdgeKind::ParameterOverride) {
            endpoint_ids.push_back(binding.endpoint_stable_id);
        }
    }
    REQUIRE(endpoint_ids.size() == 2);
    CHECK(endpoint_ids[0] != endpoint_ids[1]);
}

TEST_CASE("AstIndex retains unresolved parameter override sources without global recovery",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][unresolved][no-fallback]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; child #(.WIDTH(missing_width)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto binding = std::find_if(view.design_graph_binding_index.connection_bindings.begin(),
                                      view.design_graph_binding_index.connection_bindings.end(),
                                      [](const auto& value) {
                                          return value.kind == SnapshotConeEdgeKind::ParameterOverride;
                                      });
    REQUIRE(binding != view.design_graph_binding_index.connection_bindings.end());
    CHECK(binding->unresolved);
    CHECK(std::none_of(view.cone_adjacency_index.edges.begin(),
                       view.cone_adjacency_index.edges.end(),
                       [](const auto& edge) {
                           return edge.kind == SnapshotConeEdgeKind::ParameterOverride;
                       }));
}

TEST_CASE("AstIndex lowers named literal parameter overrides without syntax recovery",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][literal][named]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; child #(.WIDTH(8)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK_FALSE(facts.front()->unresolved);
    CHECK(facts.front()->source_parts.empty());
}

TEST_CASE("AstIndex lowers positional literal parameter overrides without syntax recovery",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][literal][positional]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; child #(8) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK_FALSE(facts.front()->unresolved);
    CHECK(facts.front()->source_parts.empty());
}

TEST_CASE("AstIndex retains parent parameter identity in a named override",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][parent]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top #(parameter int WIDTH = 8); child #(.WIDTH(WIDTH)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK_FALSE(facts.front()->source_parts.front().source_symbol_id.empty());
}

TEST_CASE("AstIndex retains each source in an arithmetic parameter override",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][arithmetic]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int A = 2; localparam int B = 3; child #(.WIDTH(A + B)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts.front()->source_parts.size() == 2);
}

TEST_CASE("AstIndex retains static bit-select parameter override slices",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][bit-select]") {
    auto output = buildGraphSource(
        "module child #(parameter logic BIT = 1'b0)(); endmodule\n"
        "module top(input logic [3:0] data); child #(.BIT(data[2])) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Exact);
}

TEST_CASE("AstIndex retains static part-select parameter override slices",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][part-select]") {
    auto output = buildGraphSource(
        "module child #(parameter logic [1:0] PART = 2'b00)(); endmodule\n"
        "module top(input logic [3:0] data); child #(.PART(data[3:2])) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Exact);
}

TEST_CASE("AstIndex marks dynamic parameter override selects without an exact slice",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][dynamic-select][no-fallback]") {
    auto output = buildGraphSource(
        "module child #(parameter logic BIT = 1'b0)(); endmodule\n"
        "module top; logic [3:0] data; logic [1:0] index; child #(.BIT(data[index])) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(std::any_of(facts.front()->source_parts.begin(),
                      facts.front()->source_parts.end(),
                      [](const auto& part) {
                          return part.source_slice.precision == SnapshotConeSlicePrecision::Dynamic;
                      }));
}

TEST_CASE("AstIndex keeps every source part in a parameter override concatenation",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][concat]") {
    auto output = buildGraphSource(
        "module child #(parameter logic [1:0] PAIR = 2'b00)(); endmodule\n"
        "module top; localparam logic A = 1'b0; localparam logic B = 1'b1; child #(.PAIR({A, B})) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts.front()->source_parts.size() == 2);
}

TEST_CASE("AstIndex keeps nested parameter override concatenation sources deterministic",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][nested-concat]") {
    auto output = buildGraphSource(
        "module child #(parameter logic [3:0] WORD = 4'b0000)(); endmodule\n"
        "module top; localparam logic A = 1'b0; localparam logic B = 1'b1; localparam logic [1:0] C = 2'b10; child #(.WORD({{A, B}, C})) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts.front()->source_parts.size() == 3);
}

TEST_CASE("AstIndex preserves child parameter declaration order for reverse named overrides",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][named-order]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1, parameter int DEPTH = 2)(); endmodule\n"
        "module top; localparam int W = 4; localparam int D = 8; child #(.DEPTH(D), .WIDTH(W)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts[0]->endpoint_index == 0);
    CHECK(facts[1]->endpoint_index == 1);
}

TEST_CASE("AstIndex indexes only explicitly supplied parameter overrides",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][default]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1, parameter int DEPTH = 2)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts.front()->endpoint_index == 0);
}

TEST_CASE("AstIndex keeps parameter overrides separate from named port bindings",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][named-port]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(input logic in, output logic out); assign out = in; endmodule\n"
        "module top; localparam int W = 4; logic a; logic y; child #(.WIDTH(W)) u_child(.in(a), .out(y)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::count_if(view.design_graph_binding_index.connection_bindings.begin(),
                        view.design_graph_binding_index.connection_bindings.end(),
                        [](const auto& value) { return value.kind == SnapshotConeEdgeKind::InstancePort; }) == 2);
}

TEST_CASE("AstIndex keeps parameter overrides separate from positional port bindings",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][positional-port]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(input logic in, output logic out); assign out = in; endmodule\n"
        "module top; localparam int W = 4; logic a; logic y; child #(W) u_child(a, y); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    CHECK(std::count_if(view.design_graph_binding_index.connection_bindings.begin(),
                        view.design_graph_binding_index.connection_bindings.end(),
                        [](const auto& value) { return value.kind == SnapshotConeEdgeKind::InstancePort; }) == 2);
}

TEST_CASE("AstIndex retains direct parameter endpoints for descending packed values",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][descending-range]") {
    auto output = buildGraphSource(
        "module child #(parameter logic [3:0] VALUE = 4'b0000)(); endmodule\n"
        "module top(input logic [3:0] value); child #(.VALUE(value)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Whole);
}

TEST_CASE("AstIndex retains direct parameter endpoints for ascending packed values",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][ascending-range]") {
    auto output = buildGraphSource(
        "module child #(parameter logic [0:3] VALUE = 4'b0000)(); endmodule\n"
        "module top(input logic [0:3] value); child #(.VALUE(value)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Whole);
}

TEST_CASE("AstIndex tracks parameter overrides for distinct instances in one declaration",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][multi-instance]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int A = 2; localparam int B = 3; child #(.WIDTH(A)) left(), right(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts[0]->instance_stable_id != facts[1]->instance_stable_id);
}

TEST_CASE("AstIndex clears parameter syntax and AST scratch views before provider queries",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][scratch-state]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u_child(); endmodule\n");
    REQUIRE(output.data != nullptr);
    CHECK(output.data->parameter_override_syntax_facts.empty());
    CHECK(output.data->instance_symbols_by_stable_id.empty());
    CHECK_FALSE(parameterOverrideFacts(*output.data).empty());
}

TEST_CASE("AstIndex gives matching parameter syntax locations a stable direct endpoint",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][location]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4;\n  child #(.WIDTH(W)) u_child();\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = parameterOverrideFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(hasDirectParameterEndpoints(facts));
    CHECK(facts.front()->location.uri == "file:///workspace/top.sv");
    CHECK(facts.front()->location.range.start_line == 2);
}

TEST_CASE("AstIndex preserves parameter override endpoint identities across document order",
          "[analysis][semantic][ast-index][design-graph-binding][parameter][deterministic]") {
    auto first = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u_child(); endmodule\n",
        92);
    auto second = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u_child(); endmodule\n",
        93);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    const auto first_facts = parameterOverrideFacts(*first.data);
    const auto second_facts = parameterOverrideFacts(*second.data);
    REQUIRE(first_facts.size() == 1);
    REQUIRE(second_facts.size() == 1);
    CHECK(first_facts.front()->endpoint_stable_id == second_facts.front()->endpoint_stable_id);
}

TEST_CASE("AstIndex indexes every resolved source in a complex instance connection",
          "[analysis][semantic][ast-index][design-graph-binding][connection]") {
    auto output = buildGraphSource(
        "module child(input logic in, output logic out); assign out = in; endmodule\n"
        "module top; logic a; logic b; logic y;\n"
        "  child u_child(.in(a & b), .out(y));\nendmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto binding = std::find_if(view.design_graph_binding_index.connection_bindings.begin(),
                                      view.design_graph_binding_index.connection_bindings.end(),
                                      [](const SnapshotGraphConnectionBindingFact& value) {
                                          return value.kind == SnapshotConeEdgeKind::InstancePort &&
                                                 value.source_symbol_ids.size() == 2;
                                      });
    REQUIRE(binding != view.design_graph_binding_index.connection_bindings.end());
    CHECK(binding->source_symbol_ids[0] < binding->source_symbol_ids[1]);
}

TEST_CASE("AstIndex maps positional instance ports through graph endpoint facts",
          "[analysis][semantic][ast-index][design-graph-binding][positional]") {
    auto output = buildGraphSource(
        "module child(input logic in, output logic out); assign out = in; endmodule\n"
        "module top; logic a; logic y; child u_child(a, y); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto port_bindings = std::count_if(view.design_graph_binding_index.connection_bindings.begin(),
                                             view.design_graph_binding_index.connection_bindings.end(),
                                             [](const SnapshotGraphConnectionBindingFact& value) {
                                                 return value.kind == SnapshotConeEdgeKind::InstancePort;
                                             });
    CHECK(port_bindings == 2);
}

TEST_CASE("AstIndex records interface endpoints without inventing cone direction",
          "[analysis][semantic][ast-index][design-graph-binding][interface][no-fallback]") {
    auto output = buildGraphSource(
        "interface bus_if; logic data; endinterface\n"
        "module child(bus_if bus); endmodule\n"
        "module top; bus_if bus(); child u_child(.bus(bus)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto endpoint = view.design_graph_binding_index.endpoints_by_module_member.find("child\x1f" "bus");
    REQUIRE(endpoint != view.design_graph_binding_index.endpoints_by_module_member.end());
    CHECK(endpoint->second.kind == SnapshotGraphEndpointKind::InterfacePort);
    CHECK(endpoint->second.direction == SnapshotGraphPortDirection::Unknown);
    CHECK(std::none_of(view.cone_adjacency_index.edges.begin(),
                       view.cone_adjacency_index.edges.end(),
                       [&](const SnapshotConeAdjacencyEdge& edge) {
                           return edge.from_symbol_id == endpoint->second.stable_id ||
                                  edge.to_symbol_id == endpoint->second.stable_id;
                       }));
}

TEST_CASE("AstIndex connects resolved modport members with indexed directions",
          "[analysis][semantic][ast-index][design-graph-binding][interface][modport][cone]") {
    auto output = buildGraphSource(
        "interface bus_if;\n"
        "  logic ready;\n"
        "  logic valid;\n"
        "  modport master(input ready, output valid);\n"
        "endinterface\n"
        "module child(bus_if.master bus); endmodule\n"
        "module top; bus_if bus(); child u_child(.bus(bus.master)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    const auto endpoint = view.design_graph_binding_index.endpoints_by_module_member.find("child\x1f" "bus");
    REQUIRE(endpoint != view.design_graph_binding_index.endpoints_by_module_member.end());
    REQUIRE_FALSE(endpoint->second.modport_stable_id.empty());
    const auto port_binding = view.interface_modport_binding_index.ports_by_stable_id.find(endpoint->second.stable_id);
    REQUIRE(port_binding != view.interface_modport_binding_index.ports_by_stable_id.end());
    REQUIRE(port_binding->second.resolved);
    REQUIRE_FALSE(port_binding->second.connected_modport_stable_id.empty());

    const auto& child_members = view.interface_modport_binding_index.members_by_modport_stable_id.at(
        port_binding->second.modport_stable_id);
    const auto& parent_members = view.interface_modport_binding_index.members_by_modport_stable_id.at(
        port_binding->second.connected_modport_stable_id);
    const auto child_ready = std::find_if(child_members.begin(), child_members.end(), [](const auto& member) {
        return member.name == "ready";
    });
    const auto parent_ready = std::find_if(parent_members.begin(), parent_members.end(), [](const auto& member) {
        return member.name == "ready";
    });
    REQUIRE(child_ready != child_members.end());
    REQUIRE(parent_ready != parent_members.end());
    CHECK(std::any_of(view.cone_adjacency_index.edges.begin(),
                      view.cone_adjacency_index.edges.end(),
                      [&](const SnapshotConeAdjacencyEdge& edge) {
                          return edge.from_symbol_id == child_ready->stable_id &&
                                 edge.to_symbol_id == parent_ready->stable_id &&
                                 edge.kind == SnapshotConeEdgeKind::InstancePort;
                      }));
}

TEST_CASE("AstIndex keeps generated connection bindings distinct",
          "[analysis][semantic][ast-index][design-graph-binding][generated]") {
    SnapshotBuildInput input{
        .generation = 76,
        .config = SemanticEngineConfig{.top_modules = {"top"}},
        .documents = {{"file:///workspace/generated-bindings.sv",
                       SemanticEngineDocument{.uri = "file:///workspace/generated-bindings.sv",
                                              .text = "module child(input logic in, output logic out); assign out = in; endmodule\n"
                                                      "module top; logic a; logic y0; logic y1; genvar i; generate\n"
                                                      "  for (i = 0; i < 2; i = i + 1) begin : g\n"
                                                      "    child u(.in(a), .out(i ? y1 : y0));\n  end\nendgenerate endmodule\n",
                                              .is_open = true}}}};
    auto output = SnapshotBuilder{}.build(std::move(input));
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);

    std::vector<std::string> generated_ids;
    for (const auto& binding : view.design_graph_binding_index.connection_bindings) {
        if (binding.kind == SnapshotConeEdgeKind::InstancePort) {
            generated_ids.push_back(binding.instance_stable_id);
        }
    }
    std::sort(generated_ids.begin(), generated_ids.end());
    generated_ids.erase(std::unique(generated_ids.begin(), generated_ids.end()), generated_ids.end());
    CHECK(generated_ids.size() >= 2);
}

TEST_CASE("AstIndex keeps connection binding ordering deterministic across equivalent builds",
          "[analysis][semantic][ast-index][design-graph-binding][deterministic]") {
    const auto build = [](std::uint64_t generation) {
        return buildGraphSource(
            "module child(input logic in, output logic out); assign out = in; endmodule\n"
            "module top; logic a; logic b; logic y; child u(.in(a & b), .out(y)); endmodule\n",
            generation);
    };
    auto first = build(77);
    auto second = build(78);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    const auto& lhs = first.data->design_graph_binding_index.connection_bindings;
    const auto& rhs = second.data->design_graph_binding_index.connection_bindings;
    REQUIRE(lhs.size() == rhs.size());
    for (size_t index = 0; index < lhs.size(); ++index) {
        CHECK(lhs[index].instance_stable_id == rhs[index].instance_stable_id);
        CHECK(lhs[index].endpoint_stable_id == rhs[index].endpoint_stable_id);
        CHECK(lhs[index].source_symbol_ids == rhs[index].source_symbol_ids);
    }
}

TEST_CASE("AstIndex projects named module connections into schematic facts",
          "[analysis][semantic][ast-index][schematic-projection][named]") {
    auto output = buildGraphSource(
        "module child(input logic in, output logic out); endmodule\n"
        "module top; logic a; logic y; child u(.in(a), .out(y)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(std::all_of(facts.begin(), facts.end(), [](const auto* fact) {
        return fact->caller_module_name == "top" && fact->kind == SnapshotConeEdgeKind::InstancePort &&
               !fact->endpoint_stable_id.empty() && !fact->display_label.empty();
    }));
}

TEST_CASE("AstIndex projects positional module connections into schematic facts",
          "[analysis][semantic][ast-index][schematic-projection][positional]") {
    auto output = buildGraphSource(
        "module child(input logic in, output logic out); endmodule\n"
        "module top; logic a; logic y; child u(a, y); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(facts[0]->endpoint_index == 0);
    CHECK(facts[1]->endpoint_index == 1);
}

TEST_CASE("AstIndex projects concatenated schematic sources without raw expression recovery",
          "[analysis][semantic][ast-index][schematic-projection][concat]") {
    auto output = buildGraphSource(
        "module child(input logic [1:0] in); endmodule\n"
        "module top; logic a; logic b; child u(.in({a, b})); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->source_parts.size() == 2);
    CHECK(facts.front()->display_label == "{a,b}");
    CHECK(facts.front()->display_label.find('&') == std::string::npos);
}

TEST_CASE("AstIndex preserves static bit-select slice projection for schematic facts",
          "[analysis][semantic][ast-index][schematic-projection][bit-select]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic [3:0] bus; child u(.in(bus[2])); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    REQUIRE(facts.front()->source_parts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(facts.front()->display_label == "bus[2]");
}

TEST_CASE("AstIndex preserves static part-select slice projection for schematic facts",
          "[analysis][semantic][ast-index][schematic-projection][part-select]") {
    auto output = buildGraphSource(
        "module child(input logic [1:0] in); endmodule\n"
        "module top; logic [3:0] bus; child u(.in(bus[3:2])); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->source_parts.front().source_slice.precision == SnapshotConeSlicePrecision::Exact);
    CHECK(facts.front()->display_label == "bus[3:2]");
}

TEST_CASE("AstIndex marks dynamic schematic connections partial",
          "[analysis][semantic][ast-index][schematic-projection][dynamic]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic [3:0] bus; logic [1:0] index; child u(.in(bus[index])); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->display_label == "<partial>");
    CHECK(output.data->design_graph_binding_index.schematic_partial_connection_fact_count == 1);
}

TEST_CASE("AstIndex projects constant module connections without synthesizing a net source",
          "[analysis][semantic][ast-index][schematic-projection][constant]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; child u(.in(1'b1)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->display_label == "<constant>");
    CHECK(facts.front()->source_parts.empty());
}

TEST_CASE("AstIndex projects named parameter overrides without text parsing",
          "[analysis][semantic][ast-index][schematic-projection][parameter][named]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->kind == SnapshotConeEdgeKind::ParameterOverride);
    CHECK(facts.front()->display_label == "W");
}

TEST_CASE("AstIndex projects positional parameter overrides without text parsing",
          "[analysis][semantic][ast-index][schematic-projection][parameter][positional]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1, parameter int DEPTH = 2)(); endmodule\n"
        "module top; localparam int W = 4; localparam int D = 8; child #(W, D) u(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(std::all_of(facts.begin(), facts.end(), [](const auto* fact) {
        return fact->kind == SnapshotConeEdgeKind::ParameterOverride && !fact->display_label.empty();
    }));
}

TEST_CASE("AstIndex keeps repeated instance schematic identities distinct",
          "[analysis][semantic][ast-index][schematic-projection][repeated]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic a; logic b; child u0(.in(a)); child u1(.in(b)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 2);
    CHECK(facts[0]->instance_stable_id != facts[1]->instance_stable_id);
    CHECK(facts[0]->display_label != facts[1]->display_label);
}

TEST_CASE("AstIndex keeps generated schematic connection identities distinct",
          "[analysis][semantic][ast-index][schematic-projection][generated]") {
    const std::vector<std::string> top_modules{"top"};
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic a; genvar i; generate for (i=0; i<2; i=i+1) begin : g child u(.in(a)); end endgenerate endmodule\n",
        91,
        top_modules);
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() >= 2);
    CHECK(output.data->design_graph_binding_index.schematic_connections_by_module.contains("top"));
    CHECK(facts[0]->instance_stable_id != facts[1]->instance_stable_id);
}
TEST_CASE("AstIndex carries interface endpoint direction into schematic facts",
          "[analysis][semantic][ast-index][schematic-projection][interface]") {
    auto output = buildGraphSource(
        "interface bus_if; logic ready; endinterface\n"
        "module child(bus_if bus); endmodule\n"
        "module top; bus_if bus(); child u(.bus(bus)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->endpoint_direction == SnapshotGraphPortDirection::Unknown);
    CHECK(facts.front()->endpoint_name == "bus");
}

TEST_CASE("AstIndex projection updates schematic cell connections from typed facts",
          "[analysis][semantic][ast-index][schematic-projection][cell]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic a; child u(.in(a)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto view = buildAstIndexView(output.data.get(), output.snapshot.generation);
    const auto* cell = schematicCell(view, "top", "u");
    REQUIRE(cell != nullptr);
    REQUIRE(cell->connections.size() == 1);
    CHECK(cell->connections.front().port_name == "in");
    CHECK(cell->connections.front().signal == "a");
}

TEST_CASE("AstIndex projection retains parameter display on module instances",
          "[analysis][semantic][ast-index][schematic-projection][parameter][display]") {
    auto output = buildGraphSource(
        "module child #(parameter int WIDTH = 1)(); endmodule\n"
        "module top; localparam int W = 4; child #(.WIDTH(W)) u(); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto instances = output.data->module_instances_by_uri.find("file:///workspace/top.sv");
    REQUIRE(instances != output.data->module_instances_by_uri.end());
    REQUIRE(instances->second.size() == 1);
    REQUIRE(instances->second.front().parameter_connections.size() == 1);
    CHECK(instances->second.front().parameter_connections.front().signal == "W");
}

TEST_CASE("AstIndex schematic projection ordering is deterministic",
          "[analysis][semantic][ast-index][schematic-projection][deterministic]") {
    const auto build = [](std::uint64_t generation) {
        return buildGraphSource(
            "module child(input logic a, input logic b); endmodule\n"
            "module top; logic a; logic b; child u(.b(b), .a(a)); endmodule\n", generation);
    };
    auto first = build(801);
    auto second = build(802);
    REQUIRE(first.data != nullptr);
    REQUIRE(second.data != nullptr);
    const auto lhs = schematicConnectionFacts(*first.data);
    const auto rhs = schematicConnectionFacts(*second.data);
    REQUIRE(lhs.size() == rhs.size());
    for (size_t index = 0; index < lhs.size(); ++index) {
        CHECK(lhs[index]->endpoint_stable_id == rhs[index]->endpoint_stable_id);
        CHECK(lhs[index]->display_label == rhs[index]->display_label);
    }
}

TEST_CASE("AstIndex schematic facts keep shadowed source identities separate",
          "[analysis][semantic][ast-index][schematic-projection][shadowing]") {
    const std::vector<std::string> top_modules{"top"};
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic data; generate begin : scope logic data; child u(.in(data)); end endgenerate endmodule\n",
        91,
        top_modules);
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    REQUIRE(facts.front()->source_parts.size() == 1);
    const auto source = output.data->symbols_by_id.find(facts.front()->source_parts.front().source_symbol_id);
    REQUIRE(source != output.data->symbols_by_id.end());
    CHECK(source->second.identity.location.range.start_line == 1);
}
TEST_CASE("AstIndex leaves missing module connections without a typed schematic fallback",
          "[analysis][semantic][ast-index][schematic-projection][unresolved][no-fallback]") {
    auto output = buildGraphSource("module top; logic y; missing_child u(.out(y)); endmodule\n");
    REQUIRE(output.data != nullptr);
    CHECK(schematicConnectionFacts(*output.data).empty());
    CHECK(output.data->design_graph_binding_index.schematic_connection_fact_count == 0);
}

TEST_CASE("AstIndex records typed schematic projection counters",
          "[analysis][semantic][ast-index][schematic-projection][metrics]") {
    auto output = buildGraphSource(
        "module child(input logic in, output logic out); endmodule\n"
        "module top; logic a; logic y; child u(.in(a), .out(y)); endmodule\n");
    REQUIRE(output.data != nullptr);
    CHECK(output.data->design_graph_binding_index.schematic_connection_fact_count == 2);
    CHECK(output.data->design_graph_binding_index.schematic_partial_connection_fact_count == 0);
}

TEST_CASE("AstIndex does not preserve raw binary connection text in schematic display",
          "[analysis][semantic][ast-index][schematic-projection][no-text-fallback]") {
    auto output = buildGraphSource(
        "module child(input logic in); endmodule\n"
        "module top; logic a; logic b; child u(.in(a & b)); endmodule\n");
    REQUIRE(output.data != nullptr);
    const auto facts = schematicConnectionFacts(*output.data);
    REQUIRE(facts.size() == 1);
    CHECK(facts.front()->display_label == "<partial>");
    CHECK(output.data->design_graph_binding_index.schematic_partial_connection_fact_count == 1);
    CHECK(facts.front()->display_label.find('&') == std::string::npos);
}

} // namespace
} // namespace pristine::analysis::semantic
