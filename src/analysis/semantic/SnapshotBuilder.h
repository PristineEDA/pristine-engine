#pragma once

#include "AffectedDependencyGraph.h"
#include "SignatureInlayProvider.h"
#include "pristine/analysis/SemanticEngine.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slang {
class SourceManager;
}

namespace slang::ast {
class Compilation;
class Symbol;
}

namespace slang::syntax {
class SyntaxTree;
struct ParameterValueAssignmentSyntax;
struct HierarchicalInstanceSyntax;
}

namespace slang::ast {
class InstanceSymbol;
}

namespace pristine::analysis::semantic {

struct SnapshotIndexedSymbol {
    SemanticSymbolIdentity identity;
    const slang::ast::Symbol* symbol = nullptr;
    std::string type_display;
    std::string value_display;
};

struct SnapshotIndexedReference {
    std::string stable_id;
    std::string name;
    SemanticLocation location;
    bool is_declaration = false;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
};

// URI-local occurrence indexes keep cursor lookup independent from the total
// number of references in the snapshot. The ranges are stored as indexes into
// SnapshotData::references so the canonical occurrence facts have one owner.
struct SnapshotReferenceOccurrenceIndex {
    std::vector<size_t> reference_indexes;
    std::vector<ParseRange> prefix_max_end_ranges;
};

// Graph construction consumes these URI-local, deterministically ordered views
// instead of reconstructing them from the global symbol/reference stores.
struct SnapshotUriSymbolRangeFact {
    std::string stable_id;
    ParseRange range;
};

struct SnapshotUriReferenceRangeFact {
    std::string stable_id;
    ParseRange range;
    bool is_declaration = false;
};

// Navigation queries use their own value-type occurrence view so providers do
// not need to reach back into SnapshotData::references or AST-owned symbols.
struct SnapshotNavigationOccurrence {
    std::string stable_id;
    SemanticLocation location;
    bool is_declaration = false;
    SemanticReferenceRole role = SemanticReferenceRole::Read;
    bool has_type_display = false;
};

struct SnapshotNavigationOccurrenceIndex {
    std::vector<SnapshotNavigationOccurrence> occurrences;
    std::vector<ParseRange> prefix_max_end_ranges;
};

struct SnapshotNavigationTargetFact {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    std::vector<SemanticLocation> type_definition_locations;
    bool rename_eligible = false;
};

struct SnapshotImplementationEdge {
    std::string target_stable_id;
    std::string implementation_stable_id;
    SemanticLocation location;
    std::string kind;
};

struct SnapshotImplementationEdgeIndex {
    std::vector<SnapshotImplementationEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_target_stable_id;
};

struct SnapshotSelectionRangeIndex {
    std::vector<ParseRange> ranges;
    std::vector<ParseRange> prefix_max_end_ranges;
};

struct SnapshotMemberCompletion {
    std::string qualifier;
    SemanticSymbolIdentity identity;
    std::string type_display;
};

enum class SnapshotVisibilityOrigin {
    Local,
    ExplicitImport,
    WildcardImport,
    PackageExport,
    Workspace,
};

struct SnapshotVisibilityCandidate {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    SnapshotVisibilityOrigin origin = SnapshotVisibilityOrigin::Local;
};

struct SnapshotScopeVisibility {
    std::string stable_id;
    std::string parent_stable_id;
    std::string uri;
    ParseRange range;
    std::string context_kind;
    int lexical_depth = 0;
    std::vector<SnapshotVisibilityCandidate> candidates;
};

struct SnapshotPackageVisibility {
    std::string package_name;
    std::string uri;
    std::vector<SnapshotVisibilityCandidate> candidates;
    std::vector<std::string> exported_packages;
};

struct SnapshotVisibleMacro {
    MacroDefinition definition;
    std::string source_uri;
    ParseRange available_after;
    std::optional<ParseRange> unavailable_after;
};

enum class SnapshotCompletionResolveKind {
    Symbol,
    Member,
    Module,
    Port,
    Macro,
};

// A completion item's data is an opaque key into this build-time view. Providers
// must not reconstruct a target by parsing the key or scanning unrelated facts.
struct SnapshotCompletionResolveFact {
    SnapshotCompletionResolveKind kind = SnapshotCompletionResolveKind::Symbol;
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
    std::string module_uri;
    std::optional<ModuleDefinition> module;
    std::optional<SchematicPort> port;
    std::optional<MacroDefinition> macro;
};

struct SnapshotDiagnosticLookupIndex {
    std::unordered_map<std::string, std::vector<std::string>> package_definition_ids_by_name;
    std::unordered_map<std::string, std::vector<SemanticLocation>> type_definition_locations_by_name;
    std::unordered_map<std::string, std::vector<std::string>> package_names_by_member;
    std::unordered_map<std::string, size_t> package_member_definition_counts;
    std::unordered_map<std::string, std::vector<SemanticSymbolIdentity>> duplicate_symbols_by_uri;
};

struct SnapshotInactiveRegion {
    SemanticLocation location;
    std::string directive_id;
};

struct SnapshotMacroUndef {
    std::string name;
    std::string source_uri;
    ParseRange range;
};

struct SnapshotModuleInstance {
    std::string module_name;
    std::string instance_name;
    std::string instance_stable_id;
    std::string type_display;
    std::string target_stable_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
    ParseRange module_selection_range;
    std::vector<SchematicConnection> port_connections;
    std::vector<SchematicConnection> parameter_connections;
};

struct SnapshotModuleEntry {
    std::string uri;
    ModuleDefinition definition;
};

struct SnapshotModuleCallHierarchyItem {
    std::string id;
    std::string name;
    std::string kind;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
};

struct SnapshotModuleCallEdge {
    std::string caller_item_id;
    std::string callee_item_id;
    std::string instance_id;
    std::string uri;
    ParseRange range;
    ParseRange selection_range;
};

struct SnapshotModuleCallHierarchyRange {
    ParseRange range;
    std::string item_id;
};

struct SnapshotModuleCallEdgeIndex {
    std::unordered_map<std::string, SnapshotModuleCallHierarchyItem> items_by_id;
    std::vector<SnapshotModuleCallEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_caller_item_id;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_callee_item_id;
    std::unordered_map<std::string, std::vector<SnapshotModuleCallHierarchyRange>> items_by_uri;
};

enum class SnapshotConeEdgeKind {
    Assignment,
    InstancePort,
    InterfaceMember,
    ParameterOverride,
    ControlDependency,
    PrimitiveCell,
    AssertionSample,
};
enum class SnapshotConeSourceRole { Data, Control, Sampled, Clock, Disable, Abort };
enum class SnapshotConeControlOrigin {
    None,
    ConditionalStatement,
    CaseStatement,
    TernaryCondition,
    DynamicSelect,
    PrimitiveControl,
    EventControl,
    EventIff,
    AssertionClock,
    AssertionDisable,
    AssertionAbort,
    AssertionDefaultClock,
    AssertionDefaultDisable,
    AssertionConditional,
    AssertionCase,
};
enum class SnapshotConeEventKind {
    None,
    Any,
    PosEdge,
    NegEdge,
    BothEdges,
    EventList,
    Implicit,
    Repeated,
    Unsupported,
};
enum class SnapshotConeSliceKind {
    Whole,
    ElementSelect,
    RangeSelect,
    Concatenation,
    MemberAccess,
    DynamicSelect,
};

// A slice fact describes the precision of a signal dependency without making
// providers reconstruct selections from source text. Exact ranges preserve the
// declared SystemVerilog indices; aggregate and dynamic facts deliberately
// remain partial instead of claiming a bit-precise relation.
enum class SnapshotConeSlicePrecision { Whole, Exact, Aggregate, Dynamic, Unresolved };

struct SnapshotConeSliceFact {
    SnapshotConeSlicePrecision precision = SnapshotConeSlicePrecision::Whole;
    std::optional<std::int64_t> msb;
    std::optional<std::int64_t> lsb;
};

struct SnapshotConeDataSourceSeed {
    ParseRange range;
    std::string expression;
    SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
    SnapshotConeSliceFact source_slice;
    SnapshotConeSliceFact sink_slice;
    std::vector<std::string> source_symbol_ids;
    std::vector<std::string> source_symbol_names;
    bool unresolved = false;
};

struct SnapshotConeControlSourceSeed {
    ParseRange range;
    std::string expression;
    SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
    SnapshotConeSliceFact source_slice;
    std::vector<std::string> source_symbol_ids;
    std::vector<std::string> source_symbol_names;
    SnapshotConeControlOrigin origin = SnapshotConeControlOrigin::None;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    bool unresolved = false;
};

// Captured while the slang AST timing control is still alive. Providers only
// receive the copied control sources, never a timing-control AST pointer.
struct SnapshotConeEventControlFact {
    SemanticLocation statement_location;
    SemanticLocation timing_location;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    std::vector<SnapshotConeControlSourceSeed> sources;
    bool unresolved = false;
};

// The assertion AST is lowered into an ordered temporal path while it is
// alive. Providers receive only these value facts and never reconstruct
// property operators, delays, or repetition bounds from text.
enum class SnapshotAssertionTemporalRelation {
    None,
    SequenceDelay,
    ConsecutiveRepeat,
    NonconsecutiveRepeat,
    GotoRepeat,
    NextTime,
    SNextTime,
    Always,
    SAlways,
    Eventually,
    SEventually,
    And,
    Or,
    Intersect,
    Throughout,
    Within,
    Iff,
    Until,
    SUntil,
    UntilWith,
    SUntilWith,
    Implies,
    OverlappedImplication,
    NonOverlappedImplication,
    OverlappedFollowedBy,
    NonOverlappedFollowedBy,
    FirstMatch,
    Strong,
    Weak,
    AcceptOn,
    RejectOn,
    SyncAcceptOn,
    SyncRejectOn,
    ConditionalBranch,
    CaseBranch,
};

struct SnapshotAssertionTemporalFact {
    SnapshotAssertionTemporalRelation relation = SnapshotAssertionTemporalRelation::None;
    SemanticLocation location;
    std::optional<std::int64_t> min_cycles;
    std::optional<std::int64_t> max_cycles;
};

// Concurrent and immediate assertions are materialized while the assertion
// AST is alive. The synthetic observation id is a normal value-type cone root;
// providers never reparse a property specification to recover sampled signals.
struct SnapshotAssertionSourceFact {
    SemanticLocation location;
    std::string expression;
    SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Sampled;
    SnapshotConeControlOrigin control_origin = SnapshotConeControlOrigin::None;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
    SnapshotConeSliceFact source_slice;
    std::vector<std::string> source_symbol_ids;
    std::vector<SnapshotAssertionTemporalFact> temporal_path;
    // Assertion formal-to-actual expansion is complete before provider
    // queries. These identities preserve the source invocation without
    // retaining AST pointers in the snapshot.
    std::string invocation_stable_id;
    std::string invocation_formal_stable_id;
    bool unresolved = false;
};

struct SnapshotAssertionObservationFact {
    std::string stable_id;
    std::string assertion_kind;
    SemanticLocation location;
    std::vector<SnapshotAssertionSourceFact> sources;
    bool concurrent = true;
    bool unresolved = false;
};

// Captures the scope-derived assertion defaults while slang's compilation and
// scope objects are alive. Explicit assertion syntax remains represented by
// the observation sources; this fact only records defaults actually applied.
struct SnapshotAssertionContextFact {
    std::string observation_stable_id;
    SemanticLocation scope_location;
    std::vector<SnapshotAssertionSourceFact> sources;
    bool has_default_clock = false;
    bool has_default_disable = false;
    bool unresolved = false;
};

enum class SnapshotAssertionActualKind { Expression, Assertion, Timing, Unresolved };

// Formal-to-actual relationships are preserved in declaration order so named
// assertion arguments do not inherit the unordered AST argument container's
// traversal order in signature and inlay queries.
struct SnapshotAssertionInvocationBindingFact {
    std::string invocation_stable_id;
    std::string target_stable_id;
    std::string formal_stable_id;
    std::string formal_name;
    size_t parameter_index = 0;
    SemanticLocation invocation_location;
    SemanticLocation actual_location;
    SnapshotAssertionActualKind actual_kind = SnapshotAssertionActualKind::Unresolved;
    std::vector<std::string> source_symbol_ids;
    bool unresolved = false;
};

struct SnapshotAssignmentEdgeSeed {
    std::string uri;
    ParseRange scope_range;
    ParseRange assignment_range;
    ParseRange left_range;
    ParseRange right_range;
    std::string left_expression;
    std::string right_expression;
    std::vector<std::string> left_symbol_ids;
    std::vector<std::string> left_symbol_names;
    SnapshotConeSliceFact sink_slice;
    std::vector<SnapshotConeDataSourceSeed> data_sources;
    std::vector<SnapshotConeControlSourceSeed> control_sources;
};

struct SnapshotAssignmentEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    SemanticLocation target_location;
    SemanticLocation expression_location;
    std::string expression;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::Assignment;
    SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Data;
    SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
    SnapshotConeControlOrigin control_origin = SnapshotConeControlOrigin::None;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    SnapshotConeSliceFact source_slice;
    SnapshotConeSliceFact sink_slice;
    std::vector<SnapshotAssertionTemporalFact> assertion_temporal_path;
    std::string assertion_invocation_stable_id;
    std::string assertion_invocation_formal_stable_id;
};

