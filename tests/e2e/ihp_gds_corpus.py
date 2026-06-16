import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time
from typing import BinaryIO


FRAME_HEADER = struct.Struct("<4sHHIIII")
PROTOCOL_VERSION = 3
HELLO = 1
CATALOG_REQUEST = 3
ERROR_RESPONSE = 7
CLOSE = 8
TILE_GEOMETRY_REQUEST = 9
TILE_GEOMETRY_RESPONSE = 10
HIT_TEST_REQUEST = 11
HIT_TEST_RESPONSE = 12


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


def frame(message_type: int, request_id: int, payload: bytes = b"") -> bytes:
    return FRAME_HEADER.pack(b"PLD1", PROTOCOL_VERSION, message_type, request_id, 0, len(payload), 0) + payload


def read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise RuntimeError("pipe closed while reading layout frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(stream: BinaryIO) -> tuple[int, int, int, bytes]:
    header = read_exact(stream, FRAME_HEADER.size)
    magic, version, message_type, request_id, flags, payload_size, _reserved = FRAME_HEADER.unpack(header)
    if magic != b"PLD1":
        raise AssertionError(f"bad layout frame magic: {magic!r}")
    if version != PROTOCOL_VERSION:
        raise AssertionError(f"bad layout protocol version: {version}")
    return message_type, request_id, flags, read_exact(stream, payload_size)


def connect_pipe(kind: str, path: str) -> BinaryIO:
    deadline = time.monotonic() + 5.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            if kind == "namedPipe":
                return open(path, "r+b", buffering=0)
            if kind == "unixSocket":
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(path)
                return sock.makefile("rwb", buffering=0)
            raise AssertionError(f"unknown endpoint kind: {kind}")
        except OSError as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"failed to connect layout pipe {path}: {last_error}")


def tile_payload(root_cell_index: int, bbox: tuple[float, float, float, float], max_shapes: int) -> bytes:
    payload = bytearray(struct.pack("<IIIIIII", 1, root_cell_index, max_shapes, 0, 1_000_000, 2, 0))
    payload += struct.pack("<dddd", *bbox)
    payload += struct.pack("<III", 0, 0, 0)
    return bytes(payload)


def hit_payload(root_cell_index: int, x: float, y: float, radius: float) -> bytes:
    payload = bytearray(struct.pack("<III", 0, root_cell_index, 8))
    payload += struct.pack("<ddd", x, y, radius)
    payload += struct.pack("<III", 0, 0, 0)
    return bytes(payload)


