# pristine-engine Agent Notes

## Scope

This file is for coding agents working in this repository.

- Put agent-only workflow rules here, not in `README.md`, unless the user explicitly asks for end-user documentation changes.
- Keep edits narrow, preserve existing layering, and validate the smallest affected slice before widening scope.

## Product Snapshot

`pristine-engine` is a standalone design snapshot + AST symbol identity-backed SystemVerilog LSP server implemented in C++20.

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
- slang AST/Compilation is the semantic fact source. Legacy syntax/text logic is migration debt only; it must not become a parallel semantic authority
- hover, definition, type definition, implementation, references, document highlights, prepare rename, and rename are the first AST-backed query slice owned by `SemanticEngine`
- completion, completion resolve, signature help, diagnostics, inlay hints, semantic tokens, selection ranges, and workspace symbols use `SemanticEngine` value-type APIs; completion context detection, item assembly, resolve docs/snippets, and completion-specific metadata are owned by `CompletionProvider`; diagnostics aggregation is owned by `DiagnosticProvider`; code-action quickfix selection is owned by `CodeActionProvider`; signature help and inlay hint assembly are owned by `SignatureInlayProvider`; semantic token and selection range assembly are owned by `NavigationProvider`; remaining context gaps belong in analysis instead of `ServerSession`
- HDL module hierarchy, schematic, call hierarchy, and backward cone traversal are owned by `DesignGraphProvider` behind `SemanticEngine` value-type APIs; schematic module/cell/port facts and cone driver/load facts must come from AST-derived module signatures, assignment edge views, and design graph views, not syntax schematic or identifier extraction
- code actions for unresolved include/module/type and missing port connections are produced through `CodeActionProvider` behind `SemanticEngine` value-type APIs; `ServerSession` only serializes them to LSP JSON
- `CompilationService` remains the syntax fast path for parse diagnostics, document symbols, document links/folding, and non-semantic text utilities; it must not become a visible semantic answer source or a provider query-time source for diagnostics, code actions, completion, signature, graph, schematic, or cone facts
- snapshot construction, SourceManager/SyntaxTree/Compilation ownership, slang diagnostic collection, and include/import/package/module reverse-edge output are owned by `SnapshotBuilder`; `SemanticEngine::rebuildSnapshot` should only assemble build input, receive build output, update generation-scoped state, and clear dirty/cache flags
- AST symbol identity, declaration/reference indexing, module instance binding, module/interface signature views, include/import/macro/type/member/module/interface/package/class indexes, declared type reference views, function/task argument views, port/param binding views, AST-derived assignment driver/load edge views, workspace symbol generation, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views are owned by `AstIndex` behind `SemanticEngine` value-type APIs; repeated visible query results are cached by the generation-keyed query cache; do not add a second analysis fallback index
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
- split-out internal analysis providers for the large engine implementation: `src/analysis/semantic/SnapshotBuilder.*` for SourceManager/SyntaxTree/Compilation construction, snapshot data ownership, slang diagnostics, snapshot utilities, and include/import/module dependency edges; `src/analysis/semantic/AstIndex.*` for AST symbol identity, declaration/reference index construction, module instance binding, AST-derived module/interface signatures and schematic facts, include/import/macro/type/member/function/task/port/param/assignment semantic views, declared type reference views, AST-derived assignment edge views, workspace symbol query assembly, and provider-facing symbol/reference/module/signature/diagnostic/code-action/design-graph views; `src/analysis/semantic/NavigationProvider.*` for semantic token and selection range query assembly, `src/analysis/semantic/CompletionProvider.*` for completion context detection, item construction, resolve docs/snippets/data, filtering, and completion-specific query helpers, `src/analysis/semantic/SignatureInlayProvider.*` for signature help and inlay hint query assembly, `src/analysis/semantic/DesignGraphProvider.*` for module hierarchy, schematic, call hierarchy, and backward cone query assembly from `AstIndex` / design graph views only, `src/analysis/semantic/CodeActionProvider.*` for quickfix selection and edit/create-file assembly from indexed diagnostics/code-action facts, `src/analysis/semantic/DiagnosticProvider.*` for slang + UX diagnostic aggregation/sort/dedupe from indexed diagnostic facts, and `src/analysis/semantic/QueryCache.*`
- compatibility facade and document-state adapter; it should delegate migrated capabilities directly to `SemanticEngine` and should not provide diagnostics, symbol resolution, or LSP-visible fallback find paths: `src/analysis/SemanticWorkspace.cpp`
- shared URI/path/source-range/UTF-16 offset conversion helpers for analysis code: `src/analysis/SourceUtil.cpp`
- parse pipeline, syntax symbol extraction, identifier scanning, syntax hover content, and syntax diagnostics: `src/analysis/CompilationService.cpp`
- open-document state and UTF-16 incremental edits: `src/document/DocumentStore.cpp`
- workspace root resolution and `.slang/server.json` loading: `src/workspace/WorkspaceManager.cpp`
- stdio transport and JSON-RPC framing: `src/transport/StdioTransport.cpp`, `src/jsonrpc/MessageStream.cpp`, `src/jsonrpc/JsonRpcServer.cpp`
- process entry point, CLI switches, Windows stdio mode, and version output: `src/main.cpp`
- unit coverage for framing, lifecycle, sync, workspace, diagnostics, UTF-16, symbols, hover, AST identity, navigation/completion, design hierarchy, schematic, call hierarchy, and backward cone: `tests/unit`
- subprocess LSP smoke coverage for core LSP plus `systemverilog/*`: `tests/e2e`
- opt-in semantic performance baselines for 100/1000/5000-file workspaces: `tests/perf`

