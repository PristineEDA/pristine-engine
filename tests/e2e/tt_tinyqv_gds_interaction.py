import json
import math
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
NO_INDEX = 0xFFFFFFFF

HELLO = 1
HELLO_RESPONSE = 2
CATALOG_REQUEST = 3
CATALOG_RESPONSE = 4
ERROR_RESPONSE = 7
CLOSE = 8
TILE_GEOMETRY_REQUEST = 9
TILE_GEOMETRY_RESPONSE = 10
HIT_TEST_REQUEST = 11
HIT_TEST_RESPONSE = 12
INSPECT_REQUEST = 13
INSPECT_RESPONSE = 14
SELECTION_GEOMETRY_REQUEST = 15
SELECTION_GEOMETRY_RESPONSE = 16
SEARCH_REQUEST = 17
SEARCH_RESPONSE = 18
CATALOG_SUMMARY_REQUEST = 19
CATALOG_SUMMARY_RESPONSE = 20
CATALOG_PAGE_REQUEST = 21
CATALOG_PAGE_RESPONSE = 22
STATUS_REQUEST = 23
STATUS_RESPONSE = 24

STATUS_STATE_PARSING = 1
STATUS_STATE_READY = 2
STATUS_STATE_FAILED = 3

STATUS_PHASE_UNKNOWN = 0
STATUS_PHASE_READ = 1
STATUS_PHASE_RECORDS = 2
STATUS_PHASE_FINALIZE = 3
STATUS_PHASE_RESOLVE = 4
STATUS_PHASE_READY = 5
STATUS_PHASE_FAILED = 6

CATALOG_PAGE_LAYERS = 1
CATALOG_PAGE_CELLS = 2

SEARCH_KIND_CELL = 1 << 0
SEARCH_KIND_REFERENCE = 1 << 1
SEARCH_KIND_TEXT = 1 << 2
SEARCH_KIND_LAYER = 1 << 3


def percentile(values: list[int], pct: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * pct))
    return ordered[max(0, min(index, len(ordered) - 1))]


def metric_summary(values: list[int]) -> dict[str, int]:
    return {
        "count": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "max": max(values) if values else 0,
    }


def tile_group(name: str) -> str:
    if name.startswith("full_lod2") or name.startswith("overview_"):
        return "overview"
    if name.startswith("wide_lod1_cold"):
        return "cold_wide_zoom"
    if name.startswith("wide_lod1_warm"):
        return "warm_wide_zoom"
    if name.startswith("center_") or name.startswith("quadrant_"):
        return "zoom"
    if name.startswith("pan_"):
        return "pan_grid"
    if name.startswith("burst_"):
        return "burst"
    if name.startswith("repeat_cache_"):
        return "cache_repeat"
    if name.startswith("continuation_"):
        return "continuation"
    return "other"


def tile_group_summary(entries: list[dict]) -> dict:
    return {
        "count": len(entries),
        "query_micros": metric_summary([entry["query_micros"] for entry in entries]),
        "encode_micros": metric_summary([entry["encode_micros"] for entry in entries]),
        "wall_micros": metric_summary([entry["wall_micros"] for entry in entries]),
        "payload_bytes": metric_summary([entry["payload_bytes"] for entry in entries]),
        "shape_count": metric_summary([entry["shape_count"] for entry in entries]),
        "point_count": metric_summary([entry["point_count"] for entry in entries]),
        "cache_hits": sum(entry["cache_hits"] for entry in entries),
        "cache_misses": sum(entry["cache_misses"] for entry in entries),
        "grid_build_micros": metric_summary([entry.get("grid_build_micros", 0) for entry in entries]),
        "grid_candidates": metric_summary([entry.get("grid_candidates", 0) for entry in entries]),
        "grid_hits": sum(entry.get("grid_hits", 0) for entry in entries),
        "grid_misses": sum(entry.get("grid_misses", 0) for entry in entries),
        "grid_bins": metric_summary([entry.get("grid_bins", 0) for entry in entries]),
        "truncated": sum(1 for entry in entries if entry["truncated"]),
        "tokens": sum(1 for entry in entries if entry["next_token"]),
    }


def status_phase_name(phase: int) -> str:
    return {
        STATUS_PHASE_UNKNOWN: "unknown",
        STATUS_PHASE_READ: "read",
        STATUS_PHASE_RECORDS: "records",
        STATUS_PHASE_FINALIZE: "finalize",
        STATUS_PHASE_RESOLVE: "resolve",
        STATUS_PHASE_READY: "ready",
        STATUS_PHASE_FAILED: "failed",
    }.get(phase, f"phase_{phase}")


def status_progress_summary(samples: list[dict]) -> dict:
    if not samples:
        return {}
    phase_samples: dict[int, list[dict]] = {}
    for sample in samples:
        phase_samples.setdefault(sample["phase"], []).append(sample)
    phase_elapsed = {}
    for phase, entries in sorted(phase_samples.items()):
        elapsed = [entry["elapsed_micros"] for entry in entries if entry["elapsed_micros"]]
        if elapsed:
            phase_elapsed[status_phase_name(phase)] = {
                "first_elapsed_micros": min(elapsed),
                "last_elapsed_micros": max(elapsed),
                "sample_count": len(entries),
            }
    first = samples[0]
    last = samples[-1]
    elapsed_delta = max(0, int(last["elapsed_micros"]) - int(first["elapsed_micros"]))
    record_delta = max(0, int(last["record_count"]) - int(first["record_count"]))
    point_delta = max(0, int(last["point_count"]) - int(first["point_count"]))
    return {
        "phase_elapsed": phase_elapsed,
        "first_phase": status_phase_name(first["phase"]),
        "last_phase": status_phase_name(last["phase"]),
        "first_elapsed_micros": int(first["elapsed_micros"]),
        "last_elapsed_micros": int(last["elapsed_micros"]),
        "records_per_second": int(record_delta * 1_000_000 / elapsed_delta) if elapsed_delta else 0,
        "points_per_second": int(point_delta * 1_000_000 / elapsed_delta) if elapsed_delta else 0,
        "final_record_count": int(last["record_count"]),
        "final_point_count": int(last["point_count"]),
        "final_cell_count": int(last["cell_count"]),
        "final_reference_count": int(last["reference_count"]),
        "final_element_count": int(last["element_count"]),
        "final_diagnostic_count": int(last["diagnostic_count"]),
    }


def delta_percent(previous: int, current: int) -> float | None:
    if previous == 0:
        return None
    return round(((current - previous) * 100.0) / previous, 2)


