#include "DiagnosticProvider.h"

#include <algorithm>

namespace pristine::analysis::semantic {

namespace {

bool diagnosticLess(const SemanticEngineDiagnostic& lhs, const SemanticEngineDiagnostic& rhs) {
    if (lhs.range.start_line != rhs.range.start_line) {
        return lhs.range.start_line < rhs.range.start_line;
    }
    if (lhs.range.start_character != rhs.range.start_character) {
        return lhs.range.start_character < rhs.range.start_character;
    }
    if (lhs.code != rhs.code) {
        return lhs.code < rhs.code;
    }
    return lhs.message < rhs.message;
}

bool sameDiagnostic(const SemanticEngineDiagnostic& lhs, const SemanticEngineDiagnostic& rhs) {
    return lhs.uri == rhs.uri && lhs.code == rhs.code && lhs.message == rhs.message &&
           lhs.range.start_line == rhs.range.start_line &&
           lhs.range.start_character == rhs.range.start_character &&
           lhs.range.end_line == rhs.range.end_line &&
           lhs.range.end_character == rhs.range.end_character;
}

} // namespace

void sortAndDedupeDiagnostics(std::vector<SemanticEngineDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(), diagnosticLess);
    diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end(), sameDiagnostic),
                      diagnostics.end());
}

} // namespace pristine::analysis::semantic
