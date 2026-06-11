import json
import pathlib
import subprocess
import sys
import time


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
    if len(sys.argv) != 3:
        print("usage: ihp_lef_corpus.py <pristine-engine> <ihp-open-pdk-root>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    ihp_root = pathlib.Path(sys.argv[2]).resolve()
    if not ihp_root.is_dir():
        print(f"SKIP: missing optional IHP Open PDK checkout at {ihp_root}")
        return 77

    lef_files = sorted(path for path in ihp_root.rglob("*.lef") if path.is_file())
    if not lef_files:
        raise AssertionError(f"no LEF files found under {ihp_root}")

    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen(
            [str(server_path), "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        root_uri = ihp_root.as_uri()
        request(
            process,
            1,
            "initialize",
            {
                "processId": None,
                "rootUri": root_uri,
                "workspaceFolders": [{"uri": root_uri, "name": "ihp-open-pdk"}],
                "capabilities": {},
            },
        )

        total_layers = 0
        total_macros = 0
        total_diagnostics = 0
        for index, lef in enumerate(lef_files, start=1):
            response = request(
                process,
                index + 1,
                "systemverilog/layout/open",
                {"lefUris": [lef.as_uri()], "title": lef.name},
            )
            result = response["result"]
            total_layers += int(result["layerCount"])
            total_macros += int(result["macroCount"])
            total_diagnostics += int(result["diagnosticCount"])
            request(
                process,
                100000 + index,
                "systemverilog/layout/close",
                {"sessionId": result["sessionId"]},
            )

        if total_layers == 0 and total_macros == 0:
            raise AssertionError(f"IHP LEF corpus parsed no layers or macros across {len(lef_files)} files")
        if total_diagnostics > len(lef_files) * 50:
            raise AssertionError(
                f"IHP LEF corpus produced too many diagnostics: {total_diagnostics} across {len(lef_files)} files"
            )

        request(process, 200000, "shutdown", None)
        notify(process, "exit", None)
        assert process.wait(timeout=5) == 0
        process = None
        print(
            f"IHP LEF corpus parsed {len(lef_files)} files, "
            f"{total_layers} layers, {total_macros} macros, {total_diagnostics} diagnostics"
        )
        return 0
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process is not None and process.stderr is not None:
            stderr = process.stderr.read().decode("utf-8", errors="replace")
            if stderr:
                print(stderr, file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