def summarize_before_after(previous: dict, current: dict) -> dict:
    keys = [
        ("lsp_open_wall_micros", "p95"),
        ("cancel_close_wall_micros", "p95"),
        ("open_wall_micros", "p95"),
        ("ready_wall_micros", "p95"),
        ("element_finalize_micros", "p95"),
        ("tile_query_micros", "p95"),
        ("tile_query_micros", "max"),
        ("hit_query_micros", "p95"),
        ("search_query_micros", "p95"),
    ]
    result = {}
    previous_summary = previous.get("summary", {})
    current_summary = current.get("summary", {})
    for key, field in keys:
        before = previous_summary.get(key, {}).get(field, 0)
        after = current_summary.get(key, {}).get(field, 0)
        result[f"{key}_{field}"] = {
            "before": before,
            "after": after,
            "delta_percent": delta_percent(before, after),
        }
    before_full = previous_summary.get("tile_groups", {}).get("overview", {}).get("query_micros", {}).get("max", 0)
    after_full = current_summary.get("tile_groups", {}).get("overview", {}).get("query_micros", {}).get("max", 0)
    result["overview_query_micros_max"] = {
        "before": before_full,
        "after": after_full,
        "delta_percent": delta_percent(before_full, after_full),
    }
    before_wide = previous_summary.get("tile_groups", {}).get("cold_wide_zoom", {}).get("query_micros", {}).get("max", 0)
    after_wide = current_summary.get("tile_groups", {}).get("cold_wide_zoom", {}).get("query_micros", {}).get("max", 0)
    result["wide_lod1_cold_query_micros_max"] = {
        "before": before_wide,
        "after": after_wide,
        "delta_percent": delta_percent(before_wide, after_wide),
    }
    before_warm = previous_summary.get("tile_groups", {}).get("warm_wide_zoom", {}).get("query_micros", {}).get("max", 0)
    after_warm = current_summary.get("tile_groups", {}).get("warm_wide_zoom", {}).get("query_micros", {}).get("max", 0)
    result["wide_lod1_post_warm_query_micros_max"] = {
        "before": before_warm,
        "after": after_warm,
        "delta_percent": delta_percent(before_warm, after_warm),
    }
    before_cache_wall = previous_summary.get("tile_groups", {}).get("cache_repeat", {}).get("wall_micros", {}).get("max", 0)
    after_cache_wall = current_summary.get("tile_groups", {}).get("cache_repeat", {}).get("wall_micros", {}).get("max", 0)
    result["cache_repeat_wall_micros_max"] = {
        "before": before_cache_wall,
        "after": after_cache_wall,
        "delta_percent": delta_percent(before_cache_wall, after_cache_wall),
    }
    return result


def trace(message: str) -> None:
    print(f"[tt-tinyqv] {message}", flush=True)


def now_micros() -> int:
    return time.perf_counter_ns() // 1000


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


def frame(message_type: int, request_id: int, payload: bytes = b"", flags: int = 0) -> bytes:
    return FRAME_HEADER.pack(b"PLD1", PROTOCOL_VERSION, message_type, request_id, flags, len(payload), 0) + payload


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
    deadline = time.monotonic() + 10.0
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


def tile_payload(
    root_cell_index: int,
    bbox: tuple[float, float, float, float],
    lod: int,
    max_shapes: int = 4_096,
    max_points: int = 65_536,
    max_bytes: int = 2_000_000,
    continuation_token: int = 0,
) -> bytes:
    payload = bytearray(
        struct.pack(
            "<IIIIIII",
            1,
            root_cell_index,
            max_shapes,
            max_points,
            max_bytes,
            lod,
            continuation_token,
        )
    )
    payload += struct.pack("<dddd", *bbox)
    payload += struct.pack("<III", 0, 0, 0)
    return bytes(payload)


def hit_payload(root_cell_index: int, x: float, y: float, radius: float, max_results: int = 16) -> bytes:
    payload = bytearray(struct.pack("<III", 0, root_cell_index, max_results))
    payload += struct.pack("<ddd", x, y, radius)
    payload += struct.pack("<III", 0, 0, 0)
    return bytes(payload)


def object_payload(
    kind: int,
    cell_index: int,
    reference_index: int,
    element_index: int,
    layer_index: int = NO_INDEX,
    datatype: int = 0,
    instance_path_hash: int = 0,
) -> bytes:
    return struct.pack(
        "<IIIIIIIQ",
        0,
        kind,
        cell_index,
        reference_index,
        element_index,
        layer_index,
        datatype,
        instance_path_hash,
    )


def search_payload(
    query: str,
    max_results: int,
    kind_mask: int,
    root_cell_index: int = NO_INDEX,
    bbox: tuple[float, float, float, float] | None = None,
) -> bytes:
    query_bytes = query.encode("utf-8")
    payload = bytearray(struct.pack("<IIII", 1 if bbox else 0, max_results, kind_mask, root_cell_index))
    if bbox:
        payload += struct.pack("<dddd", *bbox)
    payload += struct.pack("<I", len(query_bytes))
    payload += query_bytes
    return bytes(payload)


def error_text(payload: bytes) -> str:
    if len(payload) < 8:
        return "<malformed error payload>"
    size = struct.unpack_from("<I", payload, 4)[0]
    return payload[8 : 8 + size].decode("utf-8", errors="replace")


def send_pipe(
    pipe: BinaryIO,
    message_type: int,
    request_id: int,
    payload: bytes = b"",
    expect: int | None = None,
) -> tuple[int, int, bytes, int]:
    start = now_micros()
    pipe.write(frame(message_type, request_id, payload))
    pipe.flush()
    actual_type, actual_request, flags, response = read_frame(pipe)
    elapsed = now_micros() - start
    if actual_type == ERROR_RESPONSE:
        raise AssertionError(f"pipe request {message_type}/{request_id} failed: {error_text(response)}")
    if expect is not None and actual_type != expect:
        raise AssertionError(f"expected message type {expect}, got {actual_type}")
    if actual_request != request_id:
        raise AssertionError(f"expected request id {request_id}, got {actual_request}")
    return flags, elapsed, response, actual_type


def expect_pipe_error(pipe: BinaryIO, message_type: int, request_id: int, payload: bytes) -> str:
    pipe.write(frame(message_type, request_id, payload))
    pipe.flush()
    actual_type, actual_request, _flags, response = read_frame(pipe)
    if actual_request != request_id:
        raise AssertionError(f"expected request id {request_id}, got {actual_request}")
    if actual_type != ERROR_RESPONSE:
        raise AssertionError(f"expected pipe error frame, got {actual_type}")
    return error_text(response)


def maybe_pipe_error(pipe: BinaryIO, message_type: int, request_id: int, payload: bytes) -> str | None:
    pipe.write(frame(message_type, request_id, payload))
    pipe.flush()
    actual_type, actual_request, _flags, response = read_frame(pipe)
    if actual_request != request_id:
        raise AssertionError(f"expected request id {request_id}, got {actual_request}")
    if actual_type == ERROR_RESPONSE:
        return error_text(response)
    return None


def table_string(payload: bytes, string_table_offset: int, string_offset: int) -> str:
    start = string_table_offset + string_offset
    size = struct.unpack_from("<I", payload, start)[0]
    begin = start + 4
    return payload[begin : begin + size].decode("utf-8", errors="replace")


def catalog_page_payload(table_kind: int, offset: int = 0, limit: int = 4096, max_bytes: int = 8_000_000) -> bytes:
    return struct.pack("<IIIII", 0, table_kind, offset, limit, max_bytes)


def status_payload(flags: int = 0) -> bytes:
    return struct.pack("<I", flags)


