#include "pristine/analysis/SemanticEngine.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMicros(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

} // namespace

int main() {
    pristine::analysis::SemanticEngine engine;

    const auto start_index = Clock::now();
    for (int index = 0; index < 100; ++index) {
        const auto name = std::string("sig_") + std::to_string(index);
        engine.updateDocument("file:///perf/unit_" + std::to_string(index) + ".sv",
                              "module unit_" + std::to_string(index) + ";\n"
                              "  logic " + name + ";\n"
                              "  assign " + name + " = " + name + ";\n"
                              "endmodule\n",
                              pristine::analysis::SemanticEngineDocumentState{.version = 1});
    }
    const auto end_index = Clock::now();

    const auto start_query = Clock::now();
    const auto references = engine.referencesAt("file:///perf/unit_42.sv", 1, 10, true);
    const auto end_query = Clock::now();

    std::cout << "{"
              << "\"documents\":100,"
              << "\"indexMicros\":" << elapsedMicros(start_index, end_index) << ","
              << "\"referenceMicros\":" << elapsedMicros(start_query, end_query) << ","
              << "\"referenceCount\":" << references.locations.size() << ","
              << "\"unresolved\":" << (references.unresolved ? "true" : "false")
              << "}\n";
    return references.unresolved ? 1 : 0;
}
