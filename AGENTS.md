# pristine-engine Agent Notes

## Project Summary

`pristine-engine` is a standalone SystemVerilog LSP server implemented in C++20.

The repository currently contains an executable MVP with the first `slang`-backed language slice. The server boots as a stdio process, implements JSON-RPC framing, supports the base LSP lifecycle, synchronizes open documents, publishes parse diagnostics, answers syntax-driven `textDocument/documentSymbol`, and serves a minimal declaration-oriented `textDocument/hover`.

Implemented LSP methods today:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`
- `textDocument/hover`

Current non-goals / missing pieces:

- no cross-file navigation yet
- no `definition`, `references`, `workspace/symbol`, or completion yet
- current symbol and hover behavior is still syntax-oriented rather than deep semantic analysis

The workspace root is resolved from `initialize`, and the server loads a minimal `.slang/server.json` when present.

Recognized `.slang/server.json` fields:

- `build`
- `buildPattern`
- `buildRelativePaths`
- `flags`
- `index[].dirs`
- `index[].excludeDirs`

## Repository Layout

- `CMakeLists.txt`: root build, tests, install, and notice target wiring
- `CMakePresets.json`: development configure/build presets
- `cmake/Dependencies.cmake`: local third-party wiring and `slang` integration
- `cmake/DepsLock.cmake`: pinned third-party archive URLs and hashes
- `cmake/attributions.cmake`: canonical third-party attribution source
- `scripts/bootstrap_deps.cmake`: bootstraps pinned archives into `.deps/`
- `scripts/generate-notice.cmake`: regenerates checked-in `ATTRIBUTIONS.md` and `NOTICE`
- `scripts/validate-install-tree.cmake`: validates staged install layout
- `src/transport`: stdio transport
- `src/jsonrpc`: `Content-Length` framing and JSON-RPC dispatch
- `src/lsp`: LSP protocol structures and parsing
- `src/analysis`: thin `slang` compilation and syntax-analysis seam
- `src/document`: in-memory open document store and incremental UTF-16 edits
- `src/workspace`: workspace root resolution and `.slang/server.json` loading
- `src/server`: session wiring for lifecycle, sync, diagnostics, `documentSymbol`, and `hover`
- `tests/unit`: framing, lifecycle, text sync, workspace/config, diagnostics, UTF-16, symbol, and hover tests

## Build And Validation

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

Run tests:

```powershell
ctest --test-dir build/dev --output-on-failure
```

Version smoke test:

```powershell
./build/dev/pristine-engine --version
```

Validate generated notice files from the build graph:

```powershell
cmake --build --preset dev --target pristine_validate_notice
```

Stage and validate the install tree:

```powershell
cmake --install build/dev --prefix build/install-smoke
cmake -DPRISTINE_INSTALL_PREFIX=build/install-smoke -P scripts/validate-install-tree.cmake
```

Windows note:

- use a Visual Studio Developer Command Prompt or equivalent MSVC environment before configuring

## Dependency And Packaging Guardrails

- Keep third-party sources pinned in `cmake/DepsLock.cmake` and bootstrapped via `scripts/bootstrap_deps.cmake`.
- Do not introduce git submodules for third-party code.
- Do not rely on implicit configure-time network access.
- `fmt` is intentionally overridden through `FETCHCONTENT_SOURCE_DIR_FMT` so that `slang` uses the locally bootstrapped copy.
- `SLANG_USE_MIMALLOC` is forced `OFF` in `cmake/Dependencies.cmake` so a clean build tree does not trigger a remote `mimalloc` fetch during configure.
- If `mimalloc` is ever re-enabled, it must be converted into a pinned local dependency first, and the redistributed notice scope must be updated accordingly.

## Notices And Install Layout

Third-party notice artifacts are generated from `cmake/attributions.cmake`.

Current redistributed scope:

- `slang`
- `fmt`
- `nlohmann/json`
- the vendored `boost_unordered` header used by `slang` when no suitable Boost package is found

Generated / installed notice artifacts:

- `LICENSE`
- `ATTRIBUTIONS.md`
- `NOTICE`

Installed destination:

- `share/pristine-engine/licenses`

## Binary Naming

- The CMake executable target is still named `pristine-lsp`.
- The produced runtime binary name is `pristine-engine` via `OUTPUT_NAME`.
- CLI smoke tests, install validation, and CI all expect the runtime artifact to be named `pristine-engine`.

## CI

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

## Current Verified State

The current repository state has been locally verified on Windows with:

- clean `build/dev` removal and full reconfigure
- full rebuild
- notice validation
- `pristine-engine --version`
- unit tests passing

## Likely Next Steps

- deepen semantic analysis beyond the current syntax-driven symbol tree
- add definition / references / workspace symbols / completion
- extend hover beyond the current declaration-oriented slice