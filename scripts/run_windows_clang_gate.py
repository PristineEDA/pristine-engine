#!/usr/bin/env python3
"""Configure, build, and test the Windows clang-cl gate with a structured summary."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import gate_contract


DEFAULT_MANIFEST_PATH = gate_contract.repository_root() / "tests" / "gate_manifest.json"
DEFAULT_LOG_DIR = gate_contract.repository_root() / "build" / "clang-cl-gate-logs"


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


def vs_amd64_environment_errors(
    environment: dict[str, str] | os._Environ[str] = os.environ,
    *,
    platform_name: str | None = None,
) -> list[str]:
    platform_name = platform_name or os.name
    if platform_name != "nt":
        return ["Windows clang-cl gate can only run on Windows"]
    target = str(environment.get("VSCMD_ARG_TGT_ARCH", "")).strip().lower()
    if target not in {"x64", "amd64"}:
        return [
            "VS amd64 shell is required; run Launch-VsDevShell.ps1 "
            "-SkipAutomaticLocation -Arch amd64"
        ]
    return []


def emit_status(phase: str, started_at: float, detail: str = "") -> None:
    elapsed = max(0.0, now_seconds() - started_at)
    line = f"[pristine-test] test=pristine_windows_clang_gate phase={phase} elapsed={elapsed:.1f}s"
    if detail:
        line += f" detail={detail}"
    print(line, file=sys.stderr, flush=True)


def run_step(
    name: str,
    command: list[str],
    workspace: Path,
    logs_dir: Path,
    *,
    capture_output: bool = False,
) -> StepResult:
    started = now_seconds()
    logs_dir.mkdir(parents=True, exist_ok=True)
    log_path = logs_dir / f"{name}.log"
    emit_status(name, started, " ".join(command))
    process = subprocess.Popen(
        command,
        cwd=str(workspace),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    captured: list[str] = []
    with log_path.open("w", encoding="utf-8", errors="replace") as stream:
        for line in process.stdout:
            if capture_output:
                captured.append(line)
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
    return StepResult(
        name,
        command,
        returncode,
        duration,
        log_path,
        "".join(captured).strip() if capture_output else "",
    )


def guarded_remove_build_dir(
    workspace: Path,
    build_dir: Path,
    *,
    dry_run: bool = False,
) -> CleanPreparation:
    workspace = workspace.resolve()
    build_dir = build_dir.resolve()
    if not gate_contract.is_inside_workspace(build_dir, workspace):
        raise ValueError(f"refusing to delete build dir outside workspace: {build_dir}")
    if not build_dir.exists():
        return CleanPreparation("alreadyAbsent", build_dir, False)
    if not dry_run:
        shutil.rmtree(build_dir)
    return CleanPreparation("deleted", build_dir, True)


def list_ctest_names(ctest: str, build_dir: Path) -> list[str]:
    completed = subprocess.run(
        [ctest, "--test-dir", str(build_dir), "-N"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return []
    pattern = re.compile(r"^\s*Test\s+#?\d+:\s+(\S+)\s*$")
    return [
        match.group(1)
        for line in completed.stdout.splitlines()
        if (match := pattern.match(line))
    ]


def planned_steps(cmake: str, ctest: str, preset: str, build_dir: Path) -> list[tuple[str, list[str]]]:
    return [
        (
            "configure",
            [cmake, "--preset", preset, "-DPRISTINE_BUILD_PERF_TESTS=ON"],
        ),
        ("build", [cmake, "--build", "--preset", preset]),
        (
            "ctest",
            [ctest, "--test-dir", str(build_dir), "--output-on-failure"],
        ),
    ]


def planned_release_steps(
    cmake: str,
    preset: str,
    build_dir: Path,
) -> list[tuple[str, list[str]]]:
    engine = gate_contract.engine_binary_path(build_dir)
    python = sys.executable
    return [
        ("release-configure", [cmake, "--preset", preset]),
        ("release-build", [cmake, "--build", "--preset", preset]),
        ("release-version", [str(engine), "--version"]),
        ("release-lsp-smoke", [python, "tests/e2e/lsp_core_smoke.py", str(engine)]),
        (
            "release-waveform-smoke",
            [python, "tests/e2e/waveform_pipe_smoke.py", str(engine)],
        ),
        ("release-layout-smoke", [python, "tests/e2e/layout_pipe_smoke.py", str(engine)]),
    ]


def clang_build_errors(build: dict, *, expected_type: str, label: str) -> list[str]:
    compiler = build.get("compiler") if isinstance(build, dict) else None
    if not isinstance(compiler, dict):
        return [f"{label} compiler metadata is missing"]
    errors: list[str] = []
    if build.get("type") != expected_type:
        errors.append(f"{label} build type is {build.get('type')!r}, expected {expected_type!r}")
    if compiler.get("id") != "Clang":
        errors.append(f"{label} compiler id is {compiler.get('id')!r}, expected 'Clang'")
    compiler_name = Path(str(compiler.get("path", ""))).name.lower()
    if "clang-cl" not in compiler_name:
        errors.append(f"{label} compiler path is {compiler.get('path')!r}, expected clang-cl")
    if not str(build.get("binarySha256", "")).strip():
        errors.append(f"{label} binary SHA256 is missing")
    return errors


def build_summary(
    *,
    status: str,
    workspace: Path,
    build_dir: Path,
    preset: str,
    release_build_dir: Path,
    release_preset: str,
    release_clean_preparation: CleanPreparation,
    cmake: str,
    ctest: str,
    run_context: dict,
    started_at: str,
    duration_seconds: float,
    steps: list[StepResult],
    ctest_tests: list[str],
    gate_errors: list[str],
    failed_step: str = "",
    release_version_output: str = "",
) -> dict:
    provenance = gate_contract.summary_provenance(run_context, workspace)
    debug_build = gate_contract.build_metadata(build_dir, preset=preset, build_type="debug")
    release_build = gate_contract.build_metadata(
        release_build_dir,
        preset=release_preset,
        build_type="release",
    )
    effective_errors = list(gate_errors)
    if not provenance["sourceStable"]:
        effective_errors.append("source changed while Windows clang-cl gate was running")
    if status == "passed" and steps and all(step.returncode == 0 for step in steps):
        effective_errors.extend(clang_build_errors(debug_build, expected_type="debug", label="Debug"))
        effective_errors.extend(
            clang_build_errors(release_build, expected_type="release", label="Release")
        )
        if release_clean_preparation.status not in {"deleted", "alreadyAbsent"}:
            effective_errors.append(
                "clang-cl Release clean preparation must be deleted or alreadyAbsent"
            )
        if release_clean_preparation.path != release_build_dir:
            effective_errors.append("clang-cl Release clean preparation path does not match buildDir")
        if not release_version_output:
            effective_errors.append("clang-cl Release version output is missing")
    effective_status = status if not effective_errors else "failed"
    return {
        "schemaVersion": gate_contract.SUMMARY_SCHEMA_VERSION,
        "gateType": "windows-clang",
        "status": effective_status,
        "startedAt": started_at,
        "endedAt": gate_contract.utc_timestamp(),
        "durationSeconds": round(duration_seconds, 3),
        "workspace": str(workspace),
        "buildDir": str(build_dir),
        "preset": preset,
        "releaseBuildDir": str(release_build_dir),
        "releasePreset": release_preset,
        "cmakePath": cmake,
        "ctestPath": ctest,
        "provenance": provenance,
        "build": debug_build,
        "debugBuild": debug_build,
        "releaseBuild": release_build,
        "releaseCleanPreparation": {
            "status": release_clean_preparation.status,
            "path": str(release_clean_preparation.path),
            "existedBefore": release_clean_preparation.existed_before,
        },
        "releaseVersionOutput": release_version_output,
        "gateErrors": effective_errors,
        "failedStep": failed_step,
        "ctestTests": ctest_tests,
        "ctestCount": len(ctest_tests),
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
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/clang-cl", type=Path)
    parser.add_argument("--preset", default="clang-cl")
    parser.add_argument("--release-build-dir", default="build/clang-cl-release", type=Path)
    parser.add_argument("--release-preset", default="clang-cl-release")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST_PATH, type=Path)
    parser.add_argument("--run-context", default=None, type=Path)
    parser.add_argument("--cmake", default=None)
    parser.add_argument("--ctest", default=None)
    parser.add_argument("--logs-dir", default=DEFAULT_LOG_DIR, type=Path)
    parser.add_argument("--summary", default=None, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    workspace = gate_contract.repository_root()
    build_dir = (workspace / args.build_dir).resolve() if not args.build_dir.is_absolute() else args.build_dir.resolve()
    release_build_dir = (
        (workspace / args.release_build_dir).resolve()
        if not args.release_build_dir.is_absolute()
        else args.release_build_dir.resolve()
    )
    logs_dir = (workspace / args.logs_dir).resolve() if not args.logs_dir.is_absolute() else args.logs_dir.resolve()
    summary_path = (args.summary or logs_dir / "summary.json").resolve()
    manifest_path = (workspace / args.manifest).resolve() if not args.manifest.is_absolute() else args.manifest.resolve()
    cmake = gate_contract.find_cmake(args.cmake)
    ctest = gate_contract.find_ctest(args.ctest)
    started = now_seconds()
    started_wall = gate_contract.utc_timestamp()

    try:
        run_context = (
            gate_contract.load_run_context(args.run_context.resolve())
            if args.run_context
            else gate_contract.create_run_context(workspace, manifest_path)
        )
        errors = gate_contract.validate_run_context(
            run_context,
            workspace,
            manifest_path=manifest_path,
        )
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    errors.extend(vs_amd64_environment_errors())
    errors.extend(gate_contract.required_environment_errors(manifest_path))

    debug_steps = planned_steps(cmake, ctest, args.preset, build_dir)
    release_steps = planned_release_steps(cmake, args.release_preset, release_build_dir)
    steps_to_run = debug_steps + release_steps
    release_clean_preparation = CleanPreparation(
        "notAttempted",
        release_build_dir,
        release_build_dir.exists(),
    )
    if args.dry_run:
        try:
            release_clean_preparation = guarded_remove_build_dir(
                workspace,
                release_build_dir,
                dry_run=True,
            )
        except ValueError as exc:
            errors.append(str(exc))
        payload = build_summary(
            status="dry-run",
            workspace=workspace,
            build_dir=build_dir,
            preset=args.preset,
            release_build_dir=release_build_dir,
            release_preset=args.release_preset,
            release_clean_preparation=release_clean_preparation,
            cmake=cmake,
            ctest=ctest,
            run_context=run_context,
            started_at=started_wall,
            duration_seconds=max(0.0, now_seconds() - started),
            steps=[],
            ctest_tests=[],
            gate_errors=errors,
        )
        payload["plannedSteps"] = [
            {"name": name, "command": command} for name, command in steps_to_run
        ]
        gate_contract.write_json(summary_path, payload)
        return 0 if not errors else 1

    if errors:
        payload = build_summary(
            status="failed",
            workspace=workspace,
            build_dir=build_dir,
            preset=args.preset,
            release_build_dir=release_build_dir,
            release_preset=args.release_preset,
            release_clean_preparation=release_clean_preparation,
            cmake=cmake,
            ctest=ctest,
            run_context=run_context,
            started_at=started_wall,
            duration_seconds=max(0.0, now_seconds() - started),
            steps=[],
            ctest_tests=[],
            gate_errors=errors,
            failed_step="preflight",
        )
        gate_contract.write_json(summary_path, payload)
        for error in errors:
            print(f"PRECHECK-ERROR {error}", file=sys.stderr)
        return 1

    try:
        release_clean_preparation = guarded_remove_build_dir(workspace, release_build_dir)
    except ValueError as exc:
        release_clean_preparation = CleanPreparation(
            "failed",
            release_build_dir,
            release_build_dir.exists(),
        )
        payload = build_summary(
            status="failed",
            workspace=workspace,
            build_dir=build_dir,
            preset=args.preset,
            release_build_dir=release_build_dir,
            release_preset=args.release_preset,
            release_clean_preparation=release_clean_preparation,
            cmake=cmake,
            ctest=ctest,
            run_context=run_context,
            started_at=started_wall,
            duration_seconds=max(0.0, now_seconds() - started),
            steps=[],
            ctest_tests=[],
            gate_errors=[str(exc)],
            failed_step="release-clean",
        )
        gate_contract.write_json(summary_path, payload)
        print(f"FAILED release-clean: {exc} summary={summary_path}", file=sys.stderr)
        return 1

    results: list[StepResult] = []
    failed_step = ""
    release_version_output = ""
    for name, command in steps_to_run:
        result = run_step(
            name,
            command,
            workspace,
            logs_dir,
            capture_output=name == "release-version",
        )
        results.append(result)
        if name == "release-version" and result.returncode == 0:
            release_version_output = result.captured_output
        if result.returncode != 0:
            failed_step = name
            break

    ctest_tests = list_ctest_names(ctest, build_dir)
    payload = build_summary(
        status="passed" if not failed_step else "failed",
        workspace=workspace,
        build_dir=build_dir,
        preset=args.preset,
        release_build_dir=release_build_dir,
        release_preset=args.release_preset,
        release_clean_preparation=release_clean_preparation,
        cmake=cmake,
        ctest=ctest,
        run_context=run_context,
        started_at=started_wall,
        duration_seconds=max(0.0, now_seconds() - started),
        steps=results,
        ctest_tests=ctest_tests,
        gate_errors=[],
        failed_step=failed_step,
        release_version_output=release_version_output,
    )
    gate_contract.write_json(summary_path, payload)
    emit_status("summary", started, f"status={payload['status']} summary={summary_path}")
    return 0 if payload["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
