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
- `systemverilog/moduleHierarchy`
- `systemverilog/schematic`
- `systemverilog/backwardCone`

Current behavior follows a split-provider SemanticEngine single-fact-source model:

- `SemanticEngine` is the public value-type facade and coordinator for document state, generation, invalidation, and query cache; split providers own snapshot construction, AST indexing, and feature-specific query assembly
- visible semantic facts are exclusively backed by slang AST/Compilation, `AstIndex` views, and design graph provider data; syntax/text utilities must not become semantic answer sources
- `WorkspaceDiscoveryIndex` provides a lightweight initialize/warm-path discovery index for module/interface/package/macro/file candidates, top-candidate hints, include plus package import/export dependency-closure hints, closure metrics, and timing counters; `SemanticEngine` caches only the value-type discovery snapshot with a generation/config/file-set cache key, including `.slang/server.json` `index[].dirs` / `excludeDirs` filtering, so it may guide closure-limited hierarchy/schematic deep builds but must not become the final semantic authority for hover, definition, hierarchy, schematic, cone, diagnostics, or code actions. Hierarchy/schematic may expose optional `discoveryClosure*` telemetry such as root, candidate/selected/missing/deduped document counts, build/query timing, and cache hit state, but those fields are perf/debug data only
- current validation scale is 254+ focused unit `TEST_CASE`s and 277+ JSON semantic golden fixtures; the next maturity target is 260+ unit `TEST_CASE`s and 320+ golden fixtures focused on true complex-SV provider behavior plus fixture-driven differential, cache, affected-rebuild, and perf coverage rather than debt-only fixtures
- Pristine integration readiness is documented in `docs/pristine-integration.md`; `systemverilog/moduleHierarchy` and `systemverilog/schematic` are Pristine-consumable custom requests for hierarchy tree and schematic canvas integration, with `roots/messages/unresolved/partial/truncated` and `rootModuleId/modules/messages/unresolved/partial/truncated` as the externally visible contract
- retroSoC LSP stress is the real language-service closure path for Pristine hierarchy/schematic integration: it uses an opt-in lsp-framework client to drive `pristine-engine --stdio`, send `didOpen`, `systemverilog/moduleHierarchy`, and `systemverilog/schematic`, and record operation timings/logs; it supports `probe` mode for a synthetic one-file data-path smoke, `real` mode for opening an actual retroSoC RTL file, client-side source-discovery/open-file-selection timing, optional discovery-closure root/candidate/selected/missing/deduped/build/query/cache-hit telemetry from hierarchy/schematic responses, and optional JSONL protocol tracing
- current deepening work is complex SystemVerilog provider completeness plus validation scale-out: package import/export and wildcard import resolution, typedef alias chains, AST-backed parameter override inlay labels, struct/enum/class/interface/modport members, class property/method member completion facts, array-of-struct member completion facts, interface instance member completion facts, modport-restricted interface port member completion facts, typed member completion resolve documentation, nested function/task signature help, function/nested-call selection ranges, interface/modport schematic graph endpoints, interface modport instance type inlay labels, parameterized cross-module backward cone, parameterized width-assignment backward cone, generated scopes, parameterized instances, port/param bindings, assignment width/type facts, macro define quickfixes, package-export discovery closure and closure metrics, watched-file/document add/change/delete affected rebuild coverage for workspace symbols and Pristine-facing hierarchy/schematic custom requests, config-filter invalidation coverage, unresolved hierarchy/schematic message dedupe, hierarchy subtree memoization, fixture-driven differential coverage, query-cache/affected-rebuild baselines, slang-server v0.2.5 attribution coverage, and clang/GCC-oriented non-Windows validation. Typedef alias-chain, package-qualified alias-chain, interface modport port typeDefinition, parameter override inlay labels, array-of-struct member completion, class member completion, interface instance member completion, modport-restricted interface port member completion, typed member completion resolve docs, nested function/task signature help, function/nested-call selection ranges, interface/modport schematic net endpoints, interface modport instance type inlay labels, parameterized cross-module backward cone, parameterized width-assignment backward cone, and macro define code actions are now positive AstIndex/provider/golden assertions and should stay AST-backed
- recent debt cleanup renamed the already-positive parameterized backward cone, interface outgoing call hierarchy, array-of-struct member completion, interface member completion resolve, parameter override inlay, interface modport typeDefinition, package-qualified alias-chain typeDefinition, and macro define code-action golden fixtures to non-debt filenames; nested function, multi-argument task signature-help, parameterized width-assignment backward cone, and function/nested-call selection range fixtures are now positive AST-backed assertions; the remaining debt-named fixture is the second-instance missing-port code action; there are no remaining `*debt*.json` semantic golden filenames, so future debt fixtures must use explicit `unresolved` / `partial` / `messages` expectations and be converted when the capability lands
- `SnapshotBuilder` preloads open/in-memory documents into the shared slang `SourceManager` before constructing syntax trees, so an opened include/header can also be included by another closure document without duplicate-path buffer assignment on Linux/macOS; keep this regression covered when changing snapshot construction
- visible semantic lookups must be deterministic across macOS, Linux, and Windows; same-range AST symbols use explicit `AstIndex` tie-breaks instead of unordered traversal, pointer, or platform-specific container order
- hover, definition, type definition, implementation, references, document highlights, prepare rename, and rename are the first AST-backed query slice owned by `SemanticEngine`
- completion, completion resolve, signature help, diagnostics, inlay hints, semantic tokens, selection ranges, and workspace symbols use `SemanticEngine` value-type APIs; completion context detection, member qualifier parsing for array/hierarchical access, connected named-port exclusion, item assembly, resolve docs/snippets, and completion-specific metadata are owned by `CompletionProvider`; diagnostics aggregation is owned by `DiagnosticProvider` from AstIndex diagnostic facts; code-action quickfix selection is owned by `CodeActionProvider` from diagnostics plus indexed facts; signature help plus module/type/port/function/task inlay hint assembly are owned by `SignatureInlayProvider`; semantic token and selection range assembly are owned by `NavigationProvider`; remaining context gaps belong in analysis instead of `ServerSession`
- macro-function signature help and AST-backed function/task signature help plus argument inlay hints are owned by `SignatureInlayProvider` and must consume `AstIndex` / snapshot macro and call views; do not reimplement argument parsing, active-parameter calculation, or argument hint placement in `ServerSession`
- HDL module hierarchy, schematic, call hierarchy, and backward cone traversal are owned by `DesignGraphProvider` behind `SemanticEngine` value-type APIs; schematic module/cell/port/instance facts and cone driver/load facts must come from AST-derived module signatures, module instance bindings, assignment edge views, and design graph views, not syntax module models, syntax schematic, or identifier extraction
- backward cone traversal, stable sorting, dedupe, result caps, `partial/truncated/messages`, and large-result behavior are provider-owned `DesignGraphProvider` semantics; `SemanticEngine` should only prepare context/cache and return the value-type result
- code actions for unresolved include/module/type and missing port connections are produced through `CodeActionProvider` behind `SemanticEngine` value-type APIs; `ServerSession` only serializes them to LSP JSON
- `CompilationService` remains the syntax fast path for parse diagnostics, document symbols, document links/folding, include/macro/package lexical utilities, syntax hover, and completion-prefix text utility; it must not produce LSP-visible semantic facts or become a provider query-time source for diagnostics, code actions, completion, signature, graph, schematic, cone, module signature, or module instance facts
- snapshot construction, SourceManager/SyntaxTree/Compilation ownership, slang diagnostic collection, and include/import/package/module reverse-edge output are owned by `SnapshotBuilder`; `SemanticEngine::rebuildSnapshot` should only assemble build input, receive build output, update generation-scoped state, and clear dirty/cache flags
- AST symbol identity, declaration/reference indexing, deterministic same-range symbol tie-breaks, declared/port/interface type display, module instance binding, module/interface signature views, function/task call signature views, module map and module URI ownership, include/import/macro/type/member/module/interface/package/class indexes, declared type reference views, function/task argument views, port/param binding views, AST-derived instance connection views, AST-derived assignment driver/load edge views, assignment width/type facts, workspace symbol generation, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views are owned by `AstIndex` behind `SemanticEngine` value-type APIs; repeated visible query results are cached by the generation-keyed query cache; do not add a second analysis fallback index
- declared type reference precision, including package-qualified references such as `pkg::type_t`, is owned by `AstIndex`; range narrowing may use slang source ranges plus syntax token/name ranges from the indexed declaration, but must not reintroduce query-time identifier scans or whole-text matching as semantic truth
- `SemanticWorkspace` is a facade and document-state adapter around `SemanticEngine`; do not add diagnostics, symbol-resolution, or LSP-visible semantic query rules there
- `ServerSession` should route LSP requests and serialize responses; it should not grow new SystemVerilog semantic rules

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