def sample_spatial_pipe(session: dict) -> dict[str, int]:
    bounds = session.get("bounds") or {}
    x0 = float(bounds.get("x0", 0.0))
    y0 = float(bounds.get("y0", 0.0))
    x1 = float(bounds.get("x1", x0 + 1.0))
    y1 = float(bounds.get("y1", y0 + 1.0))
    if x1 <= x0:
        x1 = x0 + 1.0
    if y1 <= y0:
        y1 = y0 + 1.0
    root_cell_index = 0
    with connect_pipe(session["endpoint"]["kind"], session["endpoint"]["path"]) as pipe:
        pipe.write(frame(CATALOG_REQUEST, 100))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        if message_type == ERROR_RESPONSE:
            raise AssertionError("catalog request returned error")
        assert message_type == 4
        assert request_id == 100
        assert payload[:4] == b"PLCT"
        root_cell_index = struct.unpack_from("<I", payload, 24)[0]

        sample_width = max((x1 - x0) / 1024.0, 1.0)
        sample_height = max((y1 - y0) / 1024.0, 1.0)
        sample_bbox = (x0, y0, x0 + sample_width, y0 + sample_height)
        pipe.write(frame(TILE_GEOMETRY_REQUEST, 101, tile_payload(root_cell_index, sample_bbox, 1)))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        if message_type == ERROR_RESPONSE:
            raise AssertionError("tile geometry request returned error")
        assert message_type == TILE_GEOMETRY_RESPONSE
        assert request_id == 101
        assert payload[:4] == b"PLTG"
        tile_shape_count = struct.unpack_from("<I", payload, 24)[0]
        tile_metrics = {
            "tile_shapes": tile_shape_count,
            "tile_index_build_micros": struct.unpack_from("<Q", payload, 32)[0],
            "tile_query_micros": struct.unpack_from("<Q", payload, 40)[0],
            "tile_encode_micros": struct.unpack_from("<Q", payload, 48)[0],
            "tile_visited_cells": struct.unpack_from("<I", payload, 56)[0],
            "tile_element_candidates": struct.unpack_from("<I", payload, 60)[0],
            "tile_reference_candidates": struct.unpack_from("<I", payload, 64)[0],
            "tile_traversed_refs": struct.unpack_from("<I", payload, 68)[0],
        }

        pipe.write(frame(HIT_TEST_REQUEST, 102, hit_payload(root_cell_index, (x0 + x1) / 2.0, (y0 + y1) / 2.0, max(x1 - x0, y1 - y0) / 1000.0)))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        if message_type == ERROR_RESPONSE:
            raise AssertionError("hit-test request returned error")
        assert message_type == HIT_TEST_RESPONSE
        assert request_id == 102
        assert payload[:4] == b"PLHT"
        hit_count = struct.unpack_from("<I", payload, 8)[0]
        hit_metrics = {
            "hit_count": hit_count,
            "hit_index_build_micros": struct.unpack_from("<Q", payload, 32)[0],
            "hit_query_micros": struct.unpack_from("<Q", payload, 40)[0],
            "hit_encode_micros": struct.unpack_from("<Q", payload, 48)[0],
            "hit_tile_shapes": struct.unpack_from("<I", payload, 56)[0],
            "hit_precise_candidates": struct.unpack_from("<I", payload, 60)[0],
        }

        pipe.write(frame(CLOSE, 103))
        pipe.flush()
        return tile_metrics | hit_metrics


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
        total_open_micros = 0
        total_parse_micros = 0
        max_open_micros = 0
        max_parse_micros = 0
        max_open_file = ""
        max_parse_file = ""
        spatial_samples = 0
        spatial_tile_shapes = 0
        spatial_hit_candidates = 0
        spatial_tile_index_micros = 0
        spatial_tile_query_micros = 0
        spatial_tile_encode_micros = 0
        spatial_hit_query_micros = 0
        spatial_tile_element_candidates = 0
        spatial_tile_reference_candidates = 0
        spatial_tile_traversed_refs = 0
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
            metrics = result.get("gdsMetrics", {})
            open_micros = int(metrics.get("openMicros", 0))
            parse_micros = int(metrics.get("parseMicros", 0))
            total_open_micros += open_micros
            total_parse_micros += parse_micros
            if open_micros > max_open_micros:
                max_open_micros = open_micros
                max_open_file = str(relative)
            if parse_micros > max_parse_micros:
                max_parse_micros = parse_micros
                max_parse_file = str(relative)

            if (
                spatial_samples < 5
                and gds.stat().st_size <= 2 * 1024 * 1024
                and int(result["cellCount"]) > 0
                and int(result["elementCount"]) > 0
            ):
                try:
                    sample = sample_spatial_pipe(result)
                    spatial_tile_shapes += sample["tile_shapes"]
                    spatial_hit_candidates += sample["hit_count"]
                    spatial_tile_index_micros += sample["tile_index_build_micros"]
                    spatial_tile_query_micros += sample["tile_query_micros"]
                    spatial_tile_encode_micros += sample["tile_encode_micros"]
                    spatial_hit_query_micros += sample["hit_query_micros"]
                    spatial_tile_element_candidates += sample["tile_element_candidates"]
                    spatial_tile_reference_candidates += sample["tile_reference_candidates"]
                    spatial_tile_traversed_refs += sample["tile_traversed_refs"]
                    spatial_samples += 1
                except AssertionError as exc:
                    failures.append(f"{relative}: spatial sample failed: {exc}")

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
                    f"{total_diagnostics} diagnostics, "
                    f"open avg {total_open_micros // index}us, parse avg {total_parse_micros // index}us",
                    flush=True,
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
            f"{total_layers} layers, {total_diagnostics} diagnostics, "
            f"{spatial_samples} spatial samples, {spatial_tile_shapes} sampled tile shapes, "
            f"{spatial_hit_candidates} sampled hit candidates, "
            f"open avg {total_open_micros // len(gds_files)}us max {max_open_micros}us {max_open_file}, "
            f"parse avg {total_parse_micros // len(gds_files)}us max {max_parse_micros}us {max_parse_file}, "
            f"tile index/query/encode totals "
            f"{spatial_tile_index_micros}/{spatial_tile_query_micros}/{spatial_tile_encode_micros}us, "
            f"hit query total {spatial_hit_query_micros}us, "
            f"sample candidates elem/ref/traversed "
            f"{spatial_tile_element_candidates}/{spatial_tile_reference_candidates}/{spatial_tile_traversed_refs}"
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