## Working Rules

- Prefer changing the layer that computes behavior, not the layer that only forwards or serializes it.
- When behavior changes, update or add the closest unit test in `tests/unit`.
- Preserve the current architecture split: transport -> jsonrpc -> lsp/workspace/document/analysis -> server.
- Do not add new SystemVerilog semantic logic to `ServerSession.cpp`; put it in `SemanticEngine` or the nearest analysis-layer helper.
- Do not use string matching as the primary semantic authority when slang AST lookup / symbol identity can answer the question.
- If AST/Compilation facts conflict with legacy syntax/text fallback, delete or demote the legacy path instead of keeping two primary answers.
- Do not add a new analysis fallback index or `SemanticWorkspace::find*` as an LSP fallback path; visible requests should consume `SemanticEngine` value-type results.
- Snapshot construction belongs in `SnapshotBuilder`; `SemanticEngine::rebuildSnapshot` may prepare `SnapshotBuildInput`, accept `SnapshotBuildOutput`, and update mutable snapshot/cache state, but should not grow SourceManager/SyntaxTree/Compilation construction, slang diagnostic aggregation, or dependency-edge building branches.
- AST index construction belongs in `AstIndex`; `SemanticEngine` may request an `AstIndexView` and cache results, but should not grow new symbol identity generation, declaration/reference indexing, module instance binding, workspace symbol filtering, kind-mapping, or per-provider symbol/reference mapping branches.
- Provider contexts for completion, signature/inlay, diagnostics, code actions, workspace symbols, hierarchy, schematic, call hierarchy, and backward cone should be populated from `AstIndexView` or other value-type provider views, not by each query rereading `SnapshotData` semantic fields directly.
- Do not add query-time `CompilationService` identifier/module/port scans for graph, cone, completion, signature, diagnostics, or code-action answers; promote the needed fact into `SnapshotBuilder` / `AstIndex` first.
- `DesignGraphProvider` must consume AST/design graph views from `AstIndex` for module/interface signatures, instance cells, schematic ports, hierarchy, call hierarchy, and cone inputs; do not reintroduce `CompilationService::moduleSchematics`, syntax schematic maps, `assignments_by_uri`, or `identifiers_by_uri` as visible graph/cone sources.
- `DiagnosticProvider` and `CodeActionProvider` must consume indexed include/import/module/type/member/assignment facts from provider contexts; do not add request-time `CompilationService` scans in those providers.
- Do not add diagnostics or symbol-resolution rules to `SemanticWorkspace`; keep those rules in `SemanticEngine` or a `src/analysis/semantic/*` helper owned by it.
- Completion item `data` must be generated by the analysis layer and resolved through the `CompletionProvider` query path behind `SemanticEngine::resolveCompletion`; do not infer completion docs, snippets, ports, or macro bodies in `ServerSession`.
- Completion context detection for package `::`, hierarchical `.`, macro, and module-instantiation positions plus completion item/doc/snippet construction and resolve behavior belongs in `CompletionProvider`; do not duplicate that logic in `SemanticEngine` or `ServerSession`.
- Signature help and inlay hint assembly belongs in `SignatureInlayProvider`; `SemanticEngine` may prepare value-type snapshot context, but should not grow new signature/inlay semantic branches.
- Semantic token and selection range assembly belongs in `NavigationProvider`; `SemanticEngine` may prepare value-type navigation context, but should not grow new token/selection semantic branches.
- Module hierarchy, schematic, call hierarchy, and backward cone assembly belongs in `DesignGraphProvider`; `SemanticEngine` may prepare value-type design graph context and cache results, but should not grow new HDL graph semantic branches.
- Diagnostics aggregation belongs in `DiagnosticProvider`; `SemanticEngine` may prepare value-type diagnostic context and cache results, but should not grow new UX diagnostic branches.
- Code-action quickfix selection belongs in `CodeActionProvider`; `SemanticEngine` may prepare value-type code-action context and cache results, but should not grow new quickfix semantic branches.
- Diagnostics behavior changes must be covered in both the nearest engine/unit test and an LSP-facing smoke or golden-style test when the published shape changes.
- Do not add public APIs that expose slang AST pointers; keep snapshot-owned slang objects behind value-type analysis results.
- Do not duplicate URI/path/source-range conversion logic; use `SourceUtil` from analysis code.
- Keep `SemanticEngine` public APIs value-type based, with generation/messages/unresolved/partial/truncated metadata where a query can be incomplete, including while splitting implementation into `src/analysis/semantic/*` helpers.
- Provider splits must not change `SemanticEngine` public request/response contracts unless the user explicitly asks for an API break.
- Cache keys for visible semantic queries must include snapshot generation and all user-visible inputs; mutation/configuration paths must invalidate affected cached results. This applies to diagnostics, references, rename, completion, workspace/symbol, hierarchy, schematic, backward cone, and codeAction cache entries.
- All semantic behavior changes must update the nearest unit, golden-style, or e2e coverage.
- Changes that scan, cache, or rebuild workspace-wide state must include or update a performance-oriented test/benchmark plan or JSON baseline before being considered complete.
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