- lifecycle, notification handling, diagnostics publishing, LSP request routing, and response serialization: `src/server/ServerSession.cpp`. This file must not own navigation, completion, completion resolve, signature, diagnostics, workspace symbol, hierarchy, schematic, cone, or code action semantic rules
- LSP protocol structures and JSON payload parsing: `src/lsp/Protocol.cpp`
- public value-type query facade, document-state coordination, generation, invalidation, and cache orchestration: `src/analysis/SemanticEngine.cpp`
- split-out internal analysis providers for the large engine implementation: `src/analysis/semantic/SnapshotBuilder.*` for SourceManager/SyntaxTree/Compilation construction, snapshot data ownership, slang diagnostics, snapshot utilities, and include/import/module dependency edges; `src/analysis/semantic/WorkspaceDiscoveryIndex.*` for lightweight candidate discovery and deterministic module/interface/package/macro/file lookup hints only; `src/analysis/semantic/AstIndex.*` for AST symbol identity, declaration/reference index construction, deterministic lookup tie-breaks, declared/port/interface type display, reference range ownership, module map/URI ownership, module instance binding, AST-derived module/interface signatures and schematic facts, AST-derived function/task call signatures, include/import/macro/type/member/function/task/port/param/assignment semantic views, declared type reference views, AST-derived assignment edge and width/type facts, workspace symbol query assembly, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views; `src/analysis/semantic/NavigationProvider.*` for semantic token and selection range query assembly, `src/analysis/semantic/CompletionProvider.*` for completion context detection, item construction, resolve docs/snippets/data, filtering, and completion-specific query helpers, `src/analysis/semantic/SignatureInlayProvider.*` for signature help plus module/type/port/function/task inlay hint query assembly from AstIndex views, `src/analysis/semantic/DesignGraphProvider.*` for module hierarchy, schematic, call hierarchy, and backward cone query assembly from `AstIndex` / design graph views only, `src/analysis/semantic/CodeActionProvider.*` for quickfix selection and edit/create-file assembly from indexed diagnostics/code-action facts, `src/analysis/semantic/DiagnosticProvider.*` for slang + UX diagnostic aggregation/sort/dedupe from indexed diagnostic facts, and `src/analysis/semantic/QueryCache.*`
- compatibility facade and document-state adapter; it should delegate migrated capabilities directly to `SemanticEngine` and should not provide diagnostics, symbol resolution, or LSP-visible fallback find paths: `src/analysis/SemanticWorkspace.cpp`
- shared URI/path/source-range/UTF-16 offset conversion helpers for analysis code: `src/analysis/SourceUtil.cpp`
- parse pipeline, syntax document-symbol extraction, document link/folding helpers, include/macro/package import/export lexical utilities, syntax hover content, completion-prefix text utility, and syntax diagnostics: `src/analysis/CompilationService.cpp`
- open-document state and UTF-16 incremental edits: `src/document/DocumentStore.cpp`
- workspace root resolution and `.slang/server.json` loading: `src/workspace/WorkspaceManager.cpp`
- stdio transport and JSON-RPC framing: `src/transport/StdioTransport.cpp`, `src/jsonrpc/MessageStream.cpp`, `src/jsonrpc/JsonRpcServer.cpp`
- process entry point, CLI switches, Windows stdio mode, and version output: `src/main.cpp`
- unit coverage for framing, lifecycle, sync, workspace, diagnostics, UTF-16, symbols, hover, AST identity, navigation/completion, design hierarchy, schematic, call hierarchy, backward cone, and JSON semantic golden request shapes: `tests/unit`
- JSON semantic golden fixtures for observable provider behavior, currently 260+ fixtures covering hover, definition/typeDefinition, references, rename, completion/resolve, signatureHelp, inlayHint, semanticTokens, selectionRange, diagnostics, codeAction, workspace/symbol, moduleHierarchy, schematic, backwardCone, and callHierarchy: `tests/golden/semantic`
- subprocess LSP smoke coverage for core LSP plus `systemverilog/*`: `tests/e2e`
- opt-in fixture-driven differential coverage against a local `slang-server v0.2.5` checkout: `tests/differential`; `pristine_differential_slang_server` skips unless `SLANG_SERVER_ROOT` points at a local slang-server tree with a built binary, and rewritten fixtures should compare only capabilities exposed by both servers unless the fixture is explicitly pristine-only
- opt-in semantic performance baselines for 100/1000/5000-file workspaces: `tests/perf`

