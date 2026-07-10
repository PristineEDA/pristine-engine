#!/usr/bin/env python3
"""Create a source-stable run context shared by all required validation gates."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import gate_contract


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default="tests/gate_manifest.json", type=Path)
    parser.add_argument("--output", default="build/gate-artifacts/run-context.json", type=Path)
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args()

    workspace = gate_contract.repository_root()
    manifest = (workspace / args.manifest).resolve() if not args.manifest.is_absolute() else args.manifest.resolve()
    output = (workspace / args.output).resolve() if not args.output.is_absolute() else args.output.resolve()
    try:
        context = gate_contract.create_run_context(
            workspace,
            manifest,
            run_id=args.run_id,
        )
        gate_contract.write_run_context(output, context)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        f"created gate run context: {output} "
        f"runId={context['runId']} sourceFingerprint={context['sourceFingerprint']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
