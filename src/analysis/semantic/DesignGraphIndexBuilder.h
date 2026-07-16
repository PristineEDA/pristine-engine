#pragma once

namespace pristine::analysis::semantic {

struct SnapshotData;

// Builds deterministic, value-type graph bindings and cone adjacency after
// AstIndex has collected symbols, signatures, assignment edges, and instances.
void buildDesignGraphIndexes(SnapshotData& data);

} // namespace pristine::analysis::semantic
