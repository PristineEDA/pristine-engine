# pristine-engine Agent Notes

## Scope

This file is for coding agents working in this repository.

- Put agent-only workflow rules here, not in `README.md`, unless the user explicitly asks for end-user documentation changes.
- Keep edits narrow, preserve existing layering, and validate the smallest affected slice before widening scope.

## Product Snapshot

`pristine-engine` is a standalone SystemVerilog LSP server implemented in C++20.

Current implemented LSP surface:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`
- `textDocument/hover`
- `textDocument/definition`
- `textDocument/references`
- `workspace/symbol`
- `textDocument/completion`

Current behavior is still a thin MVP:

- parse diagnostics for open documents are implemented
- `textDocument/documentSymbol` is syntax-driven and supports top-level declarations plus common nested members
- `textDocument/hover` is declaration-oriented and built from the same syntax symbol tree
- `textDocument/definition`, `textDocument/references`, `workspace/symbol`, and `textDocument/completion` are backed by a lightweight syntax/text workspace index
- deeper semantic analysis, rename, document highlights, inlay hints, code actions, call hierarchy, and precise SystemVerilog scope resolution are not implemented yet

The server resolves the workspace root from `initialize` and loads a minimal `.slang/server.json` when present.

Recognized `.slang/server.json` fields:

- `build`
- `buildPattern`
- `buildRelativePaths`
- `flags`
- `index[].dirs`
- `index[].excludeDirs`

## Code Map

Use the nearest owning layer instead of patching around it from a wrapper.

- lifecycle, notification handling, diagnostics publishing, `documentSymbol`, `hover`, definition, references, workspace symbols, and completion session wiring: `src/server/ServerSession.cpp`
- LSP protocol structures and JSON payload parsing: `src/lsp/Protocol.cpp`
- parse pipeline, syntax symbol extraction, identifier scanning, hover content, and diagnostics: `src/analysis/CompilationService.cpp`
- lightweight workspace symbol/reference/completion index: `src/analysis/SymbolIndex.cpp`
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
- Avoid broad refactors unless the user asks for them or the local change cannot be made safely otherwise.
- Do not reintroduce agent process rules into `README.md` unless explicitly requested.

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

- The CMake executable target remains `pristine-lsp`.
- The produced runtime binary name is `pristine-engine` via `OUTPUT_NAME`.
- CLI smoke tests, install validation, and CI all expect the runtime artifact to be named `pristine-engine`.

If you change this contract, update all affected surfaces in the same change:

- `CMakeLists.txt`
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
- bootstrap dependencies
- configure `dev`
- validate notice files
- build
- run unit tests
- run `pristine-engine --version`
- install to a staged prefix
- validate the staged install tree
- upload the staged install artifact

## Current Verified Local State

The current repository state has been locally verified on Windows with:

- clean `build/dev` removal and full reconfigure
- full rebuild
- notice validation
- `pristine-engine --version`
- unit tests passing

## Likely Near-Term Work

- deepen semantic analysis beyond the current syntax-driven symbol tree
- deepen semantic analysis beyond the current syntax/text index
- extend hover beyond the current declaration-oriented slice
- add rename, document highlights, inlay hints, code actions, and call hierarchy