enum class SnapshotGraphEndpointKind { Port, Parameter, Instance, InterfacePort, ModportPort };
enum class SnapshotGraphPortDirection { Input, Output, Inout, Ref, Unknown };

// A graph endpoint retains the AST-resolved identity and direction used by
// hierarchy, schematic, and cone queries. It is intentionally value-only so
// provider queries never need to recover endpoint meaning from a port name.
struct SnapshotGraphEndpointFact {
    std::string stable_id;
    std::string module_name;
    std::string name;
    SnapshotGraphEndpointKind kind = SnapshotGraphEndpointKind::Port;
    SnapshotGraphPortDirection direction = SnapshotGraphPortDirection::Unknown;
    SemanticLocation location;
    std::string generated_instance_id;
    std::string interface_definition_stable_id;
    std::string modport_stable_id;
    SnapshotConeSliceFact declared_slice;
};

// Interface and modport bindings are captured while the slang AST is alive so
// providers never need to reconstruct them from source text or symbol names.
struct SnapshotInterfaceModportDefinitionFact {
    std::string stable_id;
    std::string interface_definition_stable_id;
    std::string name;
    SemanticLocation location;
};

struct SnapshotInterfaceModportMemberFact {
    std::string stable_id;
    std::string interface_definition_stable_id;
    std::string modport_stable_id;
    std::string name;
    SnapshotGraphPortDirection direction = SnapshotGraphPortDirection::Unknown;
    std::string internal_symbol_stable_id;
    SemanticLocation location;
};

