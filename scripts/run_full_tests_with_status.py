#!/usr/bin/env python3
"""Run CTest one test at a time with heartbeat status output."""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


OPTIONAL_SKIP_TESTS = {"pristine_differential_slang_server"}
DEFAULT_HEARTBEAT_SECONDS = 30.0
REQUIRED_CTEST_GATES = (
    "pristine_unit_tests",
    "pristine_lsp_core_e2e",
    "pristine_waveform_pipe_e2e",
    "pristine_layout_pipe_e2e",
    "pristine_ihp_lef_corpus",
    "pristine_ihp_gds_corpus",
    "pristine_tt_tinyqv_gds_interaction",
    "pristine_lefdef_corpus",
    "pristine_differential_slang_server",
    "pristine_wellen_fst_perf",
    "pristine_rtl_e2e_stress",
    "pristine_rtl_e2e_smoke",
    "pristine_rtl_e2e_large_workspace",
    "pristine_rtl_e2e_real_retrosoc",
    "pristine_rtl_e2e_lsp_stress",
)
ENVIRONMENT_SUMMARY_KEYS = (
    "SLANG_SERVER_ROOT",
    "PRISTINE_REQUIRE_IHP_OPEN_PDK",
    "PRISTINE_REQUIRE_TT_TINYQV_GDS",
    "PRISTINE_TEST_STATUS_INTERVAL_SECONDS",
    "PRISTINE_TEST_STATUS_JSONL",
    "RTL_E2E_ROOT",
    "RTL_E2E_CORPUS",
    "RTL_E2E_CACHE_DIR",
    "RTL_E2E_LSP_MODE",
)


@dataclass
class TestResult:
    name: str
    status: str
    returncode: int
    duration_seconds: float
    log_path: Path
    skip_reason: str = ""


def now_seconds() -> float:
    return time.monotonic()


def status_line(test: str, phase: str, started_at: float, detail: str = "") -> str:
    elapsed = max(0.0, now_seconds() - started_at)
    line = f"[pristine-test] test={test} phase={phase} elapsed={elapsed:.1f}s"
    if detail:
        line += f" detail={detail}"
    return line


def emit_status(
    test: str,
    phase: str,
    started_at: float,
    detail: str = "",
    jsonl_path: Path | None = None,
) -> None:
    line = status_line(test, phase, started_at, detail)
    print(line, file=sys.stderr, flush=True)
    if jsonl_path is not None:
        event = {
            "test": test,
            "phase": phase,
            "elapsedSeconds": round(max(0.0, now_seconds() - started_at), 3),
            "detail": detail,
            "timestampMillis": int(time.time() * 1000),
        }
        with jsonl_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, separators=(",", ":")) + "\n")


def find_ctest(explicit: str | None) -> str:
    if explicit:
        return explicit
    env_ctest = os.environ.get("CTEST")
    if env_ctest:
        return env_ctest
    path_ctest = shutil.which("ctest")
    if path_ctest:
        return path_ctest
    if os.name == "nt":
        candidates: list[Path] = []
        roots = [
            Path("C:/Program Files/Microsoft Visual Studio"),
            Path("C:/Program Files (x86)/Microsoft Visual Studio"),
        ]
        suffix = Path("Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe")
        for root in roots:
            if root.is_dir():
                candidates.extend(sorted(root.glob(f"*/*/{suffix.as_posix()}"), reverse=True))
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
    return "ctest"


def parse_ctest_names(output: str) -> list[str]:
    names: list[str] = []
    pattern = re.compile(r"^\s*Test\s+#?\d+:\s+(\S+)\s*$")
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            names.append(match.group(1))
    return names


