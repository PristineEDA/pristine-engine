#include "SnapshotBuilder.h"

#include "slang/ast/Compilation.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace pristine::analysis::semantic {

SnapshotData::SnapshotData() = default;
SnapshotData::~SnapshotData() = default;
SnapshotData::SnapshotData(SnapshotData&&) noexcept = default;
SnapshotData& SnapshotData::operator=(SnapshotData&&) noexcept = default;

} // namespace pristine::analysis::semantic
