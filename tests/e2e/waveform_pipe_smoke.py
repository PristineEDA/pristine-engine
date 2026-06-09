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
PROTOCOL_VERSION = 1

HELLO = 1
HELLO_RESPONSE = 2
CATALOG_REQUEST = 3
CATALOG_RESPONSE = 4
VIEWPORT_FRAME_REQUEST = 5
VIEWPORT_FRAME_RESPONSE = 6
CLOSE = 8
VIEWPORT_FRAME_REQUEST_V2 = 9
VIEWPORT_FRAME_RESPONSE_V2 = 10


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
    return FRAME_HEADER.pack(b"PWF1", PROTOCOL_VERSION, message_type, request_id, flags, len(payload), 0) + payload


def read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise RuntimeError("pipe closed while reading waveform frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(stream: BinaryIO) -> tuple[int, int, int, bytes]:
    header = read_exact(stream, FRAME_HEADER.size)
    magic, version, message_type, request_id, flags, payload_size, _reserved = FRAME_HEADER.unpack(header)
    if magic != b"PWF1":
        raise AssertionError(f"bad waveform frame magic: {magic!r}")
    if version != PROTOCOL_VERSION:
        raise AssertionError(f"bad waveform protocol version: {version}")
    return message_type, request_id, flags, read_exact(stream, payload_size)


def write_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def viewport_request_payload(signal_ids: list[str], start: float, end: float, max_segments: int) -> bytes:
    return (
        struct.pack("<ddfffII", start, end, 500.0, 24.0, 22.0, max_segments, len(signal_ids))
        + b"".join(write_string(signal_id) for signal_id in signal_ids)
    )


def viewport_request_payload_v2(
    signal_ids: list[str],
    prepared_start: float,
    prepared_end: float,
    viewport_start: float,
    viewport_end: float,
    max_segments: int,
) -> bytes:
    return (
        struct.pack(
            "<ddddfffII",
            prepared_start,
            prepared_end,
            viewport_start,
            viewport_end,
            500.0,
            24.0,
            22.0,
            max_segments,
            len(signal_ids),
        )
        + b"".join(write_string(signal_id) for signal_id in signal_ids)
    )


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
    raise RuntimeError(f"failed to connect waveform pipe {path}: {last_error}")


def append_fst_varint(output: bytearray, value: int) -> None:
    while value >> 7:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value & 0x7F)


def append_null_string(output: bytearray, value: str) -> None:
    output.extend(value.encode("utf-8"))
    output.append(0)


def append_fst_block(output: bytearray, block_type: int, payload: bytes | bytearray) -> None:
    output.append(block_type)
    output.extend(struct.pack(">Q", len(payload) + 8))
    output.extend(payload)