struct SnapshotInterfacePortBindingFact {
    std::string port_stable_id;
    std::string interface_definition_stable_id;
    std::string modport_stable_id;
    std::string connected_interface_instance_stable_id;
    std::string connected_modport_stable_id;
    SemanticLocation interface_type_location;
    SemanticLocation modport_location;
    SemanticLocation connection_location;
    SemanticLocation interface_definition_location;
    SemanticLocation modport_definition_location;
    bool resolved = false;
};

// A resolved interface/modport connection links the exact member identities on
// both sides of one module instance. It is built while the AST bindings are
// alive so schematic and cone providers never need to join members by text.
struct SnapshotInterfaceMemberConnectionFact {
    std::string caller_module_name;
    std::string child_instance_stable_id;
    std::string child_instance_name;
    std::string child_endpoint_stable_id;
    std::string child_endpoint_name;
    std::string child_member_stable_id;
    std::string parent_interface_instance_stable_id;
    std::string parent_member_stable_id;
    std::string member_name;
    SnapshotGraphPortDirection direction = SnapshotGraphPortDirection::Unknown;
    SemanticLocation location;
    std::string generated_instance_id;
    bool unresolved = false;
};

struct SnapshotInterfaceModportBindingIndex {
    std::unordered_map<std::string, SnapshotInterfaceModportDefinitionFact> modports_by_stable_id;
    std::unordered_map<std::string, std::string> modport_ids_by_interface_definition_name;
    std::unordered_map<std::string, std::vector<SnapshotInterfaceModportMemberFact>>
        members_by_modport_stable_id;
    std::unordered_map<std::string, SnapshotInterfacePortBindingFact> ports_by_stable_id;
    size_t member_count = 0;
    size_t resolved_port_binding_count = 0;
};

