# pristine-engine Agent Notes

## Scope

This file is for coding agents working in this repository.

- Put agent-only workflow rules here, not in `README.md`, unless the user explicitly asks for end-user documentation changes.
- Keep edits narrow, preserve existing layering, and validate the smallest affected slice before widening scope.

## Product Snapshot

`pristine-engine` is a standalone AST symbol identity-backed SystemVerilog LSP server implemented in C++20.

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

Current behavior follows an AST-backed semantic model with syntax/text fallback kept for compatibility:

- `SemanticEngine` is the owning layer for slang AST/Compilation snapshots, AST lookup, symbol identity, dependency invalidation, semantic diagnostics, and reference indexes
- semantic queries prefer slang AST/Compilation facts when available; fallback must never override an AST-backed answer
- hover, definition, type definition, references, document highlights, prepare rename, and rename are the first AST-backed query slice owned by `SemanticEngine`
- completion, completion resolve, signature help, inlay hints, semantic tokens, and selection ranges are the second semantic query slice and should use `SemanticEngine` value-type APIs before syntax/text fallback
- `CompilationService` remains the syntax fast path for parse diagnostics, document symbols, syntax extraction, hierarchy/schematic extraction, and text utilities
- `SymbolIndex` and the legacy syntax/text `SemanticWorkspace` resolution are fallback and cold-workspace indexing aids, not the preferred semantic authority
- `ServerSession` should route LSP requests and serialize responses; it should not grow new SystemVerilog semantic rules
- some feature handlers still use syntax/text fallback while AST-backed behavior is phased in; when replacing them, keep LSP response compatibility and add focused tests

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

- lifecycle, notification handling, diagnostics publishing, LSP request routing, and response serialization: `src/server/ServerSession.cpp`
- LSP protocol structures and JSON payload parsing: `src/lsp/Protocol.cpp`
- deep semantic ownership, slang AST/Compilation snapshots, AST lookup, symbol identity, dependency invalidation, semantic diagnostics, reference index, instance graph, and cone index: `src/analysis/SemanticEngine.cpp`
- compatibility facade for semantic document state and legacy syntax/text fallback queries; it should delegate to `SemanticEngine` first: `src/analysis/SemanticWorkspace.cpp`
- shared URI/path/source-range conversion helpers for analysis code: `src/analysis/SourceUtil.cpp`
- parse pipeline, syntax symbol extraction, identifier scanning, syntax hover content, and syntax diagnostics: `src/analysis/CompilationService.cpp`
- lightweight workspace symbol/reference/completion index used for cold workspace indexing and fallback: `src/analysis/SymbolIndex.cpp`
- open-document state and UTF-16 incremental edits: `src/document/DocumentStore.cpp`
- workspace root resolution and `.slang/server.json` loading: `src/workspace/WorkspaceManager.cpp`
- stdio transport and JSON-RPC framing: `src/transport/StdioTransport.cpp`, `src/jsonrpc/MessageStream.cpp`, `src/jsonrpc/JsonRpcServer.cpp`
- process entry point, CLI switches, Windows stdio mode, and version output: `src/main.cpp`
- unit coverage for framing, lifecycle, sync, workspace, diagnostics, UTF-16, symbols, hover, and Tier 1 LSP navigation/completion: `tests/unit`
- subprocess LSP smoke coverage: `tests/e2e`

## Working Rules

- Prefer changing the layer that computes behavior, not the layer that only forwards or serializes it.
- When behavior changes, update or add the closest unit test in `tests/unit`.
- Preserve the current architecture split: transport -> jsonrpc -> lsp/workspace/document/analysis -> server.
- Do not add new SystemVerilog semantic logic to `ServerSession.cpp`; put it in `SemanticEngine` or the nearest analysis-layer helper.
- Do not use string matching as the primary semantic authority when slang AST lookup / symbol identity can answer the question.
- Do not add public APIs that expose slang AST pointers; keep snapshot-owned slang objects behind value-type analysis results.
- Do not duplicate URI/path/source-range conversion logic; use `SourceUtil` from analysis code.
- Keep `SemanticEngine` public APIs value-type based. Do not expose slang AST pointers outside snapshot-owned internals.
- All semantic behavior changes must update the nearest unit, golden-style, or e2e coverage.
- Changes that scan or rebuild workspace-wide state must include or update a performance-oriented test/benchmark plan before being considered complete.
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

Add or update focused semantic unit tests for AST-backed diagnostics, lookup, hover, definition, references, rename, completion, completion resolve, signature help, inlay hints, semantic tokens, selection ranges, hierarchy, or schematic behavior as appropriate. Golden semantic cases and subprocess LSP smoke tests should change with externally observable semantic behavior. For workspace-wide indexing or invalidation changes, include a performance baseline plan covering initialize, didOpen, didChange, hover, completion, references, rename, workspace/symbol, and moduleHierarchy on small and large synthetic workspaces.

For opt-in performance baselines, configure with `PRISTINE_BUILD_PERF_TESTS=ON` and run `pristine_perf_tests`; the perf target prints JSON and is not part of the default `ctest` suite.

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

- migrate hover, definition, references, rename, completion, inlay hints, semantic tokens, hierarchy, schematic, and backward cone from syntax/text fallback to `SemanticEngine`
- add AST-backed symbol identity tests for packages, typedefs, structs/enums, interfaces/modports, macros, generate scopes, and parameterized designs
- add query-time affected rebuilds, generation snapshots, and reference caches for large workspaces
- split large server helper blocks out of `ServerSession.cpp` once their behavior is owned by analysis-layer APIs
