# Pristine Integration Notes

`pristine-engine` can be used as the SystemVerilog language backend for the Pristine desktop app. The engine exposes standard LSP requests for editor features and custom `systemverilog/*` requests for design views.

## Standard LSP Surface

The current server supports initialization and document sync, diagnostics, `documentSymbol`, `hover`, `definition`, `typeDefinition`, `implementation`, `references`, `documentHighlight`, `documentLink`, `inlayHint`, `codeAction`, `foldingRange`, `semanticTokens/full`, `selectionRange`, `signatureHelp`, call hierarchy, `workspace/symbol`, completion plus resolve, prepare rename, rename, and watched-file refresh.

All visible semantic results are value-type responses derived from slang AST / `Compilation`, `AstIndex` views, and design graph provider data. Syntax helpers remain available for lexical tasks such as document links or folding, but they are not semantic fallback sources.

## Custom Design Requests

### `systemverilog/moduleHierarchy`

Request params:

```json
{
  "moduleName": "optional top module name",
  "maxDepth": 64
}
```

Response shape:

```json
{
  "roots": [],
  "messages": [],
  "unresolved": false,
  "partial": false,
  "truncated": false
}
```

Each hierarchy node includes `moduleName`, `kind`, `instanceName`, source ranges, and `children`. This is the request Pristine can use for its hierarchy tree. When a module is unknown, cyclic, or depth-capped, the response uses `messages`, `partial`, and `truncated` rather than inventing syntax-derived fallback children.

### `systemverilog/schematic`

Request params:

```json
{
  "moduleName": "optional root module name",
  "maxDepth": 64
}
```

Response shape:

```json
{
  "rootModuleId": "top",
  "modules": [],
  "messages": [],
  "unresolved": false,
  "partial": false,
  "truncated": false
}
```

Each module view includes `id`, `name`, `uri`, `ports`, `cells`, and `nets`. External ports are encoded as endpoints whose `nodeId` starts with `$port:`; instance endpoints use the cell id and port name. This matches the fields consumed by Pristine's `lspSchematicToGraph()` adapter.

### `systemverilog/backwardCone`

Request params use the standard LSP `textDocument` plus `position` shape. This request is available for future signal tracing or debug views, but it is not required for the first Pristine hierarchy/schematic integration.

## Compatibility Boundary

- Pristine can call its existing `moduleHierarchy(options)` and `schematic(options)` IPC/preload APIs without changes to the request names.
- `moduleName` is optional. If omitted, the engine uses configured `topModules` and otherwise infers an uninstantiated top.
- `maxDepth` is optional and prevents runaway cyclic or very large designs.
- `messages`, `partial`, and `truncated` are part of the contract and should be surfaced or logged by callers when present.
- Real design data is never fetched by default tests. Repository-scale stress runs are opt-in through the RTL E2E corpus framework and can use `RTL_E2E_ROOT`.

## Validation

The repository contains provider-level mock tests and semantic golden fixtures for hierarchy and schematic wire shapes. Optional RTL E2E pressure tests are enabled only when `PRISTINE_BUILD_PERF_TESTS=ON`; the default corpus is retroSoC and can use `RTL_E2E_ROOT` for a local checkout at the expected commit.
