import json
import os
import pathlib
import subprocess
import sys
import tempfile
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
)


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


def request(
    process: subprocess.Popen[bytes],
    request_id: int,
    method: str,
    params: dict[str, Any] | None,
) -> dict[str, Any]:
    message: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)
    while True:
        response = read_message(process)
        if response.get("id") == request_id:
            if "error" in response:
                raise AssertionError(f"{method} failed: {response['error']}")
            return response


def notify(process: subprocess.Popen[bytes], method: str, params: dict[str, Any] | None) -> None:
    message: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)


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


def initialize(process: subprocess.Popen[bytes], root_uri: str) -> None:
    request(process, 1, "initialize", {"rootUri": root_uri, "capabilities": {}})
    notify(process, "initialized", {})


def did_open(process: subprocess.Popen[bytes], uri: str, text: str) -> None:
    notify(
        process,
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


def shutdown(process: subprocess.Popen[bytes]) -> None:
    try:
        request(process, 999, "shutdown", None)
        notify(process, "exit", None)
    finally:
        process.terminate()


def response_items(result: Any) -> list[dict[str, Any]]:
    items = result.get("items", result) if isinstance(result, dict) else result
    if not isinstance(items, list):
        return []
    return [item for item in items if isinstance(item, dict)]


def check_completion(process: subprocess.Popen[bytes], request_id: int, uri: str, position: dict[str, int]) -> set[str]:
    response = request(
        process,
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
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
    include_declaration: bool,
) -> int:
    response = request(
        process,
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


def check_workspace_symbol(process: subprocess.Popen[bytes], request_id: int, query: str) -> set[str]:
    response = request(process, request_id, "workspace/symbol", {"query": query})
    return {item.get("name", "") for item in response_items(response.get("result"))}


def run_fixture(server: pathlib.Path, root: pathlib.Path, fixture: DifferentialFixture) -> dict[str, Any]:
    root_uri = root.resolve().as_uri()
    source_uris: dict[str, str] = {}
    for relative, text in fixture.sources.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        source_uris[relative] = path.resolve().as_uri()

    process = subprocess.Popen(
        [str(server)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        initialize(process, root_uri)
        for relative, text in fixture.sources.items():
            did_open(process, source_uris[relative], text)

        observed: dict[str, Any] = {}
        request_id = 10
        for check in fixture.checks:
            key = f"{check.kind}:{check.uri or check.query}:{request_id}"
            if check.kind == "completion":
                assert check.uri is not None and check.position is not None
                observed[key] = check_completion(process, request_id, source_uris[check.uri], check.position)
            elif check.kind == "references":
                assert check.uri is not None and check.position is not None
                observed[key] = check_references(
                    process,
                    request_id,
                    source_uris[check.uri],
                    check.position,
                    check.include_declaration,
                )
            elif check.kind == "workspaceSymbol":
                observed[key] = check_workspace_symbol(process, request_id, check.query)
            else:
                raise AssertionError(f"Unsupported differential check kind: {check.kind}")
            request_id += 1
        return observed
    finally:
        shutdown(process)


def validate_observed(server_name: str, fixture: DifferentialFixture, observed: dict[str, Any]) -> None:
    for check, value in zip(fixture.checks, observed.values()):
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
