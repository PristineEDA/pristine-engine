import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import test_status


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
        print("usage: lefdef_corpus.py <pristine-engine> <lefdef-root>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    corpus_root = pathlib.Path(sys.argv[2]).resolve()
    if not corpus_root.is_dir():
        print(f"SKIP: missing optional cibyr/lefdef checkout at {corpus_root}")
        return 77

    lef_files = sorted(path for path in corpus_root.rglob("*.lef") if path.is_file())
    def_files = sorted(path for path in corpus_root.rglob("*.def") if path.is_file())
    if not lef_files and not def_files:
        raise AssertionError(f"no LEF/DEF files found under {corpus_root}")
    test_status.emit(
        "pristine_lefdef_corpus",
        "begin",
        f"lef={len(lef_files)} def={len(def_files)} root={corpus_root}",
    )

    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen(
            [str(server_path), "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        root_uri = corpus_root.as_uri()
        request(
            process,
            1,
            "initialize",
            {
                "processId": None,
                "rootUri": root_uri,
                "workspaceFolders": [{"uri": root_uri, "name": "lefdef"}],
                "capabilities": {},
            },
        )

        total_layers = 0
        total_macros = 0
        total_components = 0
        total_nets = 0
        request_id = 2
        for index, lef in enumerate(lef_files, start=1):
            test_status.emit("pristine_lefdef_corpus", "open-lef", f"{index}/{len(lef_files)} {lef.name}")
            result = request(
                process,
                request_id,
                "systemverilog/layout/open",
                {"lefUris": [lef.as_uri()], "title": lef.name},
            )["result"]
            request_id += 1
            total_layers += int(result["layerCount"])
            total_macros += int(result["macroCount"])
            request(
                process,
                request_id,
                "systemverilog/layout/close",
                {"sessionId": result["sessionId"]},
            )
            request_id += 1

        for index, deffile in enumerate(def_files, start=1):
            test_status.emit("pristine_lefdef_corpus", "open-def", f"{index}/{len(def_files)} {deffile.name}")
            result = request(
                process,
                request_id,
                "systemverilog/layout/open",
                {"lefUris": [], "defUri": deffile.as_uri(), "title": deffile.name},
            )["result"]
            request_id += 1
            total_components += int(result["componentCount"])
            total_nets += int(result["netCount"])
            request(
                process,
                request_id,
                "systemverilog/layout/close",
                {"sessionId": result["sessionId"]},
            )
            request_id += 1

        if lef_files and total_layers == 0 and total_macros == 0:
            raise AssertionError(f"lefdef LEF corpus parsed no layers or macros across {len(lef_files)} files")
        if def_files and total_components == 0 and total_nets == 0:
            raise AssertionError(f"lefdef DEF corpus parsed no components or nets across {len(def_files)} files")

        request(process, request_id, "shutdown", None)
        notify(process, "exit", None)
        assert process.wait(timeout=5) == 0
        process = None
        print(
            f"lefdef corpus parsed {len(lef_files)} LEF and {len(def_files)} DEF files, "
            f"{total_layers} layers, {total_macros} macros, "
            f"{total_components} components, {total_nets} nets"
        )
        test_status.emit(
            "pristine_lefdef_corpus",
            "summary",
            f"layers={total_layers} macros={total_macros} components={total_components} nets={total_nets}",
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