def parse_status(payload: bytes) -> dict:
    if payload[:4] != b"PLST":
        raise AssertionError("status payload missing PLST magic")
    if len(payload) < 116:
        raise AssertionError("status payload truncated")
    string_offset = struct.unpack_from("<I", payload, 92)[0]
    error_offset = struct.unpack_from("<I", payload, 100)[0]
    error = ""
    if string_offset and error_offset != NO_INDEX:
        error = table_string(payload, string_offset, error_offset)
    return {
        "version": struct.unpack_from("<H", payload, 4)[0],
        "state": struct.unpack_from("<I", payload, 8)[0],
        "phase": struct.unpack_from("<I", payload, 12)[0],
        "file_size_bytes": struct.unpack_from("<Q", payload, 16)[0],
        "bytes_read": struct.unpack_from("<Q", payload, 24)[0],
        "record_count": struct.unpack_from("<I", payload, 32)[0],
        "cell_count": struct.unpack_from("<I", payload, 36)[0],
        "reference_count": struct.unpack_from("<I", payload, 40)[0],
        "element_count": struct.unpack_from("<I", payload, 44)[0],
        "point_count": struct.unpack_from("<I", payload, 48)[0],
        "string_count": struct.unpack_from("<I", payload, 52)[0],
        "diagnostic_count": struct.unpack_from("<I", payload, 56)[0],
        "elapsed_micros": struct.unpack_from("<Q", payload, 60)[0],
        "open_micros": struct.unpack_from("<Q", payload, 68)[0],
        "parse_micros": struct.unpack_from("<Q", payload, 76)[0],
        "warmup_scheduled": bool(struct.unpack_from("<I", payload, 84)[0]),
        "warmup_ready": bool(struct.unpack_from("<I", payload, 88)[0]),
        "error": error,
        "payload_bytes": len(payload),
    }


def parse_catalog_summary(payload: bytes) -> dict:
    if payload[:4] != b"PLCS":
        raise AssertionError("catalog summary payload missing PLCS magic")
    layer_offset = struct.unpack_from("<I", payload, 64)[0]
    layer_summary_count = struct.unpack_from("<I", payload, 68)[0]
    string_offset = struct.unpack_from("<I", payload, 112)[0]
    bounds = None
    if struct.unpack_from("<I", payload, 20)[0]:
        bounds = {
            "x0": struct.unpack_from("<d", payload, 28)[0],
            "y0": struct.unpack_from("<d", payload, 36)[0],
            "x1": struct.unpack_from("<d", payload, 44)[0],
            "y1": struct.unpack_from("<d", payload, 52)[0],
        }
    layer_names = []
    for index in range(layer_summary_count):
        row = layer_offset + index * 32
        layer_names.append(table_string(payload, string_offset, struct.unpack_from("<I", payload, row)[0]))
    return {
        "source": struct.unpack_from("<I", payload, 12)[0],
        "shape_count": struct.unpack_from("<I", payload, 16)[0],
        "top_cell_index": struct.unpack_from("<I", payload, 24)[0],
        "bounds": bounds,
        "layer_count": struct.unpack_from("<I", payload, 60)[0],
        "layer_names": layer_names,
        "macro_count": struct.unpack_from("<I", payload, 72)[0],
        "component_count": struct.unpack_from("<I", payload, 76)[0],
        "def_pin_count": struct.unpack_from("<I", payload, 80)[0],
        "net_count": struct.unpack_from("<I", payload, 84)[0],
        "cell_count": struct.unpack_from("<I", payload, 88)[0],
        "reference_count": struct.unpack_from("<I", payload, 92)[0],
        "element_count": struct.unpack_from("<I", payload, 96)[0],
        "point_count": struct.unpack_from("<I", payload, 100)[0],
        "string_count": struct.unpack_from("<I", payload, 104)[0],
        "diagnostic_count": struct.unpack_from("<I", payload, 108)[0],
        "parse_micros": struct.unpack_from("<Q", payload, 120)[0],
        "open_micros": struct.unpack_from("<Q", payload, 144)[0],
    }


def parse_catalog_page(payload: bytes, table_kind: int) -> dict:
    if payload[:4] != b"PLCP":
        raise AssertionError("catalog page payload missing PLCP magic")
    actual_kind = struct.unpack_from("<I", payload, 8)[0]
    if actual_kind != table_kind:
        raise AssertionError(f"catalog page kind mismatch: {actual_kind} != {table_kind}")
    row_offset = 40
    offset = struct.unpack_from("<I", payload, 12)[0]
    count = struct.unpack_from("<I", payload, 16)[0]
    total = struct.unpack_from("<I", payload, 20)[0]
    next_offset = struct.unpack_from("<I", payload, 24)[0]
    string_offset = struct.unpack_from("<I", payload, 28)[0]
    result = {
        "offset": offset,
        "count": count,
        "total": total,
        "next_offset": next_offset,
        "payload_bytes": len(payload),
    }
    if table_kind == CATALOG_PAGE_CELLS and count:
        first_name = table_string(payload, string_offset, struct.unpack_from("<I", payload, row_offset)[0])
        result["first_name"] = first_name
        result["first_flags"] = struct.unpack_from("<I", payload, row_offset + 20)[0]
    elif table_kind == CATALOG_PAGE_LAYERS:
        names = []
        for index in range(min(count, 16)):
            row = row_offset + index * 32
            names.append(table_string(payload, string_offset, struct.unpack_from("<I", payload, row)[0]))
        result["layer_names"] = names
    return result


def parse_catalog(payload: bytes) -> dict:
    if payload[:4] != b"PLCT":
        raise AssertionError("catalog payload missing PLCT magic")
    top_cell_index = struct.unpack_from("<I", payload, 24)[0]
    string_table_offset = struct.unpack_from("<I", payload, 28)[0]
    layer_count = struct.unpack_from("<I", payload, 36)[0]
    layer_offset = struct.unpack_from("<I", payload, 40)[0]
    cell_count = struct.unpack_from("<I", payload, 92)[0]
    cell_offset = struct.unpack_from("<I", payload, 96)[0]
    reference_count = struct.unpack_from("<I", payload, 100)[0]
    element_count = struct.unpack_from("<I", payload, 108)[0]
    if cell_count == 0 or element_count == 0 or layer_count == 0:
        raise AssertionError("GDS catalog is unexpectedly empty")
    top_name = ""
    top_bounds = None
    if top_cell_index < cell_count:
        top_row = cell_offset + top_cell_index * 56
        top_name = table_string(payload, string_table_offset, struct.unpack_from("<I", payload, top_row)[0])
        if struct.unpack_from("<I", payload, top_row + 20)[0]:
            top_bounds = {
                "x0": struct.unpack_from("<d", payload, top_row + 24)[0],
                "y0": struct.unpack_from("<d", payload, top_row + 32)[0],
                "x1": struct.unpack_from("<d", payload, top_row + 40)[0],
                "y1": struct.unpack_from("<d", payload, top_row + 48)[0],
            }
    layer_names = []
    for index in range(min(layer_count, 16)):
        row = layer_offset + index * 32
        layer_names.append(table_string(payload, string_table_offset, struct.unpack_from("<I", payload, row)[0]))
    return {
        "top_cell_index": top_cell_index,
        "top_name": top_name,
        "top_bounds": top_bounds,
        "layer_names": layer_names,
        "cell_count": cell_count,
        "reference_count": reference_count,
        "element_count": element_count,
        "layer_count": layer_count,
    }