## Working Rules

- Prefer changing the layer that computes behavior, not the layer that only forwards or serializes it.
- When behavior changes, update or add the closest unit test in `tests/unit`.
- Preserve the current architecture split: transport -> jsonrpc -> lsp/workspace/document/analysis -> server.
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
- Completion item `data` must be generated by the analysis layer and resolved through the `CompletionProvider` query path behind `SemanticEngine::resolveCompletion`; do not infer completion docs, snippets, ports, or macro bodies in `ServerSession`.
- Completion context detection for package `::`, hierarchical `.`, macro, and module-instantiation positions plus completion item/doc/snippet construction and resolve behavior belongs in `CompletionProvider`; do not duplicate that logic in `SemanticEngine` or `ServerSession`.
- CompletionProvider-owned member qualifier parsing must handle array element and hierarchical selectors, and module-port completion must use provider-owned connected named-port exclusion instead of ad hoc scans in `SemanticEngine` or `ServerSession`.
- If member completion has both port-list and general member interpretations, first consume indexed module instance context when the cursor is inside an indexed instance range, then fall back to `AstIndex` member/symbol views; do not mark the query unresolved merely because the current file has no module instances.
- Signature help and inlay hint assembly belongs in `SignatureInlayProvider`; `SemanticEngine` may prepare value-type snapshot context, but should not grow new signature/inlay semantic branches.
- Function/task argument inlay hints must consume `AstIndex` call signature views. Text scanning may locate argument insertion offsets within the indexed call range, but it must not determine the callee, parameter list, or semantic type.
- Semantic token and selection range assembly belongs in `NavigationProvider`; `SemanticEngine` may prepare value-type navigation context, but should not grow new token/selection semantic branches.
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
- Differential fixtures should be fixture-driven and rewritten for this repository by default. If a slang-server MIT fixture is copied instead of rewritten, update notice/attribution in the same change.
- The local `slang-server v0.2.5` checkout is covered in the attribution/notice pipeline as a differential reference. Keep `cmake/attributions.cmake`, `licenses/texts/`, `ATTRIBUTIONS.md`, and `NOTICE` in sync whenever copied upstream fixture material changes.
- Changes that scan, cache, or rebuild workspace-wide state must include or update a performance-oriented test/benchmark plan or JSON baseline before being considered complete.
- Real-design stress tests such as retroSoC must stay opt-in and must not vendor RTL. The LSP stress path may download retroSoC into `.cache/retrosoc` only when perf tests are explicitly built and the retroSoC stress test is explicitly run; user-provided `RETROSOC_ROOT` must be verified but not auto-checked-out or modified. `RETROSOC_LSP_MODE=probe` is the quick synthetic one-file data-path smoke, while `RETROSOC_LSP_MODE=real` opens an actual RTL file and should be used for real hierarchy/schematic timing. Update notice/attribution before copying any external fixture into this repository.
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

