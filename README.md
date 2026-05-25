# pristine-engine

Standalone SystemVerilog LSP server in C++20.

Current status: the repository contains an executable MVP with the first `slang`-backed language slice.
It boots as a standalone stdio server, implements JSON-RPC framing, supports the base LSP lifecycle and document sync methods below, publishes parse diagnostics for open documents, and answers syntax-driven `textDocument/documentSymbol` requests with top-level declarations plus common nested members:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`

The server also resolves the workspace root from `initialize`, loads a minimal `.slang/server.json` when present, and parses open buffers through locally bootstrapped `slang` sources.

This is intentionally still a thin foundation. Parse diagnostics and basic hierarchical document symbols are wired in, but deeper semantic analysis, cross-file navigation, and completion are not yet implemented.

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
- `src/server`: lifecycle, text sync, workspace/config, `publishDiagnostics`, and `documentSymbol` session wiring
- `tests/unit`: framing, lifecycle, text sync, workspace/config, parse diagnostic, UTF-16 position, and document symbol tests

## Next steps

- Expand `documentSymbol` beyond the current syntax-level hierarchy into richer semantic containers and declarations
- Then add hover, definition, references, workspace symbols, and completion