def parse_tile(payload: bytes, wall_micros: int, flags: int) -> dict:
    if payload[:4] != b"PLTG":
        raise AssertionError("tile payload missing PLTG magic")
    header_size = struct.unpack_from("<H", payload, 6)[0]
    geometry_offset = struct.unpack_from("<I", payload, 16)[0]
    geometry_size = struct.unpack_from("<I", payload, 20)[0]
    shape_count = struct.unpack_from("<I", payload, 24)[0]
    point_count = 0
    sample_points: list[list[float]] = []
    if geometry_size and payload[geometry_offset : geometry_offset + 4] == b"PLGE":
        geometry = payload[geometry_offset : geometry_offset + geometry_size]
        point_count = struct.unpack_from("<I", geometry, 16)[0]
        x0_offset = struct.unpack_from("<I", geometry, 28)[0]
        y0_offset = struct.unpack_from("<I", geometry, 32)[0]
        x1_offset = struct.unpack_from("<I", geometry, 36)[0]
        y1_offset = struct.unpack_from("<I", geometry, 40)[0]
        for index in range(min(shape_count, 8)):
            x0 = struct.unpack_from("<d", geometry, x0_offset + index * 8)[0]
            y0 = struct.unpack_from("<d", geometry, y0_offset + index * 8)[0]
            x1 = struct.unpack_from("<d", geometry, x1_offset + index * 8)[0]
            y1 = struct.unpack_from("<d", geometry, y1_offset + index * 8)[0]
            if math.isfinite(x0) and math.isfinite(y0) and math.isfinite(x1) and math.isfinite(y1):
                sample_points.append([(x0 + x1) / 2.0, (y0 + y1) / 2.0])
    result = {
        "wall_micros": wall_micros,
        "frame_flags": flags,
        "truncated": bool(struct.unpack_from("<I", payload, 8)[0] & 1),
        "next_token": struct.unpack_from("<I", payload, 12)[0],
        "payload_bytes": len(payload),
        "geometry_bytes": geometry_size,
        "shape_count": shape_count,
        "point_count": point_count,
        "index_build_micros": struct.unpack_from("<Q", payload, 32)[0],
        "query_micros": struct.unpack_from("<Q", payload, 40)[0],
        "encode_micros": struct.unpack_from("<Q", payload, 48)[0],
        "visited_cells": struct.unpack_from("<I", payload, 56)[0],
        "element_candidates": struct.unpack_from("<I", payload, 60)[0],
        "reference_candidates": struct.unpack_from("<I", payload, 64)[0],
        "traversed_refs": struct.unpack_from("<I", payload, 68)[0],
        "lod_shapes": struct.unpack_from("<I", payload, 72)[0],
        "cache_hits": struct.unpack_from("<I", payload, 76)[0],
        "cache_misses": struct.unpack_from("<I", payload, 80)[0],
        "grid_build_micros": 0,
        "grid_hits": 0,
        "grid_misses": 0,
        "grid_candidates": 0,
        "grid_bins": 0,
        "sample_points": sample_points,
    }
    if header_size >= 108:
        result["grid_build_micros"] = struct.unpack_from("<Q", payload, 84)[0]
        result["grid_hits"] = struct.unpack_from("<I", payload, 92)[0]
        result["grid_misses"] = struct.unpack_from("<I", payload, 96)[0]
        result["grid_candidates"] = struct.unpack_from("<I", payload, 100)[0]
        result["grid_bins"] = struct.unpack_from("<I", payload, 104)[0]
    return result


def parse_hit(payload: bytes, wall_micros: int) -> dict:
    if payload[:4] != b"PLHT":
        raise AssertionError("hit payload missing PLHT magic")
    count = struct.unpack_from("<I", payload, 8)[0]
    result = {
        "wall_micros": wall_micros,
        "payload_bytes": len(payload),
        "hit_count": count,
        "index_build_micros": struct.unpack_from("<Q", payload, 32)[0],
        "query_micros": struct.unpack_from("<Q", payload, 40)[0],
        "encode_micros": struct.unpack_from("<Q", payload, 48)[0],
        "tile_shapes": struct.unpack_from("<I", payload, 56)[0],
        "precise_candidates": struct.unpack_from("<I", payload, 60)[0],
        "object": None,
    }
    if count:
        row = struct.unpack_from("<I", payload, 12)[0]
        result["object"] = {
            "kind": struct.unpack_from("<H", payload, row)[0],
            "cell_index": struct.unpack_from("<I", payload, row + 4)[0],
            "reference_index": struct.unpack_from("<I", payload, row + 8)[0],
            "element_index": struct.unpack_from("<I", payload, row + 12)[0],
            "layer_index": struct.unpack_from("<I", payload, row + 16)[0],
            "datatype": struct.unpack_from("<I", payload, row + 20)[0],
            "instance_path_hash": struct.unpack_from("<Q", payload, row + 32)[0],
        }
    return result


def parse_search(payload: bytes, wall_micros: int) -> dict:
    if payload[:4] != b"PLSR":
        raise AssertionError("search payload missing PLSR magic")
    count = struct.unpack_from("<I", payload, 8)[0]
    row_offset = struct.unpack_from("<I", payload, 12)[0]
    row_stride = struct.unpack_from("<I", payload, 16)[0]
    string_offset = struct.unpack_from("<I", payload, 20)[0]
    results = []
    for index in range(count):
        row = row_offset + index * row_stride
        label_offset = struct.unpack_from("<I", payload, row + 24)[0]
        results.append(
            {
                "kind": struct.unpack_from("<H", payload, row)[0],
                "class": struct.unpack_from("<H", payload, row + 2)[0],
                "cell_index": struct.unpack_from("<I", payload, row + 4)[0],
                "reference_index": struct.unpack_from("<I", payload, row + 8)[0],
                "element_index": struct.unpack_from("<I", payload, row + 12)[0],
                "layer_index": struct.unpack_from("<I", payload, row + 16)[0],
                "datatype": struct.unpack_from("<I", payload, row + 20)[0],
                "label": table_string(payload, string_offset, label_offset),
                "instance_path_hash": struct.unpack_from("<Q", payload, row + 32)[0],
                "source_cell_index": struct.unpack_from("<I", payload, row + 40)[0],
                "rank": struct.unpack_from("<I", payload, row + 44)[0],
            }
        )
    return {
        "wall_micros": wall_micros,
        "payload_bytes": len(payload),
        "result_count": count,
        "index_build_micros": struct.unpack_from("<Q", payload, 32)[0],
        "query_micros": struct.unpack_from("<Q", payload, 40)[0],
        "encode_micros": struct.unpack_from("<Q", payload, 48)[0],
        "results": results,
    }


def make_bbox(bounds: dict, scale: float, dx: float = 0.0, dy: float = 0.0) -> tuple[float, float, float, float]:
    x0 = float(bounds["x0"])
    y0 = float(bounds["y0"])
    x1 = float(bounds["x1"])
    y1 = float(bounds["y1"])
    cx = (x0 + x1) / 2.0 + (x1 - x0) * dx
    cy = (y0 + y1) / 2.0 + (y1 - y0) * dy
    width = max((x1 - x0) * scale, 1.0)
    height = max((y1 - y0) * scale, 1.0)
    return (cx - width / 2.0, cy - height / 2.0, cx + width / 2.0, cy + height / 2.0)


def compare_baseline(metrics: dict) -> None:
    baseline_path = os.environ.get("PRISTINE_LAYOUT_PERF_BASELINE")
    enforce = os.environ.get("PRISTINE_LAYOUT_PERF_ENFORCE") == "1"
    if not baseline_path:
        return
    baseline = json.loads(pathlib.Path(baseline_path).read_text(encoding="utf-8"))
    failures = []
    for key in [
        "tile_query_micros",
        "tile_encode_micros",
        "hit_query_micros",
        "search_query_micros",
        "open_wall_micros",
    ]:
        current = metrics.get("summary", {}).get(key, {}).get("p95")
        previous = baseline.get("summary", {}).get(key, {}).get("p95")
        if current is None or previous in (None, 0):
            continue
        if current > int(math.ceil(previous * 1.25)):
            failures.append(f"{key} p95 {current}us > baseline {previous}us by more than 25%")
    if failures and enforce:
        raise AssertionError("; ".join(failures))
    if failures:
        print("PERF WARNING: " + "; ".join(failures))


