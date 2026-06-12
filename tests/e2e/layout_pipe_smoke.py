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
PROTOCOL_VERSION = 2
SHAPE_TABLE_STRIDE_V2 = 28
PIN_TABLE_STRIDE = 28
NO_MACRO_INDEX = 0xFFFFFFFF

HELLO = 1
HELLO_RESPONSE = 2
CATALOG_REQUEST = 3
CATALOG_RESPONSE = 4
GEOMETRY_REQUEST = 5
GEOMETRY_RESPONSE = 6
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


def geometry_payload(max_shapes: int) -> bytes:
    return struct.pack("<IIII", 0, max_shapes, 0, 0)


def find_repo_root(server_path: pathlib.Path) -> pathlib.Path:
    for parent in [server_path.parent, *server_path.parents]:
        if (parent / "AGENTS.md").is_file() and (parent / ".git").is_dir():
            return parent
    raise AssertionError(f"could not locate pristine-engine repo root from {server_path}")


def write_layout_workspace(root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    lef = root / "stdcells.lef"
    lef.write_text(
        """
VERSION 5.8 ;
UNITS DATABASE MICRONS 1000 ;
LAYER M1
  TYPE ROUTING ;
  PITCH 0.5 ;
  WIDTH 0.2 ;
END M1
MACRO invx1
  CLASS CORE ;
  SIZE 1.0 BY 2.0 ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER M1 ;
        RECT 0 0 0.2 0.2 ;
    END
  END A
  PIN VDD
    DIRECTION INOUT ;
    USE POWER ;
    PORT
      LAYER M1 ;
        RECT 0 1.8 1.0 2.0 ;
    END
  END VDD
  PIN VSS
    DIRECTION INOUT ;
    USE GROUND ;
    PORT
      LAYER M1 ;
        RECT 0 0 1.0 0.2 ;
    END
  END VSS
  OBS
    LAYER M1 ;
      RECT 0.8 0 1.0 2.0 ;
  END
END invx1
END LIBRARY
""".strip(),
        encoding="utf-8",
    )
    deffile = root / "top.def"
    deffile.write_text(
        """
VERSION 5.8 ;
DESIGN top ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 2000 2000 ) ;
COMPONENTS 1 ;
  - U1 invx1 + PLACED ( 100 200 ) N ;
END COMPONENTS
NETS 1 ;
  - n1 ( U1 A ) + ROUTED M1 ( 100 200 ) ( 300 200 ) ;
END NETS
END DESIGN
""".strip(),
        encoding="utf-8",
    )
    return lef, deffile


def exercise_layout_pipe(session: dict) -> None:
    with connect_pipe(session["endpoint"]["kind"], session["endpoint"]["path"]) as pipe:
        pipe.write(frame(HELLO, 10))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        assert message_type == HELLO_RESPONSE
        assert request_id == 10
        assert struct.unpack_from("<H", payload, 0)[0] == PROTOCOL_VERSION
        assert struct.unpack_from("<I", payload, 4)[0] == 1000
        assert struct.unpack_from("<I", payload, 8)[0] == 1

        pipe.write(frame(CATALOG_REQUEST, 11))
        pipe.flush()
        message_type, request_id, _flags, payload = read_frame(pipe)
        assert message_type == CATALOG_RESPONSE
        assert request_id == 11
        assert payload[:4] == b"PLCT"
        assert struct.unpack_from("<H", payload, 4)[0] == PROTOCOL_VERSION
        assert struct.unpack_from("<H", payload, 6)[0] == 80
        assert struct.unpack_from("<I", payload, 8)[0] == 1000
        assert struct.unpack_from("<I", payload, 12)[0] == 1
        assert struct.unpack_from("<I", payload, 16)[0] == 1
        pin_count = struct.unpack_from("<I", payload, 72)[0]
        pin_table_offset = struct.unpack_from("<I", payload, 76)[0]
        string_table_offset = struct.unpack_from("<I", payload, 60)[0]
        assert pin_count == 3

        def table_string(offset: int) -> str:
            start = string_table_offset + offset
            size = struct.unpack_from("<I", payload, start)[0]
            begin = start + 4
            return payload[begin : begin + size].decode("utf-8")

        pins = {}
        for index in range(pin_count):
            row = pin_table_offset + index * PIN_TABLE_STRIDE
            macro_index, pin_index, name_offset, use_offset = struct.unpack_from("<IIII", payload, row)
            direction = struct.unpack_from("<H", payload, row + 16)[0]
            first_shape, shape_count = struct.unpack_from("<II", payload, row + 20)
            pins[(macro_index, pin_index)] = {
                "name": table_string(name_offset),
                "use": table_string(use_offset),
                "direction": direction,
                "first_shape": first_shape,
                "shape_count": shape_count,
            }
        assert pins[(0, 0)] == {
            "name": "A",
            "use": "SIGNAL",
            "direction": 1,
            "first_shape": 0,
            "shape_count": 1,
        }
        assert pins[(0, 1)]["name"] == "VDD"
        assert pins[(0, 1)]["use"] == "POWER"
        assert pins[(0, 1)]["direction"] == 3
        assert pins[(0, 1)]["shape_count"] == 1
        assert pins[(0, 2)]["name"] == "VSS"
        assert pins[(0, 2)]["use"] == "GROUND"
        assert pins[(0, 2)]["direction"] == 3
        assert pins[(0, 2)]["shape_count"] == 1

        pipe.write(frame(GEOMETRY_REQUEST, 12, geometry_payload(64)))
        pipe.flush()
        message_type, request_id, flags, payload = read_frame(pipe)
        assert message_type == GEOMETRY_RESPONSE
        assert request_id == 12
        assert flags == 0
        assert payload[:4] == b"PLGE"
        assert struct.unpack_from("<I", payload, 8)[0] == 1000
        shape_count = struct.unpack_from("<I", payload, 12)[0]
        assert shape_count >= 3
        shape_table_offset = struct.unpack_from("<I", payload, 24)[0]
        macro_indices = [
            struct.unpack_from("<I", payload, shape_table_offset + index * SHAPE_TABLE_STRIDE_V2 + 12)[0]
            for index in range(shape_count)
        ]
        assert 0 in macro_indices
        assert NO_MACRO_INDEX in macro_indices
        first_owner_kind = struct.unpack_from("<H", payload, shape_table_offset + 6)[0]
        first_owner_index = struct.unpack_from("<I", payload, shape_table_offset + 8)[0]
        first_macro_index = struct.unpack_from("<I", payload, shape_table_offset + 12)[0]
        assert first_owner_kind == 4
        assert pins[(first_macro_index, first_owner_index)]["name"] == "A"

        pipe.write(frame(CLOSE, 13))
        pipe.flush()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: layout_pipe_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    repo_root = find_repo_root(server_path)
    temp_workspace = repo_root / "build" / f"pristine-engine-layout-e2e-{os.getpid()}-{int(time.time() * 1000)}"
    process: subprocess.Popen[bytes] | None = None
    try:
        temp_workspace.mkdir(parents=True, exist_ok=True)
        lef, deffile = write_layout_workspace(temp_workspace)
        workspace_uri = temp_workspace.as_uri()
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
                "workspaceFolders": [{"uri": workspace_uri, "name": "layout-e2e"}],
                "capabilities": {},
            },
        )
        provider = initialize["result"]["capabilities"]["experimental"]["pristineLayoutProvider"]
        assert provider == {
            "transport": "pipe",
            "protocol": "pristine-layout-columnar-v2",
            "sources": ["lefdef"],
        }

        open_response = request(
            process,
            2,
            "systemverilog/layout/open",
            {"lefUris": [lef.as_uri()], "defUri": deffile.as_uri(), "title": "tiny-layout"},
        )
        session = open_response["result"]
        assert session["protocol"] == "pristine-layout-columnar-v2"
        assert session["lefCount"] == 1
        assert session["defPresent"] is True
        assert session["layerCount"] == 1
        assert session["macroCount"] == 1
        assert session["componentCount"] == 1
        assert session["netCount"] == 1
        exercise_layout_pipe(session)

        close_response = request(
            process,
            3,
            "systemverilog/layout/close",
            {"sessionId": session["sessionId"]},
        )
        assert close_response["result"]["closed"] is True

        shutdown = request(process, 4, "shutdown", None)
        assert shutdown["result"] is None
        notify(process, "exit", None)
        assert process.wait(timeout=5) == 0
        process = None
        return 0
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process is not None and process.stderr is not None:
            stderr = process.stderr.read().decode("utf-8", errors="replace")
            if stderr:
                print(stderr, file=sys.stderr)
        if temp_workspace.exists():
            for path in sorted(temp_workspace.rglob("*"), key=lambda value: len(value.parts), reverse=True):
                if path.is_file() or path.is_symlink():
                    path.unlink()
                elif path.is_dir():
                    path.rmdir()
            temp_workspace.rmdir()


if __name__ == "__main__":
    raise SystemExit(main())