struct SnapshotGraphConnectionBindingFact {
    std::string instance_stable_id;
    std::string endpoint_stable_id;
    SemanticLocation location;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::InstancePort;
    std::vector<std::string> source_symbol_ids;
    struct SourcePart {
        std::string source_symbol_id;
        SemanticLocation source_location;
        SnapshotConeSliceFact source_slice;
        SnapshotConeSliceFact endpoint_slice;
        SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
        SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Data;
        SnapshotConeControlOrigin control_origin = SnapshotConeControlOrigin::None;
        bool unresolved = false;
    };
    std::vector<SourcePart> source_parts;
    bool unresolved = false;
};

// Build-only syntax ownership for resolving parameter override expressions in
// the caller instance scope. These pointers are discarded before providers run.
struct SnapshotParameterOverrideSyntaxFact {
    const slang::syntax::ParameterValueAssignmentSyntax* syntax = nullptr;
    std::string uri;
    std::string module_name;
    std::string instance_name;
    ParseRange instance_range;
};

// Build-only structural port ranges preserve empty named connections such as
// `.clk()` for signature help. They are discarded before providers run.
struct SnapshotPortConnectionSyntaxFact {
    const slang::syntax::HierarchicalInstanceSyntax* syntax = nullptr;
    std::string uri;
    std::string module_name;
    std::string instance_name;
    ParseRange instance_range;
};

