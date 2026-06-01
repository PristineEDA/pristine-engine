#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_retrosoc_stress.py <pristine_retrosoc_stress_binary>", file=sys.stderr)
        return 2

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

    command = [sys.argv[1], root]
    top = os.environ.get("RETROSOC_TOP")
    if top:
        command.append(top)

    completed = subprocess.run(command, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