def write_tiny_fst_fixture(directory: pathlib.Path) -> pathlib.Path:
    data = bytearray()
    data.append(0)
    data.extend(struct.pack(">Q", 329))
    data.extend(struct.pack(">Q", 0))
    data.extend(struct.pack(">Q", 40))
    data.extend(struct.pack("d", 2.7182818284590452354))
    data.extend(struct.pack(">Q", 0))
    data.extend(struct.pack(">Q", 1))
    data.extend(struct.pack(">Q", 2))
    data.extend(struct.pack(">Q", 2))
    data.extend(struct.pack(">Q", 1))
    data.append(0xF7)  # int8 -9, nanoseconds.
    data.extend(bytes(128))
    data.extend(bytes(119))
    data.append(0)
    data.extend(struct.pack(">Q", 0))
    assert len(data) == 330

    hierarchy = bytearray()
    hierarchy.append(254)
    hierarchy.append(0)
    append_null_string(hierarchy, "tb")
    append_null_string(hierarchy, "")
    hierarchy.append(16)
    hierarchy.append(0)
    append_null_string(hierarchy, "clk")
    append_fst_varint(hierarchy, 1)
    append_fst_varint(hierarchy, 0)
    hierarchy.append(16)
    hierarchy.append(0)
    append_null_string(hierarchy, "data")
    append_fst_varint(hierarchy, 4)
    append_fst_varint(hierarchy, 0)
    hierarchy.append(255)
    append_fst_block(data, 4, hierarchy)

    geometry_data = bytearray()
    append_fst_varint(geometry_data, 1)
    append_fst_varint(geometry_data, 4)
    geometry = bytearray()
    geometry.extend(struct.pack(">Q", len(geometry_data)))
    geometry.extend(struct.pack(">Q", 2))
    geometry.extend(geometry_data)
    append_fst_block(data, 3, geometry)

    value_payload = bytearray()
    value_payload.extend(struct.pack(">Q", 0))
    value_payload.extend(struct.pack(">Q", 40))
    value_payload.extend(struct.pack(">Q", 0))

    initial_frame = bytearray(b"0xxxx")
    append_fst_varint(value_payload, len(initial_frame))
    append_fst_varint(value_payload, len(initial_frame))
    append_fst_varint(value_payload, 2)
    value_payload.extend(initial_frame)

    append_fst_varint(value_payload, 2)
    value_payload.append(ord("Z"))

    handle1_chain = bytearray()
    append_fst_varint(handle1_chain, 0)
    append_fst_varint(handle1_chain, 6)
    append_fst_varint(handle1_chain, 8)

    handle2_chain = bytearray()
    append_fst_varint(handle2_chain, 0)
    append_fst_varint(handle2_chain, 4)
    handle2_chain.append(0x30)
    append_fst_varint(handle2_chain, 5)
    handle2_chain.extend(b"zzzz")

    value_payload.extend(handle1_chain)
    value_payload.extend(handle2_chain)

    chain_index = bytearray()
    append_fst_varint(chain_index, 3)
    append_fst_varint(chain_index, 7)
    value_payload.extend(chain_index)
    value_payload.extend(struct.pack(">Q", len(chain_index)))

    time_table = bytearray()
    append_fst_varint(time_table, 0)
    append_fst_varint(time_table, 10)
    append_fst_varint(time_table, 5)
    append_fst_varint(time_table, 5)
    append_fst_varint(time_table, 10)
    value_payload.extend(time_table)
    value_payload.extend(struct.pack(">Q", len(time_table)))
    value_payload.extend(struct.pack(">Q", len(time_table)))
    value_payload.extend(struct.pack(">Q", 5))
    append_fst_block(data, 1, value_payload)

    path = directory / "tiny.fst"
    path.write_bytes(data)
    return path


def find_repo_root(server_path: pathlib.Path) -> pathlib.Path:
    for parent in [server_path.parent, *server_path.parents]:
        if (parent / "AGENTS.md").is_file() and (parent / ".deps").is_dir():
            return parent
    raise AssertionError(f"could not locate pristine-engine repo root from {server_path}")


def find_wellen_fixtures(repo_root: pathlib.Path) -> list[pathlib.Path]:
    root = repo_root
    inputs = root / ".deps" / "src" / "wellen" / "wellen" / "inputs"
    if not inputs.is_dir():
        raise AssertionError(f"missing pinned wellen FST fixture directory: {inputs}")

    fixtures = sorted(inputs.rglob("*.fst"))
    if len(fixtures) != 61:
        raise AssertionError(f"expected 61 wellen FST fixtures in {inputs}, found {len(fixtures)}")

    representative = [
        inputs / "ghdl" / "alu.vcd.fst",
        inputs / "nvc" / "shortstring.fst",
        inputs / "verilator" / "swerv1.vcd.fst",
        inputs / "vivado" / "iladata.vcd.fst",
        inputs / "systemc" / "waveform.vcd.fastlz.fst",
        inputs / "systemc" / "waveform.vcd.dual_lz4.fst",
    ]
    missing = [path for path in representative if not path.is_file()]
    if missing:
        raise AssertionError(f"missing representative wellen FST fixtures: {missing}")
    return [path.resolve() for path in representative]


def assert_columnar_frame_v1(payload: bytes, expected_signal_count: int, max_segments: int) -> None:
    assert payload[:4] == b"PWVF"
    frame_version = struct.unpack_from("<H", payload, 4)[0]
    signal_count = struct.unpack_from("<I", payload, 8)[0]
    segment_count = struct.unpack_from("<I", payload, 12)[0]
    offsets = struct.unpack_from("<IIIIIII", payload, 16)
    assert frame_version == 1
    assert signal_count == expected_signal_count
    assert 0 < segment_count <= max_segments
    assert all(offset % 4 == 0 for offset in offsets[:4] + offsets[5:6])


