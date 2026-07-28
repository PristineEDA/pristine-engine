#include "pristine/analysis/SemanticEngine.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMicros(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

const auto kStatusStart = Clock::now();

double elapsedSeconds() {
    return std::chrono::duration<double>(Clock::now() - kStatusStart).count();
}

void emitStatus(std::string_view phase, std::string_view detail = {}) {
    std::cerr << "[pristine-test] test=pristine_perf_tests phase=" << phase
              << " elapsed=" << elapsedSeconds() << "s";
    if (!detail.empty()) {
        std::cerr << " detail=" << detail;
    }
    std::cerr << '\n';
}

void writeQueryCacheStats(const pristine::analysis::SemanticQueryCacheStats& stats) {
    std::cout << "\"queryCacheHits\":" << stats.hits << ","
              << "\"queryCacheMisses\":" << stats.misses << ","
              << "\"queryCacheStores\":" << stats.stores << ","
              << "\"queryCacheEvictions\":" << stats.evictions << ","
              << "\"queryCacheEntries\":" << stats.total_entries << ","
              << "\"queryCacheWorkspaceSymbolEntries\":" << stats.workspace_symbols_entries << ","
              << "\"queryCacheModuleHierarchyEntries\":" << stats.module_hierarchy_entries << ","
              << "\"queryCacheSchematicEntries\":" << stats.schematic_entries << ","
              << "\"queryCacheBackwardConeEntries\":" << stats.backward_cone_entries << ","
              << "\"referenceLookupScannedOccurrences\":"
              << stats.reference_lookup_scanned_occurrences << ","
              << "\"callHierarchyScannedEdges\":" << stats.call_hierarchy_scanned_edges << ","
              << "\"callHierarchyScannedModules\":" << stats.call_hierarchy_scanned_modules << ","
              << "\"graphBindingLookupScannedFacts\":"
              << stats.graph_binding_lookup_scanned_facts << ","
              << "\"coneAdjacencyScannedEdges\":" << stats.cone_adjacency_scanned_edges << ","
              << "\"graphScannedGlobalSymbols\":" << stats.graph_scanned_global_symbols << ","
              << "\"coneScannedGlobalEdges\":" << stats.cone_scanned_global_edges << ",";
}

} // namespace

