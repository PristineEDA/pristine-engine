# pristine-engine

Standalone SystemVerilog LSP server in C++20.

Current status: the repository contains the first executable MVP slice.
It boots as a standalone stdio server, implements JSON-RPC framing, and now supports the base LSP lifecycle and document sync methods below:

- `initialize`
- `initialized`
- `shutdown`
- `exit`
- `textDocument/didOpen`
- `textDocument/didChange`
- `textDocument/didSave`
- `textDocument/didClose`

This is intentionally still a thin foundation. `slang` integration, diagnostics, symbol navigation, and completion are not wired in yet.

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
- `src/document`: in-memory open document state and incremental text edit application
- `src/server`: lifecycle and text sync session wiring
- `tests/unit`: framing, lifecycle, and text sync tests

## Next steps

- Add workspace and config state
- Introduce `slang` bootstrap and compilation service
- Layer in parse diagnostics and `documentSymbol`
- Then add hover, definition, references, symbols, and completion