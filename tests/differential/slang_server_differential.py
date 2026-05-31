import json
import os
import pathlib
import queue
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
    required: tuple[str, ...] = ()
    min_count: int | None = None
    include_declaration: bool = True
    optional: bool = False


@dataclass(frozen=True)
class DifferentialFixture:
    name: str
    sources: dict[str, str]
    checks: tuple[DifferentialCheck, ...]


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
            item = self.messages.get(timeout=remaining)
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
        self.request(1, "initialize", {"rootUri": root_uri, "capabilities": {}})
        self.notify("initialized", {})

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
                if check.kind == "completion":
                    assert check.uri is not None and check.position is not None
                    observed[key] = check_completion(session, request_id, source_uris[check.uri], check.position)
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
                raise AssertionError(
                    f"{server_name} fixture '{fixture.name}' missing {check.kind} entries: {sorted(missing)}"
                )
        if check.min_count is not None and value < check.min_count:
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