int main() {
    pristine::analysis::SemanticEngine engine;

    std::vector<int> workspace_sizes{100, 1000, 5000};
    bool unresolved = false;
    bool completion_contract_failed = false;
    std::cout << "{\"baselines\":[";
    bool first_baseline = true;

    for (const int workspace_size : workspace_sizes) {
        emitStatus("baseline", "documents=" + std::to_string(workspace_size));
        engine.clear();

        const auto document_text = [](int index, bool include_workspace_probe) {
            const auto name = std::string("sig_") + std::to_string(index);
            if (index == 0) {
                return std::string("module unit_0 #(parameter int WIDTH = 1)(input logic in, output logic out);\n") +
                       "  assign out = in;\nendmodule\n";
            }
            auto text = std::string("module unit_") + std::to_string(index) + " #(parameter int WIDTH = 4);\n" +
                        "  logic " + name + ";\n" +
                        "  assign " + name + " = " + name + ";\n";
            if (include_workspace_probe) {
                text += "  logic connected;\n";
                text += "  unit_0 #(.WIDTH(WIDTH)) u0(.in(" + name + " & " + name + "), .out(connected));\n";
                text += "  function automatic int add(input int lhs, input int rhs);\n";
                text += "    return lhs + rhs;\n";
                text += "  endfunction\n";
                text += "  localparam int sum = add(1, 2);\n";
                text += "`define TWICE(x) ((x) + (x))\n";
                text += "  localparam int doubled = `TWICE(3);\n";
                text += "  logic mux_select;\n";
                text += "  logic muxed;\n";
                text += "  assign muxed = mux_select ? connected : " + name + ";\n";
                text += "  logic seq_clk;\n";
                text += "  logic seq_data;\n";
                text += "  logic seq_y;\n";
                text += "  always @(posedge seq_clk) seq_y <= seq_data;\n";
                text += "  property sampled_p(logic lhs, logic rhs); @(posedge seq_clk) lhs |-> rhs; endproperty\n";
                text += "  assert property (sampled_p(seq_data, seq_y));\n";
                text += "  default clocking @(negedge seq_clk); endclocking\n";
                text += "  default disable iff (!seq_data);\n";
                text += "  assert property (seq_data |-> seq_y);\n";
            }
            text += "endmodule\n";
            return text;
        };

        const auto start_index = Clock::now();
        for (int index = 0; index < workspace_size; ++index) {
            engine.updateDocument("file:///perf/unit_" + std::to_string(index) + ".sv",
                                  document_text(index, false),
                                  pristine::analysis::SemanticEngineDocumentState{.version = 1});
        }
        const auto end_index = Clock::now();

        const auto target_index = workspace_size == 100 ? 42 : 420;
        const auto target_uri = "file:///perf/unit_" + std::to_string(target_index) + ".sv";
        engine.updateDocument(target_uri,
                              document_text(target_index, true),
                              pristine::analysis::SemanticEngineDocumentState{.version = 2});

        const auto start_hover = Clock::now();
        const auto hover = engine.hoverAt(target_uri, 1, 10);
        const auto end_hover = Clock::now();

        const auto start_completion = Clock::now();
        const auto completion = engine.completionsAt(target_uri, 1, 12, "sig_");
        const auto end_completion = Clock::now();
        const auto start_completion_warm = Clock::now();
        const auto completion_warm = engine.completionsAt(target_uri, 1, 12, "sig_");
        const auto end_completion_warm = Clock::now();
        long long completion_resolve_micros = 0;
        if (!completion.items.empty()) {
            const auto start_completion_resolve = Clock::now();
            (void)engine.resolveCompletion(completion.items.front().stable_id,
                                           completion.items.front().label);
            completion_resolve_micros = elapsedMicros(start_completion_resolve, Clock::now());
        }

        const auto start_workspace_completion = Clock::now();
        const auto workspace_completion = engine.completionsAt(target_uri, 4, 6, "unit");
        const auto workspace_completion_micros = elapsedMicros(start_workspace_completion, Clock::now());

        const auto start_query = Clock::now();
        const auto references = engine.referencesAt(target_uri, 1, 10, true);
        const auto end_query = Clock::now();
        const auto start_query_warm = Clock::now();
        const auto references_warm = engine.referencesAt(target_uri, 1, 10, true);
        const auto end_query_warm = Clock::now();
        const auto start_highlight = Clock::now();
        const auto highlights = engine.documentHighlightsAt(target_uri, 1, 10);
        const auto end_highlight = Clock::now();

        const auto start_rename = Clock::now();
        const auto rename = engine.renameAt(target_uri, 1, 10, "renamed");
        const auto end_rename = Clock::now();

        const auto start_signature = Clock::now();
        const auto signature = engine.signatureHelpAt(target_uri, 8, 29);
        const auto end_signature = Clock::now();
        const auto start_signature_warm = Clock::now();
        const auto signature_warm = engine.signatureHelpAt(target_uri, 8, 29);
        const auto end_signature_warm = Clock::now();

        const auto start_inlay = Clock::now();
        const auto inlay = engine.inlayHints(target_uri,
                                             pristine::analysis::ParseRange{.start_line = 0,
                                                                            .start_character = 0,
                                                                            .end_line = 12,
                                                                            .end_character = 0});
        const auto end_inlay = Clock::now();
        const auto start_inlay_warm = Clock::now();
        const auto inlay_warm = engine.inlayHints(target_uri,
                                                  pristine::analysis::ParseRange{
                                                      .start_line = 0,
                                                      .start_character = 0,
                                                                            .end_line = 12,
                                                      .end_character = 0});
        const auto end_inlay_warm = Clock::now();

        const auto start_macro_definition = Clock::now();
        const auto macro_definition = engine.definitionsAt(target_uri, 10, 31);
        const auto end_macro_definition = Clock::now();
        const auto start_macro_expand = Clock::now();
        const auto macro_expand = engine.codeActionsAt(
            target_uri,
            pristine::analysis::ParseRange{.start_line = 10,
                                           .start_character = 27,
                                           .end_line = 10,
                                           .end_character = 36});
        const auto end_macro_expand = Clock::now();

        const auto start_semantic_tokens = Clock::now();
        const auto semantic_tokens = engine.semanticTokens(target_uri);
        const auto end_semantic_tokens = Clock::now();

        const auto target_module_name = std::string("unit_") + std::to_string(target_index);

        const auto start_hierarchy = Clock::now();
        const auto hierarchy = engine.moduleHierarchy(target_module_name, 4);
        const auto end_hierarchy = Clock::now();
        const auto hierarchy_warm = engine.moduleHierarchy(target_module_name, 4);

        const auto start_call_hierarchy = Clock::now();
        const auto call_prepare = engine.prepareCallHierarchy(target_uri, 0, 8);
        pristine::analysis::SemanticCallHierarchyCallsResult call_outgoing;
        if (!call_prepare.items.empty()) {
            call_outgoing = engine.outgoingCalls(call_prepare.items.front());
        }
        const auto end_call_hierarchy = Clock::now();

        const auto start_schematic = Clock::now();
        const auto schematic = engine.schematic(target_module_name, 4);
        const auto end_schematic = Clock::now();
        const auto schematic_warm = engine.schematic(target_module_name, 4);

        const auto start_cone = Clock::now();
        const auto cone = engine.backwardConeAt(target_uri, 3, 10);
        const auto end_cone = Clock::now();
        const auto cone_warm = engine.backwardConeAt(target_uri, 3, 10);
        const auto start_ternary_cone = Clock::now();
        const auto ternary_cone = engine.backwardConeAt(target_uri, 13, 10);
        const auto end_ternary_cone = Clock::now();
        const auto ternary_cone_warm = engine.backwardConeAt(target_uri, 13, 10);
        const auto start_event_cone = Clock::now();
        const auto event_cone = engine.backwardConeAt(target_uri, 17, 30);
        const auto end_event_cone = Clock::now();
        const auto event_cone_warm = engine.backwardConeAt(target_uri, 17, 30);
        const auto start_assertion_cone = Clock::now();
        const auto assertion_cone = engine.backwardConeAt(target_uri, 19, 2);
        const auto end_assertion_cone = Clock::now();
        const auto assertion_cone_warm = engine.backwardConeAt(target_uri, 19, 2);
        const auto start_default_assertion_cone = Clock::now();
        const auto default_assertion_cone = engine.backwardConeAt(target_uri, 22, 2);
        const auto end_default_assertion_cone = Clock::now();
        const auto default_assertion_cone_warm = engine.backwardConeAt(target_uri, 22, 2);

        const auto start_code_action = Clock::now();
        const auto code_actions = engine.codeActionsAt(
            target_uri,
            pristine::analysis::ParseRange{.start_line = 0,
                                           .start_character = 0,
                                           .end_line = 11,
                                           .end_character = 0});
        const auto end_code_action = Clock::now();
        const auto cache_stats = engine.queryCacheStats();

        completion_contract_failed = completion_contract_failed || completion.unresolved ||
                                     completion_warm.unresolved || completion.items.empty() ||
                                     completion_warm.items.empty() ||
                                     completion.scanned_global_symbol_count != 0 ||
                                     workspace_completion.unresolved ||
                                     workspace_completion.items.empty() ||
                                     workspace_completion.scanned_global_symbol_count != 0 ||
                                     signature.unresolved || signature_warm.unresolved ||
                                     signature.label.empty() || signature_warm.label != signature.label ||
                                     signature.scanned_global_symbol_count != 0 ||
                                     inlay.unresolved || inlay_warm.unresolved ||
                                     inlay.scanned_global_symbol_count != 0 ||
                                     macro_definition.locations.empty() || macro_expand.actions.empty() ||
                                     references.unresolved || references_warm.unresolved ||
                                     references.locations.size() != references_warm.locations.size() ||
                                     highlights.unresolved ||
                                     call_prepare.unresolved || call_prepare.items.empty() ||
                                     call_outgoing.unresolved || cache_stats.call_hierarchy_scanned_modules != 0 ||
                                     hierarchy_warm.unresolved || schematic_warm.unresolved ||
                                     schematic.schematic_connection_fact_lookup_count == 0 ||
                                     schematic.schematic_source_part_scan_count == 0 ||
                                     schematic.schematic_cell_pin_fact_lookup_count == 0 ||
                                     schematic.schematic_cell_pin_scan_count == 0 ||
                                     cache_stats.schematic_entries == 0 || cone_warm.unresolved ||
                                      ternary_cone.unresolved || ternary_cone_warm.unresolved ||
                                      event_cone.unresolved || event_cone_warm.unresolved ||
                                      assertion_cone.unresolved || assertion_cone_warm.unresolved ||
                                      default_assertion_cone.unresolved ||
                                      default_assertion_cone_warm.unresolved ||
                                      cone.nodes.size() < 2 || cache_stats.cone_adjacency_scanned_edges == 0 ||
                                      ternary_cone.cone_control_edge_count == 0 ||
                                      ternary_cone.cone_ternary_control_edge_count == 0 ||
                                      event_cone.cone_event_control_edge_count == 0 ||
                                      event_cone.cone_timing_fact_lookup_count == 0 ||
                                      assertion_cone.cone_assertion_sample_edge_count == 0 ||
                                      assertion_cone.cone_assertion_invocation_edge_count == 0 ||
                                      assertion_cone.cone_assertion_clock_edge_count == 0 ||
                                      default_assertion_cone.cone_assertion_default_clock_edge_count == 0 ||
                                      default_assertion_cone.cone_assertion_default_disable_edge_count == 0 ||
                                      cache_stats.graph_scanned_global_symbols != 0 ||
                                     cache_stats.cone_scanned_global_edges != 0;

        if (!first_baseline) {
            std::cout << ",";
        }
        first_baseline = false;
        std::cout << "{"
                  << "\"documents\":" << workspace_size << ","
                  << "\"initializeMicros\":" << elapsedMicros(start_index, end_index) << ","
                  << "\"didOpenMicros\":" << elapsedMicros(start_index, end_index) << ","
                  << "\"didChangeMicros\":0,"
                  << "\"hoverMicros\":" << elapsedMicros(start_hover, end_hover) << ","
                  << "\"completionMicros\":" << elapsedMicros(start_completion, end_completion) << ","
                  << "\"completionWarmMicros\":"
                  << elapsedMicros(start_completion_warm, end_completion_warm) << ","
                  << "\"completionResolveMicros\":" << completion_resolve_micros << ","
                  << "\"workspaceCompletionMicros\":" << workspace_completion_micros << ","
                  << "\"referenceMicros\":" << elapsedMicros(start_query, end_query) << ","
                  << "\"referenceWarmMicros\":" << elapsedMicros(start_query_warm, end_query_warm) << ","
                  << "\"documentHighlightMicros\":" << elapsedMicros(start_highlight, end_highlight) << ","
                  << "\"renameMicros\":" << elapsedMicros(start_rename, end_rename) << ","
                  << "\"signatureHelpMicros\":" << elapsedMicros(start_signature, end_signature) << ","
                  << "\"signatureHelpWarmMicros\":"
                  << elapsedMicros(start_signature_warm, end_signature_warm) << ","
                  << "\"inlayHintMicros\":" << elapsedMicros(start_inlay, end_inlay) << ","
                  << "\"inlayHintWarmMicros\":" << elapsedMicros(start_inlay_warm, end_inlay_warm) << ","
                  << "\"macroDefinitionMicros\":"
                  << elapsedMicros(start_macro_definition, end_macro_definition) << ","
                  << "\"macroExpandMicros\":" << elapsedMicros(start_macro_expand, end_macro_expand) << ","
                  << "\"semanticTokensMicros\":" << elapsedMicros(start_semantic_tokens, end_semantic_tokens) << ","
                  << "\"workspaceSymbolMicros\":0,"
                  << "\"moduleHierarchyMicros\":" << elapsedMicros(start_hierarchy, end_hierarchy) << ","
                  << "\"callHierarchyMicros\":"
                  << elapsedMicros(start_call_hierarchy, end_call_hierarchy) << ","
                  << "\"schematicMicros\":" << elapsedMicros(start_schematic, end_schematic) << ","
                  << "\"backwardConeMicros\":" << elapsedMicros(start_cone, end_cone) << ","
                  << "\"ternaryBackwardConeMicros\":"
                  << elapsedMicros(start_ternary_cone, end_ternary_cone) << ","
                  << "\"eventBackwardConeMicros\":"
                  << elapsedMicros(start_event_cone, end_event_cone) << ","
                  << "\"assertionBackwardConeMicros\":"
                  << elapsedMicros(start_assertion_cone, end_assertion_cone) << ","
                  << "\"defaultAssertionBackwardConeMicros\":"
                  << elapsedMicros(start_default_assertion_cone, end_default_assertion_cone) << ","
                  << "\"codeActionMicros\":" << elapsedMicros(start_code_action, end_code_action) << ","
                  ;
        writeQueryCacheStats(cache_stats);
        std::cout
                  << "\"referenceCount\":" << references.locations.size() << ","
                  << "\"referenceWarmCount\":" << references_warm.locations.size() << ","
                  << "\"documentHighlightCount\":" << highlights.locations.size() << ","
                  << "\"callHierarchyOutgoingCount\":" << call_outgoing.calls.size() << ","
                  << "\"completionCount\":" << completion.items.size() << ","
                  << "\"completionWarmCount\":" << completion_warm.items.size() << ","
                  << "\"completionScannedCandidateCount\":"
                  << completion.scanned_candidate_count << ","
                  << "\"completionScannedScopeCandidates\":"
                  << completion.scanned_scope_candidate_count << ","
                  << "\"completionScannedWorkspaceCandidates\":"
                  << completion.scanned_workspace_candidate_count << ","
                  << "\"completionScannedGlobalSymbols\":"
                  << completion.scanned_global_symbol_count << ","
                  << "\"completionContractPassed\":"
                  << (completion.unresolved || completion_warm.unresolved || completion.items.empty() ||
                              completion_warm.items.empty() || completion.scanned_global_symbol_count != 0
                              || workspace_completion.unresolved || workspace_completion.items.empty() ||
                              workspace_completion.scanned_global_symbol_count != 0
                          ? "false"
                          : "true")
                  << ","
                  << "\"workspaceCompletionCount\":" << workspace_completion.items.size() << ","
                  << "\"workspaceCompletionScannedCandidates\":"
                  << workspace_completion.scanned_candidate_count << ","
                  << "\"workspaceCompletionScannedWorkspaceCandidates\":"
                  << workspace_completion.scanned_workspace_candidate_count << ","
                  << "\"inlayHintCount\":" << inlay.hints.size() << ","
                  << "\"signatureScannedInvocations\":"
                  << signature.scanned_invocation_count << ","
                  << "\"signatureMacroScannedVisibleDefinitions\":"
                  << signature.scanned_macro_definition_count << ","
                  << "\"signatureScannedGlobalSymbols\":"
                  << signature.scanned_global_symbol_count << ","
                  << "\"inlayScannedInvocations\":" << inlay.scanned_invocation_count << ","
                  << "\"inlayMacroScannedVisibleDefinitions\":"
                  << inlay.scanned_macro_definition_count << ","
                  << "\"inlayScannedGlobalSymbols\":" << inlay.scanned_global_symbol_count << ","
                  << "\"macroDefinitionCount\":" << macro_definition.locations.size() << ","
                  << "\"macroExpandActionCount\":" << macro_expand.actions.size() << ","
                  << "\"semanticTokenCount\":" << semantic_tokens.tokens.size() << ","
                  << "\"moduleHierarchyRootCount\":" << hierarchy.roots.size() << ","
                  << "\"schematicModuleCount\":" << schematic.modules.size() << ","
                  << "\"schematicTypedConnectionFactLookups\":"
                  << schematic.schematic_connection_fact_lookup_count << ","
                  << "\"schematicLocalSourcePartScans\":"
                  << schematic.schematic_source_part_scan_count << ","
                  << "\"schematicPartialConnectionFacts\":"
                  << schematic.schematic_partial_connection_fact_count << ","
                  << "\"schematicTypedCellPinFactLookups\":"
                  << schematic.schematic_cell_pin_fact_lookup_count << ","
                  << "\"schematicLocalCellPinScans\":"
                  << schematic.schematic_cell_pin_scan_count << ","
                  << "\"schematicPartialCellPinFacts\":"
                  << schematic.schematic_partial_cell_pin_fact_count << ","
                  << "\"backwardConeNodeCount\":" << cone.nodes.size() << ","
                  << "\"coneControlEdgeCount\":" << cone.cone_control_edge_count << ","
                  << "\"ternaryBackwardConeNodeCount\":" << ternary_cone.nodes.size() << ","
                  << "\"ternaryConeControlEdgeCount\":"
                  << ternary_cone.cone_control_edge_count << ","
                  << "\"ternaryConeTernaryControlEdgeCount\":"
                  << ternary_cone.cone_ternary_control_edge_count << ","
                  << "\"eventBackwardConeNodeCount\":" << event_cone.nodes.size() << ","
                  << "\"eventConeControlEdgeCount\":"
                  << event_cone.cone_event_control_edge_count << ","
                  << "\"eventConeTimingFactLookups\":"
                  << event_cone.cone_timing_fact_lookup_count << ","
                  << "\"assertionBackwardConeNodeCount\":" << assertion_cone.nodes.size() << ","
                  << "\"assertionConeSampleEdges\":"
                  << assertion_cone.cone_assertion_sample_edge_count << ","
                  << "\"assertionConeClockEdges\":"
                  << assertion_cone.cone_assertion_clock_edge_count << ","
                  << "\"assertionConeInvocationEdges\":"
                  << assertion_cone.cone_assertion_invocation_edge_count << ","
                  << "\"assertionDefaultConeClockEdges\":"
                  << default_assertion_cone.cone_assertion_default_clock_edge_count << ","
                  << "\"assertionDefaultConeDisableEdges\":"
                  << default_assertion_cone.cone_assertion_default_disable_edge_count << ","
                  << "\"coneSliceFactCount\":" << cone.cone_slice_fact_count << ","
                  << "\"graphBuildScopedSymbolCandidates\":"
                  << cone.graph_build_scoped_symbol_candidates << ","
                  << "\"graphBuildConnectionReferenceCandidates\":"
                  << cone.graph_build_connection_reference_candidates << ","
                  << "\"codeActionCount\":" << code_actions.actions.size() << ","
                  << "\"signatureUnresolved\":" << (signature.unresolved ? "true" : "false") << ","
                  << "\"unresolved\":" << (references.unresolved || hover.unresolved || rename.unresolved ||
                                           inlay.unresolved || semantic_tokens.unresolved ||
                                           hierarchy.unresolved || schematic.unresolved || cone.unresolved ||
                                           code_actions.unresolved
                                                ? "true"
                                                : "false")
                  << "}";
        unresolved = unresolved || references.unresolved || hover.unresolved || rename.unresolved ||
                     inlay.unresolved || semantic_tokens.unresolved || hierarchy.unresolved ||
                     schematic.unresolved || cone.unresolved || code_actions.unresolved;
    }

    std::cout << "]}\n";
    const bool failed = unresolved || completion_contract_failed;
    emitStatus("summary", failed ? "status=failed" : "status=passed");
    return failed ? 1 : 0;
}