For optional local slang-server comparison, set `SLANG_SERVER_ROOT` to the local `slang-server v0.2.5` checkout with a built `slang-server` binary, then run:

```powershell
ctest --test-dir build\dev -L differential --output-on-failure
```

The differential runner should remain fixture-driven and skip cleanly when `SLANG_SERVER_ROOT` or the binary is unavailable. Use the `$maksyuki-test` style for validation reporting: record the smallest reproducer, exact command, failure bucket, and whether a broader suite was rerun after the targeted slice.

For macOS/Linux CI-risk semantic changes, include a no-fallback regression for deterministic `AstIndex` lookup and hover/type metadata, especially same-range port/internal symbols such as `input logic [WIDTH-1:0] data`. On Windows, run the clang-oriented build and tests when the toolchain is available:

```powershell
cmake --preset clang-cl
cmake --build --preset clang-cl
ctest --test-dir build\clang-cl --output-on-failure
```

GitHub CI also runs an explicit Ubuntu clang++ configure/build/test gate:

```powershell
cmake -S . -B build/clang-linux -G Ninja -D CMAKE_BUILD_TYPE=Debug -D CMAKE_CXX_COMPILER=clang++ -D PRISTINE_BUILD_TESTS=ON
cmake --build build/clang-linux
ctest --test-dir build/clang-linux --output-on-failure
```

