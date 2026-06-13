import json
import os
import pathlib
import subprocess
import sys


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
        print("usage: ihp_gds_corpus.py <pristine-engine> <ihp-open-pdk-root>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    ihp_root = pathlib.Path(sys.argv[2]).resolve()
    if not ihp_root.is_dir():
        if os.environ.get("PRISTINE_REQUIRE_IHP_OPEN_PDK"):
            print(f"ERROR: required IHP Open PDK checkout is missing at {ihp_root}", file=sys.stderr)
            return 1
        print(f"SKIP: missing optional IHP Open PDK checkout at {ihp_root}")
        return 77

    gds_files = sorted(path for path in ihp_root.rglob("*") if path.is_file() and path.suffix.lower() == ".gds")
    if not gds_files:
        raise AssertionError(f"no GDS files found under {ihp_root}")

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

        total_cells = 0
        total_references = 0
        total_elements = 0
        total_layers = 0
        total_diagnostics = 0
        failures: list[str] = []
        request_id = 2
        for index, gds in enumerate(gds_files, start=1):
            relative = gds.relative_to(ihp_root)
            try:
                response = request(
                    process,
                    request_id,
                    "systemverilog/layout/open",
                    {"gdsUri": gds.as_uri(), "title": gds.name},
                )
            except AssertionError as exc:
                failures.append(f"{relative}: {exc}")
                request_id += 1
                continue
            request_id += 1

            result = response["result"]
            if result["protocol"] != "pristine-layout-columnar-v3":
                failures.append(f"{relative}: expected GDS protocol v3, got {result['protocol']!r}")
            if result["source"] != "gds":
                failures.append(f"{relative}: expected source 'gds', got {result['source']!r}")

            total_cells += int(result["cellCount"])
            total_references += int(result["referenceCount"])
            total_elements += int(result["elementCount"])
            total_layers += int(result["layerCount"])
            total_diagnostics += int(result["diagnosticCount"])

            request(
                process,
                request_id,
                "systemverilog/layout/close",
                {"sessionId": result["sessionId"]},
            )
            request_id += 1
            if index % 25 == 0 or index == len(gds_files):
                print(
                    f"IHP GDS progress {index}/{len(gds_files)} files, "
                    f"{total_cells} cells, {total_elements} elements, "
                    f"{total_diagnostics} diagnostics"
                )

        if failures:
            preview = "\n".join(failures[:20])
            raise AssertionError(f"IHP GDS corpus had {len(failures)} failures:\n{preview}")
        if total_cells == 0 and total_elements == 0 and total_layers == 0:
            raise AssertionError(f"IHP GDS corpus parsed no cells, elements, or layers across {len(gds_files)} files")
        if total_diagnostics > len(gds_files) * 50:
            raise AssertionError(
                f"IHP GDS corpus produced too many diagnostics: {total_diagnostics} across {len(gds_files)} files"
            )

        request(process, request_id, "shutdown", None)
        notify(process, "exit", None)
        assert process.wait(timeout=5) == 0
        process = None
        print(
            f"IHP GDS corpus parsed {len(gds_files)} files, "
            f"{total_cells} cells, {total_references} references, {total_elements} elements, "
            f"{total_layers} layers, {total_diagnostics} diagnostics"
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
