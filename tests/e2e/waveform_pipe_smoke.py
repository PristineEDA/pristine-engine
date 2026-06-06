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


def viewport_request_payload() -> bytes:
    return (
        struct.pack("<ddfffII", 0.0, 100.0, 500.0, 24.0, 22.0, 32, 2)
        + write_string("tb_top_module1-clk")
        + write_string("u_top_module1-counting")
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


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: waveform_pipe_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    process = subprocess.Popen(
        [str(server_path), "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        initialize = request(process, 1, "initialize", {"processId": None, "capabilities": {}})
        provider = initialize["result"]["capabilities"]["experimental"]["pristineWaveformProvider"]
        assert provider == {
            "transport": "pipe",
            "protocol": "pristine-waveform-columnar-v1",
            "mock": True,
        }

        open_response = request(process, 2, "systemverilog/waveform/open", {"source": "mock"})
        session = open_response["result"]
        endpoint = session["endpoint"]
        assert session["signalCount"] == 168
        assert session["groupCount"] == 3
        assert session["protocol"] == "pristine-waveform-columnar-v1"

        with connect_pipe(endpoint["kind"], endpoint["path"]) as pipe:
            pipe.write(frame(HELLO, 10))
            pipe.flush()
            message_type, request_id, _flags, payload = read_frame(pipe)
            assert message_type == HELLO_RESPONSE
            assert request_id == 10
            version, _reserved = struct.unpack_from("<HH", payload, 0)
            duration = struct.unpack_from("<d", payload, 4)[0]
            group_count, signal_count = struct.unpack_from("<II", payload, 12)
            assert version == 1
            assert duration == 200.0
            assert group_count == 3
            assert signal_count == 168

            pipe.write(frame(CATALOG_REQUEST, 11))
            pipe.flush()
            message_type, request_id, _flags, payload = read_frame(pipe)
            assert message_type == CATALOG_RESPONSE
            assert request_id == 11
            assert struct.unpack_from("<II", payload, 0) == (3, 168)

            pipe.write(frame(VIEWPORT_FRAME_REQUEST, 12, viewport_request_payload()))
            pipe.flush()
            message_type, request_id, flags, payload = read_frame(pipe)
            assert message_type == VIEWPORT_FRAME_RESPONSE
            assert request_id == 12
            assert flags in (0, 1)
            assert payload[:4] == b"PWVF"
            frame_version = struct.unpack_from("<H", payload, 4)[0]
            signal_count = struct.unpack_from("<I", payload, 8)[0]
            segment_count = struct.unpack_from("<I", payload, 12)[0]
            offsets = struct.unpack_from("<IIIIIII", payload, 16)
            assert frame_version == 1
            assert signal_count == 2
            assert 0 < segment_count <= 32
            assert all(offset % 4 == 0 for offset in offsets[:4] + offsets[5:6])

            pipe.write(frame(CLOSE, 13))
            pipe.flush()

        close_response = request(
            process,
            3,
            "systemverilog/waveform/close",
            {"sessionId": session["sessionId"]},
        )
        assert close_response["result"]["closed"] is True

        shutdown = request(process, 4, "shutdown", None)
        assert shutdown["result"] is None
        notify(process, "exit", None)
        assert process.wait(timeout=5) == 0
        return 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process.stderr is not None:
            stderr = process.stderr.read().decode("utf-8", errors="replace")
            if stderr:
                print(stderr, file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