Add or update focused semantic unit tests for AST-backed diagnostics, lookup, hover, definition, references, rename, completion, completion resolve, signature help, inlay hints, semantic tokens, selection ranges, module hierarchy, call hierarchy, schematic, backward cone, and code action behavior as appropriate. Golden semantic cases live under `tests/golden/semantic` as JSON fixtures and subprocess LSP smoke tests should change with externally observable semantic behavior, including semantic diagnostics publication, completion resolve data, unresolved, partial, truncated, bad syntax recovery, missing include, broken build config, cyclic hierarchy, large result caps, and no-fallback regression shapes when relevant. When migrating graph/schematic facts, include an AST-derived schematic/cone case that would fail if syntax schematic extraction were used. After splitting `SemanticEngine` helpers, run the nearest semantic unit tests plus the LSP e2e smoke that exercises the affected provider. For workspace-wide indexing, query cache, or invalidation changes, include a performance baseline plan covering initialize, didOpen, didChange, didSave, diagnostics, completion, resolveCompletion, signatureHelp, inlayHint, semanticTokens, references, rename, workspace/symbol, moduleHierarchy, schematic, backwardCone, and codeAction on small and large synthetic workspaces.

For opt-in performance baselines, configure with `PRISTINE_BUILD_PERF_TESTS=ON` and run `pristine_perf_tests`; the perf target prints JSON for 100/1000/5000-file synthetic workspaces and is not part of the default `ctest` suite.

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

- continue splitting `SemanticEngine.cpp` into `src/analysis/semantic/*` providers without changing value-type public APIs or exposing slang AST pointers
- replace remaining legacy syntax/text semantic migration debt, especially metadata-derived diagnostic facts, with AST/Compilation-backed provider views instead of adding fallback paths
- add AST-backed symbol identity tests for packages, typedefs, structs/enums, interfaces/modports, classes, macros, generate scopes, parameterized modules, wildcard ports, and cyclic hierarchy
- add robustness/property/differential tests for malformed JSON-RPC, illegal URIs, broken includes, recoverable syntax errors, UTF-16 incremental edits, duplicate references, and large-result truncation
- add query-time affected rebuilds, generation snapshots, reference/hierarchy/cone caches, and perf baselines for large workspaces
- split large server helper blocks out of `ServerSession.cpp` once their behavior is owned by analysis-layer APIs