def assert_columnar_frame_v2(payload: bytes, expected_signal_count: int, max_segments: int) -> None:
    assert payload[:4] == b"PWVF"
    frame_version, header_size = struct.unpack_from("<HH", payload, 4)
    signal_count = struct.unpack_from("<I", payload, 8)[0]
    segment_count = struct.unpack_from("<I", payload, 12)[0]
    signal_table_offset, x0_offset, x1_offset, lane_y_offset = struct.unpack_from("<IIII", payload, 16)
    value_kind_offset, label_index_offset, label_bytes_offset = struct.unpack_from("<III", payload, 32)
    time0_offset, time1_offset = struct.unpack_from("<II", payload, 56)
    prepared_start, prepared_end, viewport_start, viewport_end = struct.unpack_from("<dddd", payload, 64)
    assert frame_version == 2
    assert header_size == 96
    assert signal_count == expected_signal_count
    assert 0 < segment_count <= max_segments
    assert signal_table_offset % 4 == 0
    assert x0_offset % 4 == 0
    assert x1_offset % 4 == 0
    assert lane_y_offset % 4 == 0
    assert label_index_offset % 4 == 0
    assert label_bytes_offset % 4 == 0
    assert time0_offset % 8 == 0
    assert time1_offset % 8 == 0
    assert value_kind_offset >= 96
    assert prepared_start <= viewport_start < viewport_end <= prepared_end


