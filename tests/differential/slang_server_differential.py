import json
import os
import pathlib
import subprocess
import sys
import tempfile


def write_message(process: subprocess.Popen[bytes], message: dict) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    assert process.stdin is not None
    process.stdin.write(header + payload)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes]) -> dict:
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


def request(process: subprocess.Popen[bytes], request_id: int, method: str, params: dict | None) -> dict:
    message = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)
    while True:
        response = read_message(process)
        if response.get("id") == request_id:
            if "error" in response:
                raise AssertionError(f"{method} failed: {response['error']}")
            return response


def notify(process: subprocess.Popen[bytes], method: str, params: dict | None) -> None:
    message = {"jsonrpc": "2.0", "method": method}
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


def shutdown(process: subprocess.Popen[bytes]) -> None:
    try:
        request(process, 99, "shutdown", None)
        notify(process, "exit", None)
    finally:
        process.terminate()


def run_completion(server: pathlib.Path, root_uri: str, uri: str) -> list[str]:
    process = subprocess.Popen(
        [str(server)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        initialize(process, root_uri)
        response = request(
            process,
            2,
            "textDocument/completion",
            {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": 16},
                "context": {"triggerKind": 1},
            },
        )
        result = response.get("result")
        items = result.get("items", result) if isinstance(result, dict) else result
        if not isinstance(items, list):
            return []
        return sorted(item.get("label", "") for item in items if isinstance(item, dict))
    finally:
        shutdown(process)


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
        root = pathlib.Path(temp_dir)
        source = root / "top.sv"
        source.write_text(
            "module top;\n"
            "  logic ready;\n"
            "  logic valid;\n"
            "  assign ready = val\n"
            "endmodule\n",
            encoding="utf-8",
        )
        root_uri = root.resolve().as_uri()
        uri = source.resolve().as_uri()

        pristine_labels = run_completion(pristine_engine, root_uri, uri)
        slang_labels = run_completion(slang_server, root_uri, uri)

        required = {"valid"}
        missing_pristine = required.difference(pristine_labels)
        missing_slang = required.difference(slang_labels)
        if missing_pristine:
            raise AssertionError(f"pristine-engine missing completion labels: {sorted(missing_pristine)}")
        if missing_slang:
            raise AssertionError(f"slang-server missing completion labels: {sorted(missing_slang)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