def run_interaction(server_path: pathlib.Path, gds_path: pathlib.Path) -> dict:
    trace(f"starting engine {server_path}")
    process: subprocess.Popen[bytes] | None = None
    request_id = 1
    try:
        process = subprocess.Popen(
            [str(server_path), "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        workspace_uri = gds_path.parent.as_uri()
        trace("initialize")
        request(
            process,
            request_id,
            "initialize",
            {
                "processId": None,
                "rootUri": workspace_uri,
                "workspaceFolders": [{"uri": workspace_uri, "name": "tt-tinyqv"}],
                "capabilities": {},
            },
        )
        request_id += 1
        notify(process, "initialized", {})

        trace("cancel staged GDS layout session")
        cancel_open_start = now_micros()
        cancel_session = request(
            process,
            request_id,
            "systemverilog/layout/open",
            {"gdsUri": gds_path.as_uri(), "title": f"cancel-{gds_path.name}", "openMode": "staged"},
        )["result"]
        cancel_lsp_open_wall_micros = now_micros() - cancel_open_start
        request_id += 1
        if cancel_session["protocol"] != "pristine-layout-columnar-v3" or cancel_session["source"] != "gds":
            raise AssertionError(f"bad cancel session metadata: {cancel_session}")
        cancel_close_start = now_micros()
        request(
            process,
            request_id,
            "systemverilog/layout/close",
            {"sessionId": cancel_session["sessionId"]},
        )
        cancel_close_wall_micros = now_micros() - cancel_close_start
        request_id += 1

        trace("open GDS layout session")
        open_start = now_micros()
        session = request(
            process,
            request_id,
            "systemverilog/layout/open",
            {"gdsUri": gds_path.as_uri(), "title": gds_path.name, "openMode": "auto"},
        )["result"]
        lsp_open_wall_micros = now_micros() - open_start
        request_id += 1
        if session["protocol"] != "pristine-layout-columnar-v3" or session["source"] != "gds":
            raise AssertionError(f"bad layout session metadata: {session}")
        metrics: dict = {
            "fixture": str(gds_path),
            "session": {
                "protocol": session["protocol"],
                "source": session["source"],
                "cellCount": session.get("cellCount", 0),
                "referenceCount": session.get("referenceCount", 0),
                "elementCount": session.get("elementCount", 0),
                "layerCount": session.get("layerCount", 0),
                "diagnosticCount": session.get("diagnosticCount", 0),
                "gdsMetrics": session.get("gdsMetrics", {}),
                "status": session.get("status"),
                "deferred": bool(session.get("deferred", False)),
            },
            "warmup": {
                "scheduled": bool(session.get("gdsMetrics", {}).get("warmupScheduled", False)),
                "pointArenaCount": int(session.get("gdsMetrics", {}).get("pointArenaCount", 0)),
            },
            "lsp_open_wall_micros": [lsp_open_wall_micros],
            "cancel_lsp_open_wall_micros": [cancel_lsp_open_wall_micros],
            "cancel_close_wall_micros": [cancel_close_wall_micros],
            "open_wall_micros": [],
            "ready_wall_micros": [],
            "status_wall_micros": [],
            "status_samples": [],
            "pending_errors": 0,
            "tiles": [],
            "hits": [],
            "searches": [],
            "inspect_wall_micros": [],
            "selection_wall_micros": [],
            "error_frames": 0,
        }

        with connect_pipe(session["endpoint"]["kind"], session["endpoint"]["path"]) as pipe:
            trace("pipe hello")
            _flags, _wall, hello, _ = send_pipe(pipe, HELLO, 10, expect=HELLO_RESPONSE)
            if struct.unpack_from("<H", hello, 0)[0] != PROTOCOL_VERSION:
                raise AssertionError("hello payload did not report v3")

            if metrics["session"]["deferred"]:
                pending = maybe_pipe_error(pipe, CATALOG_SUMMARY_REQUEST, 9000, b"")
                if pending is not None:
                    metrics["pending_errors"] += 1

            trace("status until ready")
            ready_status = None
            status_start = now_micros()
            for poll_index in range(2400):
                _flags, status_wall, status_bytes, _ = send_pipe(
                    pipe, STATUS_REQUEST, 10_000 + poll_index, status_payload(), expect=STATUS_RESPONSE
                )
                status = parse_status(status_bytes)
                metrics["status_wall_micros"].append(status_wall)
                metrics["status_samples"].append(status)
                if status["state"] == STATUS_STATE_READY:
                    ready_status = status
                    break
                if status["state"] == STATUS_STATE_FAILED:
                    raise AssertionError(f"GDS staged open failed: {status.get('error', '<missing error>')}")
                time.sleep(0.05)
            if ready_status is None:
                raise AssertionError("GDS staged open did not reach ready within 120s")
            previous_phase = 0
            previous_counts = {
                "record_count": 0,
                "cell_count": 0,
                "reference_count": 0,
                "element_count": 0,
                "point_count": 0,
                "string_count": 0,
                "diagnostic_count": 0,
            }
            for sample in metrics["status_samples"]:
                phase = sample["phase"]
                if phase and phase < previous_phase:
                    raise AssertionError(f"status phase regressed: {phase} < {previous_phase}")
                previous_phase = max(previous_phase, phase)
                for field, previous in previous_counts.items():
                    current = sample[field]
                    if current < previous:
                        raise AssertionError(f"status {field} regressed: {current} < {previous}")
                    previous_counts[field] = current
            ready_wall_micros = now_micros() - open_start
            metrics["ready_wall_micros"].append(ready_wall_micros)
            metrics["open_wall_micros"].append(ready_wall_micros)
            metrics["status_poll_count"] = len(metrics["status_samples"])
            metrics["ready_status"] = ready_status
            metrics["status_progress"] = status_progress_summary(metrics["status_samples"])
            metrics["warmup"]["scheduled"] = ready_status["warmup_scheduled"]
            metrics["warmup"]["ready_at_layout_ready"] = ready_status["warmup_ready"]
            metrics["warmup"]["ready"] = ready_status["warmup_ready"]
            metrics["warmup"]["pointArenaCount"] = ready_status["point_count"]
            warmup_wait_start = now_micros()
            warmup_ready_status = ready_status
            if ready_status["warmup_scheduled"] and not ready_status["warmup_ready"]:
                for warmup_poll in range(200):
                    _flags, status_wall, status_bytes, _ = send_pipe(
                        pipe,
                        STATUS_REQUEST,
                        12_000 + warmup_poll,
                        status_payload(),
                        expect=STATUS_RESPONSE,
                    )
                    status = parse_status(status_bytes)
                    metrics["status_wall_micros"].append(status_wall)
                    metrics["status_samples"].append(status)
                    if status["state"] == STATUS_STATE_FAILED:
                        raise AssertionError(f"GDS warmup status failed: {status.get('error', '<missing error>')}")
                    if status["warmup_ready"]:
                        warmup_ready_status = status
                        metrics["warmup"]["ready"] = True
                        break
                    time.sleep(0.01)
            metrics["warmup"]["readyWallMicros"] = now_micros() - warmup_wait_start
            metrics["warmup"]["readyStatus"] = warmup_ready_status
            trace(
                "ready after "
                f"{ready_wall_micros}us ({len(metrics['status_samples'])} status polls, "
                f"parse {ready_status['parse_micros']}us)"
            )

            trace("catalog summary")
            _flags, summary_wall, summary_payload, _ = send_pipe(
                pipe, CATALOG_SUMMARY_REQUEST, 11, expect=CATALOG_SUMMARY_RESPONSE
            )
            catalog = parse_catalog_summary(summary_payload)
            metrics["session"]["cellCount"] = catalog.get("cell_count", metrics["session"].get("cellCount", 0))
            metrics["session"]["referenceCount"] = catalog.get(
                "reference_count", metrics["session"].get("referenceCount", 0)
            )
            metrics["session"]["elementCount"] = catalog.get(
                "element_count", metrics["session"].get("elementCount", 0)
            )
            metrics["session"]["layerCount"] = catalog.get("layer_count", metrics["session"].get("layerCount", 0))
            metrics["session"]["diagnosticCount"] = catalog.get(
                "diagnostic_count", metrics["session"].get("diagnosticCount", 0)
            )
            metrics["catalog_summary_wall_micros"] = [summary_wall]
            trace("catalog layer page")
            _flags, layer_page_wall, layer_page_payload_bytes, _ = send_pipe(
                pipe,
                CATALOG_PAGE_REQUEST,
                12,
                catalog_page_payload(CATALOG_PAGE_LAYERS, 0, 64),
                expect=CATALOG_PAGE_RESPONSE,
            )
            layer_page = parse_catalog_page(layer_page_payload_bytes, CATALOG_PAGE_LAYERS)
            metrics["catalog_page_wall_micros"] = [layer_page_wall]
            metrics["catalog_pages"] = [layer_page]
            trace("catalog cell page")
            _flags, cell_page_wall, cell_page_payload_bytes, _ = send_pipe(
                pipe,
                CATALOG_PAGE_REQUEST,
                13,
                catalog_page_payload(CATALOG_PAGE_CELLS, 0, 64),
                expect=CATALOG_PAGE_RESPONSE,
            )
            cell_page = parse_catalog_page(cell_page_payload_bytes, CATALOG_PAGE_CELLS)
            metrics["catalog_page_wall_micros"].append(cell_page_wall)
            metrics["catalog_pages"].append(cell_page)
            top_index_for_name = catalog.get("top_cell_index")
            if top_index_for_name is not None and top_index_for_name != NO_INDEX:
                trace(f"catalog top cell page offset={top_index_for_name}")
                _flags, top_cell_page_wall, top_cell_page_payload_bytes, _ = send_pipe(
                    pipe,
                    CATALOG_PAGE_REQUEST,
                    14,
                    catalog_page_payload(CATALOG_PAGE_CELLS, int(top_index_for_name), 1),
                    expect=CATALOG_PAGE_RESPONSE,
                )
                top_cell_page = parse_catalog_page(top_cell_page_payload_bytes, CATALOG_PAGE_CELLS)
                metrics["catalog_page_wall_micros"].append(top_cell_page_wall)
                metrics["catalog_pages"].append(top_cell_page)
                catalog["top_name"] = top_cell_page.get("first_name", "")
            if layer_page.get("layer_names"):
                catalog["layer_names"] = layer_page["layer_names"]
            metrics["catalog"] = catalog
            top = catalog["top_cell_index"]
            bounds = session.get("bounds") or catalog.get("bounds")
            if not bounds:
                raise AssertionError("GDS catalog did not report top-cell bounds")

            full_bbox = (
                float(bounds["x0"]),
                float(bounds["y0"]),
                float(bounds["x1"]),
                float(bounds["y1"]),
            )
            if full_bbox[2] <= full_bbox[0] or full_bbox[3] <= full_bbox[1]:
                raise AssertionError(f"invalid GDS bounds: {bounds}")

            scenarios = [("full_lod2", full_bbox, 2)]
            for dx in (-0.25, 0.25):
                for dy in (-0.25, 0.25):
                    scenarios.append((f"overview_quadrant_{dx}_{dy}", make_bbox(bounds, 0.35, dx, dy), 2))
            scenarios.append(("wide_lod1_cold", make_bbox(bounds, 0.20), 1))
            scenarios.append(("wide_lod1_warm_overlap", make_bbox(bounds, 0.18, 0.03, -0.02), 1))
            scenarios.append(("wide_lod1_warm_neighbor", make_bbox(bounds, 0.18, -0.03, 0.02), 1))
            for scale, lod in [(0.05, 1), (0.02, 0), (0.01, 0)]:
                scenarios.append((f"center_s{scale}_lod{lod}", make_bbox(bounds, scale), lod))
            for dx in (-0.25, 0.25):
                for dy in (-0.25, 0.25):
                    scenarios.append((f"quadrant_{dx}_{dy}", make_bbox(bounds, 0.04, dx, dy), 1))
            for ix in range(5):
                for iy in range(5):
                    scenarios.append(
                        (
                            f"pan_{ix}_{iy}",
                            make_bbox(bounds, 0.025, (ix - 2) * 0.035, (iy - 2) * 0.035),
                            1,
                        )
                    )
            for index in range(60):
                scale = 0.008 + (index % 6) * 0.004
                dx = ((index % 10) - 4.5) * 0.01
                dy = (((index // 10) % 6) - 2.5) * 0.014
                scenarios.append((f"burst_{index}", make_bbox(bounds, scale, dx, dy), index % 3))

            next_request_id = 100
            for name, bbox, lod in scenarios:
                trace(f"tile {name} lod={lod}")
                flags, wall, payload, _ = send_pipe(
                    pipe,
                    TILE_GEOMETRY_REQUEST,
                    next_request_id,
                    tile_payload(top, bbox, lod),
                    expect=TILE_GEOMETRY_RESPONSE,
                )
                next_request_id += 1
                parsed = parse_tile(payload, wall, flags)
                parsed["name"] = name
                parsed["lod"] = lod
                metrics["tiles"].append(parsed)

            repeat_bbox = make_bbox(bounds, 0.20)
            for index in range(3):
                trace(f"tile repeat_cache_{index}")
                flags, wall, payload, _ = send_pipe(
                    pipe,
                    TILE_GEOMETRY_REQUEST,
                    next_request_id,
                    tile_payload(top, repeat_bbox, 1),
                    expect=TILE_GEOMETRY_RESPONSE,
                )
                next_request_id += 1
                parsed = parse_tile(payload, wall, flags)
                parsed["name"] = f"repeat_cache_{index}"
                parsed["lod"] = 1
                metrics["tiles"].append(parsed)

            trace("tile continuation_first")
            flags, wall, payload, _ = send_pipe(
                pipe,
                TILE_GEOMETRY_REQUEST,
                next_request_id,
                tile_payload(top, full_bbox, 2, max_shapes=1, max_bytes=1_000_000),
                expect=TILE_GEOMETRY_RESPONSE,
            )
            next_request_id += 1
            first_page = parse_tile(payload, wall, flags)
            first_page["name"] = "continuation_first"
            first_page["lod"] = 2
            metrics["tiles"].append(first_page)
            if first_page["next_token"]:
                trace("tile continuation_second")
                flags, wall, payload, _ = send_pipe(
                    pipe,
                    TILE_GEOMETRY_REQUEST,
                    next_request_id,
                    tile_payload(
                        top,
                        full_bbox,
                        2,
                        max_shapes=1,
                        max_bytes=1_000_000,
                        continuation_token=first_page["next_token"],
                    ),
                    expect=TILE_GEOMETRY_RESPONSE,
                )
                next_request_id += 1
                second_page = parse_tile(payload, wall, flags)
                second_page["name"] = "continuation_second"
                second_page["lod"] = 2
                metrics["tiles"].append(second_page)

            trace("tile bad-token error")
            message = expect_pipe_error(
                pipe,
                TILE_GEOMETRY_REQUEST,
                next_request_id,
                tile_payload(top, full_bbox, 2, max_shapes=1, continuation_token=0x00ABCDEF),
            )
            next_request_id += 1
            if "continuation token" not in message:
                raise AssertionError(f"bad token returned unexpected error: {message}")
            metrics["error_frames"] += 1

            hit_object = None
            hit_radius = max(full_bbox[2] - full_bbox[0], full_bbox[3] - full_bbox[1]) / 10000.0
            hit_points: list[tuple[float, float]] = []
            for tile in metrics["tiles"]:
                for point in tile.get("sample_points", []):
                    hit_points.append((float(point[0]), float(point[1])))
                    if len(hit_points) >= 12:
                        break
                if len(hit_points) >= 12:
                    break
            for dx, dy in [(0.0, 0.0), (-0.25, -0.25), (0.25, 0.25), (-0.25, 0.25), (0.25, -0.25)]:
                bbox = make_bbox(bounds, 0.01, dx, dy)
                hit_points.append(((bbox[0] + bbox[2]) / 2.0, (bbox[1] + bbox[3]) / 2.0))
            for x, y in hit_points:
                trace("hit-test")
                _flags, wall, payload, _ = send_pipe(
                    pipe,
                    HIT_TEST_REQUEST,
                    next_request_id,
                    hit_payload(top, x, y, hit_radius),
                    expect=HIT_TEST_RESPONSE,
                )
                next_request_id += 1
                hit = parse_hit(payload, wall)
                hit["point"] = [x, y]
                metrics["hits"].append(hit)
                if hit["object"] is not None:
                    hit_object = hit["object"]
                    break
            if hit_object is None:
                raise AssertionError("hit-test did not return any selectable object")

            object_bytes = object_payload(
                hit_object["kind"],
                hit_object["cell_index"],
                hit_object["reference_index"],
                hit_object["element_index"],
                hit_object["layer_index"],
                hit_object["datatype"],
                hit_object["instance_path_hash"],
            )
            trace("inspect hit object")
            _flags, wall, inspect_payload_bytes, _ = send_pipe(
                pipe, INSPECT_REQUEST, next_request_id, object_bytes, expect=INSPECT_RESPONSE
            )
            next_request_id += 1
            if inspect_payload_bytes[:4] != b"PLIN":
                raise AssertionError("inspect response missing PLIN magic")
            metrics["inspect_wall_micros"].append(wall)

            trace("selection hit object")
            _flags, wall, selection_payload, _ = send_pipe(
                pipe,
                SELECTION_GEOMETRY_REQUEST,
                next_request_id,
                object_bytes,
                expect=SELECTION_GEOMETRY_RESPONSE,
            )
            next_request_id += 1
            if selection_payload[:4] != b"PLGE":
                raise AssertionError("selection response missing PLGE magic")
            metrics["selection_wall_micros"].append(wall)

            queries = [
                ("top_cell", catalog["top_name"], SEARCH_KIND_CELL | SEARCH_KIND_REFERENCE),
                ("generic_tt", "tt", SEARCH_KIND_CELL | SEARCH_KIND_REFERENCE | SEARCH_KIND_TEXT),
                ("layer", catalog["layer_names"][0] if catalog["layer_names"] else "GDS:", SEARCH_KIND_LAYER),
            ]
            search_object = None
            for name, query, kind_mask in queries:
                trace(f"search {name}")
                _flags, wall, payload, _ = send_pipe(
                    pipe,
                    SEARCH_REQUEST,
                    next_request_id,
                    search_payload(query, 16, kind_mask, top),
                    expect=SEARCH_RESPONSE,
                )
                next_request_id += 1
                search = parse_search(payload, wall)
                search["name"] = name
                search["query"] = query
                metrics["searches"].append(search)
                if search["result_count"] == 0:
                    raise AssertionError(f"search {name!r} for {query!r} returned no results")
                if search_object is None:
                    search_object = search["results"][0]

            if search_object is None:
                raise AssertionError("search did not produce an object")
            search_object_bytes = object_payload(
                search_object["kind"],
                search_object["cell_index"],
                search_object["reference_index"],
                search_object["element_index"],
                search_object["layer_index"],
                search_object["datatype"],
                search_object["instance_path_hash"],
            )
            trace("inspect search object")
            trace("selection search object")
            _flags, wall, payload, _ = send_pipe(
                pipe, INSPECT_REQUEST, next_request_id, search_object_bytes, expect=INSPECT_RESPONSE
            )
            next_request_id += 1
            if payload[:4] != b"PLIN":
                raise AssertionError("search inspect response missing PLIN magic")
            metrics["inspect_wall_micros"].append(wall)

            _flags, wall, payload, _ = send_pipe(
                pipe,
                SELECTION_GEOMETRY_REQUEST,
                next_request_id,
                search_object_bytes,
                expect=SELECTION_GEOMETRY_RESPONSE,
            )
            next_request_id += 1
            if payload[:4] != b"PLGE":
                raise AssertionError("search selection response missing PLGE magic")
            metrics["selection_wall_micros"].append(wall)

            _flags, status_wall, status_bytes, _ = send_pipe(
                pipe,
                STATUS_REQUEST,
                next_request_id,
                status_payload(),
                expect=STATUS_RESPONSE,
            )
            next_request_id += 1
            final_status = parse_status(status_bytes)
            metrics["status_wall_micros"].append(status_wall)
            metrics["status_samples"].append(final_status)
            metrics["final_status"] = final_status
            if final_status["warmup_ready"]:
                metrics["warmup"]["ready"] = True
                metrics["warmup"]["readyStatus"] = final_status
                metrics["warmup"]["readyWallMicros"] = now_micros() - warmup_wait_start

            pipe.write(frame(CLOSE, next_request_id))
            pipe.flush()

        request(process, request_id, "systemverilog/layout/close", {"sessionId": session["sessionId"]})
        request_id += 1
        request(process, request_id, "shutdown", None)
        notify(process, "exit", None)
        process.wait(timeout=5)
        process = None

        tile_query = [entry["query_micros"] for entry in metrics["tiles"]]
        tile_encode = [entry["encode_micros"] for entry in metrics["tiles"]]
        hit_query = [entry["query_micros"] for entry in metrics["hits"]]
        search_query = [entry["query_micros"] for entry in metrics["searches"]]
        tile_groups: dict[str, list[dict]] = {}
        for entry in metrics["tiles"]:
            tile_groups.setdefault(tile_group(entry["name"]), []).append(entry)
        parse_metrics = metrics["session"].get("gdsMetrics", {}).get("parseMetrics", {})
        metrics["summary"] = {
            "warmup_scheduled": metrics.get("warmup", {}).get("scheduled", False),
            "warmup_ready_at_layout_ready": metrics.get("warmup", {}).get(
                "ready_at_layout_ready", False
            ),
            "warmup_ready": metrics.get("warmup", {}).get("ready", False),
            "warmup_ready_wall_micros": metric_summary(
                [int(metrics.get("warmup", {}).get("readyWallMicros", 0))]
            ),
            "point_arena_count": metrics.get("warmup", {}).get("pointArenaCount", 0),
            "lsp_open_wall_micros": metric_summary(metrics["lsp_open_wall_micros"]),
            "cancel_lsp_open_wall_micros": metric_summary(metrics["cancel_lsp_open_wall_micros"]),
            "cancel_close_wall_micros": metric_summary(metrics["cancel_close_wall_micros"]),
            "open_wall_micros": metric_summary(metrics["open_wall_micros"]),
            "ready_wall_micros": metric_summary(metrics["ready_wall_micros"]),
            "status_wall_micros": metric_summary(metrics["status_wall_micros"]),
            "status_poll_count": metrics.get("status_poll_count", 0),
            "pending_error_count": metrics.get("pending_errors", 0),
            "status_progress": metrics.get("status_progress", {}),
            "status_parse_micros": metric_summary(
                [int(metrics.get("ready_status", {}).get("parse_micros", 0))]
            ),
            "status_open_micros": metric_summary(
                [int(metrics.get("ready_status", {}).get("open_micros", 0))]
            ),
            "element_finalize_micros": metric_summary([int(parse_metrics.get("elementFinalizeMicros", 0))]),
            "element_finalize_bbox_micros": metric_summary(
                [int(parse_metrics.get("elementFinalizeBboxMicros", 0))]
            ),
            "element_finalize_reference_micros": metric_summary(
                [int(parse_metrics.get("elementFinalizeReferenceMicros", 0))]
            ),
            "element_finalize_index_micros": metric_summary(
                [int(parse_metrics.get("elementFinalizeIndexMicros", 0))]
            ),
            "element_finalize_sample_micros": metric_summary(
                [int(parse_metrics.get("elementFinalizeSampleMicros", 0))]
            ),
            "catalog_summary_wall_micros": metric_summary(metrics.get("catalog_summary_wall_micros", [])),
            "catalog_page_wall_micros": metric_summary(metrics.get("catalog_page_wall_micros", [])),
            "catalog_page_payload_bytes": metric_summary(
                [entry["payload_bytes"] for entry in metrics.get("catalog_pages", [])]
            ),
            "tile_query_micros": metric_summary(tile_query),
            "tile_encode_micros": metric_summary(tile_encode),
            "tile_wall_micros": metric_summary([entry["wall_micros"] for entry in metrics["tiles"]]),
            "hit_query_micros": metric_summary(hit_query),
            "hit_wall_micros": metric_summary([entry["wall_micros"] for entry in metrics["hits"]]),
            "search_query_micros": metric_summary(search_query),
            "search_wall_micros": metric_summary([entry["wall_micros"] for entry in metrics["searches"]]),
            "inspect_wall_micros": metric_summary(metrics["inspect_wall_micros"]),
            "selection_wall_micros": metric_summary(metrics["selection_wall_micros"]),
            "tile_payload_bytes": metric_summary([entry["payload_bytes"] for entry in metrics["tiles"]]),
            "tile_shape_count": metric_summary([entry["shape_count"] for entry in metrics["tiles"]]),
            "tile_point_count": metric_summary([entry["point_count"] for entry in metrics["tiles"]]),
            "tile_grid_build_micros": metric_summary(
                [entry.get("grid_build_micros", 0) for entry in metrics["tiles"]]
            ),
            "tile_grid_candidates": metric_summary(
                [entry.get("grid_candidates", 0) for entry in metrics["tiles"]]
            ),
            "tile_grid_hits": sum(entry.get("grid_hits", 0) for entry in metrics["tiles"]),
            "tile_grid_misses": sum(entry.get("grid_misses", 0) for entry in metrics["tiles"]),
            "tile_groups": {
                name: tile_group_summary(entries) for name, entries in sorted(tile_groups.items())
            },
        }
        return metrics
    finally:
        if process is not None:
            process.kill()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: tt_tinyqv_gds_interaction.py <pristine-engine> <tt_um_tt_tinyQV.gds>", file=sys.stderr)
        return 2
    server_path = pathlib.Path(sys.argv[1]).resolve()
    gds_path = pathlib.Path(sys.argv[2]).resolve()
    if not gds_path.is_file():
        repo_root = pathlib.Path(__file__).resolve().parents[2]
        fetch_script = repo_root / "scripts" / "fetch_tt_tinyqv_gds.mjs"
        if fetch_script.is_file():
            print(f"TT tinyQV GDS missing at {gds_path}; running {fetch_script}")
            subprocess.run(["node", str(fetch_script)], cwd=repo_root, check=False)
    if not gds_path.is_file():
        if os.environ.get("PRISTINE_REQUIRE_TT_TINYQV_GDS"):
            print(f"ERROR: required TT tinyQV GDS fixture is missing at {gds_path}", file=sys.stderr)
            return 1
        print(f"SKIP: missing optional TT tinyQV GDS fixture at {gds_path}")
        return 77

    output_path = server_path.parent / "tt_tinyqv_gds_interaction_metrics.json"
    previous_metrics = None
    if output_path.is_file():
        try:
            previous_metrics = json.loads(output_path.read_text(encoding="utf-8"))
            previous_path = output_path.with_suffix(".previous.json")
            previous_path.write_text(json.dumps(previous_metrics, indent=2, sort_keys=True), encoding="utf-8")
        except Exception:
            previous_metrics = None

    metrics = run_interaction(server_path, gds_path)
    if previous_metrics:
        metrics["previous_summary"] = previous_metrics.get("summary", {})
        metrics["before_after_delta_percent"] = summarize_before_after(previous_metrics, metrics)
    output_path.write_text(json.dumps(metrics, indent=2, sort_keys=True), encoding="utf-8")
    compare_baseline(metrics)

    summary = metrics["summary"]
    print(
        "TT tinyQV GDS interaction: "
        f"LSP open p95 {summary['lsp_open_wall_micros']['p95']}us, "
        f"cancel close p95 {summary['cancel_close_wall_micros']['p95']}us, "
        f"ready wall p95 {summary['ready_wall_micros']['p95']}us, "
        f"status parse {summary['status_parse_micros']['p95']}us, "
        f"element finalize {summary['element_finalize_micros']['p95']}us, "
        f"overview query max {summary['tile_groups'].get('overview', {}).get('query_micros', {}).get('max', 0)}us, "
        f"cold wide max {summary['tile_groups'].get('cold_wide_zoom', {}).get('query_micros', {}).get('max', 0)}us, "
        f"post-warm wide max {summary['tile_groups'].get('warm_wide_zoom', {}).get('query_micros', {}).get('max', 0)}us, "
        f"cache repeat wall max {summary['tile_groups'].get('cache_repeat', {}).get('wall_micros', {}).get('max', 0)}us, "
        f"tile query p50/p95/max {summary['tile_query_micros']['p50']}/"
        f"{summary['tile_query_micros']['p95']}/{summary['tile_query_micros']['max']}us, "
        f"hit query p95 {summary['hit_query_micros']['p95']}us, "
        f"search query p95 {summary['search_query_micros']['p95']}us, "
        f"status polls {summary.get('status_poll_count')}, "
        f"records/s {summary.get('status_progress', {}).get('records_per_second', 0)}, "
        f"points/s {summary.get('status_progress', {}).get('points_per_second', 0)}, "
        f"pending errors {summary.get('pending_error_count')}, "
        f"warmup scheduled {summary.get('warmup_scheduled')}, "
        f"warmup ready-at-ready {summary.get('warmup_ready_at_layout_ready')}, "
        f"warmup ready {summary.get('warmup_ready')}, "
        f"warmup wait {summary.get('warmup_ready_wall_micros', {}).get('p95', 0)}us, "
        f"point arena {summary.get('point_arena_count')}, "
        f"metrics {output_path}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
