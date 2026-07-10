#!/usr/bin/env python3
"""Run the complete local Windows validation suite under one source provenance context."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import gate_contract


@dataclass
class SuiteStep:
    name: str
    command: list[str]
    returncode: int
    duration_seconds: float
    log_path: Path
    skipped_reason: str = ""


def now_seconds() -> float:
    return time.monotonic()


def emit_status(phase: str, started: float, detail: str = "") -> None:
    elapsed = max(0.0, now_seconds() - started)
    line = f"[pristine-test] test=pristine_required_gate_suite phase={phase} elapsed={elapsed:.1f}s"
    if detail:
        line += f" detail={detail}"
    print(line, file=sys.stderr, flush=True)


def run_step(
    name: str,
    command: list[str],
    workspace: Path,
    logs_dir: Path,
    environment: dict[str, str],
) -> SuiteStep:
    started = now_seconds()
    logs_dir.mkdir(parents=True, exist_ok=True)
    log_path = logs_dir / f"{name}.log"
    emit_status(name, started, " ".join(command))
    process = subprocess.Popen(
        command,
        cwd=str(workspace),
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    with log_path.open("w", encoding="utf-8", errors="replace") as stream:
        for line in process.stdout:
            stream.write(line)
            stream.flush()
            print(line, end="", flush=True)
    returncode = process.wait()
    duration = max(0.0, now_seconds() - started)
    emit_status(
        "passed" if returncode == 0 else "failed",
        started,
        f"step={name} duration={duration:.1f}s log={log_path}",
    )
    return SuiteStep(name, command, returncode, duration, log_path)


def planned_commands(workspace: Path, cmake: str, context_path: Path) -> dict[str, list[str]]:
    python = sys.executable
    full_summary = workspace / "build/dev/test-status-logs/summary.json"
    clang_summary = workspace / "build/clang-cl-gate-logs/summary.json"
    release_summary = workspace / "build/clean-release-gate-logs/summary.json"
    return {
        "dev-configure": [cmake, "--preset", "dev", "-DPRISTINE_BUILD_PERF_TESTS=ON"],
        "dev-build": [cmake, "--build", "--preset", "dev"],
        "full-debug": [
            python,
            "scripts/run_full_tests_with_status.py",
            "--build-dir",
            "build/dev",
            "--include-perf",
            "--run-context",
            str(context_path),
        ],
        "windows-clang": [
            python,
            "scripts/run_windows_clang_gate.py",
            "--run-context",
            str(context_path),
        ],
        "clean-release": [
            python,
            "scripts/run_clean_release_gate.py",
            "--build-dir",
            "build/release",
            "--run-context",
            str(context_path),
        ],
        "artifact-index": [
            python,
            "scripts/write_gate_artifact_index.py",
            "--profile",
            "windows-local",
            "--run-context",
            str(context_path),
            "--full-summary",
            str(full_summary),
            "--clang-summary",
            str(clang_summary),
            "--release-summary",
            str(release_summary),
            "--output",
            str(workspace / "build/gate-artifacts/index.json"),
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("windows-local",), default="windows-local")
    parser.add_argument("--manifest", default="tests/gate_manifest.json", type=Path)
    parser.add_argument("--context", default="build/gate-artifacts/run-context.json", type=Path)
    parser.add_argument("--summary", default="build/gate-artifacts/suite-summary.json", type=Path)
    parser.add_argument("--logs-dir", default="build/gate-artifacts/suite-logs", type=Path)
    parser.add_argument("--cmake", default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    workspace = gate_contract.repository_root()
    manifest = (workspace / args.manifest).resolve() if not args.manifest.is_absolute() else args.manifest.resolve()
    context_path = (workspace / args.context).resolve() if not args.context.is_absolute() else args.context.resolve()
    summary_path = (workspace / args.summary).resolve() if not args.summary.is_absolute() else args.summary.resolve()
    logs_dir = (workspace / args.logs_dir).resolve() if not args.logs_dir.is_absolute() else args.logs_dir.resolve()
    cmake = gate_contract.find_cmake(args.cmake)
    started = now_seconds()
    started_wall = gate_contract.utc_timestamp()

    try:
        context = gate_contract.create_run_context(workspace, manifest)
        gate_contract.write_run_context(context_path, context)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    commands = planned_commands(workspace, cmake, context_path)
    if args.dry_run:
        gate_contract.write_json(
            summary_path,
            {
                "schemaVersion": 1,
                "status": "dry-run",
                "profile": args.profile,
                "runContextPath": str(context_path),
                "plannedSteps": [
                    {"name": name, "command": command} for name, command in commands.items()
                ],
            },
        )
        print(f"wrote required gate suite dry-run: {summary_path}")
        return 0

    environment = dict(os.environ)
    environment["PRISTINE_REQUIRE_IHP_OPEN_PDK"] = "1"
    environment["PRISTINE_REQUIRE_TT_TINYQV_GDS"] = "1"

    results: list[SuiteStep] = []
    dev_configure = run_step(
        "dev-configure",
        commands["dev-configure"],
        workspace,
        logs_dir,
        environment,
    )
    results.append(dev_configure)
    if dev_configure.returncode == 0:
        dev_build = run_step(
            "dev-build",
            commands["dev-build"],
            workspace,
            logs_dir,
            environment,
        )
        results.append(dev_build)
        if dev_build.returncode == 0:
            results.append(
                run_step(
                    "full-debug",
                    commands["full-debug"],
                    workspace,
                    logs_dir,
                    environment,
                )
            )
        else:
            results.append(
                SuiteStep(
                    "full-debug",
                    commands["full-debug"],
                    1,
                    0.0,
                    logs_dir / "full-debug.log",
                    "dev build failed",
                )
            )
    else:
        results.extend(
            [
                SuiteStep(
                    "dev-build",
                    commands["dev-build"],
                    1,
                    0.0,
                    logs_dir / "dev-build.log",
                    "dev configure failed",
                ),
                SuiteStep(
                    "full-debug",
                    commands["full-debug"],
                    1,
                    0.0,
                    logs_dir / "full-debug.log",
                    "dev configure failed",
                ),
            ]
        )

    results.append(
        run_step(
            "windows-clang",
            commands["windows-clang"],
            workspace,
            logs_dir,
            environment,
        )
    )
    results.append(
        run_step(
            "clean-release",
            commands["clean-release"],
            workspace,
            logs_dir,
            environment,
        )
    )
    results.append(
        run_step(
            "artifact-index",
            commands["artifact-index"],
            workspace,
            logs_dir,
            environment,
        )
    )

    failed = [result for result in results if result.returncode != 0]
    payload = {
        "schemaVersion": 1,
        "status": "failed" if failed else "passed",
        "profile": args.profile,
        "startedAt": started_wall,
        "endedAt": gate_contract.utc_timestamp(),
        "durationSeconds": round(max(0.0, now_seconds() - started), 3),
        "runContextPath": str(context_path),
        "artifactIndexPath": str(workspace / "build/gate-artifacts/index.json"),
        "steps": [
            {
                "name": result.name,
                "command": result.command,
                "returncode": result.returncode,
                "durationSeconds": round(result.duration_seconds, 3),
                "logPath": str(result.log_path),
                "skippedReason": result.skipped_reason,
            }
            for result in results
        ],
    }
    gate_contract.write_json(summary_path, payload)
    emit_status("summary", started, f"status={payload['status']} summary={summary_path}")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
