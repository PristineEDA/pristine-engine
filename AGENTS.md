# pristine-engine Agent Notes

## Scope

This file is for coding agents working in this repository.

- Put agent-only workflow rules here, not in `README.md`, unless the user explicitly asks for end-user documentation changes.
- Keep edits narrow, preserve existing layering, and validate the smallest affected slice before widening scope.

## Product Snapshot

`pristine-engine` is a standalone mature split-provider design snapshot + AST symbol identity-backed SystemVerilog LSP server implemented in C++20.

Current implemented LSP surface:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`
- `textDocument/documentSymbol`
- `textDocument/hover`
- `textDocument/definition`
- `textDocument/typeDefinition`
- `textDocument/implementation`
- `textDocument/references`
- `textDocument/documentHighlight`
- `textDocument/documentLink`
- `textDocument/inlayHint`
- `textDocument/codeAction`
- `textDocument/foldingRange`
- `textDocument/semanticTokens/full`
- `textDocument/selectionRange`
- `textDocument/signatureHelp`
- `textDocument/prepareCallHierarchy`
- `callHierarchy/incomingCalls`
- `callHierarchy/outgoingCalls`
- `workspace/symbol`
- `textDocument/completion`
- `completionItem/resolve`
- `textDocument/prepareRename`
- `textDocument/rename`
- `workspace/didChangeWatchedFiles`
- `systemverilog/outline`
- `systemverilog/moduleHierarchy`
- `systemverilog/schematic`
- `systemverilog/backwardCone`
- `systemverilog/waveform/open`
- `systemverilog/waveform/close`
- `systemverilog/layout/open`
- `systemverilog/layout/close`

Current behavior follows a split-provider SemanticEngine single-fact-source model:

- `SemanticEngine` is the public value-type facade and coordinator for document state, generation, invalidation, and query cache; split providers own snapshot construction, AST indexing, and feature-specific query assembly
- visible semantic facts are exclusively backed by slang AST/Compilation, `AstIndex` views, and design graph provider data; syntax/text utilities must not become semantic answer sources
- `WorkspaceDiscoveryIndex` provides a lightweight initialize/warm-path discovery index for module/interface/package/macro/file candidates, top-candidate hints, include plus package import/export dependency-closure hints, closure metrics, and timing counters; `SemanticEngine` caches only the value-type discovery snapshot with a generation/config/file-set cache key, including `.slang/server.json` `index[].dirs` / `excludeDirs` filtering, so it may guide closure-limited hierarchy/schematic deep builds but must not become the final semantic authority for hover, definition, hierarchy, schematic, cone, diagnostics, or code actions. Hierarchy/schematic may expose optional `discoveryClosure*` telemetry such as root, candidate/selected/missing/deduped document counts, build/query timing, and cache hit state, but those fields are perf/debug data only
- current validation scale is 505+ focused unit `TEST_CASE`s and 470+ JSON semantic golden fixtures; the next maturity target is 530+ unit `TEST_CASE`s and 495+ golden fixtures focused on bit-precise control/data/slice cone facts, URI-local graph build views, slice-aware affected rebuild, shared-LSP differential, and required synthetic perf coverage rather than debt-only fixtures
- Pristine integration readiness is documented in `docs/pristine-integration.md`; `systemverilog/moduleHierarchy` and `systemverilog/schematic` are Pristine-consumable custom requests for hierarchy tree and schematic canvas integration, with `roots/messages/unresolved/partial/truncated` and `rootModuleId/modules/messages/unresolved/partial/truncated` as the externally visible contract
- `systemverilog/outline` is an opened-document syntax fast-path request for file navigation after `didOpen`; it converts `CompilationService::documentSymbols()` into stable preorder ids, readable kinds, optional tree/flat lists, depth/limit metadata, and messages without triggering `SemanticEngine` full snapshots, `AstIndex`, `WorkspaceDiscoveryIndex`, or `DesignGraphProvider`
- waveform integration is split into LSP control-plane requests and an out-of-band local pipe data plane: `systemverilog/waveform/open` starts mock or FST-backed waveform sessions and returns endpoint metadata, while render-ready waveform payloads are sent through `pristine-waveform-columnar-v1` binary frames over the pipe; waveform data must not be serialized onto existing LSP stdio. FST-backed sessions synchronously parse the header/hierarchy/geometry/value-block index at open time, decode zlib/LZ4/FastLZ value-change compression through pinned deps, keep FST files and `.fst.hier` sidecars inside the workspace root, and leave viewport data on the pipe protocol. Wellen `wellen/inputs` FST files are a pinned untracked fixture-only dependency under `.deps/src/wellen`; do not copy those waveform fixtures into `tests/` or commit them to git
- Physical-layout integration follows the same control/data split as waveform: `systemverilog/layout/open` validates either workspace-contained `lefUris` plus optional `defUri` for LEF/DEF, or a workspace-contained `gdsUri` for GDSII, starts a `lefdef` or `gds` layout session, and returns endpoint metadata; catalog and render-ready geometry payloads are sent through layout pipe binary frames. LEF/DEF and GDS sessions both use the unified `pristine-layout-columnar-v3` protocol with a superset catalog: LEF/DEF fills macro/pin/via/component/DEF-pin/net tables, GDS fills cell/reference/element/point hierarchy tables, and unused source-specific tables are zero-count. Layout v2 is removed, is not advertised, and must not be accepted as a fallback. GeometryRequest v3 keeps the old bbox/layer/kind/maxShapes prefix and uses flag `0x2` for optional owner filters: LEF macro-index filters select macro-local shapes by stable catalog macro index, while GDS root-cell-index filters flatten the requested root cell plus reachable child cells using catalog hierarchy. LEF macro, GDS cell, bbox, layer, kind, and maxShapes filters compose as logical AND, and invalid owner indices must return a pipe error frame. GDS sessions also lazily build a Boost.Geometry 1.91.0 R-tree-backed `LayoutSpatialIndex` behind `src/layout/*` for v3 tile geometry, hit-test, inspect, selection-highlight, and search requests; Boost types must stay hidden behind layout value types and must not leak into public protocol payloads. GDS open should parse, register layers, and expose top-cell bounds only; do not eager-flatten the top hierarchy or build the spatial index during synchronous `systemverilog/layout/open`. Large GDS may use staged open via `openMode: "sync" | "staged" | "auto"`; `auto` stages files larger than `PRISTINE_LAYOUT_STAGED_OPEN_BYTES` (default 64 MiB), returns `status="parsing"` and `deferred=true` with a pipe endpoint, and moves parsing/ready publication into `src/layout/*`. `hello` and `StatusRequest/PLST` are available while parsing; catalog/page/tile/hit/search/inspect/selection requests must return pending pipe errors until the source is ready, not provisional geometry. LEF/DEF/GDS parsing, value-type layout data, catalog/geometry columnar encoding, staged GDS parse state, and named-pipe / Unix-domain-socket serving are owned by `src/layout/*`; `ServerSession` only validates file URIs, starts/closes sessions, chooses the source/open mode, and serializes session metadata. `cibyr/lefdef` and `IHP-GmbH/IHP-Open-PDK` are pinned corpus dependencies under `.deps/src/lefdef` and `.deps/src/ihp-open-pdk`; the TinyTapeout tinyQV GDS perf fixture is prepared with `node scripts/fetch_tt_tinyqv_gds.mjs` by using system download tools to download or reuse the retained TinyTapeout repo zip at `.deps/src/tinytapeout-sky-25a/tinytapeout-sky-25a-main.zip`, extracting `projects/tt_um_tt_tinyQV/tt_um_tt_tinyQV.gds.br`, Brotli-decompressing it, and writing `.deps/src/tinytapeout-sky-25a/projects/tt_um_tt_tinyQV/tt_um_tt_tinyQV.gds`. Do not copy LEF/DEF/IHP/TT external corpora into `tests/` or commit them to git. IHP LEF/GDS and TT tinyQV interaction validation are required gates for layout/parser/spatial/protocol changes when the checkouts are present or explicitly required
- RTL E2E LSP stress is the real language-service closure path for Pristine outline/hierarchy/schematic integration: it uses an lsp-framework client to drive `pristine-engine --stdio`, send `didOpen`, `systemverilog/outline`, `textDocument/hover`, `systemverilog/moduleHierarchy`, and `systemverilog/schematic`, and record operation timings/logs; `pristine_rtl_e2e_real_retrosoc` is a required gate that opens the default retroSoC corpus in real mode, while `probe` and other corpus modes remain available for additional exploratory coverage. The client records source-discovery/open-file-selection timing, optional discovery-closure root/candidate/selected/missing/deduped/build/query/cache-hit telemetry from hierarchy/schematic responses, optional JSONL protocol tracing, and optional Debug server phase tracing through `RTL_E2E_SERVER_DEBUG_TRACE` / `RTL_E2E_SERVER_DEBUG_TRACE_FILE`
- current deepening work is complex SystemVerilog provider completeness plus validation scale-out: package import/export and wildcard import resolution, typedef alias chains, AST-backed parameter override inlay labels, struct/enum/class/interface/modport members, class property/method member completion facts, array-of-struct member completion facts, interface instance member completion facts, modport-restricted interface port member completion facts, typed member completion resolve documentation, nested function/task signature help, function/nested-call selection ranges, interface/modport schematic graph endpoints, interface modport instance type inlay labels, parameterized cross-module backward cone, parameterized width-assignment backward cone, generated scopes, parameterized instances, port/param bindings, assignment width/type facts, macro define and macro expand quickfixes, second-instance missing-port quickfixes, diagnostics publish/clear, package-export discovery closure and closure metrics, watched-file/document add/change/delete affected rebuild coverage for workspace symbols and Pristine-facing hierarchy/schematic custom requests, config-filter invalidation coverage, unresolved hierarchy/schematic message dedupe, hierarchy subtree memoization, fixture-driven differential coverage, syntax/query-cache metrics and affected-rebuild baselines, background full-diagnostics publishing, slang-server v0.2.5 attribution coverage, and clang/GCC-oriented non-Windows validation. Typedef alias-chain, package-qualified alias-chain, interface modport port typeDefinition, parameter override inlay labels, localparam/parameter constant value inlay labels, packed localparam constant/type inlay labels, completion resolve constant value documentation, wildcard import typedef resolve documentation, hierarchical instance port completion resolve docs, array-of-struct member completion, class member completion, interface instance member completion facts, modport-restricted interface port member completion, typed member completion resolve docs, nested function/task signature help, function/nested-call selection ranges, interface/modport schematic net endpoints, interface modport instance type inlay labels, parameterized cross-module backward cone, parameterized width-assignment backward cone, generated schematic root cells, generated child schematic expansion, generated instance connection facts, generated schematic port/net fidelity, generated backward cone instance edges, generated-only definition assignment/cone coverage, macro define and macro expand code actions, second-instance missing-port code actions, ambiguous missing-import diagnostics, ambiguous package import no-auto-fix codeAction behavior, diagnostics publish/clear, no-config multi-root moduleHierarchy inference, visible query-cache stats telemetry for hierarchy/schematic/cone plus RTL E2E summaries, opened-document syntax document-symbol caching, and syntax-cache telemetry in outline/RTL E2E summaries are now positive AstIndex/provider/golden/e2e or focused-unit assertions and should stay AST-backed or syntax-fast-path scoped as appropriate; package-document affected rebuild coverage is now bound to completion and codeAction visible queries; module signature affected rebuild coverage is now bound to signatureHelp and inlayHint visible queries; package/module semantic affected dependency edges are now produced by `SnapshotBuilder` and bound to completion/signature/inlay visible queries; topModules visible-query cache invalidation is covered by focused affected-rebuild unit assertions; affected include and semantic reverse dependency traversal is centralized in `AffectedDependencyGraph` with explicit edge categories, stats, and deterministic edge dumps, visible query cache keys use typed field encoding with snapshot-and-reset stats, and background full diagnostics are routed through `BackgroundDiagnosticsWorker` with state snapshots and wait-idle test support; next focus prioritizes Release-only compile coverage, Windows clang-cl Debug/Release compiler parity, same-run artifact provenance, E2E/perf artifact traceability, then visible-query stale-cache matrix expansion
- current validation reproducibility focus is same-run provenance plus Release-only compiler coverage: full Debug, Windows clang-cl Debug/full CTest plus clean clang-cl Release smoke, and clean canonical Release summaries must share one `runId`, full Git SHA, manifest hash, and dirty-worktree-aware `sourceFingerprint`; `build/gate-artifacts/index.json` must reject stale or cross-run summaries, both Release preparations may be `deleted` or `alreadyAbsent` but must name their guarded build paths, and the local Windows clang gate must prove Clang/clang-cl compiler identity for both Debug and Release
- current semantic feature focus is AST-backed scope, callable invocation, macro usage, and preprocessor visibility: `AstIndex` owns lexical/enclosing scope candidates, URI-local `CallableInvocationFact`, `MacroInvocationFact`, `CompletionResolveIndex`, `DiagnosticLookupIndex`, and inactive preprocessor region range indexes, explicit and wildcard imports, transitive package exports with cycle guards, typed receiver members, include-closure macro visibility, define/redefine/undef lifetimes, and bounded nested macro expansion. `CompletionProvider` owns completion context, filtering, deterministic precedence, dedupe, snippets, resolve metadata, and lower-bound workspace prefix selection; completion resolve receives an opaque build-time key and performs one direct fact lookup. `DiagnosticProvider` consumes the indexed package/type/member/duplicate lookup views rather than iterating the full symbol table. `SignatureInlayProvider` consumes only indexed callable/macro argument ranges; macro hover, definition, signature, inlay, and expand code actions consume the same invocation identity and expansion facts. Provider queries must not scan the full symbol table, all workspace callables or macros, use stable-id substring or fuzzy membership tests, parse completion markers, locate call parentheses or macro definitions with text search, or fall back to `CompilationService` semantic answers. An unresolved package, receiver, callable, macro, import, or opaque resolve key returns no speculative global candidates and an explicit message
- navigation queries use build-time value facts: `ReferenceOccurrenceIndex` owns URI-local declaration/read/write/type/instance occurrences plus deterministic same-range alias views; `NavigationTargetIndex` owns declaration, type-definition, hover and rename eligibility facts; and `ImplementationEdgeIndex` owns module/interface instance plus base/derived callable implementation identities, including generated or repeated edges. `NavigationProvider` owns lookup, hover, definition, typeDefinition, implementation, references, documentHighlight, prepareRename, rename, semantic tokens, and selection ranges. `SemanticEngine` may only build a generation-bound query context and coordinate the cache. Query-time scans across all references, symbols, module entries, or AST pointers; name/range reconstruction; stale opaque-id fallback; and text-based symbol guessing are prohibited. Missing indexed facts return explicit unresolved/messages rather than the old declaration or workspace fallback. Explicit `topModules` snapshots use full slang elaboration rather than lint-only uninstantiated definitions, and loop-generate arrays must visit each elaborated entry so repeated generated instances keep independent identities
- design graph queries use build-time value facts: `DesignGraphIndexBuilder` is the sole graph binding and adjacency construction entry point; do not leave a legacy builder or duplicate helper path in `AstIndex.cpp`. `DesignGraphBindingIndex` owns exact URI/range-to-symbol, module-scope/name, port, parameter, generated-instance, and `GraphEndpointFact` bindings, including URI-local `SnapshotGraphConnectionBindingFact` source identity sets for named, positional, parameter-override, and complex connections. `InterfaceModportBindingIndex` owns interface definition, modport definition/member direction, interface-port, connection, and generated-instance identities from slang AST plus syntax-node token ranges; graph, typeDefinition, completion, hover, inlay, schematic, and cone must consume that same identity. Endpoint kind, port direction, cone edge kind, source role, and slice kind are typed enums; `SnapshotConeSliceFact` preserves whole/exact/aggregate/dynamic/unresolved precision. Each concat operand carries its own source and sink slice, and `SnapshotConeRootSelectionFact` deterministically merges all indexed sink slices for one left-value range before a root query filters intersecting first-hop edges. `ConeAdjacencyIndex` owns typed assignment, `ControlDependency`, instance-port, and parameter-override edges with generated-instance identity in both directions. Build-time declaration recovery is limited to deterministic URI-local symbol/reference range views; a generated continuous assignment may carry only its exact single-identifier syntax token as a build-time binding fact, never as a provider query-time fallback. Builder code must not reconstruct URI-local symbol views from `symbols_by_id`, and provider queries may not scan `symbols_by_id`, module entries, per-URI assignment maps, or reconstruct graph identity by name/range. Interface/modport resolution must not parse source text, traverse the full symbol table, test stable-id substrings, or guess identifiers. Missing bindings return `partial/messages`; they do not re-enable syntax, text, workspace, or port-name direction guesses such as `y/out/o/q`. `graphScannedGlobalSymbols` and `coneScannedGlobalEdges` must stay zero in focused, synthetic perf, and RTL E2E summaries; synthetic cross-module cone probes must also demonstrate a nonzero local adjacency scan and a successful warm query. `if`, `case`, ternary, and dynamic-select control sources are indexed; exact root slices may only traverse intersecting first-hop sink slices, while aggregate or dynamic slice facts remain explicit partial results. Never represent a dynamic select as an exact static slice.
- callable, macro, and interface/modport affected rebuild is explicit: `AffectedDependencyGraph` classifies `SemanticExport`, `CallableType`, `InterfaceModport`, and `MacroInclude` edges in addition to include/import/module/config edges. Package export, callable declaration, interface/modport declaration or member direction, macro include, document, watched-file, and config changes must invalidate completion, signature, inlay, hover, definition, code-action, hierarchy, schematic, and cone visible-query results; tests must assert changed external results, not only generation counters
- signature/inlay/perf telemetry records URI-local invocation, visible macro, completion resolve, and diagnostic lookup scan counts, and `scannedGlobalSymbols` must remain zero. `PreprocessorRegionFact` ranges are built from slang syntax/directive facts, normalize inactive spans deterministically, and only publish through client-gated experimental `textDocument/inactiveRegions` after a fresh semantic generation; didClose clears the notification state. The required 100/1000/5000-file synthetic perf gate exercises cold/warm completion, resolve, signature, inlay, macro definition/expand, cache stats, and candidate scans without machine-specific time thresholds
- RTL large-workspace and real-retroSoC request order is `didOpen -> outline -> hover -> completion -> resolve -> signatureHelp -> inlayHint -> diagnostics wait -> moduleHierarchy -> schematic -> backwardCone`; diagnostics wait must stay passive and must not send a deep-semantic heartbeat
- macro completion visibility is an indexed value fact: macros are limited to the current document/include closure and carry build-time `availableAfter` / `unavailableAfter` positions derived from define, undef, or root include directives, so definitions and includes after the cursor and definitions invalidated by `undef` are not visible. `WorkspaceDiscoveryIndex` may shallow-scan 64+ sorted documents with at most eight native worker threads, each owning a separate `CompilationService`; merge/finalization remains deterministic by canonical URI, and full `SnapshotBuilder` must stay serial
- slang-server WCP and `slang.*` command parity are not product targets; Pristine's native waveform and layout control/data planes are the corresponding product capabilities. Differential tests must compare only standard LSP behavior advertised by both servers at initialize time; optional checks such as signature help, type definition, implementation, document highlight, rename, and call hierarchy must not be sent to a server that omits the matching capability, and Pristine-only graph/cone requests do not belong in shared differential fixtures. Shared checks use explicit normalized comparators (`locationSet`, `highlightSet`, `prepareRenameRange`, `workspaceEdit`, or `callHierarchyEdges`) or `validateOnly`; never substitute counts, names, or raw temporary URIs for an exact shared comparison. At the end of each semantic round, record the shared-LSP differential result, the remaining `slang-server` reference value, non-target capabilities, and whether three consecutive Ubuntu differential passes plus graph zero-global-scan probes justify downgrading slang-server to a periodic compatibility check.
- recent debt cleanup renamed the already-positive parameterized backward cone, interface outgoing call hierarchy, array-of-struct member completion, interface member completion resolve, parameter override inlay, interface modport typeDefinition, package-qualified alias-chain typeDefinition, and macro define code-action golden fixtures to non-debt filenames; nested function, multi-argument task signature-help, parameterized width-assignment backward cone, function/nested-call selection range, and second-instance missing-port code action fixtures are now positive AST-backed/provider assertions; there are no remaining `*debt*.json` semantic golden filenames, so future debt fixtures must use explicit `unresolved` / `partial` / `messages` expectations and be converted when the capability lands
- `SnapshotBuilder` preloads open/in-memory documents into the shared slang `SourceManager` before constructing syntax trees, so an opened include/header can also be included by another closure document without duplicate-path buffer assignment on Linux/macOS; keep this regression covered when changing snapshot construction
- visible semantic lookups must be deterministic across macOS, Linux, and Windows; same-range AST symbols use explicit `AstIndex` tie-breaks instead of unordered traversal, pointer, or platform-specific container order
- hover, definition, type definition, implementation, references, document highlights, prepare rename, and rename are the first AST-backed query slice owned by `SemanticEngine`
- completion, completion resolve, signature help, diagnostics, inlay hints, semantic tokens, selection ranges, and workspace symbols use `SemanticEngine` value-type APIs; completion context detection, member qualifier parsing for array/hierarchical access, connected named-port exclusion, item assembly, resolve docs/snippets, and completion-specific metadata are owned by `CompletionProvider`; diagnostics aggregation is owned by `DiagnosticProvider` from AstIndex diagnostic facts; code-action quickfix selection is owned by `CodeActionProvider` from diagnostics plus indexed facts; signature help plus module/type/port/function/task inlay hint assembly are owned by `SignatureInlayProvider`; semantic token and selection range assembly are owned by `NavigationProvider`; remaining context gaps belong in analysis instead of `ServerSession`
- macro-function signature help and AST-backed function/task signature help plus argument inlay hints are owned by `SignatureInlayProvider` and must consume `AstIndex` / snapshot macro and call views; do not reimplement argument parsing, active-parameter calculation, or argument hint placement in `ServerSession`
- HDL module hierarchy, schematic, call hierarchy, and backward cone traversal are owned by `DesignGraphProvider` behind `SemanticEngine` value-type APIs; schematic module/cell/port/instance facts and cone driver/load facts must come from AST-derived module signatures, module instance bindings, assignment edge views, and design graph views, not syntax module models, syntax schematic, or identifier extraction
- backward cone traversal, stable sorting, dedupe, result caps, `partial/truncated/messages`, and large-result behavior are provider-owned `DesignGraphProvider` semantics; `SemanticEngine` should only prepare context/cache and return the value-type result
- `AstIndex` builds `ConeSourceSeed` facts before provider queries: ordinary expressions and ternary branches are data sources, while a `ConditionalExpression` selector is a control source with `TernaryCondition` origin. A selector must not also become a data edge unless a branch independently reads it. Source role, control origin, expression range, slice kind, stable-id set, and unresolved state participate in deterministic adjacency dedupe and sorting.
- `ConeAdjacencyIndex` retains unresolved source facts by sink stable id. `DesignGraphProvider` may only traverse indexed adjacency and unresolved facts, returning `partial/messages` for unresolved ternary data/control sources. `systemverilog/backwardCone` may expose optional `edges[].controlOrigin`, `coneTernaryControlEdges`, and `coneUnresolvedSourceFacts`; no query-time AST, text, name, stable-id substring, or workspace fallback may recover missing cone sources.
- code actions for unresolved include/module/type and missing port connections are produced through `CodeActionProvider` behind `SemanticEngine` value-type APIs; `ServerSession` only serializes them to LSP JSON
- `CompilationService` remains the syntax fast path for parse diagnostics, document symbols, document links/folding, include/macro/package lexical utilities, syntax hover, and completion-prefix text utility; it must not produce LSP-visible semantic facts or become a provider query-time source for diagnostics, code actions, completion, signature, graph, schematic, cone, module signature, or module instance facts
- snapshot construction, SourceManager/SyntaxTree/Compilation ownership, slang diagnostic collection, and include/import/package/module reverse-edge output are owned by `SnapshotBuilder`; `SemanticEngine::rebuildSnapshot` should only assemble build input, receive build output, update generation-scoped state, and clear dirty/cache flags
- AST symbol identity, declaration/reference indexing, deterministic same-range symbol tie-breaks, declared/port/interface type display, module instance binding, module/interface signature views, function/task call signature views, module map and module URI ownership, include/import/macro/type/member/module/interface/package/class indexes, declared type reference views, function/task argument views, port/param binding views, AST-derived instance connection views, AST-derived assignment driver/load edge views, assignment width/type facts, workspace symbol generation, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views are owned by `AstIndex` behind `SemanticEngine` value-type APIs; repeated visible query results are cached by the generation-keyed query cache; do not add a second analysis fallback index
- declared type reference precision, including package-qualified references such as `pkg::type_t`, is owned by `AstIndex`; range narrowing may use slang source ranges plus syntax token/name ranges from the indexed declaration, but must not reintroduce query-time identifier scans or whole-text matching as semantic truth
- `SemanticWorkspace` is a facade and document-state adapter around `SemanticEngine`; do not add diagnostics, symbol-resolution, or LSP-visible semantic query rules there
- `ServerSession` should route LSP requests and serialize responses; it should not grow new SystemVerilog semantic rules
- `ServerSession` may own waveform control-plane routing (`systemverilog/waveform/open` and `systemverilog/waveform/close`) but must not own waveform dataset generation, binary protocol framing, viewport-to-column encoding, or pipe I/O rules; keep those in `src/waveform/*`
- `ServerSession` may own layout control-plane routing (`systemverilog/layout/open` and `systemverilog/layout/close`) but must not own LEF/DEF/GDS parser rules, layout dataset generation, binary protocol framing, geometry column encoding, hierarchy flattening, or pipe I/O rules; keep those in `src/layout/*`

The server resolves the workspace root from `initialize` and loads a minimal `.slang/server.json` when present.

Recognized `.slang/server.json` fields:

- `build`
- `buildPattern`
- `buildRelativePaths`
- `flags`
- `top`
- `topModules`
- `index[].dirs`
- `index[].excludeDirs`

## Code Map

Use the nearest owning layer instead of patching around it from a wrapper.

- lifecycle, notification handling, diagnostics publishing, LSP request routing, and response serialization: `src/server/ServerSession.cpp`. Background full-diagnostics job queue/debounce/stale-guard mechanics live in `src/server/BackgroundDiagnosticsWorker.*`; `ServerSession` should only collect job inputs, publish results, and stop the worker. This file must not own navigation, completion, completion resolve, signature, diagnostics, workspace symbol, hierarchy, schematic, cone, or code action semantic rules
- LSP protocol structures and JSON payload parsing: `src/lsp/Protocol.cpp`
- public value-type query facade, document-state coordination, generation, invalidation, and cache orchestration: `src/analysis/SemanticEngine.cpp`
- split-out internal analysis providers for the large engine implementation: `src/analysis/semantic/SnapshotBuilder.*` for SourceManager/SyntaxTree/Compilation construction, snapshot data ownership, slang diagnostics, snapshot utilities, and include/import/module dependency edges; `src/analysis/semantic/AffectedDependencyGraph.*` for deterministic include/import/module/config affected-dependency traversal and stats; `src/analysis/semantic/WorkspaceDiscoveryIndex.*` for lightweight candidate discovery and deterministic module/interface/package/macro/file lookup hints only; `src/analysis/semantic/AstIndex.*` for AST symbol identity, declaration/reference index construction, deterministic lookup tie-breaks, declared/port/interface type display, reference range ownership, module map/URI ownership, module instance binding, AST-derived module/interface signatures and schematic facts, AST-derived function/task call signatures, include/import/macro/type/member/function/task/port/param/assignment semantic views, declared type reference views, AST-derived assignment edge and width/type facts, workspace symbol query assembly, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views; `src/analysis/semantic/NavigationProvider.*` for semantic token and selection range query assembly, `src/analysis/semantic/CompletionProvider.*` for completion context detection, item construction, resolve docs/snippets/data, filtering, and completion-specific query helpers, `src/analysis/semantic/SignatureInlayProvider.*` for signature help plus module/type/port/function/task inlay hint query assembly from AstIndex views, `src/analysis/semantic/DesignGraphProvider.*` for module hierarchy, schematic, call hierarchy, and backward cone query assembly from `AstIndex` / design graph views only, `src/analysis/semantic/CodeActionProvider.*` for quickfix selection and edit/create-file assembly from indexed diagnostics/code-action facts, `src/analysis/semantic/DiagnosticProvider.*` for slang + UX diagnostic aggregation/sort/dedupe from indexed diagnostic facts, and `src/analysis/semantic/QueryCache.*` for generation-scoped diagnostics and bounded visible-query cache keying/stats
- compatibility facade and document-state adapter; it should delegate migrated capabilities directly to `SemanticEngine` and should not provide diagnostics, symbol resolution, or LSP-visible fallback find paths: `src/analysis/SemanticWorkspace.cpp`
- shared URI/path/source-range/UTF-16 offset conversion helpers for analysis code: `src/analysis/SourceUtil.cpp`
- parse pipeline, syntax document-symbol extraction, `systemverilog/outline` opened-document fast-path conversion, document link/folding helpers, include/macro/package import/export lexical utilities, syntax hover content, completion-prefix text utility, and syntax diagnostics: `src/analysis/CompilationService.cpp`
- open-document state and UTF-16 incremental edits: `src/document/DocumentStore.cpp`
- workspace root resolution and `.slang/server.json` loading: `src/workspace/WorkspaceManager.cpp`
- stdio transport and JSON-RPC framing: `src/transport/StdioTransport.cpp`, `src/jsonrpc/MessageStream.cpp`, `src/jsonrpc/JsonRpcServer.cpp`
- waveform mock dataset generation, FST-backed source adaptation, binary envelope/framing, columnar viewport encoding, and local named-pipe / Unix-domain-socket session serving: `src/waveform/*`; this subsystem is independent from SystemVerilog semantic providers and owns waveform query/encoding rules. FST parser internals live under `src/waveform/fst/*`; the libfst reader source is the behavior authority, and Tim Hutt's unofficial spec is only a navigation aid when it does not conflict with libfst
- LEF/DEF/GDS physical-layout parsing, value-type layout data, binary envelope/framing, catalog/geometry columnar encoding, GDS hierarchy flattening, and local named-pipe / Unix-domain-socket session serving: `src/layout/*` and `include/pristine/layout/*`; this subsystem is independent from SystemVerilog semantic providers and owns physical-layout query/encoding rules
- process entry point, CLI switches, Windows stdio mode, and version output: `src/main.cpp`
- unit coverage for framing, lifecycle, sync, workspace, diagnostics, UTF-16, symbols, hover, AST identity, navigation/completion, design hierarchy, schematic, call hierarchy, backward cone, and JSON semantic golden request shapes: `tests/unit`
- focused LEF/DEF/GDS layout parser, data aggregation, binary protocol, and layout control-plane coverage: `tests/unit/LayoutProviderTests.cpp`, `tests/unit/ServerLifecycleTests.cpp`, `tests/e2e/layout_pipe_smoke.py`, `tests/e2e/lefdef_corpus.py`, `tests/e2e/ihp_lef_corpus.py`, and `tests/e2e/ihp_gds_corpus.py`
- JSON semantic golden fixtures for observable provider behavior, currently 260+ fixtures covering hover, definition/typeDefinition, references, rename, completion/resolve, signatureHelp, inlayHint, semanticTokens, selectionRange, diagnostics, codeAction, workspace/symbol, moduleHierarchy, schematic, backwardCone, and callHierarchy: `tests/golden/semantic`
- subprocess LSP smoke coverage for core LSP plus `systemverilog/*`: `tests/e2e`
- fixture-driven differential coverage against a local `slang-server v0.2.5` checkout: `tests/differential`; Windows/local runs may skip `pristine_differential_slang_server` when `SLANG_SERVER_ROOT` is unavailable, while Ubuntu 24.04 CI builds pinned `0ec16ae` and sets `PRISTINE_REQUIRE_SLANG_DIFFERENTIAL=1`, making the shared standard-LSP differential required. CTest must inherit `SLANG_SERVER_ROOT` at execution time rather than freezing its configure-time value, and rewritten fixtures compare only initialize-advertised capabilities exposed by both servers
- synthetic semantic performance baselines for 100/1000/5000-file workspaces: `tests/perf`; `pristine_perf_tests` is a required CTest whenever `PRISTINE_BUILD_PERF_TESTS=ON` and asserts warm completion/resolve success plus `completionScannedGlobalSymbols == 0` without a machine-specific time threshold

## Working Rules

- Prefer changing the layer that computes behavior, not the layer that only forwards or serializes it.
- When behavior changes, update or add the closest unit test in `tests/unit`.
- Preserve the current architecture split: transport -> jsonrpc -> lsp/workspace/document/analysis -> server.
- Preserve the waveform architecture split: LSP stdio is control plane only; waveform bytes use the dedicated local pipe service. Do not send waveform binary payloads, JSON waveform dumps, or render frames through `StdioTransport` / JSON-RPC.
- FST parsing, hierarchy restoration, block indexing, value decoding, viewport caching, and columnar frame encoding belong in `src/waveform/*` / `src/waveform/fst/*`; do not add FST parser or waveform encoding rules to `ServerSession.cpp`. `ServerSession` may validate `source` / `fstUri`, enforce workspace-root path boundaries, start/close sessions, and serialize session metadata only.
- FST parser behavior is measured against libfst first, with the pinned wellen FST corpus as the default regression fixture set. If a wellen fixture fails to parse, catalog, or emit a valid viewport frame, treat that as an implementation gap in `src/waveform/fst/*` unless libfst itself rejects the file.
- Preserve the layout architecture split: LSP stdio is control plane only; LEF/DEF catalog and geometry bytes use the dedicated local pipe service. Do not send layout binary payloads, JSON geometry dumps, or render frames through `StdioTransport` / JSON-RPC.
- LEF/DEF/GDS parsing, recovery diagnostics, layout data normalization, GDS cell/reference/element hierarchy modeling, hierarchy flattening, spatial indexing, tile/viewport selection, hit-test, inspect, selection-highlight geometry, catalog encoding, geometry filtering/truncation, and columnar frame encoding belong in `src/layout/*`; do not add LEF/DEF/GDS grammar, R-tree, LOD, hit-test, or physical-layout encoding rules to `ServerSession.cpp`. `ServerSession` may validate `lefUris` / `defUri` / `gdsUri`, enforce workspace-root path boundaries, start/close sessions, choose the source, and serialize session metadata only.
- LEF/DEF/GDS corpus validation is fixture-driven. `cibyr/lefdef` and `IHP-Open-PDK` live under `.deps/src/lefdef` and `.deps/src/ihp-open-pdk` when bootstrapped, and their files must not be copied into the repository. The TinyTapeout tinyQV GDS perf fixture lives only under `.deps/src/tinytapeout-sky-25a` after `scripts/fetch_tt_tinyqv_gds.mjs` uses system download tools to download or reuse the retained TinyTapeout repo zip `.deps/src/tinytapeout-sky-25a/tinytapeout-sky-25a-main.zip`, extracts `tt_um_tt_tinyQV.gds.br`, and Brotli-decompresses it; never commit the repo zip, `.gds.br`, or `.gds`. The retained zip may also be reused as an external fixture source for future OASIS parser work. For layout/parser/spatial-cache/tile/hit-test/inspect/search changes, bootstrap and run the IHP LEF/GDS corpus gates plus the TT tinyQV interaction e2e every round; IHP GDS should include sampled tile, cache, hit-test, inspect, and selection smoke plus p50/p95/max parser and query metrics, while TT should record zoom/pan burst, continuation, hit/inspect/selection, search, payload size, shape/point counts, cache hit/miss, and p50/p95/max latency. Only record these gates as not run when network or environment access makes bootstrap impossible.
- The first successful TT tinyQV paged-catalog interaction gate on this branch measured about 36.7s wall open, `PLCS` summary about 182ms, three `PLCP` pages about 15-19ms each, tile query p50/p95/max about 137ms/218ms/9.0s, hit query p95 about 136ms, and search query p95 about 17.2s. Treat those numbers as a local baseline for direction, not a portable CI threshold. Subsequent TT work showed that large-GDS parser changes should aggregate repeated recoverable unsupported-record diagnostics, cache each GDS element's local bbox during parse/finalization, keep `readMicros` scoped to file read time, and expose `record/elementFinalize/diagnostic/resolve/bbox` parser probes separately. LOD2 is a far-zoom overview path: for cells with references, return direct SREF/AREF/reference bbox placement rows without building R-trees or descending; for leaf/root cells with no references, return the cell bbox fallback. Do not build a full element R-tree for TT-sized leaf cells just to answer bounded LOD0/LOD1 tiles: a tried bulk-load path exceeded 30 minutes locally, while bounded streaming kept the gate usable. Search should use a session-local lazy index/cache over cell names, reference targets, TEXT strings, and first layer-name rows, with bbox/class computation only for entries that are actually indexed. A local post-optimization TT run measured about 24.1s wall open, overview query max about 112ms, tile query p95 about 170ms with one cold LOD1 streaming outlier near 10s, hit query p95 about 256ms, and search query p95 about 1.63s; keep reporting before/after deltas from `build/<preset>/tt_tinyqv_gds_interaction_metrics.json` each round. The next highest-leverage large-GDS work is parser/open time and replacing repeated bounded leaf scans with a cheaper tile/grid index, not raising the 128 MiB pipe payload limit. Current large-GDS parser optimization decodes GDS `XY`, single-value `INT2`, `COLROW`, `STRANS`, `MAG`, and `ANGLE` directly from record payloads instead of allocating temporary per-record vectors; `systemverilog/layout/open.result.gdsMetrics.parseMetrics` exposes `xyDecodeMicros`, `scalarDecodeMicros`, and `stringDecodeMicros` alongside older probes. Parser finalization also records `text_element_indices` and first-seen `(layer, datatype)` layer samples; layer registration should use those samples to keep catalog order stable without linearly searching all elements, and search-index construction should scan TEXT samples plus layer samples rather than all drawable elements. For large flat/leaf GDS cells, bounded LOD0/LOD1 tile and hit-test queries may use the internal coarse `GdsTileGridIndex` (cell-local bbox bins to element indices) before falling back to Boost R-tree or small-cell paths; keep it internal to `LayoutSpatialIndex`, apply layer/kind/datatype/max constraints after bin query, and verify correctness against owner metadata. The `PLTG` header keeps old offsets and appends grid build/hit/miss/candidate/bin counters for TT/IHP perf analysis.
- Current round TT tinyQV dev metrics after direct record decode, layer samples, search sample indexing, element grid, and reference grid measured about 17.2s open p95, 1.63s cold wide LOD1 max, 102ms tile query p95, 131us pan-grid p95, 149us warm-wide max, 91ms hit p95, and 644ms search p95. Relative to the previous local run, tile max improved about 83%, tile p95 about 40%, hit p95 about 61%, search p95 about 59%, and open about 27%; the remaining cold-wide cost is now mostly one-time grid build, not repeated R-tree bulk load or full element streaming. The next optimization converted parsed GDS coordinates to a library-level point arena (`LayoutGdsLibrary.points` plus per-element `first_point`/`point_count`) with fallback `LayoutGdsElement.points` only for hand-built small fixtures, changed GDS file read to file-size plus single binary `read`, and added GDS background warmup controlled by `PRISTINE_LAYOUT_WARMUP=0|1|auto` (default `auto`, large GDS only). Warmup must stay inside `src/layout/*`, build top-cell tile/search indexes without blocking `systemverilog/layout/open`, and serialize spatial queries so foreground tile/hit/inspect/selection waits for the same local build rather than racing or duplicating it. A dev TT tinyQV run after this work measured about 9.99s open p95, 448ms cold wide LOD1 max, 150us post-warm wide max, 248ms tile p95, 97ms hit p95, and 127ms search p95 with `pointArenaCount=9767655` and `warmupScheduled=true`; keep reporting the same before/after fields each round instead of relying on absolute CI thresholds.
- Latest TT tinyQV parser/tile-probe work showed that per-element parser timing can itself become a large-GDS hotspot. Default `elementFinalizeMicros` and `bboxMicros` should be sampled estimates rather than `Clock::now()` on every one of millions of elements; detailed finalize substage probes (`elementFinalizeBboxMicros`, `elementFinalizeReferenceMicros`, `elementFinalizeIndexMicros`, `elementFinalizeSampleMicros`) are opt-in with `PRISTINE_GDS_DETAILED_PARSE_PROBES=1` and should not be enabled for the default TT perf gate. Keep default finalization on the simplest hot path: one point-view lookup feeds bbox, SREF/AREF origin, and drawable/layer/text sample decisions. Cached `PLTG` payloads must rewrite cache metrics on return (`cacheHit=1`, `cacheMiss=0`, stale query/index/grid build counters cleared) so `repeat_cache` uses wall time plus cache-hit state rather than inherited cold-query counters. For LOD2 overview, do not cold-build a reference grid during the first far-zoom request; only reuse a reference grid when warmup or a prior request has already built it, otherwise keep the direct reference-bbox path. The dev full-validation TT sample for this round measured about 9.32s open p95, sampled `elementFinalizeMicros` about 2.65s, overview max about 233ms, cold wide LOD1 max about 454ms, post-warm wide max about 138us, cache repeat `queryMicros=0`, tile p95 about 219ms, hit p95 about 325us, and search p95 about 126ms; relative to the immediately preceding sample, open improved about 16.5%, sampled element-finalize about 23.8%, cold wide about 5.6%, warm wide about 36.1%, and search about 1.0%, while overview/tile/cache wall stayed roughly flat and hit remained sub-millisecond despite one noisy single-sample increase. The next high-leverage work remains parser/open reduction rather than payload-limit changes.
- Staged GDS open changed the large-file UX metric: TT tinyQV `systemverilog/layout/open` with `openMode=auto` returns a deferred session and pipe endpoint in tens of milliseconds, while `StatusRequest/PLST` reaches ready after the background parser finishes. TT e2e must report both `lsp_open_wall_micros` and `ready_wall_micros`; treat the former as UI responsiveness and the latter as parser/backend readiness. The parser has an internal `LayoutGdsParseControl` progress/cancel hook: staged sessions publish real `read/records/finalize/resolve/ready` phase and monotonic count snapshots to `PLST`, and `layout/close` sets a cancel token that the GDS record loop plus finalize/resolve path check before safely joining the background thread. Keep GDS record parsing single-threaded unless measurements prove otherwise. Default parser timing should stay coarse: `recordMicros` measures the whole records phase, while per-record/XY/scalar/string and finalize substage clock probes are only for `PRISTINE_GDS_DETAILED_PARSE_PROBES=1`; the default staged progress/cancel interval is 32768 records, with tests allowed to force interval 1. `PLST.warmupReady` now means the background warmup thread really finished; large-GDS warmup prebuilds the top-cell tile grid and reference grid/overview path plus lazy search index without changing v3 wire payloads. The latest dev TT run after top-cell grid/reference warmup measured LSP open about 32ms, cancel-close about 323ms, ready wall about 10.90s, status parse about 10.34s, about 0.96M records/s and 0.90M points/s, overview query max about 234us, cold wide LOD1 max about 105us, post-warm wide max about 144us, tile query p95 about 226us, hit query p95 about 347us, and search query p95 about 322ms, with one pending pipe error observed before ready. Compared to the preceding sample, overview max fell by roughly 99.9%, cold wide by roughly 99.98%, tile p95 by roughly 99.9%, while ready/parse stayed noise-bound. Continue using `build/<preset>/tt_tinyqv_gds_interaction_metrics.json` and `.previous.json` for before/after deltas; do not use staged initial LSP metadata as final parseMetrics because it is intentionally returned before parsing completes.
- The parser ready-wall follow-up kept v3 wire payloads unchanged and added only internal/JSON metrics: `resolveLookupMicros`, `resolveReferenceMicros`, and `resolveTopCellMicros` split the old `resolveMicros`, while TT e2e performs a post-interaction sync-open metrics probe so staged interaction remains the product path and parse breakdown is still measurable. GDS `UNITS`, `MAG`, and `ANGLE` should read REAL8 values directly instead of allocating a vector; ordinary GDS strings should use the fast direct-construction path and only strip NUL/trailing spaces when the payload actually needs cleanup. Recoverable malformed property records (`LAYER`, `DATATYPE`, `TEXTTYPE`, `XY`, `SNAME`, `COLROW`, `STRANS`, `MAG`, `ANGLE`, `STRING`) should no longer write into scratch parser state when there is no active element/reference; keep the warning behavior but avoid polluting point arenas or temporary layout state. `resolveReferences()` should use a reserved `string_view -> cell index` hash map over stable cell names instead of an ordered string map, preserving the old duplicate-name last-wins behavior. The dev TT sample for this round measured LSP open about 37ms, cancel close about 339ms, ready wall about 9.21s, sync parse probe wall about 8.70s, status parse about 8.71s, sampled element finalize about 5.07s, resolve about 45ms (lookup 133us, reference scan 29ms, top-cell inference 16ms), overview max about 156us, cold wide max about 161us, tile query p95 about 256us, hit p95 about 2.3ms, and search p95 about 121ms. Compared with the preceding TT metrics JSON, ready wall improved roughly 15%, search stayed near flat, and tile/overview stayed in the sub-millisecond range; the next high-leverage parser work is reducing element finalize/bbox point scanning rather than resolve or pipe payload changes.
- The XY-bounds parser follow-up moved GDS element bbox calculation into `parseXy()` while appending point-arena rows, so normal `finishElement()` should reuse `element.bounds` and only fall back to `pointBounds(elementPoints(...))` for hand-built fixtures or malformed/empty XY. Repeated `XY` records must reset `first_point`, `point_count`, and `bounds` to the latest record while leaving old arena rows unused and append-only. `bboxMicros` should now represent finalize fallback bbox work and may be zero for large well-formed GDS; `xyBoundsMicros` is a JSON-only parser metric for the sampled cost of doing bbox work during XY decode. The dev TT sample after this change measured LSP open about 32ms, ready wall about 10.20s, sync parse probe wall about 9.44s, status parse about 9.71s, sampled element finalize about 2.72s, bbox fallback 0us, sampled XY bounds about 191ms, resolve about 44ms, overview max about 341us, cold wide max about 187us, tile query p95 about 194us, hit p95 about 130us, and search p95 about 134ms. Compared with the prior metrics JSON, sampled element finalize fell by about 39% and bbox fallback dropped to zero, while ready wall moved within local noise; the next parser target should be record-loop/element push overhead or point decode throughput, not bbox fallback.
- Do not add new SystemVerilog semantic logic to `ServerSession.cpp`; put it in `SemanticEngine` or the nearest analysis-layer helper.
- Do not use string matching as the primary semantic authority when slang AST lookup / symbol identity can answer the question.
- Semantic lookup, sorting, and dedupe behavior must be deterministic across platform/compiler STL implementations; when multiple AST symbols share a source range, define an explicit tie-break such as declaration, typed symbol, narrower range, then stable id.
- If AST/Compilation facts conflict with legacy syntax/text fallback, delete or demote the legacy path instead of keeping two primary answers.
- Do not add a new analysis fallback index or `SemanticWorkspace::find*` as an LSP fallback path; visible requests should consume `SemanticEngine` value-type results.
- Discovery indexes are allowed only for shallow workspace candidate discovery, dependency-closure hints, initialization timing, and warm-start planning. If a discovery fact disagrees with `AstIndex` / design graph facts, the provider result must use the AST/design graph answer and may only report discovery disagreement as a message.
- Discovery closure metrics are perf/debug value-type summaries only. They may report root, candidate/selected/missing/deduped documents, cache behavior, closure build timing, and provider query timing, but they must not be serialized as mandatory LSP semantics or used to override `AstIndex` / design graph answers.
- Package export lexical scanning is a discovery-closure utility only; do not feed `CompilationService::packageExports()` into unresolved-package diagnostics, import ambiguity checks, code actions, or other visible semantic answers unless the fact is first promoted into an AST-backed `AstIndex` view.
- Discovery-guided hierarchy/schematic deep builds may narrow `SnapshotBuilder` input to the inferred dependency closure, but the returned hierarchy/schematic objects must still be assembled by `AstIndex` / `DesignGraphProvider`; optional `discoveryClosure*` telemetry may be serialized for perf diagnostics, not as semantic evidence.
- Workspace config `index[].dirs` and `index[].excludeDirs` should filter discovery input and affect discovery cache keys for open/in-memory documents as well as indexed files; this filtering must not be treated as proof that a semantic symbol exists or does not exist.
- `SemanticEngine::workspaceDiscovery()` should cache only value-type discovery summaries keyed by generation, workspace/config, and indexed file-set inputs; do not cache slang-owned objects or syntax trees in the discovery cache, and keep repeated-query cache-hit plus didChange/delete invalidation covered by focused tests.
- Snapshot construction belongs in `SnapshotBuilder`; `SemanticEngine::rebuildSnapshot` may prepare `SnapshotBuildInput`, accept `SnapshotBuildOutput`, and update mutable snapshot/cache state, but should not grow SourceManager/SyntaxTree/Compilation construction, slang diagnostic aggregation, or dependency-edge building branches.
- `SnapshotBuilder` must preserve slang `SourceManager` path uniqueness for opened headers and included documents. If a document is both present in the snapshot input and reached through an `` `include``, reuse the same SourceManager buffer rather than assigning the same path twice.
- AST index construction belongs in `AstIndex`; `SemanticEngine` may request an `AstIndexView` and cache results, but should not grow new symbol identity generation, declaration/reference indexing, module instance binding, workspace symbol filtering, kind-mapping, or per-provider symbol/reference mapping branches.
- Provider contexts for completion, signature/inlay, diagnostics, code actions, workspace symbols, hierarchy, schematic, call hierarchy, and backward cone should be populated from `AstIndexView` or other value-type provider views, not by each query rereading `SnapshotData` semantic fields directly.
- Do not add query-time `CompilationService` identifier/module/port scans for graph, cone, completion, signature, diagnostics, or code-action answers; promote the needed fact into `SnapshotBuilder` / `AstIndex` first.
- Do not use `CompilationService::moduleDefinitions` or equivalent syntax module models to populate LSP-visible module maps, module instances, hierarchy, schematic, completion, diagnostics, or code-action facts; AST-derived `AstIndex` module signatures and instance bindings own those facts.
- `DesignGraphProvider` must consume AST/design graph views from `AstIndex` for module/interface signatures, instance cells, schematic ports, hierarchy, call hierarchy, and cone inputs; do not reintroduce `CompilationService::moduleSchematics`, syntax schematic maps, `assignments_by_uri`, or `identifiers_by_uri` as visible graph/cone sources.
- `DesignGraphProvider` may memoize repeated hierarchy subtrees internally to reduce real-design cold query cost, but memoization must preserve every instance node's own name/ranges and must not merge repeated instances or change the Pristine-facing hierarchy wire shape.
- `DiagnosticProvider` and `CodeActionProvider` must consume indexed include/import/module/type/member/assignment facts from provider contexts; do not add request-time `CompilationService` scans in those providers.
- Do not add `metadata_by_uri`, `assignments_by_uri`, `identifiers_by_uri`, or equivalent syntax-derived semantic maps to snapshot data or visible provider contexts. AST range precision must come from slang source/syntax ranges or `AstIndex` views, not `CompilationService::identifiers`.
- `DiagnosticProvider` must derive duplicate, ambiguous import, unresolved type/package/module/include, and width/type mismatch diagnostics from slang diagnostics plus AstIndex symbol/type-reference/assignment-edge views. `CodeActionProvider` should consume those diagnostics and indexed facts, not syntax metadata or identifier scans.
- Do not add diagnostics or symbol-resolution rules to `SemanticWorkspace`; keep those rules in `SemanticEngine` or a `src/analysis/semantic/*` helper owned by it.
- Do not add semantic document rules to `SemanticWorkspace`; it is document sync, include stale tracking, workspace/config adaptation, and `SemanticEngine` facade only.
- Frontend-facing waveform payloads should stay columnar binary and typed-array-friendly. Prefer fixed headers plus offset/count fields for `Uint32Array`, `Float32Array`, `Float64Array`, and `Uint8Array` views; avoid per-segment JSON objects or string-heavy payloads. Keep bus labels in a lazy string table. FST value-change blocks should be decoded lazily by intersecting block time ranges and requested signals; do not convert whole FST files into mock-style full transition vectors except for tiny focused fixtures or explicit test helpers.
- Frontend-facing layout payloads should stay columnar binary and typed-array-friendly. Prefer fixed headers plus offset/count fields for `Uint32Array`, `Float64Array`, and `Uint8Array` views; keep layer/macro/net/cell labels in a lazy string table, and keep large geometry responses filtered and truncatable instead of serializing per-shape JSON over LSP. `pristine-layout-columnar-v3` is the only layout protocol: it keeps the `PLD1` frame envelope, rejects v1/v2 frames, uses `PLCT` payload version 3 with a 136-byte superset header, and reuses `PLGE` payload version 3 for all LEF/DEF/GDS render geometry. Full `PLCT` remains valid for LEF/DEF and small GDS, but large GDS must use paged catalog flow instead of raising the 128 MiB payload guardrail: request `CatalogSummaryRequest/PLCS` first, then page `layers/cells/references/elements/points/strings/diagnostics` with `CatalogPageRequest/PLCP` before using tile/hit/search data. `GeometryRequest` payload starts with `u32 flags`, `u32 maxShapes`, optional bbox `f64 x0,y0,x1,y1` when `flags & 0x1`, then `u32 layerCount + u32 layerIndices[]` and `u32 kindCount + u32 shapeKinds[]`; when `flags & 0x2`, it appends `u32 macroIndexCount + u32 macroIndices[]` and `u32 gdsRootCellIndexCount + u32 gdsRootCellIndices[]`. Unsupported flag bits, trailing bytes, truncated lists, and out-of-range macro/cell indices are invalid requests. `PLCT` header fields after magic/version/headerSize are `u32` at these offsets: 8 unitsPerMicron, 12 sourceKind (`1=lefdef`, `2=gds`), 16 shapeCount, 20 hasBounds, 24 topCellIndex, 28 stringTableOffset, 32 stringTableSize, 36 layerCount, 40 layerOffset, 44 macroCount, 48 macroOffset, 52 macroPinCount, 56 macroPinOffset, 60 viaCount, 64 viaOffset, 68 componentCount, 72 componentOffset, 76 defPinCount, 80 defPinOffset, 84 netCount, 88 netOffset, 92 gdsCellCount, 96 gdsCellOffset, 100 gdsReferenceCount, 104 gdsReferenceOffset, 108 gdsElementCount, 112 gdsElementOffset, 116 gdsPointCount, 120 gdsPointOffset, 124 diagnosticCount, 128 diagnosticOffset, and 132 reserved. `PLCS` has a 152-byte header with source, counts, top cell/bounds, first 64 layer rows, parse/open metrics, and a compact string table; `PLCP` has a 40-byte header with table kind, offset, count, total, nextOffset, and a per-page string table. Macro pin rows remain 28 bytes: `u32 macroIndex`, `u32 pinIndex`, `u32 nameStringOffset`, `u32 useStringOffset`, `u16 direction`, `u16 reserved`, `u32 firstShapeIndex`, `u32 shapeCount`. DEF top-pin rows are 40 bytes: `u32 nameStringOffset`, `u32 netNameStringOffset`, `u16 status`, `u16 reserved`, `f64 x`, `f64 y`, `u32 orientationStringOffset`, `u32 firstShapeIndex`, `u32 shapeCount`. `PLGE` shape table entries are 28 bytes: `u32 layerIndex`, `u16 kind`, `u16 ownerKind`, `u32 ownerIndex`, `u32 macroIndex`, `u32 flags`, `u32 polygonOffset`, `u32 polygonPointLength`. `macroIndex == 0xffffffff` means no macro owner. LEF macro pin and obstruction shapes must set `macroIndex` to the stable `catalog.macros` index; DEF top-pin shapes set `ownerKind == Pin` with `macroIndex == 0xffffffff`; GDS flattened shapes set `ownerKind == GdsElement`, `ownerIndex` to the GDS element index, and `macroIndex` to the source GDS cell index. Frontends must map LEF macro pin shapes to names with `(shape.macroIndex, shape.ownerIndex) -> (macroPinTable.macroIndex, macroPinTable.pinIndex)`, map DEF top pins through `defPinTable`, filter macro-local geometry by `shape.macroIndex === selectedMacro.index`, and request GDS root-cell geometry with `gdsRootCellIndices=[catalog.topCellIndex]` when rendering the full top hierarchy.
- Large-GDS interaction requests are v3 message additions, not a protocol-version bump: message ids 9/10 are `TileGeometryRequest/Response`, 11/12 are `HitTestRequest/Response`, 13/14 are `InspectRequest/Response`, 15/16 are `SelectionGeometryRequest/Response`, 17/18 are `SearchRequest/Response`, 19/20 are `CatalogSummaryRequest/Response`, 21/22 are `CatalogPageRequest/Response`, and 23/24 are `StatusRequest/Response`. `StatusRequest` is `u32 flags` and currently only accepts zero. `StatusResponse` uses `PLST` version 3 with state (`parsing=1`, `ready=2`, `failed=3`, `closing=4`), phase (`read`, `records`, `finalize`, `resolve`, `ready`, `failed`), file size, bytes read, record/cell/reference/element/point/string/diagnostic counts, elapsed/open/parse micros, warmup scheduled/ready flags, and an optional error string table; unknown flags and trailing bytes are protocol errors. `TileGeometryRequest` starts with `u32 flags`, `u32 rootCellIndex`, `u32 maxShapes`, `u32 maxPoints`, `u32 maxBytes`, `u32 lod`, `u32 continuationToken`, optional bbox `f64 x0,y0,x1,y1` when `flags & 0x1`, then layer/kind/datatype count arrays; bad, stale, expired, or mismatched continuation tokens must return pipe error frames. Tile LOD semantics are fixed in v3: `lod=0` returns precise element geometry for close inspection, `lod=1` returns element bbox/simplified geometry for mid-zoom pan, and `lod=2` returns reference/cell bbox placement geometry for far-zoom overview. Bbox, layer, kind, datatype, root cell, and max constraints compose as logical AND; `maxShapes`, `maxPoints`, and `maxBytes` set truncated plus a server-side continuation token when exceeded. `TileGeometryResponse` uses `PLTG` and embeds a `PLGE` chunk plus truncated flag and next token; the `PLTG` header carries perf counters for index-build, query, encode, visited-cell, element-candidate, reference-candidate, traversed-reference, returned LOD-shape, cache-hit, cache-miss, and optional grid build/hit/miss/candidate/bin counts while keeping the original field offsets stable. Layout sessions own an LRU cache of encoded `PLTG` payloads, defaulting to 256 MiB and configurable with `PRISTINE_LAYOUT_CACHE_BYTES`; cache keys must include root cell, bbox/tile inputs, LOD, filters, max constraints, and continuation state. `HitTestRequest` uses root cell, point/radius, max results, and layer/kind/datatype filters; `PLHT` hit rows carry stable object ids including `instance_path_hash`, and the `PLHT` header carries index-build, query, encode, tile-shape, and precise-candidate counters. Traversal should generate stable per-instance-chain hashes for SREF/AREF paths and keep a session map from hash to full instance path and transform; stale ids must return pipe error frames. `InspectRequest` and `SelectionGeometryRequest` use the same object-id payload; inspect returns `PLIN` metadata while selection returns `PLGE` highlight geometry with instance transforms applied. `SearchRequest` is a GDS text/name search payload: `u32 flags`, `u32 maxResults`, `u32 kindMask`, `u32 rootCellIndexOrFFFFFFFF`, optional bbox when `flags & 0x1`, then `u32 queryBytes + UTF-8 query`; kind bits are cell name, reference target name, TEXT string, and layer name. Search is not net connectivity or LVS; it is string search over GDS catalog/text/layer data, root-restricted when a root cell index is provided, and bbox-filtered when requested. `SearchResponse` uses `PLSR`, returns object ids usable by inspect/selection, label string offsets, object class, source cell, bbox, rank, and index/query/encode timing. `PLIN` preserves old fields and appends object class, source cell, and instance path string offset. GDS object class is best-effort only: `PATH=wire`, `TEXT=label`, reference/cell bbox=`cell`, boundary under pad/IO cell or pad-like layer=`pad`, other boundary=`shape`; inspect must always include raw GDS kind, layer, datatype, texttype, text, cell/reference/element indices, source cell, and instance path when available. Unknown flags, trailing bytes, oversized search queries, bad catalog page table/offset/limit/maxBytes, bad object ids, out-of-range roots, and invalid owner indices must return pipe error frames. `systemverilog/layout/open.result.gdsMetrics.parseMetrics` should expose parser probes for read/record/XY decode/scalar decode/string decode/element-finalize/diagnostic/resolve/bbox micros plus record, XY point, string, cell, reference, and element counts; for large GDS optimization, compare LSP open wall, status-to-ready wall, open/parse probes, `PLCS`/`PLCP` wall time and payload bytes, plus `PLTG`/`PLHT`/`PLSR` counters and IHP/TT GDS p50/p95/max output before changing hot paths.
- Completion item `data` must be generated by the analysis layer and resolved through the `CompletionProvider` query path behind `SemanticEngine::resolveCompletion`; do not infer completion docs, snippets, ports, or macro bodies in `ServerSession`.
- Completion context detection for package `::`, hierarchical `.`, macro, and module-instantiation positions plus completion item/doc/snippet construction and resolve behavior belongs in `CompletionProvider`; do not duplicate that logic in `SemanticEngine` or `ServerSession`.
- CompletionProvider-owned member qualifier parsing must handle array element and hierarchical selectors, and module-port completion must use provider-owned connected named-port exclusion instead of ad hoc scans in `SemanticEngine` or `ServerSession`.
- If member completion has both port-list and general member interpretations, first consume indexed module instance context when the cursor is inside an indexed instance range, then fall back to `AstIndex` member/symbol views; do not mark the query unresolved merely because the current file has no module instances.
- Signature help and inlay hint assembly belongs in `SignatureInlayProvider`; `SemanticEngine` may prepare value-type snapshot context, but should not grow new signature/inlay semantic branches.
- Function/task argument inlay hints must consume `AstIndex` call signature views. Text scanning may locate argument insertion offsets within the indexed call range, but it must not determine the callee, parameter list, or semantic type.
- Semantic token and selection range assembly belongs in `NavigationProvider`; `SemanticEngine` may prepare value-type navigation context, but should not grow new token/selection semantic branches.
- `systemverilog/outline` is intentionally syntax-backed through `CompilationService::documentSymbols()` for opened documents only; do not route it through hierarchy/schematic snapshots, discovery closure, or query-time provider semantic fallback unless the user explicitly asks for a deeper workspace outline API.
- Module hierarchy, schematic, call hierarchy, and backward cone assembly belongs in `DesignGraphProvider`; `SemanticEngine` may prepare value-type design graph context and cache results, but should not grow new HDL graph semantic branches.
- Hierarchy and schematic behavior changes must include nearest `DesignGraphProvider` unit coverage plus a JSON semantic golden fixture that exercises the Pristine-facing request/result shape.
- Diagnostics aggregation belongs in `DiagnosticProvider`; `SemanticEngine` may prepare value-type diagnostic context and cache results, but should not grow new UX diagnostic branches.
- Code-action quickfix selection belongs in `CodeActionProvider`; `SemanticEngine` may prepare value-type code-action context and cache results, but should not grow new quickfix semantic branches.
- Diagnostics behavior changes must be covered in both the nearest engine/unit test and an LSP-facing smoke or golden-style test when the published shape changes.
- Do not add public APIs that expose slang AST pointers; keep snapshot-owned slang objects behind value-type analysis results.
- Do not duplicate URI/path/source-range conversion logic; use `SourceUtil` from analysis code.
- Keep `SemanticEngine` public APIs value-type based, with generation/messages/unresolved/partial/truncated metadata where a query can be incomplete, including while splitting implementation into `src/analysis/semantic/*` helpers.
- Provider splits must not change `SemanticEngine` public request/response contracts unless the user explicitly asks for an API break.
- Cache keys for visible semantic queries must include snapshot generation and all user-visible inputs; mutation/configuration paths must invalidate affected cached results. This applies to diagnostics, references, rename, completion, signatureHelp, inlayHint, workspace/symbol, hierarchy, schematic, backward cone, and codeAction cache entries.
- All semantic behavior changes must update the nearest unit, golden-style, or e2e coverage.
- New visible provider behavior must have the nearest focused unit test and, when it changes an externally visible request/result shape, a JSON semantic golden fixture; debt fixtures should be converted to positive assertions once the underlying AST/provider capability lands.
- Prefer adding JSON semantic golden fixtures for externally observable provider behavior after the closest unit test owns the narrow semantic rule; keep fixtures focused on stable request/result shape and no-fallback regressions.
- Each new LSP-visible provider behavior should have the nearest focused unit test plus a JSON semantic golden fixture when the request/result shape is externally observable.
- Debt fixtures must explicitly report `unresolved`, `partial`, or `messages` instead of pretending a missing provider behavior is implemented; once the provider behavior lands, convert the fixture to a positive assertion in the same slice.
- Query-cache, invalidation, and affected-rebuild behavior should have focused unit coverage for generation plus all user-visible inputs before perf numbers are treated as meaningful.
- Affected rebuild coverage should couple discovery/graph cache assertions with at least one visible semantic query such as workspace symbols, hierarchy, or schematic so stale query-cache hits cannot hide behind fresh telemetry.
- Snapshot build input normalization, query-cache keying/stats, affected-dependency traversal, background diagnostics worker changes, and gate-runner/test-infrastructure changes must run the nearest focused tests plus the provenance-bound full Debug, Windows `clang-cl` Debug/full CTest and clean Release smoke, and clean canonical Release gates before handoff. A gate summary is valid only when its source fingerprint remains stable for the entire run.
- `tests/gate_manifest.json` is the single source of truth for required and optional full-gate CTest targets. Any added, renamed, or removed CTest must be classified there in the same change: product/integration tests go under `requiredTests` or `optionalSkipTests`, and runner-only coverage stays the `runnerSelfTest`. Full-gate and `--check-manifest-only` runs must fail on unclassified CTests, missing required gates, unexpected skips, or missing required environment values.
- Differential fixtures should be fixture-driven and rewritten for this repository by default. If a slang-server MIT fixture is copied instead of rewritten, update notice/attribution in the same change.
- The local `slang-server v0.2.5` checkout is covered in the attribution/notice pipeline as a differential reference. Keep `cmake/attributions.cmake`, `licenses/texts/`, `ATTRIBUTIONS.md`, and `NOTICE` in sync whenever copied upstream fixture material changes.
- Changes that scan, cache, or rebuild workspace-wide state must include or update a performance-oriented test/benchmark plan or JSON baseline before being considered complete.
- Real-design stress tests use the RTL E2E corpus framework and must not vendor RTL. The default required corpus is retroSoC, but the framework is named for reusable RTL E2E coverage across open-source RTL corpora. The required real gate may download retroSoC into `.cache/rtl-e2e/retrosoc` when no verified `RTL_E2E_ROOT` is provided; user-provided `RTL_E2E_ROOT` must be verified but not auto-checked-out or modified. `RTL_E2E_LSP_MODE=probe` remains the quick synthetic one-file data-path smoke, while `pristine_rtl_e2e_real_retrosoc` is the required real-mode corpus gate. Legacy `RETROSOC_*` environment variables are aliases for the default retroSoC corpus only. Update notice/attribution before copying any external fixture into this repository.
- After completing feature work, prefer the canonical Windows suite `python scripts/run_required_gate_suite.py --profile windows-local` from an amd64 VS shell. It binds full Debug, clang-cl Debug/full CTest, clean clang-cl Release smoke, and clean canonical Release to one source fingerprint. `run_windows_clang_gate.py` must guard-delete `build/clang-cl-release` before its Release configure, while `run_clean_release_gate.py` must guard-delete `build/release`; both accept proving the directory was already absent and run version plus LSP/waveform/layout smoke tests against the freshly built binary.
- At the end of each graph/deep-semantic round, record: the shared-LSP differential result, the graph/cone zero-global-scan result, slang-server capabilities still used as a reference, and explicitly excluded slang-server features. `InstanceIndexer`, `ReferenceIndexer`, and `ConeTracer` remain useful architectural and differential references until Ubuntu shared-LSP differential is clean for three consecutive rounds and real retroSoC plus synthetic graph baselines are deterministic with zero global scans. WCP, `slang.*` commands, long-lived AST pointer ownership, and waveform viewer parity remain non-targets.
- New or modified aggregate initialization and semantic/provider C++ changes should be validated with the clang toolchain before CI when practical, because GCC/Clang and MSVC reject different warning-as-error patterns. Treat unused helpers, narrowing conversions, case-sensitive include paths, partial designated initialization, filesystem path separators, and CRLF/LF-sensitive fixtures as Linux/macOS CI risks even if MSVC accepts them.
- Avoid broad refactors unless the user asks for them or the local change cannot be made safely otherwise.
- Do not reintroduce agent process rules into `README.md` unless explicitly requested.

## Cross-Platform C++ Guardrails

- Keep C++ changes portable across Windows, Linux, and the hosted CI matrix; do not rely on MSVC-only behavior just because local Windows builds pass.
- Prefer standard C++20 and standard library facilities over platform APIs. If a platform-specific branch is unavoidable, isolate it in the owning layer behind small `#if defined(_WIN32)` guards and keep the non-Windows path equally maintained.
- Assume GCC/Clang and MSVC diagnose different things. Write code that is clean under all of them, including `-Wall -Wextra -Wpedantic -Werror` on Linux and warnings-as-errors behavior on Windows.
- Be explicit with aggregate and designated initialization when later fields are defaulted or filled incrementally; GCC/Clang can reject partial initialization patterns that MSVC accepts.
- Do not hardcode Windows-only path separators, drive-letter assumptions, `.exe` suffixes, or case-insensitive file lookups. Use `std::filesystem`, repository-relative paths, and case-correct filenames.
- Treat line endings, text-vs-binary mode, and stdio behavior as platform-sensitive. Keep protocol and test data stable under both CRLF and LF, and preserve the existing Windows-specific stdio handling in `src/main.cpp` when touching process entry behavior.
- Keep shell commands, tests, and fixtures platform-aware. Do not assume PowerShell-only quoting, POSIX-only utilities, or environment variable syntax inside portable product code or cross-platform tests.
- When touching filesystem, process, encoding, or terminal behavior, validate the narrowest affected slice locally and then check whether the same code path is exercised by Linux CI.

## Build Environment

Dependencies are not tracked as git submodules. They are downloaded into the local, gitignored `.deps/` cache.

Bootstrap dependencies:

```powershell
cmake -DPRISTINE_ROOT_DIR=. -P scripts/bootstrap_deps.cmake
```

Bootstrap optional LEF/DEF parser corpora:

```powershell
cmake -DPRISTINE_COMPONENTS=lefdef -P scripts/bootstrap_deps.cmake
cmake -DPRISTINE_COMPONENTS=ihp_open_pdk -P scripts/bootstrap_deps.cmake
```

Configure and build the development preset:

```powershell
cmake --preset dev
cmake --build --preset dev
```

Create a version tag for the current project version:

```powershell
pwsh -NoProfile -File scripts/create-version-tag.ps1
```

Use `-Push` to push the tag to the remote and trigger the tag release workflow. The script derives the tag from `CMakeLists.txt` and creates an annotated `v<version>` tag. Runtime code and tests consume the generated `pristine/Version.h` header configured from the same CMake project version.

Windows note:

- use a Visual Studio Developer Command Prompt or equivalent MSVC environment before configuring
- in this workspace, the Visual Studio bundled `ctest.exe` may be more reliable than `ctest` from `PATH`

## Validation Playbook

Pick the narrowest validation that can falsify the change.

For code changes under `src/` or `include/`:

```powershell
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure
```

For semantic-analysis changes, also validate the closest semantic slice:

```powershell
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure
```

Add or update focused semantic unit tests for AST-backed diagnostics, lookup, hover, definition, references, rename, completion, completion resolve, signature help, inlay hints, semantic tokens, selection ranges, module hierarchy, call hierarchy, schematic, backward cone, and code action behavior as appropriate. Golden semantic cases live under `tests/golden/semantic` as JSON fixtures and subprocess LSP smoke tests should change with externally observable semantic behavior, including semantic diagnostics publication, completion resolve data, unresolved, partial, truncated, bad syntax recovery, missing include, broken build config, cyclic hierarchy, large result caps, and no-fallback regression shapes when relevant. When removing workspace semantic-model or syntax/text semantic APIs, validate the closest `AstIndex` / provider unit coverage plus an LSP-facing smoke for the changed result shape. When migrating graph/schematic/cone facts, include an AST-derived schematic/cone case that would fail if syntax schematic or identifier extraction were used. After splitting `SemanticEngine` helpers, run the nearest semantic unit tests plus the LSP e2e smoke that exercises the affected provider. For workspace-wide indexing, query cache, affected rebuild, or invalidation changes, include a performance baseline plan covering initialize, didOpen, didChange, didSave, diagnostics, completion, resolveCompletion, signatureHelp, inlayHint, semanticTokens, references, rename, workspace/symbol, moduleHierarchy, schematic, backwardCone, and codeAction on small and large synthetic workspaces.

For LEF/DEF/GDS layout changes, keep validation in the layout subsystem and pipe e2e path. Bootstrap IHP Open PDK first for layout/parser changes; missing `.deps/src/ihp-open-pdk` is not enough to treat the IHP gate as complete:

```powershell
cmake -DPRISTINE_COMPONENTS=ihp_open_pdk -P scripts/bootstrap_deps.cmake
cmake --build --preset dev --target pristine-engine pristine_unit_tests
build\dev\tests\pristine_unit_tests.exe "[layout]"
build\dev\tests\pristine_unit_tests.exe "[server][layout]"
python tests\e2e\layout_pipe_smoke.py build\dev\pristine-engine.exe
ctest --test-dir build/dev -R "pristine_(lefdef|ihp_lef|ihp_gds)_corpus" --output-on-failure
```

The IHP LEF and GDS corpus tests must run for layout/parser work unless bootstrap is blocked by network or environment policy; record that blocker in the handoff instead of treating the skip as a pass. `cibyr/lefdef` remains useful as an additional corpus gate when bootstrapped. GDS parser/protocol work must keep focused in-repo GDS fixtures or generated tiny GDS files in unit/e2e tests alongside the IHP GDS corpus gate. Treat parser crashes, fatal diagnostics on common LEF/DEF/GDS constructs, unstable aggregate counts, or geometry/catalog frame failures as `src/layout/*` implementation gaps.

For layout protocol constants, public headers, ABI fields, or binary wire-layout changes, do not trust an incremental `build\release` result that reports `ninja: no work to do`. A stale release build has previously mixed `initialize` metadata from rebuilt objects with `layout/open` metadata from an old `LayoutPipeService.cpp` object, leaving conflicting layout protocol metadata in one executable. After these changes, clean rebuild the release engine and run the pipe smoke against that exact binary:

```powershell
cmake --preset release
cmake --build --preset release --clean-first
python tests\e2e\layout_pipe_smoke.py build\release\pristine-engine.exe
```

When reporting release validation for layout protocol/header changes, include the actual `systemverilog/layout/open` metadata observed from the release binary: LEF/DEF and GDS must both report `protocol == "pristine-layout-columnar-v3"` with `source == "lefdef"` or `source == "gds"` respectively.

Before running full validation after an interrupted or aborted test turn, check for and stop leftover `ctest`, `pristine-engine`, and `pristine_unit_tests` processes so local pipes, sockets, temp workspaces, and file handles do not leak into the next run.

For full pristine-engine validation, run the Debug build and the complete CTest suite with IHP required:

```powershell
cmake --build --preset dev
$env:PRISTINE_REQUIRE_IHP_OPEN_PDK='1'
ctest --test-dir build/dev --output-on-failure
```

The full suite must include unit tests, `pristine_lsp_core_e2e`, waveform pipe and FST perf tests, layout pipe and LEF/DEF/GDS corpus tests, RTL E2E gates, and LSP performance/stress tests such as `pristine_rtl_e2e_stress` and `pristine_rtl_e2e_lsp_stress`. `pristine_differential_slang_server` remains optional locally when `SLANG_SERVER_ROOT` is unavailable but is required on Ubuntu 24.04 CI through `PRISTINE_REQUIRE_SLANG_DIFFERENTIAL=1`; `pristine_perf_tests`, `pristine_ihp_lef_corpus`, and `pristine_ihp_gds_corpus` must not skip in required validation.

For a complete local Windows handoff, enter the VS amd64 shell and use the canonical suite. It creates one run context, configures/builds Debug with perf tests, runs the full status suite, runs structured clang-cl Debug configure/build/CTest, removes or proves absent the old clang-cl Release directory and runs its version/LSP/waveform/layout smokes, removes or proves absent the canonical Release directory and rebuilds/smokes it, then writes the artifact index:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -SkipAutomaticLocation -Arch amd64
python scripts/run_required_gate_suite.py --profile windows-local
```

For focused long Debug validation, the status runner remains available. Generate or reuse a run context when its output will be combined with other gates:

```powershell
$env:PRISTINE_REQUIRE_IHP_OPEN_PDK='1'
$env:PRISTINE_REQUIRE_TT_TINYQV_GDS='1'
python scripts/create_gate_run_context.py --output build/gate-artifacts/run-context.json
python scripts/run_full_tests_with_status.py --build-dir build/dev --include-perf --run-context build/gate-artifacts/run-context.json
```

The runner keeps CTest as the source of truth, runs tests one by one, streams child output, writes per-test logs plus `summary.json` under `build/dev/test-status-logs`, and prints heartbeat lines such as `[pristine-test] test=<name> phase=<phase> elapsed=<Xs> detail=<...>` to stderr. Tune heartbeat cadence with `PRISTINE_TEST_STATUS_INTERVAL_SECONDS` (`30` by default, `0` disables) and set `PRISTINE_TEST_STATUS_JSONL=<path>` when a machine-readable status artifact is useful. Long-running wrappers should emit at least `begin`, major phase changes, `summary`, and `end`; keep payloads small and never print waveform/layout binary data or full LSP traces through status messages.

The status runner summary is part of the gate contract: it records schema version, run id, full Git SHA, source fingerprint and stability, start/end time, duration, manifest hash, compiler and binary hash, build preset/type, artifact root, the discovered CTest list, selected tests, ctest path, selected environment variables, required/optional skip counts, missing manifest tests, unclassified CTests, and required environment satisfaction. The 16 product/integration CTest targets are tracked in the runner manifest when perf is enabled; all non-differential product gates, including `pristine_perf_tests`, are required. `pristine_differential_slang_server` is the only optional skip locally and becomes required when `PRISTINE_REQUIRE_SLANG_DIFFERENTIAL=1`. A missing manifest target, a non-optional skip, an unclassified CTest, missing required environment, changed source, or any failed test must be treated as a failed full gate.

The required gate manifest lives at `tests/gate_manifest.json` and is shared by local validation, runner self-tests, and CI. Use these quick checks when changing CTest definitions or validation scripts:

```powershell
python scripts/run_full_tests_with_status.py --print-manifest
$env:PRISTINE_REQUIRE_IHP_OPEN_PDK='1'
$env:PRISTINE_REQUIRE_TT_TINYQV_GDS='1'
python scripts/run_full_tests_with_status.py --build-dir build/dev --check-manifest-only
```

Focused runs with `--only` or `--exclude` are allowed for iteration, but their summary must report `fullGateEnforced=false`; do not treat them as a full validation handoff.

For changes touching diagnostics, snapshot construction, `AstIndex`, query cache, background workers, or `ServerSession` request ordering, add or update a large-workspace request-order regression. At minimum cover `didOpen -> syntax diagnostics -> outline/documentSymbol -> hover -> background full diagnostics` and a stale `didChange` or `didClose` guard. The fixed reproducible baseline is `pristine_rtl_e2e_large_workspace`: it generates a 300-file RTL corpus, runs the Debug `pristine-engine --stdio` binary through the Pristine request order, records protocol/debug traces, and must not touch the network. E2E diagnostics wait loops must not use `workspace/symbol`, hierarchy, schematic, cone, completion, or other deep semantic requests as a heartbeat; use passive notification reads or syntax-fast-path requests such as `textDocument/documentSymbol` / `systemverilog/outline`.

For optional local slang-server comparison, set `SLANG_SERVER_ROOT` to the local `slang-server v0.2.5` checkout with a built `slang-server` binary, then run:

```powershell
ctest --test-dir build\dev -L differential --output-on-failure
```

The differential runner should remain fixture-driven and skip cleanly when `SLANG_SERVER_ROOT` or the binary is unavailable. Use the `$maksyuki-test` style for validation reporting: record the smallest reproducer, exact command, failure bucket, and whether a broader suite was rerun after the targeted slice.

For macOS/Linux CI-risk semantic changes, include a no-fallback regression for deterministic `AstIndex` lookup and hover/type metadata, especially same-range port/internal symbols such as `input logic [WIDTH-1:0] data`. On Windows, run the structured clang-cl gate; this is an auxiliary check and does not replace a true non-MSVC clang++ pass:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -SkipAutomaticLocation -Arch amd64
python scripts/run_windows_clang_gate.py --run-context build/gate-artifacts/run-context.json
```

Use the amd64 VS developer shell for local Windows clang-cl gates; an x86 `LIB`/toolchain environment can produce misleading CRT/STL link failures unrelated to the code. The required local gate uses native Windows `clang-cl` only and must cover both Debug/full CTest and Release smokes; do not route this gate through WSL. GitHub CI's Ubuntu 22.04 and 24.04 build-test matrix uses clang++ and remains the required real non-Windows compile gate for every PR.

```powershell
cmake -S . -B build/clang-linux -G Ninja -D CMAKE_BUILD_TYPE=Debug -D CMAKE_CXX_COMPILER=clang++ -D PRISTINE_BUILD_TESTS=ON
cmake --build build/clang-linux
ctest --test-dir build/clang-linux --output-on-failure
```

If a WSL, Linux, container, or native clang++ environment is available, record the result in the handoff. If it is unavailable, say so explicitly and rely on CI as the hard gate. Use this specifically to catch aggregate initialization, unused/static helper, narrowing conversion, case-sensitive include, filesystem path, and CRLF/LF issues that MSVC may miss. Do not weaken the Ubuntu clang++ matrix to work around compile failures; fix the code.

For the clean Release gate, prefer the scripted runner so old binaries cannot accidentally satisfy smoke tests:

```powershell
python scripts/run_clean_release_gate.py --build-dir build/release --run-context build/gate-artifacts/run-context.json
```

The runner writes per-step logs and `summary.json` under `build/clean-release-gate-logs`; failures must report the failed step, exit code, and log path.
The clean Release summary records same-run provenance, `cleanPreparation.status=deleted|alreadyAbsent`, the guarded path, binary SHA256, and `releaseVersionOutput` so a handoff can prove the smoke tests used a freshly rebuilt binary.

After full Debug and clean Release gates, generate the audit index so CI/local results have one artifact entry point:

```powershell
python scripts/write_gate_artifact_index.py `
  --profile windows-local `
  --run-context build/gate-artifacts/run-context.json `
  --full-summary build/dev/test-status-logs/summary.json `
  --clang-summary build/clang-cl-gate-logs/summary.json `
  --release-summary build/clean-release-gate-logs/summary.json `
  --output build/gate-artifacts/index.json
```

`build/gate-artifacts/index.json` must fail generation when summaries use different run ids, Git heads, source fingerprints, manifest hashes, or schema versions; when clang-cl Debug or Release evidence is missing from the Windows local profile; when either clang-cl build does not report Clang/clang-cl with the expected build type; when the full Debug summary is not clean; or when either Release does not prove a guarded clean preparation. Dirty worktrees are allowed, but the exact dirty source fingerprint must remain unchanged across every gate. Any added or removed contract field must update runner script tests in the same change.

For performance and RTL E2E baselines, configure with `PRISTINE_BUILD_PERF_TESTS=ON`. `pristine_perf_tests` is then required by the gate manifest and prints JSON for 100/1000/5000-file synthetic workspaces, including candidate-scan and cold/warm completion/resolve baselines. The required real retroSoC LSP gate runs through CTest and must not vendor RTL:

```powershell
$env:RTL_E2E_CORPUS='retrosoc'
$env:RTL_E2E_ROOT='C:\path\to\retroSoC'
$env:RTL_E2E_EXPECTED_COMMIT='76651fd'
ctest --test-dir build/dev -R pristine_rtl_e2e_real_retrosoc --output-on-failure
```

When `RTL_E2E_ROOT` is unset, the required corpus wrapper clone/fetches retroSoC into `.cache/rtl-e2e/retrosoc` and verifies commit `76651fd`; failure is a gate failure, not a skip. `RETROSOC_ROOT`, `RETROSOC_EXPECTED_COMMIT`, `RETROSOC_REPO_URL`, and `RETROSOC_CACHE_DIR` remain legacy aliases for the default retroSoC corpus only.

The direct stress output is JSON with file/byte counts, parse/index timing, hierarchy timing, schematic timing, result counts, and partial/truncated/message counters. For LSP-level retroSoC pressure, first bootstrap the opt-in test dependency:

```powershell
cmake -DPRISTINE_COMPONENTS=lsp_framework -P scripts/bootstrap_deps.cmake
cmake --preset dev -DPRISTINE_BUILD_PERF_TESTS=ON
cmake --build --preset dev
ctest --test-dir build/dev -L lsp --output-on-failure
ctest --test-dir build/dev -R pristine_rtl_e2e_large_workspace --output-on-failure
ctest --test-dir build/dev -R pristine_rtl_e2e_real_retrosoc --output-on-failure
```

`pristine_rtl_e2e_smoke` uses generated mock RTL and never touches the network. `pristine_rtl_e2e_large_workspace` is the fixed validation-escape baseline: it generates a 300-file corpus, opens a realistic top file, runs `initialize -> didOpen -> documentSymbol -> systemverilog/outline -> textDocument/hover -> passive diagnostics wait -> systemverilog/moduleHierarchy -> systemverilog/schematic -> shutdown`, and fails if the Debug server exits or the trace shows deep semantic heartbeat requests during diagnostics waiting. `pristine_rtl_e2e_real_retrosoc` is the required real RTL gate: it verifies or fetches retroSoC, runs real mode, writes `summary.json`, `operations.jsonl`, protocol trace, and debug trace under the build log directory, and fails instead of skipping when the corpus cannot be prepared. `pristine_rtl_e2e_lsp_stress` remains available for probe and additional corpus workflows. Summaries include `corpusName`, `corpusRoot`, `corpusCommit`, client source-discovery time, real/probe didOpen source selection time, opened source path, LSP initialize, outline, hover, hierarchy cold/warm, schematic, optional discovery closure document/build counters, shutdown, diagnostics count, and result-size counters. LSP protocol transaction tracing is off by default for exploratory stress; enable it with `RTL_E2E_LSP_TRACE=1` or set `RTL_E2E_LSP_TRACE_FILE=<path>` to write a JSONL trace of client->server and server->client messages. Debug server phase tracing is also opt-in for exploratory stress: set `RTL_E2E_SERVER_DEBUG_TRACE=1`, optionally `RTL_E2E_SERVER_DEBUG_TRACE_FILE=<path>`, and on Windows Debug E2E set `RTL_E2E_SERVER_SUPPRESS_ABORT_DIALOG=1` to avoid a modal abort dialog while preserving trace output.

The RTL E2E diagnostics section must remain a validation-escape guard: it records whether semantic diagnostics were published, how long the wait took, and any background diagnostics skip reason, but it must not drive that wait by sending deep semantic queries. If full diagnostics times out on the required real corpus, preserve the summary and trace so the failure can be bucketed as timeout, stale/cancel, server exit, or request-order regression.

The required real corpus gate is `pristine_rtl_e2e_real_retrosoc`. For additional exploratory LSP stress runs, the configurable `pristine_rtl_e2e_lsp_stress` still defaults to `RTL_E2E_LSP_MODE=probe`, which opens a generated one-file probe module to prove the LSP data path without depending on a real top. To run an extra real RTL file outside the required gate, set `RTL_E2E_LSP_MODE=real` and preferably `RTL_E2E_TOP=<top>`:

```powershell
$env:RTL_E2E_LSP_MODE='real'
$env:RTL_E2E_TOP='<top-module>'
ctest --test-dir build/dev -R pristine_rtl_e2e_lsp_stress --output-on-failure
```

When `RTL_E2E_TOP` is omitted in exploratory real mode, the client uses the first discovered module/interface declaration only as a test driver; treat that as a data-path smoke rather than a representative design-top baseline. `RETROSOC_LSP_MODE`, `RETROSOC_TOP`, `RETROSOC_MAX_DEPTH`, `RETROSOC_LSP_LOG_DIR`, `RETROSOC_LSP_TRACE`, and `RETROSOC_LSP_TRACE_FILE` remain legacy aliases for the default retroSoC corpus only.

If `ctest` is not on `PATH` in the current Windows shell, use:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir '.\build\dev' --output-on-failure
```
```

If the change touches CLI behavior, entrypoint wiring, binary naming, or packaging expectations, also run:

```powershell
./build/dev/pristine-engine --version
```

If the change touches notice generation, install layout, packaging metadata, or dependency scope, also run:

```powershell
cmake --build --preset dev --target pristine_validate_notice
cmake --install build/dev --prefix build/install-smoke
cmake -DPRISTINE_INSTALL_PREFIX=build/install-smoke -P scripts/validate-install-tree.cmake
```

If the change touches dependency wiring in `cmake/Dependencies.cmake`, `cmake/DepsLock.cmake`, or bootstrap scripts, validate from a clean tree to catch accidental network fetches:

```powershell
Remove-Item -Recurse -Force .\build\dev
cmake --preset dev
cmake --build --preset dev
```

## Dependency And Packaging Guardrails

- Keep third-party sources pinned in `cmake/DepsLock.cmake` and bootstrapped via `scripts/bootstrap_deps.cmake`.
- Do not introduce git submodules for third-party code.
- Do not rely on implicit configure-time network access.
- `fmt` is intentionally overridden through `FETCHCONTENT_SOURCE_DIR_FMT` so that `slang` uses the locally bootstrapped copy.
- `SLANG_USE_MIMALLOC` is forced `OFF` in `cmake/Dependencies.cmake` so a clean build tree does not trigger a remote `mimalloc` fetch during configure.
- If `mimalloc` is ever re-enabled, convert it into a pinned local dependency first and update the redistributed notice scope in the same change.
- `lsp-framework` is an opt-in test/perf dependency for the retroSoC LSP stress client. It is not installed or redistributed with `pristine-engine`; if that scope changes, update attribution, license text, and NOTICE in the same change.
- `cibyr/lefdef` and `IHP-Open-PDK` are layout parser corpus dependencies. They are downloaded under `.deps` only, are not installed or redistributed with `pristine-engine`, and must not be committed. If code or fixture content is copied from either source into this repository, update attribution, license text, and NOTICE in the same change.

## Notice Workflow

Notice metadata is owned by `cmake/attributions.cmake`.

- do not hand-maintain `ATTRIBUTIONS.md` or `NOTICE` as primary sources
- update `cmake/attributions.cmake` and any referenced text files under `licenses/texts/`
- regenerate notice outputs through `scripts/generate-notice.cmake` or the `pristine_generate_notice` / `pristine_validate_notice` targets

Current redistributed scope:

- `slang`
- `fmt`
- `nlohmann/json`
- `zlib`
- `LZ4`
- `FastLZ`
- `slang-server` copied differential reference material, when present
- Boost 1.91.0 headers for layout spatial indexing
- the vendored `boost_unordered` header used by `slang` when no suitable Boost package is found

Installed notice destination:

- `share/pristine-engine/licenses`

## Naming Contract

- The CMake executable target is `pristine-engine`.
- The produced runtime binary name is `pristine-engine`.
- CLI smoke tests, install validation, and CI all expect the runtime artifact to be named `pristine-engine`.

If you change this contract, update all affected surfaces in the same change:

- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `src/main.cpp`
- `scripts/validate-install-tree.cmake`
- `.github/workflows/ci.yml`
- notice/install docs or tests that assert the runtime name

## CI Expectations

GitHub Actions CI is defined in `.github/workflows/ci.yml`.

Current hosted matrix:

- Ubuntu 22.04 x64
- Ubuntu 24.04 x64
- Windows 2022 x64
- Windows 2025 x64
- macOS 15 arm64
- macOS 15 x64
- macOS 26 arm64
- macOS 26 x64

`macos-14` is intentionally not included.

Each CI job currently does the following:

- checkout
- resolve the CMake project version before build and assert the runtime `--version` output reports that version plus branch/commit/tag/os/arch/build metadata
- bootstrap dependencies, including the IHP Open PDK LEF corpus used by the required layout parser gate
- configure `dev`
- validate notice files
- build
- run unit tests
- run the required `pristine_ihp_lef_corpus` gate after dependency bootstrap
- run `pristine-engine --version` and assert the output reports the project version plus branch/commit/tag/os/arch/build metadata
- install to a staged prefix
- validate the staged install tree
- upload the staged install artifact

Pushes of `v*` tags run the same matrix and then publish a GitHub Release. The release job downloads all `pristine-engine-*` artifacts, packages each staged install tree as `pristine-engine-<tag>-<platform>.zip`, generates `SHA256SUMS.txt`, and uploads the assets to the matching GitHub Release.

## Current Verified Local State

The current repository state has been locally verified on Windows with:

- clean `build/dev` removal and full reconfigure
- full rebuild
- notice validation
- `pristine-engine --version`
- unit tests passing
- LEF/DEF/GDS layout slice verified with `pristine_unit_tests.exe "[layout]"`, `pristine_unit_tests.exe "[server][layout]"`, and `tests/e2e/layout_pipe_smoke.py`; GDS spatial-index tile/hit/inspect/selection smoke is covered by focused unit and pipe e2e tests, and IHP Open PDK corpus validation remains required for layout/parser/spatial-index changes after bootstrapping `.deps/src/ihp-open-pdk`
- full Debug unit suite last observed passing with 3,524,503 assertions in 358 test cases

## Likely Near-Term Work

- expand complex SystemVerilog semantic views for package shadowing, typedef alias chains, structs/enums/classes, interfaces/modports, macros, generate scopes, parameterized modules, wildcard ports, and cyclic hierarchy
- keep macOS hover/type metadata stable by expanding deterministic same-range `AstIndex` lookup and port/interface type display regressions
- mature completion, signature, and inlay providers with package `::`, hierarchical `.`, ordered/named/wildcard ports, function/task argument hints, resolved type, constant value, and interface/modport cases
- expand diagnostics and code-action coverage for missing import/package/type/module/port, macro quickfixes, width/type mismatch, diagnostics publish/clear, and no-fallback regressions
- keep expanding `tests/golden/semantic` toward 430+ fixtures and unit `TEST_CASE` coverage toward 455+, especially provider-owned navigation target/implementation edge cases, true struct/class/interface/modport member completion, wildcard import completion/resolve, wildcard/parameter inlay, complex constant/type hints, macro quickfix/expand, generated hierarchy/cone and generated schematic port/net-fidelity slices, large cone truncation, diagnostics publish/clear, and cases that would fail under old syntax/text semantic paths
- add robustness/property/differential tests for malformed JSON-RPC, illegal URIs, broken includes, recoverable syntax errors, UTF-16 incremental edits, duplicate references, large-result truncation, diagnostics/typeDefinition/references/completion/workspace symbol/call hierarchy, and comparable slang-server behavior
- add affected rebuild checks, cache hit/invalidation tests, and 100/1000/5000-file perf baselines for workspace-wide queries, including signatureHelp and inlayHint cache-key coverage
- expand AST-derived typeDefinition coverage for remaining class/interface/modport edge cases, shadowed type names, and any alias-chain cases not yet promoted into positive assertions; do not regress the positive typedef alias-chain, package-qualified alias-chain, or interface modport fixtures back to unresolved debt