def list_tests(ctest: str, build_dir: Path) -> list[str]:
    completed = subprocess.run(
        [ctest, "--test-dir", str(build_dir), "-N"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        print(completed.stdout, file=sys.stderr)
        raise RuntimeError(f"ctest -N failed with exit code {completed.returncode}")
    tests = parse_ctest_names(completed.stdout)
    if not tests:
        print(completed.stdout, file=sys.stderr)
        raise RuntimeError("ctest -N did not report any tests")
    return tests


def exact_test_regex(name: str) -> str:
    return f"^{re.escape(name)}$"


def reader_thread(stream, output_queue: queue.Queue[str]) -> None:
    try:
        for line in iter(stream.readline, ""):
            output_queue.put(line)
    finally:
        output_queue.put("")


def skip_reason_from_output(output: str) -> str:
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("SKIP:"):
            return stripped
        if "***Skipped" in stripped:
            return stripped
    return "skipped"


def run_one_test(
    ctest: str,
    build_dir: Path,
    name: str,
    logs_dir: Path,
    heartbeat_seconds: float,
    jsonl_path: Path | None,
) -> TestResult:
    started_at = now_seconds()
    logs_dir.mkdir(parents=True, exist_ok=True)
    log_path = logs_dir / f"{name}.log"
    command = [
        ctest,
        "--test-dir",
        str(build_dir),
        "-R",
        exact_test_regex(name),
        "--output-on-failure",
        "-V",
    ]

    emit_status(name, "begin", started_at, " ".join(command), jsonl_path)
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None

    output_queue: queue.Queue[str] = queue.Queue()
    thread = threading.Thread(target=reader_thread, args=(process.stdout, output_queue), daemon=True)
    thread.start()

    captured: list[str] = []
    last_output_at = now_seconds()
    next_heartbeat_at = last_output_at + heartbeat_seconds if heartbeat_seconds > 0 else float("inf")
    with log_path.open("w", encoding="utf-8", errors="replace") as log_stream:
        while True:
            try:
                line = output_queue.get(timeout=0.25)
            except queue.Empty:
                line = None

            if line:
                captured.append(line)
                log_stream.write(line)
                log_stream.flush()
                print(line, end="", flush=True)
                last_output_at = now_seconds()
                if heartbeat_seconds > 0:
                    next_heartbeat_at = last_output_at + heartbeat_seconds
            elif line == "":
                if process.poll() is not None:
                    break
            elif process.poll() is not None and not thread.is_alive() and output_queue.empty():
                break

            if heartbeat_seconds > 0 and now_seconds() >= next_heartbeat_at:
                detail = f"no child output for {heartbeat_seconds:.0f}s log={log_path}"
                emit_status(name, "heartbeat", started_at, detail, jsonl_path)
                next_heartbeat_at = now_seconds() + heartbeat_seconds

            if process.poll() is not None and line == "":
                break

    returncode = process.wait()
    thread.join(timeout=1.0)
    output = "".join(captured)
    duration = max(0.0, now_seconds() - started_at)

    skipped = "***Skipped" in output or re.search(r"The following tests did not run:", output) is not None
    if returncode == 0 and skipped:
        status = "skipped"
        skip_reason = skip_reason_from_output(output)
    elif returncode == 0:
        status = "passed"
        skip_reason = ""
    else:
        status = "failed"
        skip_reason = ""

    emit_status(name, status, started_at, f"duration={duration:.1f}s log={log_path}", jsonl_path)
    return TestResult(
        name=name,
        status=status,
        returncode=returncode,
        duration_seconds=duration,
        log_path=log_path,
        skip_reason=skip_reason,
    )


def selected_tests(all_tests: list[str], only: list[str], exclude: list[str]) -> list[str]:
    tests = all_tests
    if only:
        wanted = set(only)
        missing = sorted(wanted - set(all_tests))
        if missing:
            raise RuntimeError(f"requested test(s) not found: {', '.join(missing)}")
        tests = [name for name in all_tests if name in wanted]
    if exclude:
        excluded = set(exclude)
        tests = [name for name in tests if name not in excluded]
    return tests


def environment_summary(environ: dict[str, str] | os._Environ[str] = os.environ) -> dict[str, str]:
    return {
        key: environ[key]
        for key in ENVIRONMENT_SUMMARY_KEYS
        if key in environ and str(environ[key]).strip()
    }


def missing_required_tests(all_tests: list[str]) -> list[str]:
    available = set(all_tests)
    return [name for name in REQUIRED_CTEST_GATES if name not in available]


def required_skip_results(results: list[TestResult]) -> list[TestResult]:
    return [
        result
        for result in results
        if result.status == "skipped" and result.name not in OPTIONAL_SKIP_TESTS
    ]


def build_summary_payload(
    results: list[TestResult],
    *,
    all_tests: list[str],
    selected: list[str],
    build_dir: Path,
    ctest: str,
    environment: dict[str, str] | os._Environ[str] = os.environ,
) -> dict:
    optional_names = set(OPTIONAL_SKIP_TESTS)
    required_names = set(REQUIRED_CTEST_GATES) - optional_names
    required_skipped = [
        result for result in results if result.status == "skipped" and result.name in required_names
    ]
    optional_skipped = [
        result for result in results if result.status == "skipped" and result.name in optional_names
    ]
    return {
        "total": len(results),
        "passed": sum(1 for result in results if result.status == "passed"),
        "failed": sum(1 for result in results if result.status == "failed"),
        "skipped": sum(1 for result in results if result.status == "skipped"),
        "requiredPassed": sum(
            1 for result in results if result.status == "passed" and result.name in required_names
        ),
        "requiredSkipped": len(required_skipped),
        "optionalSkipped": len(optional_skipped),
        "requiredMissing": missing_required_tests(all_tests),
        "requiredTests": list(REQUIRED_CTEST_GATES),
        "optionalSkipTests": sorted(OPTIONAL_SKIP_TESTS),
        "ctestTests": all_tests,
        "selectedTests": selected,
        "buildDir": str(build_dir),
        "ctestPath": ctest,
        "environment": environment_summary(environment),
        "requiredSkippedTests": [
            {"name": result.name, "skipReason": result.skip_reason, "logPath": str(result.log_path)}
            for result in required_skipped
        ],
        "optionalSkippedTests": [
            {"name": result.name, "skipReason": result.skip_reason, "logPath": str(result.log_path)}
            for result in optional_skipped
        ],
        "tests": [
            {
                "name": result.name,
                "status": result.status,
                "returncode": result.returncode,
                "durationSeconds": round(result.duration_seconds, 3),
                "logPath": str(result.log_path),
                "skipReason": result.skip_reason,
            }
            for result in results
        ],
    }


def write_summary(
    results: list[TestResult],
    path: Path,
    *,
    all_tests: list[str],
    selected: list[str],
    build_dir: Path,
    ctest: str,
) -> None:
    payload = build_summary_payload(
        results,
        all_tests=all_tests,
        selected=selected,
        build_dir=build_dir,
        ctest=ctest,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/dev", type=Path)
    parser.add_argument("--ctest", default=None)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--logs-dir", default=None, type=Path)
    parser.add_argument("--summary", default=None, type=Path)
    parser.add_argument("--include-perf", action="store_true", help="accepted for command readability")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    ctest = find_ctest(args.ctest)
    heartbeat_seconds = float(os.environ.get("PRISTINE_TEST_STATUS_INTERVAL_SECONDS", DEFAULT_HEARTBEAT_SECONDS))
    jsonl_value = os.environ.get("PRISTINE_TEST_STATUS_JSONL", "").strip()
    jsonl_path = Path(jsonl_value).resolve() if jsonl_value else None
    if jsonl_path is not None:
        jsonl_path.parent.mkdir(parents=True, exist_ok=True)
        jsonl_path.write_text("", encoding="utf-8")

    logs_dir = (args.logs_dir or build_dir / "test-status-logs").resolve()
    summary_path = (args.summary or logs_dir / "summary.json").resolve()

    suite_started_at = now_seconds()
    emit_status("suite", "discover", suite_started_at, f"buildDir={build_dir}", jsonl_path)
    all_tests = list_tests(ctest, build_dir)
    tests = selected_tests(all_tests, args.only, args.exclude)
    emit_status("suite", "begin", suite_started_at, f"tests={len(tests)}", jsonl_path)

    results: list[TestResult] = []
    for index, name in enumerate(tests, start=1):
        emit_status("suite", "test", suite_started_at, f"{index}/{len(tests)} {name}", jsonl_path)
        results.append(run_one_test(ctest, build_dir, name, logs_dir, heartbeat_seconds, jsonl_path))

    write_summary(
        results,
        summary_path,
        all_tests=all_tests,
        selected=tests,
        build_dir=build_dir,
        ctest=ctest,
    )
    failed = [result for result in results if result.status == "failed"]
    required_skips = required_skip_results(results)
    required_missing = missing_required_tests(all_tests)
    enforce_required_manifest = not args.only and not args.exclude
    detail = (
        f"passed={sum(1 for result in results if result.status == 'passed')} "
        f"failed={len(failed)} skipped={sum(1 for result in results if result.status == 'skipped')} "
        f"requiredMissing={len(required_missing)} "
        f"summary={summary_path}"
    )
    emit_status("suite", "summary", suite_started_at, detail, jsonl_path)

    if failed or required_skips or (enforce_required_manifest and required_missing):
        for result in failed:
            print(f"FAILED {result.name}: log={result.log_path}", file=sys.stderr)
        for result in required_skips:
            print(
                f"REQUIRED-SKIP {result.name}: {result.skip_reason} log={result.log_path}",
                file=sys.stderr,
            )
        if enforce_required_manifest:
            for name in required_missing:
                print(f"REQUIRED-MISSING {name}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
