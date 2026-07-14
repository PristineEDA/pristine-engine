import os
import pathlib
import queue
import json
import re
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class DifferentialCheck:
    kind: str
    uri: str | None = None
    position: dict[str, int] | None = None
    query: str = ""
    label: str = ""
    required: tuple[str, ...] = ()
    min_count: int | None = None
    include_declaration: bool = True
    optional: bool = False


@dataclass(frozen=True)
class DifferentialFixture:
    name: str
    sources: dict[str, str]
    checks: tuple[DifferentialCheck, ...]


CHECK_CAPABILITIES: dict[str, str] = {
    "completion": "completionProvider",
    "completionResolve": "completionProvider",
    "signatureHelp": "signatureHelpProvider",
    "hover": "hoverProvider",
    "definition": "definitionProvider",
    "references": "referencesProvider",
    "workspaceSymbol": "workspaceSymbolProvider",
    "typeDefinition": "typeDefinitionProvider",
    "callHierarchyOutgoing": "callHierarchyProvider",
}


FIXTURES: tuple[DifferentialFixture, ...] = (
    DifferentialFixture(
        name="completion local signal prefix",
        sources={
            "top.sv": (
                "module top;\n"
                "  logic ready;\n"
                "  logic valid;\n"
                "  assign ready = val\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="top.sv",
                position={"line": 3, "character": 16},
                required=("valid",),
            ),
        ),
    ),
    DifferentialFixture(
        name="completion wildcard import parameter prefix",
        sources={
            "defs.sv": (
                "package defs;\n"
                "  parameter int CONFIG_WIDTH = 8;\n"
                "  parameter int CONFIG_DEPTH = 4;\n"
                "endpackage\n"
            ),
            "top.sv": (
                "module top;\n"
                "  import defs::*;\n"
                "  localparam int W = CONFIG_\n"
                "endmodule\n"
            ),
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="top.sv",
                position={"line": 2, "character": 29},
                required=("CONFIG_WIDTH", "CONFIG_DEPTH"),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion macro function prefix",
        sources={
            "macro.sv": (
                "`define MUX(sel, lhs, rhs) ((sel) ? (lhs) : (rhs))\n"
                "module top;\n"
                "  logic value = `MU\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="macro.sv",
                position={"line": 2, "character": 19},
                required=("MUX",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="macro hover definition and signature",
        sources={
            "macro-navigation.sv": (
                "`define ADD(lhs, rhs) ((lhs) + (rhs))\n"
                "module top;\n"
                "  int value = `ADD(1, 2);\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="hover",
                uri="macro-navigation.sv",
                position={"line": 2, "character": 17},
                required=("ADD",),
            ),
            DifferentialCheck(
                kind="definition",
                uri="macro-navigation.sv",
                position={"line": 2, "character": 17},
                min_count=1,
            ),
            DifferentialCheck(
                kind="signatureHelp",
                uri="macro-navigation.sv",
                position={"line": 2, "character": 22},
                required=("ADD",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion named port prefix",
        sources={
            "ports.sv": (
                "module child(input logic clk, output logic rst_n, input logic data);\n"
                "endmodule\n"
                "module top;\n"
                "  child u_child(.clk(clk), .r);\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="ports.sv",
                position={"line": 3, "character": 26},
                required=("rst_n",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion hierarchical instance member prefix",
        sources={
            "hier.sv": (
                "module child;\n"
                "  logic child_ready;\n"
                "endmodule\n"
                "module top;\n"
                "  child u_child();\n"
                "  initial begin\n"
                "    u_child.child_\n"
                "  end\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="hier.sv",
                position={"line": 6, "character": 18},
                required=("child_ready",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion array element member prefix",
        sources={
            "array_member.sv": (
                "module top;\n"
                "  logic lanes;\n"
                "  logic status_ready;\n"
                "  logic status_valid;\n"
                "  initial begin\n"
                "    lanes[0].status_\n"
                "  end\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="array_member.sv",
                position={"line": 5, "character": 22},
                required=("status_ready", "status_valid"),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion interface-like member prefix",
        sources={
            "interface_member.sv": (
                "interface bus_if;\n"
                "  logic status_ready;\n"
                "  modport master(input status_ready);\n"
                "endinterface\n"
                "module top;\n"
                "  bus_if bus();\n"
                "  initial begin\n"
                "    bus.master.status_\n"
                "  end\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completion",
                uri="interface_member.sv",
                position={"line": 7, "character": 22},
                required=("status_ready",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="completion resolve and package function signature",
        sources={
            "callable.sv": (
                "package math_pkg;\n"
                "  function int add(input int lhs, input int rhs); return lhs + rhs; endfunction\n"
                "endpackage\n"
                "module child(input logic clk); endmodule\n"
                "module top;\n"
                "  chi\n"
                "  int value = math_pkg::add(1, 2);\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="completionResolve",
                uri="callable.sv",
                position={"line": 5, "character": 5},
                label="child",
                required=("child",),
                optional=True,
            ),
            DifferentialCheck(
                kind="signatureHelp",
                uri="callable.sv",
                position={"line": 6, "character": 31},
                required=("add",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="references local signal identity",
        sources={
            "refs.sv": (
                "module top;\n"
                "  logic data;\n"
                "  assign data = data;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="references",
                uri="refs.sv",
                position={"line": 1, "character": 9},
                min_count=3,
            ),
        ),
    ),
    DifferentialFixture(
        name="references same-name module scope isolation",
        sources={
            "shadow.sv": (
                "module first;\n"
                "  logic ready;\n"
                "  assign ready = ready;\n"
                "endmodule\n"
                "module second;\n"
                "  logic ready;\n"
                "  assign ready = ready;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="references",
                uri="shadow.sv",
                position={"line": 1, "character": 9},
                min_count=3,
            ),
            DifferentialCheck(
                kind="references",
                uri="shadow.sv",
                position={"line": 5, "character": 9},
                min_count=3,
            ),
        ),
    ),
    DifferentialFixture(
        name="references typedef shadow isolation",
        sources={
            "typedef_shadow.sv": (
                "module first;\n"
                "  typedef logic [3:0] data_t;\n"
                "  data_t value;\n"
                "endmodule\n"
                "module second;\n"
                "  typedef logic [7:0] data_t;\n"
                "  data_t value;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="references",
                uri="typedef_shadow.sv",
                position={"line": 1, "character": 23},
                min_count=2,
                optional=True,
            ),
            DifferentialCheck(
                kind="references",
                uri="typedef_shadow.sv",
                position={"line": 5, "character": 23},
                min_count=2,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="references ignore comments and strings",
        sources={
            "no_text.sv": (
                "module top;\n"
                "  logic ready;\n"
                "  // ready in comment\n"
                "  string label = \"ready\";\n"
                "  assign ready = ready;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="references",
                uri="no_text.sv",
                position={"line": 1, "character": 9},
                min_count=3,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="workspace symbol module discovery",
        sources={
            "modules.sv": (
                "module child;\n"
                "endmodule\n"
                "module top;\n"
                "  child u_child();\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="workspaceSymbol",
                query="child",
                required=("child",),
            ),
        ),
    ),
    DifferentialFixture(
        name="workspace symbol package and typedef discovery",
        sources={
            "pkg.sv": (
                "package defs;\n"
                "  typedef logic [7:0] packet_t;\n"
                "endpackage\n"
            ),
        },
        checks=(
            DifferentialCheck(
                kind="workspaceSymbol",
                query="defs",
                required=("defs",),
            ),
            DifferentialCheck(
                kind="workspaceSymbol",
                query="packet",
                required=("packet_t",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="workspace symbol interface modport discovery",
        sources={
            "bus.sv": (
                "interface bus_if;\n"
                "  logic ready;\n"
                "  modport master(input ready);\n"
                "endinterface\n"
            ),
        },
        checks=(
            DifferentialCheck(
                kind="workspaceSymbol",
                query="bus",
                required=("bus_if",),
                optional=True,
            ),
            DifferentialCheck(
                kind="workspaceSymbol",
                query="master",
                required=("master",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="workspace symbol class interface modport discovery",
        sources={
            "types.sv": (
                "class packet;\n"
                "endclass\n"
                "interface bus_if;\n"
                "  logic ready;\n"
                "  modport master(input ready);\n"
                "endinterface\n"
            ),
        },
        checks=(
            DifferentialCheck(
                kind="workspaceSymbol",
                query="packet",
                required=("packet",),
                optional=True,
            ),
            DifferentialCheck(
                kind="workspaceSymbol",
                query="bus",
                required=("bus_if",),
                optional=True,
            ),
            DifferentialCheck(
                kind="workspaceSymbol",
                query="master",
                required=("master",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="diagnostics syntax error publication",
        sources={
            "bad.sv": (
                "module top;\n"
                "  logic ready\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="diagnostics",
                uri="bad.sv",
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="diagnostics non-adjacent duplicate publication",
        sources={
            "duplicate_non_adjacent.sv": (
                "module top;\n"
                "  logic ready;\n"
                "  logic valid;\n"
                "  logic ready;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="diagnostics",
                uri="duplicate_non_adjacent.sv",
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="diagnostics missing package publication",
        sources={
            "missing_pkg.sv": (
                "module top;\n"
                "  import missing_pkg::*;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="diagnostics",
                uri="missing_pkg.sv",
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="diagnostics width mismatch publication",
        sources={
            "width.sv": (
                "module top;\n"
                "  logic [3:0] lhs;\n"
                "  logic [7:0] rhs;\n"
                "  assign lhs = rhs;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="diagnostics",
                uri="width.sv",
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="diagnostics missing wildcard import package",
        sources={
            "missing_import_pkg.sv": (
                "module top;\n"
                "  import missing_defs::*;\n"
                "  logic ready;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="diagnostics",
                uri="missing_import_pkg.sv",
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="type definition typedef lookup",
        sources={
            "type.sv": (
                "module top;\n"
                "  typedef logic [7:0] byte_t;\n"
                "  byte_t data;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="typeDefinition",
                uri="type.sv",
                position={"line": 2, "character": 3},
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="type definition typedef alias chain",
        sources={
            "alias.sv": (
                "module top;\n"
                "  typedef logic [7:0] byte_t;\n"
                "  typedef byte_t packet_t;\n"
                "  packet_t data;\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="typeDefinition",
                uri="alias.sv",
                position={"line": 3, "character": 3},
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="type definition package-qualified typedef lookup",
        sources={
            "defs.sv": (
                "package defs;\n"
                "  typedef logic [7:0] word_t;\n"
                "endpackage\n"
            ),
            "top.sv": (
                "module top;\n"
                "  defs::word_t value;\n"
                "endmodule\n"
            ),
        },
        checks=(
            DifferentialCheck(
                kind="typeDefinition",
                uri="top.sv",
                position={"line": 1, "character": 10},
                min_count=1,
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="call hierarchy module outgoing",
        sources={
            "call.sv": (
                "module child;\n"
                "endmodule\n"
                "module top;\n"
                "  child u_child();\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="callHierarchyOutgoing",
                uri="call.sv",
                position={"line": 2, "character": 8},
                required=("child",),
                optional=True,
            ),
        ),
    ),
    DifferentialFixture(
        name="call hierarchy two outgoing children",
        sources={
            "call_two.sv": (
                "module left;\n"
                "endmodule\n"
                "module right;\n"
                "endmodule\n"
                "module top;\n"
                "  left u_left();\n"
                "  right u_right();\n"
                "endmodule\n"
            )
        },
        checks=(
            DifferentialCheck(
                kind="callHierarchyOutgoing",
                uri="call_two.sv",
                position={"line": 4, "character": 8},
                required=("left", "right"),
                optional=True,
            ),
        ),
    ),
)


class UnsupportedCheck(Exception):
    pass


def write_message(process: subprocess.Popen[bytes], message: dict[str, Any]) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    assert process.stdin is not None
    process.stdin.write(header + payload)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes]) -> dict[str, Any]:
    assert process.stdout is not None
    content_length = None
    while True:
        line = process.stdout.readline()
        if line == b"":
            raise RuntimeError("LSP server stdout closed while reading headers")
        if line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode("ascii").partition(":")
        if name.lower() == "content-length":
            content_length = int(value.strip())
    if content_length is None:
        raise RuntimeError("LSP message is missing Content-Length")
    payload = process.stdout.read(content_length)
    if len(payload) != content_length:
        raise RuntimeError("LSP server stdout closed while reading payload")
    return json.loads(payload.decode("utf-8"))


def response_items(result: Any) -> list[dict[str, Any]]:
    items = result.get("items", result) if isinstance(result, dict) else result
    if not isinstance(items, list):
        return []
    return [item for item in items if isinstance(item, dict)]


class LspSession:
    def __init__(self, server: pathlib.Path) -> None:
        self.process = subprocess.Popen(
            [str(server)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.messages: queue.Queue[dict[str, Any] | BaseException] = queue.Queue()
        self.notifications: list[dict[str, Any]] = []
        self.server_capabilities: dict[str, Any] = {}
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self) -> None:
        while True:
            try:
                self.messages.put(read_message(self.process))
            except BaseException as error:
                self.messages.put(error)
                return

    def request(
        self,
        request_id: int,
        method: str,
        params: dict[str, Any] | None,
        *,
        allow_error: bool = False,
        timeout_seconds: float = 5.0,
    ) -> dict[str, Any]:
        message: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        write_message(self.process, message)

        deadline = time.monotonic() + timeout_seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Timed out waiting for {method}")
            try:
                item = self.messages.get(timeout=remaining)
            except queue.Empty as error:
                raise TimeoutError(f"Timed out waiting for {method}") from error
            if isinstance(item, BaseException):
                raise item
            if item.get("id") == request_id:
                if "error" in item and not allow_error:
                    raise AssertionError(f"{method} failed: {item['error']}")
                return item
            self.notifications.append(item)

    def notify(self, method: str, params: dict[str, Any] | None) -> None:
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        write_message(self.process, message)

    def drain_notifications(self, timeout_seconds: float = 0.25) -> None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            try:
                item = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                return
            if isinstance(item, BaseException):
                return
            self.notifications.append(item)

    def initialize(self, root_uri: str) -> None:
        response = self.request(1, "initialize", {"rootUri": root_uri, "capabilities": {}})
        result = response.get("result")
        capabilities = result.get("capabilities") if isinstance(result, dict) else None
        self.server_capabilities = capabilities if isinstance(capabilities, dict) else {}
        self.notify("initialized", {})

    def supports_server_capability(self, name: str) -> bool:
        return self.server_capabilities.get(name) not in (None, False)

    def did_open(self, uri: str, text: str) -> None:
        self.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "systemverilog",
                    "version": 1,
                    "text": text,
                }
            },
        )

    def shutdown(self) -> None:
        try:
            self.request(999, "shutdown", None, allow_error=True, timeout_seconds=2.0)
            self.notify("exit", None)
        finally:
            self.process.terminate()


def find_slang_server_binary(root: pathlib.Path) -> pathlib.Path | None:
    candidates = [
        root / "build" / "slang-server",
        root / "build" / "slang-server.exe",
        root / "build" / "bin" / "slang-server",
        root / "build" / "bin" / "slang-server.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    for candidate in root.rglob("slang-server*"):
        if candidate.is_file() and candidate.name in {"slang-server", "slang-server.exe"}:
            return candidate
    return None


def check_completion(session: LspSession, request_id: int, uri: str, position: dict[str, int]) -> set[str]:
    response = session.request(
        request_id,
        "textDocument/completion",
        {
            "textDocument": {"uri": uri},
            "position": position,
            "context": {"triggerKind": 1},
        },
    )
    return {item.get("label", "") for item in response_items(response.get("result"))}


def check_completion_resolve(
    session: LspSession,
    request_id: int,
    uri: str,
    position: dict[str, int],
    label: str,
) -> set[str]:
    completion = session.request(
        request_id,
        "textDocument/completion",
        {"textDocument": {"uri": uri}, "position": position, "context": {"triggerKind": 1}},
        allow_error=True,
    )
    if "error" in completion:
        raise UnsupportedCheck("textDocument/completion is not supported")
    item = next((candidate for candidate in response_items(completion.get("result"))
                 if candidate.get("label") == label), None)
    if item is None:
        return set()
    resolved = session.request(request_id + 1000, "completionItem/resolve", item, allow_error=True)
    if "error" in resolved:
        raise UnsupportedCheck("completionItem/resolve is not supported")
    result = resolved.get("result")
    return {str(result.get("label", ""))} if isinstance(result, dict) else set()


def check_signature_help(
    session: LspSession, request_id: int, uri: str, position: dict[str, int]
) -> set[str]:
    response = session.request(
        request_id,
        "textDocument/signatureHelp",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in response:
        raise UnsupportedCheck("textDocument/signatureHelp is not supported")
    result = response.get("result")
    signatures = result.get("signatures", []) if isinstance(result, dict) else []
    labels: set[str] = set()
    for signature in signatures:
        label = signature.get("label", "") if isinstance(signature, dict) else ""
        labels.update(re.findall(r"[A-Za-z_$][A-Za-z0-9_$]*", label))
    return labels


def check_hover(session: LspSession, request_id: int, uri: str, position: dict[str, int]) -> set[str]:
    response = session.request(
        request_id,
        "textDocument/hover",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in response:
        raise UnsupportedCheck("textDocument/hover is not supported")
    return set(re.findall(r"[A-Za-z_$][A-Za-z0-9_$]*", json.dumps(response.get("result"))))


def check_definition(session: LspSession, request_id: int, uri: str, position: dict[str, int]) -> int:
    response = session.request(
        request_id,
        "textDocument/definition",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in response:
        raise UnsupportedCheck("textDocument/definition is not supported")
    return len(response_items(response.get("result")))


def check_references(
    session: LspSession,
    request_id: int,
    uri: str,
    position: dict[str, int],
    include_declaration: bool,
) -> int:
    response = session.request(
        request_id,
        "textDocument/references",
        {
            "textDocument": {"uri": uri},
            "position": position,
            "context": {"includeDeclaration": include_declaration},
        },
    )
    result = response.get("result")
    return len(result) if isinstance(result, list) else 0


def check_workspace_symbol(session: LspSession, request_id: int, query: str) -> set[str]:
    response = session.request(request_id, "workspace/symbol", {"query": query})
    return {item.get("name", "") for item in response_items(response.get("result"))}


def check_diagnostics(session: LspSession, request_id: int, uri: str) -> int:
    session.request(request_id, "workspace/symbol", {"query": ""}, allow_error=True)
    session.drain_notifications()
    count = 0
    for notification in session.notifications:
        if notification.get("method") != "textDocument/publishDiagnostics":
            continue
        params = notification.get("params", {})
        if params.get("uri") == uri:
            diagnostics = params.get("diagnostics", [])
            count = max(count, len(diagnostics) if isinstance(diagnostics, list) else 0)
    return count


def check_type_definition(session: LspSession, request_id: int, uri: str, position: dict[str, int]) -> int:
    response = session.request(
        request_id,
        "textDocument/typeDefinition",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in response:
        raise UnsupportedCheck("textDocument/typeDefinition is not supported")
    result = response.get("result")
    return len(result) if isinstance(result, list) else 0


def check_call_hierarchy_outgoing(session: LspSession,
                                  request_id: int,
                                  uri: str,
                                  position: dict[str, int]) -> set[str]:
    prepared = session.request(
        request_id,
        "textDocument/prepareCallHierarchy",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in prepared:
        raise UnsupportedCheck("textDocument/prepareCallHierarchy is not supported")
    items = response_items(prepared.get("result"))
    if not items:
        return set()
    outgoing = session.request(
        request_id + 1000,
        "callHierarchy/outgoingCalls",
        {"item": items[0]},
        allow_error=True,
    )
    if "error" in outgoing:
        raise UnsupportedCheck("callHierarchy/outgoingCalls is not supported")
    names: set[str] = set()
    for call in response_items(outgoing.get("result")):
        item = call.get("to") or call.get("item") or {}
        if isinstance(item, dict):
            names.add(item.get("name", ""))
    return names


def check_backward_cone(session: LspSession, request_id: int, uri: str, position: dict[str, int]) -> int:
    response = session.request(
        request_id,
        "systemverilog/backwardCone",
        {"textDocument": {"uri": uri}, "position": position},
        allow_error=True,
    )
    if "error" in response:
        raise UnsupportedCheck("systemverilog/backwardCone is not supported")
    result = response.get("result")
    if not isinstance(result, dict):
        return 0
    nodes = result.get("nodes", [])
    return len(nodes) if isinstance(nodes, list) else 0


def run_fixture(server: pathlib.Path, root: pathlib.Path, fixture: DifferentialFixture) -> dict[str, Any]:
    root_uri = root.resolve().as_uri()
    source_uris: dict[str, str] = {}
    for relative, text in fixture.sources.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        source_uris[relative] = path.resolve().as_uri()

    session = LspSession(server)
    try:
        session.initialize(root_uri)
        for relative, text in fixture.sources.items():
            session.did_open(source_uris[relative], text)
        session.drain_notifications()

        observed: dict[str, Any] = {}
        request_id = 10
        for check in fixture.checks:
            key = f"{check.kind}:{check.uri or check.query}:{request_id}"
            try:
                capability = CHECK_CAPABILITIES.get(check.kind)
                if capability and not session.supports_server_capability(capability):
                    raise UnsupportedCheck(f"{check.kind} is not advertised by the server")
                if check.kind == "completion":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_completion(session, request_id, source_uris[check.uri], check.position)
                elif check.kind == "completionResolve":
                    assert check.uri is not None and check.position is not None and check.label
                    observed[key] = check_completion_resolve(
                        session, request_id, source_uris[check.uri], check.position, check.label
                    )
                elif check.kind == "signatureHelp":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_signature_help(
                        session, request_id, source_uris[check.uri], check.position
                    )
                elif check.kind == "hover":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_hover(session, request_id, source_uris[check.uri], check.position)
                elif check.kind == "definition":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_definition(
                        session, request_id, source_uris[check.uri], check.position
                    )
                elif check.kind == "references":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_references(
                        session,
                        request_id,
                        source_uris[check.uri],
                        check.position,
                        check.include_declaration,
                    )
                elif check.kind == "workspaceSymbol":
                    observed[key] = check_workspace_symbol(session, request_id, check.query)
                elif check.kind == "diagnostics":
                    assert check.uri is not None
                    observed[key] = check_diagnostics(session, request_id, source_uris[check.uri])
                elif check.kind == "typeDefinition":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_type_definition(session, request_id, source_uris[check.uri], check.position)
                elif check.kind == "callHierarchyOutgoing":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_call_hierarchy_outgoing(
                        session, request_id, source_uris[check.uri], check.position
                    )
                elif check.kind == "backwardCone":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_backward_cone(session, request_id, source_uris[check.uri], check.position)
                else:
                    raise AssertionError(f"Unsupported differential check kind: {check.kind}")
            except UnsupportedCheck:
                if not check.optional:
                    raise
                observed[key] = None
            request_id += 1
        return observed
    finally:
        session.shutdown()


def validate_observed(server_name: str, fixture: DifferentialFixture, observed: dict[str, Any]) -> None:
    for check, value in zip(fixture.checks, observed.values()):
        if value is None:
            continue
        if check.required:
            missing = set(check.required).difference(value)
            if missing:
                if check.optional:
                    continue
                raise AssertionError(
                    f"{server_name} fixture '{fixture.name}' missing {check.kind} entries: {sorted(missing)}"
                )
        if check.min_count is not None and value < check.min_count:
            if check.optional:
                continue
            raise AssertionError(
                f"{server_name} fixture '{fixture.name}' {check.kind} count {value} < {check.min_count}"
            )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: slang_server_differential.py <pristine-engine>", file=sys.stderr)
        return 2

    slang_root_value = os.environ.get("SLANG_SERVER_ROOT")
    if not slang_root_value:
        print("SKIP: SLANG_SERVER_ROOT is not set")
        return 0

    slang_server = find_slang_server_binary(pathlib.Path(slang_root_value))
    if slang_server is None:
        print("SKIP: slang-server binary was not found under SLANG_SERVER_ROOT")
        return 0

    pristine_engine = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="pristine-diff-") as temp_dir:
        base = pathlib.Path(temp_dir)
        for fixture in FIXTURES:
            pristine_root = base / "pristine" / fixture.name.replace(" ", "-")
            slang_root = base / "slang" / fixture.name.replace(" ", "-")
            pristine_observed = run_fixture(pristine_engine, pristine_root, fixture)
            slang_observed = run_fixture(slang_server, slang_root, fixture)
            validate_observed("pristine-engine", fixture, pristine_observed)
            validate_observed("slang-server", fixture, slang_observed)

    print(f"Compared {len(FIXTURES)} rewritten fixture(s) against slang-server")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
