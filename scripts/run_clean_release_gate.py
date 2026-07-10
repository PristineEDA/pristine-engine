#!/usr/bin/env python3
"""Cleanly rebuild release and run release smoke tests."""

from __future__ import annotations

import argparse
import json
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


DEFAULT_LOG_DIR_NAME = "clean-release-gate-logs"
DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parents[1] / "tests" / "gate_manifest.json"
SUMMARY_SCHEMA_VERSION = gate_contract.SUMMARY_SCHEMA_VERSION


@dataclass
class StepResult:
    name: str
    command: list[str]
    returncode: int
    duration_seconds: float
    log_path: Path
    captured_output: str = ""


@dataclass(frozen=True)
class CleanPreparation:
    status: str
    path: Path
    existed_before: bool


def now_seconds() -> float:
    return time.monotonic()


def workspace_root() -> Path:
    return gate_contract.repository_root()


def git_output(args: list[str], cwd: Path) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return ""
    if completed.returncode != 0:
        return ""
    return completed.stdout.strip()


def git_head(cwd: Path) -> str:
    try:
        return gate_contract.git_head(cwd)
    except RuntimeError:
        return "unknown"


def git_dirty(cwd: Path) -> bool | None:
    try:
        return gate_contract.git_dirty(cwd)
    except RuntimeError:
        return None


def is_inside_workspace(path: Path, workspace: Path) -> bool:
    return gate_contract.is_inside_workspace(path, workspace)


def find_cmake(explicit: str | None) -> str:
    return gate_contract.find_cmake(explicit)


def release_binary_path(build_dir: Path) -> Path:
    return gate_contract.engine_binary_path(build_dir)


def guarded_remove_release_dir(
    workspace: Path,
    build_dir: Path,
    *,
    dry_run: bool = False,
) -> CleanPreparation:
    workspace = workspace.resolve()
    build_dir = build_dir.resolve()
    if not is_inside_workspace(build_dir, workspace):
        raise ValueError(f"refusing to delete build dir outside workspace: {build_dir}")
    if not build_dir.exists():
        return CleanPreparation("alreadyAbsent", build_dir, False)
    if dry_run:
        return CleanPreparation("deleted", build_dir, True)
    import shutil

    shutil.rmtree(build_dir)
    return CleanPreparation("deleted", build_dir, True)


def command_text(command: list[str]) -> str:
    return " ".join(command)


def emit_status(phase: str, started_at: float, detail: str = "") -> None:
    elapsed = max(0.0, now_seconds() - started_at)
    line = f"[pristine-test] test=pristine_clean_release_gate phase={phase} elapsed={elapsed:.1f}s"
    if detail:
        line += f" detail={detail}"
    print(line, file=sys.stderr, flush=True)


def planned_steps(cmake: str, build_dir: Path, preset: str, engine: Path) -> list[tuple[str, list[str]]]:
    python = sys.executable
    return [
        ("configure", [cmake, "--preset", preset]),
        ("build", [cmake, "--build", "--preset", preset]),
        ("version", [str(engine), "--version"]),
        ("lsp-smoke", [python, "tests/e2e/lsp_core_smoke.py", str(engine)]),
        ("waveform-smoke", [python, "tests/e2e/waveform_pipe_smoke.py", str(engine)]),
        ("layout-smoke", [python, "tests/e2e/layout_pipe_smoke.py", str(engine)]),
    ]


