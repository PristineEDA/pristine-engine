#include "pristine/analysis/SemanticEngine.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMicros(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

} // namespace

int main() {
    pristine::analysis::SemanticEngine engine;

    std::vector<int> workspace_sizes{100, 1000, 5000};
    bool unresolved = false;
    std::cout << "{\"baselines\":[";
    bool first_baseline = true;

    for (const int workspace_size : workspace_sizes) {
        engine.clear();

        const auto start_index = Clock::now();
        for (int index = 0; index < workspace_size; ++index) {
            const auto name = std::string("sig_") + std::to_string(index);
            engine.updateDocument("file:///perf/unit_" + std::to_string(index) + ".sv",
                                  "module unit_" + std::to_string(index) + ";\n"
                                  "  logic " + name + ";\n"
                                  "  assign " + name + " = " + name + ";\n"
                                  "endmodule\n",
                                  pristine::analysis::SemanticEngineDocumentState{.version = 1});
        }
        const auto end_index = Clock::now();

        const auto target_index = workspace_size == 100 ? 42 : 420;
        const auto target_uri = "file:///perf/unit_" + std::to_string(target_index) + ".sv";

        const auto start_hover = Clock::now();
        const auto hover = engine.hoverAt(target_uri, 1, 10);
        const auto end_hover = Clock::now();

        const auto start_completion = Clock::now();
        const auto completion = engine.completionsAt(target_uri, 1, 4, "sig_");
        const auto end_completion = Clock::now();

        const auto start_query = Clock::now();
        const auto references = engine.referencesAt(target_uri, 1, 10, true);
        const auto end_query = Clock::now();

        const auto start_rename = Clock::now();
        const auto rename = engine.renameAt(target_uri, 1, 10, "renamed");
        const auto end_rename = Clock::now();

        const auto start_signature = Clock::now();
        const auto signature = engine.signatureHelpAt(target_uri, 2, 10);
        const auto end_signature = Clock::now();

        const auto start_inlay = Clock::now();
        const auto inlay = engine.inlayHints(target_uri,
                                             pristine::analysis::ParseRange{.start_line = 0,
                                                                            .start_character = 0,
                                                                            .end_line = 4,
                                                                            .end_character = 0});
        const auto end_inlay = Clock::now();

        const auto start_semantic_tokens = Clock::now();
        const auto semantic_tokens = engine.semanticTokens(target_uri);
        const auto end_semantic_tokens = Clock::now();

        const auto target_module_name = std::string("unit_") + std::to_string(target_index);

        const auto start_hierarchy = Clock::now();
        const auto hierarchy = engine.moduleHierarchy(target_module_name, 4);
        const auto end_hierarchy = Clock::now();

        const auto start_schematic = Clock::now();
        const auto schematic = engine.schematic(target_module_name, 4);
        const auto end_schematic = Clock::now();

        const auto start_cone = Clock::now();
        const auto cone = engine.backwardConeAt(target_uri, 1, 10);
        const auto end_cone = Clock::now();

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
                  << "\"referenceMicros\":" << elapsedMicros(start_query, end_query) << ","
                  << "\"renameMicros\":" << elapsedMicros(start_rename, end_rename) << ","
                  << "\"signatureHelpMicros\":" << elapsedMicros(start_signature, end_signature) << ","
                  << "\"inlayHintMicros\":" << elapsedMicros(start_inlay, end_inlay) << ","
                  << "\"semanticTokensMicros\":" << elapsedMicros(start_semantic_tokens, end_semantic_tokens) << ","
                  << "\"workspaceSymbolMicros\":0,"
                  << "\"moduleHierarchyMicros\":" << elapsedMicros(start_hierarchy, end_hierarchy) << ","
                  << "\"schematicMicros\":" << elapsedMicros(start_schematic, end_schematic) << ","
                  << "\"backwardConeMicros\":" << elapsedMicros(start_cone, end_cone) << ","
                  << "\"referenceCount\":" << references.locations.size() << ","
                  << "\"completionCount\":" << completion.items.size() << ","
                  << "\"inlayHintCount\":" << inlay.hints.size() << ","
                  << "\"semanticTokenCount\":" << semantic_tokens.tokens.size() << ","
                  << "\"moduleHierarchyRootCount\":" << hierarchy.roots.size() << ","
                  << "\"schematicModuleCount\":" << schematic.modules.size() << ","
                  << "\"backwardConeNodeCount\":" << cone.nodes.size() << ","
                  << "\"signatureUnresolved\":" << (signature.unresolved ? "true" : "false") << ","
                  << "\"unresolved\":" << (references.unresolved || hover.unresolved || rename.unresolved ||
                                           inlay.unresolved || semantic_tokens.unresolved ||
                                           hierarchy.unresolved || schematic.unresolved || cone.unresolved
                                                ? "true"
                                                : "false")
                  << "}";
        unresolved = unresolved || references.unresolved || hover.unresolved || rename.unresolved ||
                     inlay.unresolved || semantic_tokens.unresolved || hierarchy.unresolved ||
                     schematic.unresolved || cone.unresolved;
    }

    std::cout << "]}\n";
    return unresolved ? 1 : 0;
}
