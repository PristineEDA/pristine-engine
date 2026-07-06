from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path


_STARTED_AT = time.monotonic()


def _jsonl_path() -> Path | None:
    value = os.environ.get("PRISTINE_TEST_STATUS_JSONL", "").strip()
    if not value:
        return None
    path = Path(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def emit(test: str, phase: str, detail: str = "") -> None:
    elapsed = max(0.0, time.monotonic() - _STARTED_AT)
    line = f"[pristine-test] test={test} phase={phase} elapsed={elapsed:.1f}s"
    if detail:
        line += f" detail={detail}"
    print(line, file=sys.stderr, flush=True)

    path = _jsonl_path()
    if path is None:
        return
    event = {
        "test": test,
        "phase": phase,
        "elapsedSeconds": round(elapsed, 3),
        "detail": detail,
        "timestampMillis": int(time.time() * 1000),
    }
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(event, separators=(",", ":")) + "\n")