// Resolved connection slices are produced while the slang AST and the parent
// scope are alive. DesignGraphIndexBuilder only consumes this value-type view;
// it must never recover signal semantics from schematic strings or source text.
struct SnapshotResolvedConnectionSliceFact {
    std::string instance_stable_id;
    std::string endpoint_stable_id;
    std::string endpoint_name;
    int endpoint_index = -1;
    SemanticLocation location;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::InstancePort;
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart> source_parts;
    std::string literal_display;
    bool unresolved = false;
};

// The schematic projection mirrors a resolved connection while the snapshot
// owns all AST-derived identities. Providers use this instead of treating the
// display-only SchematicConnection::signal text as a semantic net key.
struct SnapshotSchematicConnectionFact {
    std::string caller_module_name;
    std::string instance_stable_id;
    std::string instance_name;
    ParseRange instance_selection_range;
    std::string endpoint_stable_id;
    std::string endpoint_name;
    int endpoint_index = -1;
    SnapshotGraphPortDirection endpoint_direction = SnapshotGraphPortDirection::Unknown;
    SemanticLocation location;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::InstancePort;
    std::vector<SnapshotGraphConnectionBindingFact::SourcePart> source_parts;
    std::string display_label;
    bool unresolved = false;
};

enum class SnapshotSchematicCellKind { Primitive, Assignment, Operator };

enum class SnapshotSchematicCellPinDirection { Input, Output, Inout, Control, Unknown };

// Every primitive and assignment schematic pin is projected from AST-derived
// identities while the snapshot owns the compilation. Providers group nets by
// stable id and slice; display labels are never semantic keys.
struct SnapshotSchematicCellPinFact {
    std::string caller_module_name;
    std::string cell_id;
    ParseRange cell_selection_range;
    SemanticLocation location;
    SnapshotSchematicCellKind cell_kind = SnapshotSchematicCellKind::Assignment;
    std::string cell_type;
    std::string pin_name;
    int pin_index = -1;
    SnapshotSchematicCellPinDirection pin_direction = SnapshotSchematicCellPinDirection::Unknown;
    std::string net_symbol_id;
    std::string display_label;
    SnapshotConeSliceFact net_slice;
    SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Data;
    bool unresolved = false;
    bool literal = false;
};

struct SnapshotConeAdjacencyEdge {
    std::string from_symbol_id;
    std::string to_symbol_id;
    SemanticLocation location;
    SemanticLocation target_location;
    SemanticLocation expression_location;
    std::string expression;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::Assignment;
    SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Data;
    SnapshotConeSliceKind slice_kind = SnapshotConeSliceKind::Whole;
    SnapshotConeControlOrigin control_origin = SnapshotConeControlOrigin::None;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    SnapshotConeSliceFact source_slice;
    SnapshotConeSliceFact sink_slice;
    std::string generated_instance_id;
    std::vector<SnapshotAssertionTemporalFact> assertion_temporal_path;
    std::string assertion_invocation_stable_id;
    std::string assertion_invocation_formal_stable_id;
};

