#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path


def truthy_env(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: run_retrosoc_lsp_stress.py <client> <pristine-engine> <binary-dir>",
            file=sys.stderr,
        )
        return 2

    client = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve()
    binary_dir = Path(sys.argv[3]).resolve()
    repo_root = Path(__file__).resolve().parents[2]
    prepare = repo_root / "tests" / "perf" / "prepare_retrosoc.py"

    try:
        prepared = subprocess.run(
            [sys.executable, str(prepare)],
            stderr=subprocess.STDOUT,
            stdout=subprocess.PIPE,
            text=True,
            check=False,
        )
        if prepared.returncode != 0:
            print(prepared.stdout)
            return prepared.returncode
        root = prepared.stdout.strip().splitlines()[-1]
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    log_dir = Path(os.environ.get("RETROSOC_LSP_LOG_DIR", binary_dir / "retrosoc-lsp-logs")).resolve()
    command = [
        str(client),
        "--server",
        str(server),
        "--root",
        root,
        "--log-dir",
        str(log_dir),
        "--max-depth",
        os.environ.get("RETROSOC_MAX_DEPTH", "64"),
    ]
    top = os.environ.get("RETROSOC_TOP")
    if top:
        command.extend(["--top", top])
    trace_file = os.environ.get("RETROSOC_LSP_TRACE_FILE")
    if trace_file:
        command.extend(["--trace-file", str(Path(trace_file).resolve())])
    elif truthy_env("RETROSOC_LSP_TRACE"):
        command.append("--trace")

    print("RUN:", " ".join(command), flush=True)
    completed = subprocess.run(command, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
