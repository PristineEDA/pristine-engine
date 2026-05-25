# pristine-engine

Standalone SystemVerilog LSP server in C++20.

Current status: the repository contains an executable MVP with the first `slang`-backed language slice.
It boots as a standalone stdio server, implements JSON-RPC framing, supports the base LSP lifecycle and document sync methods below, publishes parse diagnostics for open documents, answers syntax-driven `textDocument/documentSymbol` requests with top-level declarations plus common nested members such as parameters, ports, variables, typedefs, enum members, functions, class contents, instances, named generate blocks, if / loop generate branches, and interface modports, and serves a minimal declaration-oriented `textDocument/hover` built from the same syntax symbol tree:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`
- `textDocument/hover`

The server also resolves the workspace root from `initialize`, loads a minimal `.slang/server.json` when present, and parses open buffers through locally bootstrapped `slang` sources.

This is intentionally still a thin foundation. Parse diagnostics, basic hierarchical document symbols, and declaration hover are wired in, but deeper semantic analysis, cross-file navigation, and completion are not yet implemented.

The current config slice recognizes these workspace fields from `.slang/server.json`:

- `build`
- `buildPattern`
- `buildRelativePaths`
- `flags`
- `index[].dirs`
- `index[].excludeDirs`

## Dependency bootstrap

Third-party sources are not tracked as git submodules.
Instead, pinned source archives are downloaded into the local `.deps/` cache, which is gitignored.

Run:

```powershell
cmake -DPRISTINE_ROOT_DIR=. -P scripts/bootstrap_deps.cmake
```

The current lock file is in `cmake/DepsLock.cmake`.

## Third-party notices

Third-party notice artifacts are generated from `cmake/attributions.cmake`.
The current redistributed scope covers:

- `slang`
- `fmt`
- `nlohmann/json`
- `mimalloc`
- the vendored `boost_unordered` header used by `slang` when no suitable Boost package is found

Regenerate the checked-in notice files:

```powershell
cmake -DPRISTINE_ROOT_DIR=. -P scripts/generate-notice.cmake
```

Validate that `ATTRIBUTIONS.md` and `NOTICE` are up to date from the build graph:

```powershell
cmake --build --preset dev --target pristine_validate_notice
```

## Build

Requirements:

- CMake 3.23+
- A C++20 compiler
- Ninja
- On Windows, run from a Visual Studio Developer Command Prompt or equivalent environment

Configure and build the development preset:

```powershell
cmake --preset dev
cmake --build --preset dev
```

Run tests:

```powershell
ctest --test-dir build/dev --output-on-failure
```

## Install layout

Stage an install tree locally:

```powershell
cmake --install build/dev --prefix build/install-smoke
```

The staged install layout contains:

- `bin/pristine-lsp`
- `share/pristine-engine/licenses/LICENSE`
- `share/pristine-engine/licenses/ATTRIBUTIONS.md`
- `share/pristine-engine/licenses/NOTICE`

Validate the staged install tree:

```powershell
cmake -DPRISTINE_INSTALL_PREFIX=build/install-smoke -P scripts/validate-install-tree.cmake
```

## CI

GitHub Actions CI in `.github/workflows/ci.yml` runs the hosted phase-1 matrix below:

- Ubuntu 22.04 x64
- Ubuntu 24.04 x64
- Windows 2022 x64
- Windows 2025 x64
- macOS 14 arm64
- macOS 15 arm64
- macOS 15 x64
- macOS 26 arm64
- macOS 26 x64

The one requested hosted combination that is not covered in phase 1 is macOS 14 x64 because GitHub does not provide a standard `macos-14-intel` label.

## Binary

The server executable is built as `pristine-lsp`.

Supported CLI arguments today:

- `--stdio`
- `--version`

Example:

```powershell
./build/dev/pristine-lsp --version
```

## Implemented architecture slice

- `src/main.cpp`: process entry point, Windows binary stdio mode, CLI dispatch
- `src/transport`: stdio transport for LSP payload exchange
- `src/jsonrpc`: `Content-Length` framing and JSON-RPC request/notification routing
- `src/lsp`: minimal initialize result construction
- `src/analysis`: thin `slang` compilation / parse diagnostic / syntax-level symbol seam
- `src/document`: in-memory open document state and incremental text edit application
- `src/workspace`: workspace root resolution and minimal `.slang/server.json` loading
- `src/server`: lifecycle, text sync, workspace/config, `publishDiagnostics`, `documentSymbol`, and `hover` session wiring
- `tests/unit`: framing, lifecycle, text sync, workspace/config, parse diagnostic, UTF-16 position, document symbol, and hover tests

## Next steps

- Expand `documentSymbol` beyond the current syntax-level hierarchy into richer semantic containers, deeper interface constructs, remaining generate forms, and more semantic instance detail
- Then add definition, references, workspace symbols, and completion