struct SnapshotConeUnresolvedSourceFact {
    std::string from_symbol_id;
    SemanticLocation location;
    SemanticLocation expression_location;
    std::string expression;
    SnapshotConeEdgeKind kind = SnapshotConeEdgeKind::Assignment;
    SnapshotConeSourceRole source_role = SnapshotConeSourceRole::Data;
    SnapshotConeControlOrigin control_origin = SnapshotConeControlOrigin::None;
    SnapshotConeEventKind event_kind = SnapshotConeEventKind::None;
    std::vector<SnapshotAssertionTemporalFact> assertion_temporal_path;
    std::string assertion_invocation_stable_id;
    std::string assertion_invocation_formal_stable_id;
};

inline std::string assertionTemporalPathKey(
    const std::vector<SnapshotAssertionTemporalFact>& temporal_path) {
    std::string key;
    for (const auto& step : temporal_path) {
        key += std::to_string(static_cast<int>(step.relation));
        key += "@" + step.location.uri + ":" +
               std::to_string(step.location.range.start_line) + ":" +
               std::to_string(step.location.range.start_character);
        key += "[" + (step.min_cycles.has_value() ? std::to_string(*step.min_cycles) : "?") +
               ":" + (step.max_cycles.has_value() ? std::to_string(*step.max_cycles) : "*") + "]\n";
    }
    return key;
}

struct SnapshotConeRootSelectionFact {
    std::string symbol_id;
    ParseRange range;
    SnapshotConeSliceFact slice;
};

// Design graph queries consume these precomputed maps rather than rebuilding
// name, range, or assignment relationships while serving a request.
struct SnapshotDesignGraphBindingIndex {
    std::unordered_map<std::string, std::string> symbol_ids_by_uri_range;
    std::unordered_map<std::string, std::string> symbol_ids_by_module_scope_name;
    std::unordered_map<std::string, std::string> port_symbol_ids_by_module_port;
    std::unordered_map<std::string, std::string> parameter_symbol_ids_by_module_parameter;
    std::unordered_map<std::string, std::string> instance_ids_by_uri_range;
    std::unordered_map<std::string, std::string> schematic_cell_ids_by_instance_id;
    std::unordered_map<std::string, std::string> caller_module_names_by_instance_id;
    std::unordered_map<std::string, SnapshotGraphEndpointFact> endpoints_by_module_member;
    std::unordered_map<std::string, SnapshotGraphEndpointFact> endpoints_by_stable_id;
    std::vector<SnapshotGraphConnectionBindingFact> connection_bindings;
    std::unordered_map<std::string, std::vector<size_t>> connection_bindings_by_uri_range;
    std::unordered_map<std::string, std::vector<size_t>> connection_bindings_by_instance_id;
    std::unordered_map<std::string, std::vector<SnapshotSchematicConnectionFact>>
        schematic_connections_by_module;
    std::unordered_map<std::string, std::vector<SnapshotSchematicCellPinFact>>
        schematic_cell_pins_by_module;
    std::unordered_map<std::string, std::vector<SnapshotInterfaceMemberConnectionFact>>
        interface_member_connections_by_module;
    size_t scoped_symbol_candidate_count = 0;
    size_t connection_reference_candidate_count = 0;
    size_t schematic_connection_fact_count = 0;
    size_t schematic_partial_connection_fact_count = 0;
    size_t schematic_cell_pin_fact_count = 0;
    size_t schematic_partial_cell_pin_fact_count = 0;
    size_t interface_member_connection_fact_count = 0;
    size_t partial_interface_member_connection_fact_count = 0;
};

struct SnapshotConeAdjacencyIndex {
    std::vector<SnapshotConeAdjacencyEdge> edges;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_from_symbol_id;
    std::unordered_map<std::string, std::vector<size_t>> edges_by_to_symbol_id;
    std::unordered_map<std::string, std::vector<SnapshotConeRootSelectionFact>> root_selections_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotConeUnresolvedSourceFact>>
        unresolved_sources_by_from_symbol_id;
    // Temporal paths remain side-indexed so legacy graph edge construction is
    // not widened with query-only assertion metadata.
};