If a WSL, Linux, container, or native clang++ environment is available, add an equivalent non-MSVC ABI configure/build/test pass and record the result in the handoff. Use this specifically to catch aggregate initialization, unused/static helper, narrowing conversion, case-sensitive include, filesystem path, and CRLF/LF issues that MSVC may miss.

For opt-in performance baselines, configure with `PRISTINE_BUILD_PERF_TESTS=ON` and run `pristine_perf_tests`; the perf target prints JSON for 100/1000/5000-file synthetic workspaces and is not part of the default `ctest` suite. The env-gated retroSoC stress CTests can be used for real-design hierarchy/schematic timing without vendoring RTL:

```powershell
$env:RETROSOC_ROOT='C:\path\to\retroSoC'
$env:RETROSOC_EXPECTED_COMMIT='76651fd'
ctest --test-dir build/dev -L retrosoc --output-on-failure
```

When `RETROSOC_ROOT` is unset, the retroSoC wrappers clone/fetch into `.cache/retrosoc/retroSoC`; keep this behind explicit perf builds/runs so default CI does not depend on that network path.

The direct stress output is JSON with file/byte counts, parse/index timing, hierarchy timing, schematic timing, result counts, and partial/truncated/message counters. For LSP-level retroSoC pressure, first bootstrap the opt-in test dependency:

```powershell
cmake -DPRISTINE_COMPONENTS=lsp_framework -P scripts/bootstrap_deps.cmake
cmake --preset dev -DPRISTINE_BUILD_PERF_TESTS=ON
cmake --build --preset dev
ctest --test-dir build/dev -L lsp --output-on-failure
ctest --test-dir build/dev -R pristine_retrosoc_lsp_stress --output-on-failure
```

