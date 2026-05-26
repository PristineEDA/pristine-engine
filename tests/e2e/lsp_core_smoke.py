import json
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


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lsp_core_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="pristine-lsp-e2e-") as temp_dir:
        root = pathlib.Path(temp_dir)
        rtl = root / "rtl"
        rtl.mkdir()
        child = rtl / "child.sv"
        top = rtl / "top.sv"
        child.write_text("module child; endmodule\n", encoding="utf-8")
        top_text = (
            "module top;\n"
            "  child child_i();\n"
            "  logic ready;\n"
            "  assign ready = ready;\n"
            "endmodule\n"
        )
        top.write_text(top_text, encoding="utf-8")
        child_uri = child.resolve().as_uri()
        top_uri = top.resolve().as_uri()

        process = subprocess.Popen(
            [str(server_path), "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            initialize = request(
                process,
                1,
                "initialize",
                {
                    "processId": None,
                    "rootUri": root.resolve().as_uri(),
                    "capabilities": {},
                },
            )
            capabilities = initialize["result"]["capabilities"]
            assert capabilities["definitionProvider"] is True
            assert capabilities["referencesProvider"] is True
            assert capabilities["workspaceSymbolProvider"] is True
            assert capabilities["completionProvider"]["resolveProvider"] is False

            notify(process, "initialized", {})
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": top_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": top_text,
                    }
                },
            )

            definition = request(
                process,
                2,
                "textDocument/definition",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 1, "character": 3},
                },
            )["result"]
            assert len(definition) == 1
            assert definition[0]["uri"] == child_uri
            assert definition[0]["range"]["start"] == {"line": 0, "character": 7}

            references = request(
                process,
                3,
                "textDocument/references",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 2, "character": 9},
                    "context": {"includeDeclaration": False},
                },
            )["result"]
            assert len(references) == 2
            assert all(item["range"]["start"]["line"] == 3 for item in references)

            workspace_symbols = request(
                process,
                4,
                "workspace/symbol",
                {"query": "ch"},
            )["result"]
            assert any(
                item["name"] == "child" and item["location"]["uri"] == child_uri
                for item in workspace_symbols
            )

            completions = request(
                process,
                5,
                "textDocument/completion",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 1, "character": 4},
                    "context": {"triggerKind": 1},
                },
            )["result"]
            labels = {item["label"] for item in completions}
            assert "child" in labels
            assert "child_i" in labels

            request(process, 6, "shutdown", None)
            notify(process, "exit", None)
            return_code = process.wait(timeout=5)
            assert return_code == 0
        finally:
            if process.poll() is None:
                process.kill()
            stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
            if process.returncode not in (0, None) and stderr:
                print(stderr, file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