struct SnapshotTypeReference {
    SemanticLocation reference;
    std::string type_name;
    std::vector<SemanticLocation> definitions;
};

struct SemanticModuleSignature {
    ModuleDefinition definition;
    ModuleSchematic schematic;
    std::string uri;
};

struct SnapshotData {
    SnapshotData();
    ~SnapshotData();
    SnapshotData(SnapshotData&&) noexcept;
    SnapshotData& operator=(SnapshotData&&) noexcept;
    SnapshotData(const SnapshotData&) = delete;
    SnapshotData& operator=(const SnapshotData&) = delete;

    std::unique_ptr<slang::SourceManager> source_manager;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> syntax_trees;
    std::unique_ptr<slang::ast::Compilation> compilation;
    std::unordered_map<std::string, SnapshotIndexedSymbol> symbols_by_id;
    std::unordered_map<const slang::ast::Symbol*, std::string> ids_by_symbol;
    std::vector<SnapshotIndexedReference> references;
    std::unordered_map<std::string, std::vector<size_t>> references_by_symbol;
    std::unordered_map<std::string, std::vector<std::string>> reference_aliases_by_id;
    std::unordered_map<std::string, SnapshotReferenceOccurrenceIndex> reference_occurrences_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotUriSymbolRangeFact>> graph_symbols_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotUriReferenceRangeFact>> graph_references_by_uri;
    std::unordered_map<std::string, SnapshotNavigationOccurrenceIndex> navigation_occurrences_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotNavigationOccurrence>>
        navigation_occurrences_by_symbol;
    std::unordered_map<std::string, SnapshotNavigationTargetFact> navigation_targets_by_id;
    SnapshotImplementationEdgeIndex implementation_edge_index;
    std::unordered_map<std::string, std::vector<SemanticCompletionItem>> completions_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>> member_completions_by_uri;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<SnapshotMemberCompletion>>>
        member_completions_by_qualifier_by_uri;
    std::unordered_map<std::string, SnapshotMemberCompletion> member_completions_by_stable_id;
    std::unordered_map<std::string, std::vector<SnapshotScopeVisibility>> scope_visibility_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotVisibilityCandidate>> document_visibility_by_uri;
    std::unordered_map<std::string, SnapshotPackageVisibility> package_visibility_by_name;
    std::vector<SnapshotVisibilityCandidate> workspace_visibility;
    std::unordered_map<std::string, std::string> module_definition_ids_by_name;
    std::unordered_map<std::string, std::vector<SnapshotVisibleMacro>> visible_macros_by_uri;
    std::unordered_map<std::string, SnapshotCompletionResolveFact> completion_resolve_by_id;
    SnapshotDiagnosticLookupIndex diagnostic_lookup_index;
    std::unordered_map<std::string, std::vector<SnapshotInactiveRegion>> inactive_regions_by_uri;
    size_t scope_visibility_count = 0;
    size_t package_visibility_count = 0;
    size_t member_visibility_count = 0;
    size_t callable_visibility_count = 0;
    std::int64_t scope_visibility_build_micros = 0;
    size_t inactive_region_count = 0;
    std::int64_t inactive_region_build_micros = 0;
    std::vector<SnapshotModuleEntry> module_entries;
    SnapshotModuleCallEdgeIndex module_call_edge_index;
    std::unordered_map<std::string, ModuleDefinition> modules_by_name;
    std::unordered_map<std::string, std::string> module_uris_by_name;
    std::unordered_map<std::string, SemanticModuleSignature> ast_module_signatures_by_name;
    std::vector<SnapshotAssignmentEdgeSeed> assignment_edge_seeds;
    std::unordered_map<std::string, std::vector<SnapshotAssignmentEdge>> assignment_edges_by_uri;
    std::vector<SnapshotConeUnresolvedSourceFact> unresolved_cone_sources;
    std::unordered_map<std::string, std::vector<SnapshotConeEventControlFact>>
        event_control_facts_by_uri;
    size_t event_control_fact_count = 0;
    std::unordered_map<std::string, std::vector<SnapshotAssertionObservationFact>>
        assertion_observations_by_uri;
    size_t assertion_observation_fact_count = 0;
    size_t assertion_temporal_fact_count = 0;
    size_t assertion_temporal_partial_fact_count = 0;
    std::unordered_map<std::string, SnapshotAssertionContextFact>
        assertion_context_by_observation_id;
    std::unordered_map<std::string, std::vector<SnapshotAssertionInvocationBindingFact>>
        assertion_invocation_bindings_by_uri;
    size_t assertion_context_fact_count = 0;
    size_t assertion_invocation_binding_fact_count = 0;
    size_t assertion_invocation_expansion_fact_count = 0;
    size_t assertion_invocation_partial_binding_fact_count = 0;
    // Collected while slang AST primitive instances are alive; the design graph
    // builder merges these with assignment pins into its provider-facing view.
    std::vector<SnapshotSchematicCellPinFact> schematic_cell_pin_facts;
    std::unordered_map<std::string, std::vector<SnapshotResolvedConnectionSliceFact>>
        resolved_connection_slices_by_instance_id;
    std::unordered_map<std::string, SnapshotConeSliceFact> endpoint_declared_slices_by_id;
    SnapshotDesignGraphBindingIndex design_graph_binding_index;
    SnapshotConeAdjacencyIndex cone_adjacency_index;
    SnapshotInterfaceModportBindingIndex interface_modport_binding_index;
    std::unordered_map<std::string, std::vector<SnapshotTypeReference>> type_references_by_uri;
    std::unordered_map<std::string, std::vector<IncludeDirective>> include_directives_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotModuleInstance>> module_instances_by_uri;
    std::vector<SnapshotParameterOverrideSyntaxFact> parameter_override_syntax_facts;
    std::vector<SnapshotPortConnectionSyntaxFact> port_connection_syntax_facts;
    std::unordered_map<std::string, const slang::ast::InstanceSymbol*>
        instance_symbols_by_stable_id;
    size_t parameter_override_syntax_binding_count = 0;
    size_t parameter_override_syntax_binding_miss_count = 0;
    size_t parameter_override_available_endpoint_count = 0;
    size_t parameter_override_matched_endpoint_count = 0;
    size_t parameter_override_resolved_fact_count = 0;
    std::unordered_map<std::string, std::vector<CallableInvocationFact>> callable_invocations_by_uri;
    std::unordered_map<std::string, std::vector<MacroInvocationFact>> macro_invocations_by_uri;
    std::unordered_map<std::string, std::vector<SignatureInlaySymbol>> inlay_symbols_by_uri;
    std::unordered_map<std::string, std::vector<ParseRange>> selection_ranges_by_uri;
    std::unordered_map<std::string, SnapshotSelectionRangeIndex> selection_range_indexes_by_uri;
    std::unordered_map<std::string, std::vector<MacroDefinition>> macros_by_uri;
    std::unordered_map<std::string, std::vector<SnapshotMacroUndef>> macro_undefs_by_uri;
    std::unordered_map<std::string, std::vector<PackageImport>> package_imports_by_uri;
};

struct SnapshotBuildInput {
    std::uint64_t generation = 0;
    SemanticEngineConfig config;
    std::vector<std::string> dirty_document_uris;
    std::unordered_map<std::string, SemanticEngineDocument> documents;
};

struct SnapshotBuildInputSummary {
    size_t document_count = 0;
    size_t open_document_count = 0;
    size_t dirty_document_count = 0;
    size_t top_module_count = 0;
    size_t index_config_count = 0;
};

struct SnapshotBuildOutput {
    SemanticEngineSnapshot snapshot;
    std::unique_ptr<SnapshotData> data;
    AffectedDependencyGraph affected_dependencies;
};

[[nodiscard]] constexpr std::string_view snapshotBuilderProviderName() {
    return "SnapshotBuilder";
}

[[nodiscard]] SnapshotBuildInput normalizeSnapshotBuildInput(SnapshotBuildInput input);
[[nodiscard]] SnapshotBuildInputSummary snapshotBuildInputSummary(const SnapshotBuildInput& input);

class SnapshotBuilder {
public:
    [[nodiscard]] SnapshotBuildOutput build(SnapshotBuildInput input) const;
};

} // namespace pristine::analysis::semantic
