#!/usr/bin/env python3
"""Cleanly rebuild release and run release smoke tests."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_LOG_DIR_NAME = "clean-release-gate-logs"


@dataclass
class StepResult:
    name: str
    command: list[str]
    returncode: int
    duration_seconds: float
    log_path: Path


def now_seconds() -> float:
    return time.monotonic()


def workspace_root() -> Path:
    return Path(__file__).resolve().parents[1]


def is_inside_workspace(path: Path, workspace: Path) -> bool:
    resolved_path = path.resolve()
    resolved_workspace = workspace.resolve()
    try:
        resolved_path.relative_to(resolved_workspace)
        return resolved_path != resolved_workspace
    except ValueError:
        if os.name != "nt":
            return False
        path_text = os.path.normcase(str(resolved_path))
        workspace_text = os.path.normcase(str(resolved_workspace))
        return (
            os.path.commonpath([path_text, workspace_text]) == workspace_text
            and path_text != workspace_text
        )


def find_cmake(explicit: str | None) -> str:
    if explicit:
        return explicit
    env_cmake = os.environ.get("CMAKE")
    if env_cmake:
        return env_cmake
    path_cmake = shutil.which("cmake")
    if path_cmake:
        return path_cmake
    if os.name == "nt":
        roots = [
            Path("C:/Program Files/Microsoft Visual Studio"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio"),
        ]
        suffix = Path("Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
        candidates: list[Path] = []
        for root in roots:
            if root.is_dir():
                candidates.extend(sorted(root.glob(f"*/*/{suffix.as_posix()}"), reverse=True))
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
    return "cmake"


def release_binary_path(build_dir: Path) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return build_dir / f"pristine-engine{suffix}"


def guarded_remove_release_dir(workspace: Path, build_dir: Path, *, dry_run: bool = False) -> bool:
    workspace = workspace.resolve()
    build_dir = build_dir.resolve()
    if not is_inside_workspace(build_dir, workspace):
        raise ValueError(f"refusing to delete build dir outside workspace: {build_dir}")
    if not build_dir.exists():
        return False
    if dry_run:
        return True
    shutil.rmtree(build_dir)
    return True


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


def run_step(name: str, command: list[str], cwd: Path, logs_dir: Path) -> StepResult:
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
    with log_path.open("w", encoding="utf-8", errors="replace") as log_stream:
        for line in process.stdout:
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
    )


def write_summary(
    path: Path,
    *,
    status: str,
    workspace: Path,
    build_dir: Path,
    cmake: str,
    preset: str,
    removed_build_dir: bool,
    dry_run: bool,
    steps: list[StepResult],
    failed_step: str = "",
    planned: list[tuple[str, list[str]]] | None = None,
) -> None:
    payload = {
        "status": status,
        "workspace": str(workspace),
        "buildDir": str(build_dir),
        "cmakePath": cmake,
        "preset": preset,
        "removedBuildDir": removed_build_dir,
        "dryRun": dry_run,
        "failedStep": failed_step,
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
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/release", type=Path)
    parser.add_argument("--cmake", default=None)
    parser.add_argument("--preset", default="release")
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

    emit_status("begin", gate_started_at, f"workspace={workspace} buildDir={build_dir}")
    try:
        removed_build_dir = guarded_remove_release_dir(workspace, build_dir, dry_run=args.dry_run)
    except ValueError as exc:
        write_summary(
            summary_path,
            status="failed",
            workspace=workspace,
            build_dir=build_dir,
            cmake=cmake,
            preset=args.preset,
            removed_build_dir=False,
            dry_run=args.dry_run,
            steps=[],
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
            removed_build_dir=removed_build_dir,
            dry_run=True,
            steps=[],
            planned=steps_to_run,
        )
        emit_status("summary", gate_started_at, f"dry-run summary={summary_path}")
        return 0

    results: list[StepResult] = []
    for name, command in steps_to_run:
        result = run_step(name, command, workspace, logs_dir)
        results.append(result)
        if result.returncode != 0:
            write_summary(
                summary_path,
                status="failed",
                workspace=workspace,
                build_dir=build_dir,
                cmake=cmake,
                preset=args.preset,
                removed_build_dir=removed_build_dir,
                dry_run=False,
                steps=results,
                failed_step=name,
                planned=steps_to_run,
            )
            print(f"FAILED {name}: exit={result.returncode} log={result.log_path}", file=sys.stderr)
            return result.returncode or 1

    write_summary(
        summary_path,
        status="passed",
        workspace=workspace,
        build_dir=build_dir,
        cmake=cmake,
        preset=args.preset,
        removed_build_dir=removed_build_dir,
        dry_run=False,
        steps=results,
        planned=steps_to_run,
    )
    emit_status("summary", gate_started_at, f"passed summary={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
