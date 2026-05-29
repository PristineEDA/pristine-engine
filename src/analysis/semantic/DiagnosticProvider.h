#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <vector>

namespace pristine::analysis::semantic {

void sortAndDedupeDiagnostics(std::vector<SemanticEngineDiagnostic>& diagnostics);

} // namespace pristine::analysis::semantic