`pristine_lsp_framework_client_smoke` uses generated mock RTL and never touches the network. `pristine_retrosoc_lsp_stress` verifies `RETROSOC_ROOT` when provided, or prepares retroSoC at commit `76651fd` under `.cache/retrosoc`; it writes `summary.json` and `operations.jsonl` under the build log directory or `RETROSOC_LSP_LOG_DIR`. The summary includes client source-discovery time, real/probe didOpen source selection time, opened source path, LSP initialize, hierarchy cold/warm, schematic, optional discovery closure document/build counters, shutdown, and result-size counters. LSP protocol transaction tracing is off by default; enable it with `RETROSOC_LSP_TRACE=1` or set `RETROSOC_LSP_TRACE_FILE=<path>` to write a JSONL trace of client->server and server->client messages.

By default the LSP stress uses `RETROSOC_LSP_MODE=probe`, which opens a generated one-file probe module to prove the LSP data path without depending on a real top. To time a real retroSoC RTL file, set `RETROSOC_LSP_MODE=real` and preferably `RETROSOC_TOP=<top>`:

```powershell
$env:RETROSOC_LSP_MODE='real'
$env:RETROSOC_TOP='<top-module>'
ctest --test-dir build/dev -R pristine_retrosoc_lsp_stress --output-on-failure
```

When `RETROSOC_TOP` is omitted in real mode, the client uses the first discovered module/interface declaration only as a test driver; treat that as a data-path smoke rather than a representative design-top baseline.

If `ctest` is not on `PATH` in the current Windows shell, use:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir '.\build\dev' --output-on-failure
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

## Notice Workflow

Notice metadata is owned by `cmake/attributions.cmake`.

- do not hand-maintain `ATTRIBUTIONS.md` or `NOTICE` as primary sources
- update `cmake/attributions.cmake` and any referenced text files under `licenses/texts/`
- regenerate notice outputs through `scripts/generate-notice.cmake` or the `pristine_generate_notice` / `pristine_validate_notice` targets

Current redistributed scope:

- `slang`
- `fmt`
- `nlohmann/json`
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
- resolve the CMake project version before build and assert the runtime `--version` output matches it
- bootstrap dependencies
- configure `dev`
- validate notice files
- build
- run unit tests
- run `pristine-engine --version` and assert the output matches the project version
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

## Likely Near-Term Work

- expand complex SystemVerilog semantic views for package shadowing, typedef alias chains, structs/enums/classes, interfaces/modports, macros, generate scopes, parameterized modules, wildcard ports, and cyclic hierarchy
- keep macOS hover/type metadata stable by expanding deterministic same-range `AstIndex` lookup and port/interface type display regressions
- mature completion, signature, and inlay providers with package `::`, hierarchical `.`, ordered/named/wildcard ports, function/task argument hints, resolved type, constant value, and interface/modport cases
- expand diagnostics and code-action coverage for missing import/package/type/module/port, macro quickfixes, width/type mismatch, diagnostics publish/clear, and no-fallback regressions
- keep expanding `tests/golden/semantic` toward 320+ fixtures and unit `TEST_CASE` coverage toward 260+, especially true struct/class/interface/modport member completion, wildcard import completion/resolve, wildcard/parameter inlay, macro quickfix, generated hierarchy/schematic cells, large cone truncation, diagnostics publish/clear, and cases that would fail under old syntax/text semantic paths
- add robustness/property/differential tests for malformed JSON-RPC, illegal URIs, broken includes, recoverable syntax errors, UTF-16 incremental edits, duplicate references, large-result truncation, diagnostics/typeDefinition/references/completion/workspace symbol/call hierarchy, and comparable slang-server behavior
- add affected rebuild checks, cache hit/invalidation tests, and 100/1000/5000-file perf baselines for workspace-wide queries, including signatureHelp and inlayHint cache-key coverage
- expand AST-derived typeDefinition coverage for remaining class/interface/modport edge cases, shadowed type names, and any alias-chain cases not yet promoted into positive assertions; do not regress the positive typedef alias-chain, package-qualified alias-chain, or interface modport fixtures back to unresolved debt