def run_step(
    name: str,
    command: list[str],
    cwd: Path,
    logs_dir: Path,
    *,
    capture_output: bool = False,
) -> StepResult:
    started_at = now_seconds()
    logs_dir.mkdir(parents=True, exist_ok=True)
    log_path = logs_dir / f"{name}.log"
    emit_status(name, started_at, command_text(command))
    process = subprocess.Popen(
        command,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    captured: list[str] = []
    with log_path.open("w", encoding="utf-8", errors="replace") as log_stream:
        for line in process.stdout:
            if capture_output:
                captured.append(line)
            log_stream.write(line)
            log_stream.flush()
            print(line, end="", flush=True)
    returncode = process.wait()
    duration = max(0.0, now_seconds() - started_at)
    status = "passed" if returncode == 0 else "failed"
    emit_status(status, started_at, f"step={name} duration={duration:.1f}s log={log_path}")
    return StepResult(
        name=name,
        command=command,
        returncode=returncode,
        duration_seconds=duration,
        log_path=log_path,
        captured_output="".join(captured).strip() if capture_output else "",
    )


def write_summary(
    path: Path,
    *,
    status: str,
    workspace: Path,
    build_dir: Path,
    cmake: str,
    preset: str,
    clean_preparation: CleanPreparation,
    dry_run: bool,
    steps: list[StepResult],
    run_context: dict,
    started_at: str,
    duration_seconds: float,
    failed_step: str = "",
    failed_log_path: str = "",
    release_version_output: str = "",
    planned: list[tuple[str, list[str]]] | None = None,
    gate_errors: list[str] | None = None,
) -> dict:
    gate_errors = list(gate_errors or [])
    provenance = gate_contract.summary_provenance(run_context, workspace)
    effective_status = status
    if gate_errors or not provenance["sourceStable"]:
        effective_status = "failed"
    payload = {
        "schemaVersion": SUMMARY_SCHEMA_VERSION,
        "gateType": "clean-release",
        "status": effective_status,
        "startedAt": started_at,
        "endedAt": gate_contract.utc_timestamp(),
        "durationSeconds": round(duration_seconds, 3),
        "provenance": provenance,
        "gateErrors": gate_errors,
        "workspace": str(workspace),
        "buildDir": str(build_dir),
        "cmakePath": cmake,
        "preset": preset,
        "gitHead": provenance["gitHead"],
        "gitDirty": provenance["gitDirty"],
        "build": gate_contract.build_metadata(build_dir, preset=preset, build_type="release"),
        "cleanPreparation": {
            "status": clean_preparation.status,
            "path": str(clean_preparation.path),
            "existedBefore": clean_preparation.existed_before,
        },
        "removedBuildDir": clean_preparation.status == "deleted",
        "deletedPath": str(clean_preparation.path),
        "dryRun": dry_run,
        "failedStep": failed_step,
        "failedLogPath": failed_log_path,
        "releaseVersionOutput": release_version_output,
        "steps": [
            {
                "name": step.name,
                "command": step.command,
                "returncode": step.returncode,
                "durationSeconds": round(step.duration_seconds, 3),
                "logPath": str(step.log_path),
            }
            for step in steps
        ],
        "plannedSteps": [
            {"name": name, "command": command}
            for name, command in (planned or [])
        ],
    }
    gate_contract.write_json(path, payload)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/release", type=Path)
    parser.add_argument("--cmake", default=None)
    parser.add_argument("--preset", default="release")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST_PATH, type=Path)
    parser.add_argument("--run-context", default=None, type=Path)
    parser.add_argument("--logs-dir", default=None, type=Path)
    parser.add_argument("--summary", default=None, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    workspace = workspace_root()
    build_dir = (workspace / args.build_dir).resolve() if not args.build_dir.is_absolute() else args.build_dir.resolve()
    logs_dir = (args.logs_dir or build_dir.parent / DEFAULT_LOG_DIR_NAME).resolve()
    summary_path = (args.summary or logs_dir / "summary.json").resolve()
    cmake = find_cmake(args.cmake)
    engine = release_binary_path(build_dir)
    gate_started_at = now_seconds()
    gate_started_wall = gate_contract.utc_timestamp()
    manifest_path = (
        (workspace / args.manifest).resolve()
        if not args.manifest.is_absolute()
        else args.manifest.resolve()
    )

    try:
        run_context = (
            gate_contract.load_run_context(args.run_context.resolve())
            if args.run_context
            else gate_contract.create_run_context(workspace, manifest_path)
        )
        context_errors = gate_contract.validate_run_context(
            run_context,
            workspace,
            manifest_path=manifest_path,
        )
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    emit_status("begin", gate_started_at, f"workspace={workspace} buildDir={build_dir}")
    if context_errors:
        preparation = CleanPreparation("notAttempted", build_dir, build_dir.exists())
        write_summary(
            summary_path,
            status="failed",
            workspace=workspace,
            build_dir=build_dir,
            cmake=cmake,
            preset=args.preset,
            clean_preparation=preparation,
            dry_run=args.dry_run,
            steps=[],
            run_context=run_context,
            started_at=gate_started_wall,
            duration_seconds=max(0.0, now_seconds() - gate_started_at),
            failed_step="provenance-preflight",
            gate_errors=context_errors,
        )
        for error in context_errors:
            print(f"PROVENANCE-ERROR {error}", file=sys.stderr)
        return 1

    try:
        preparation = guarded_remove_release_dir(workspace, build_dir, dry_run=args.dry_run)
    except ValueError as exc:
        preparation = CleanPreparation("failed", build_dir, build_dir.exists())
        write_summary(
            summary_path,
            status="failed",
            workspace=workspace,
            build_dir=build_dir,
            cmake=cmake,
            preset=args.preset,
            clean_preparation=preparation,
            dry_run=args.dry_run,
            steps=[],
            run_context=run_context,
            started_at=gate_started_wall,
            duration_seconds=max(0.0, now_seconds() - gate_started_at),
            failed_step="guard-delete",
        )
        print(f"FAILED guard-delete: {exc} summary={summary_path}", file=sys.stderr)
        return 1

    steps_to_run = planned_steps(cmake, build_dir, args.preset, engine)
    if args.dry_run:
        write_summary(
            summary_path,
            status="dry-run",
            workspace=workspace,
            build_dir=build_dir,
            cmake=cmake,
            preset=args.preset,
            clean_preparation=preparation,
            dry_run=True,
            steps=[],
            run_context=run_context,
            started_at=gate_started_wall,
            duration_seconds=max(0.0, now_seconds() - gate_started_at),
            planned=steps_to_run,
        )
        emit_status("summary", gate_started_at, f"dry-run summary={summary_path}")
        return 0

    results: list[StepResult] = []
    release_version_output = ""
    for name, command in steps_to_run:
        result = run_step(name, command, workspace, logs_dir, capture_output=name == "version")
        results.append(result)
        if name == "version":
            release_version_output = result.captured_output
        if result.returncode != 0:
            write_summary(
                summary_path,
                status="failed",
                workspace=workspace,
                build_dir=build_dir,
                cmake=cmake,
                preset=args.preset,
                clean_preparation=preparation,
                dry_run=False,
                steps=results,
                run_context=run_context,
                started_at=gate_started_wall,
                duration_seconds=max(0.0, now_seconds() - gate_started_at),
                failed_step=name,
                failed_log_path=str(result.log_path),
                release_version_output=release_version_output,
                planned=steps_to_run,
            )
            print(f"FAILED {name}: exit={result.returncode} log={result.log_path}", file=sys.stderr)
            return result.returncode or 1

    summary = write_summary(
        summary_path,
        status="passed",
        workspace=workspace,
        build_dir=build_dir,
        cmake=cmake,
        preset=args.preset,
        clean_preparation=preparation,
        dry_run=False,
        steps=results,
        run_context=run_context,
        started_at=gate_started_wall,
        duration_seconds=max(0.0, now_seconds() - gate_started_at),
        release_version_output=release_version_output,
        planned=steps_to_run,
    )
    if summary["status"] != "passed":
        print(
            "FAILED provenance: source changed while the clean Release gate was running",
            file=sys.stderr,
        )
        return 1
    emit_status("summary", gate_started_at, f"passed summary={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