def exercise_pipe_session(
    session: dict,
    *,
    expected_duration: float,
    expected_group_count: int,
    expected_signal_count: int,
    signal_ids: list[str],
    start: float,
    end: float,
    request_id_base: int,
) -> None:
    endpoint = session["endpoint"]
    with connect_pipe(endpoint["kind"], endpoint["path"]) as pipe:
        pipe.write(frame(HELLO, request_id_base))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        assert message_type == HELLO_RESPONSE
        assert request_id == request_id_base
        version, _reserved = struct.unpack_from("<HH", payload, 0)
        duration = struct.unpack_from("<d", payload, 4)[0]
        group_count, signal_count = struct.unpack_from("<II", payload, 12)
        assert version == 1
        assert duration == expected_duration
        assert group_count == expected_group_count
        assert signal_count == expected_signal_count

        pipe.write(frame(CATALOG_REQUEST, request_id_base + 1))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        assert message_type == CATALOG_RESPONSE
        assert request_id == request_id_base + 1
        assert struct.unpack_from("<II", payload, 0) == (expected_group_count, expected_signal_count)

        pipe.write(frame(VIEWPORT_FRAME_REQUEST, request_id_base + 2, viewport_request_payload(signal_ids, start, end, 32)))
        pipe.flush()
        message_type, request_id, flags, payload = read_frame(pipe)
        assert message_type == VIEWPORT_FRAME_RESPONSE
        assert request_id == request_id_base + 2
        assert flags in (0, 1)
        assert_columnar_frame_v1(payload, len(signal_ids), 32)

        pipe.write(
            frame(
                VIEWPORT_FRAME_REQUEST_V2,
                request_id_base + 3,
                viewport_request_payload_v2(signal_ids, start, end, start, end, 32),
            )
        )
        pipe.flush()
        message_type, request_id, flags, payload = read_frame(pipe)
        assert message_type == VIEWPORT_FRAME_RESPONSE_V2
        assert request_id == request_id_base + 3
        assert flags in (0, 1)
        assert_columnar_frame_v2(payload, len(signal_ids), 32)

        pipe.write(frame(CLOSE, request_id_base + 4))
        pipe.flush()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: waveform_pipe_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    process: subprocess.Popen[bytes] | None = None
    try:
        temp_workspace: pathlib.Path | None = None
        try:
            workspace = find_repo_root(server_path).resolve()
            temp_workspace = (
                workspace
                / "build"
                / f"pristine-engine-waveform-e2e-{os.getpid()}-{int(time.time() * 1000)}"
            )
            temp_workspace.mkdir(parents=True, exist_ok=True)
            workspace_uri = workspace.as_uri()
            process = subprocess.Popen(
                [str(server_path), "--stdio"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            initialize = request(
                process,
                1,
                "initialize",
                {
                    "processId": None,
                    "rootUri": workspace_uri,
                    "workspaceFolders": [{"uri": workspace_uri, "name": "waveform-e2e"}],
                    "capabilities": {},
                },
            )
            provider = initialize["result"]["capabilities"]["experimental"]["pristineWaveformProvider"]
            assert provider == {
                "transport": "pipe",
                "protocol": "pristine-waveform-columnar-v1",
                "mock": True,
                "sources": ["mock", "fst"],
            }

            open_response = request(process, 2, "systemverilog/waveform/open", {"source": "mock"})
            session = open_response["result"]
            assert session["signalCount"] == 168
            assert session["groupCount"] == 3
            assert session["protocol"] == "pristine-waveform-columnar-v1"
            assert session["source"] == "mock"
            exercise_pipe_session(
                session,
                expected_duration=200.0,
                expected_group_count=3,
                expected_signal_count=168,
                signal_ids=["tb_top_module1-clk", "u_top_module1-counting"],
                start=0.0,
                end=100.0,
                request_id_base=10,
            )

            close_response = request(
                process,
                3,
                "systemverilog/waveform/close",
                {"sessionId": session["sessionId"]},
            )
            assert close_response["result"]["closed"] is True

            fst_path = write_tiny_fst_fixture(temp_workspace)
            fst_open = request(
                process,
                4,
                "systemverilog/waveform/open",
                {"source": "fst", "fstUri": fst_path.as_uri()},
            )
            fst_session = fst_open["result"]
            assert fst_session["source"] == "fst"
            assert fst_session["fileUri"] == fst_path.as_uri()
            assert fst_session["duration"] == 40.0
            assert fst_session["groupCount"] == 1
            assert fst_session["signalCount"] == 2
            exercise_pipe_session(
                fst_session,
                expected_duration=40.0,
                expected_group_count=1,
                expected_signal_count=2,
                signal_ids=["fst:1", "fst:2"],
                start=0.0,
                end=40.0,
                request_id_base=20,
            )
            fst_close = request(
                process,
                5,
                "systemverilog/waveform/close",
                {"sessionId": fst_session["sessionId"]},
            )
            assert fst_close["result"]["closed"] is True

            for index, wellen_fixture in enumerate(find_wellen_fixtures(workspace)):
                request_base = 6 + index * 2
                wellen_open = request(
                    process,
                    request_base,
                    "systemverilog/waveform/open",
                    {"source": "fst", "fstUri": wellen_fixture.as_uri()},
                )
                wellen_session = wellen_open["result"]
                assert wellen_session["source"] == "fst"
                assert wellen_session["fileUri"] == wellen_fixture.as_uri()
                assert wellen_session["duration"] >= 0.0
                assert wellen_session["groupCount"] > 0
                assert wellen_session["signalCount"] > 0
                exercise_pipe_session(
                    wellen_session,
                    expected_duration=wellen_session["duration"],
                    expected_group_count=wellen_session["groupCount"],
                    expected_signal_count=wellen_session["signalCount"],
                    signal_ids=["fst:1"],
                    start=0.0,
                    end=max(wellen_session["duration"], 1.0),
                    request_id_base=30 + index * 10,
                )
                wellen_close = request(
                    process,
                    request_base + 1,
                    "systemverilog/waveform/close",
                    {"sessionId": wellen_session["sessionId"]},
                )
                assert wellen_close["result"]["closed"] is True

            shutdown = request(process, 18, "shutdown", None)
            assert shutdown["result"] is None
            notify(process, "exit", None)
            assert process.wait(timeout=5) == 0
            process = None
            return 0
        finally:
            if temp_workspace is not None:
                for path in sorted(temp_workspace.rglob("*"), reverse=True):
                    if path.is_file() or path.is_symlink():
                        path.unlink()
                    elif path.is_dir():
                        path.rmdir()
                if temp_workspace.exists():
                    temp_workspace.rmdir()
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
