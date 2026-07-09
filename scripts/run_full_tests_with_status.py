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


DEFAULT_HEARTBEAT_SECONDS = 30.0
DEFAULT_MANIFEST_PATH = Path(__file__).resolve().parents[1] / "tests" / "gate_manifest.json"
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


@dataclass(frozen=True)
class GateManifest:
    path: Path
    version: int
    required_tests: tuple[str, ...]
    optional_skip_tests: frozenset[str]
    runner_self_test: str
    required_environment: tuple[dict, ...]
    groups: dict[str, tuple[str, ...]]


@dataclass
class TestResult:
    name: str
    status: str
    returncode: int
    duration_seconds: float
    log_path: Path
    skip_reason: str = ""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _string_list(data: object, field_name: str) -> list[str]:
    if not isinstance(data, list):
        raise ValueError(f"manifest field {field_name!r} must be a list")
    values: list[str] = []
    for index, value in enumerate(data):
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"manifest field {field_name!r}[{index}] must be a non-empty string")
        values.append(value)
    return values


def _duplicates(values: list[str]) -> list[str]:
    seen: set[str] = set()
    duplicates: list[str] = []
    for value in values:
        if value in seen and value not in duplicates:
            duplicates.append(value)
        seen.add(value)
    return duplicates


def load_gate_manifest(path: Path) -> GateManifest:
    resolved = path.resolve()
    try:
        raw = json.loads(resolved.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"gate manifest not found: {resolved}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"gate manifest is not valid JSON: {resolved}: {exc}") from exc

    if not isinstance(raw, dict):
        raise ValueError("gate manifest root must be an object")
    version = raw.get("version")
    if not isinstance(version, int) or version < 1:
        raise ValueError("gate manifest field 'version' must be a positive integer")

    required_tests = _string_list(raw.get("requiredTests"), "requiredTests")
    optional_skip_tests = _string_list(raw.get("optionalSkipTests", []), "optionalSkipTests")
    runner_self_test = raw.get("runnerSelfTest")
    if not isinstance(runner_self_test, str) or not runner_self_test.strip():
        raise ValueError("gate manifest field 'runnerSelfTest' must be a non-empty string")

    required_duplicates = _duplicates(required_tests)
    optional_duplicates = _duplicates(optional_skip_tests)
    if required_duplicates:
        raise ValueError(f"gate manifest has duplicate required tests: {', '.join(required_duplicates)}")
    if optional_duplicates:
        raise ValueError(f"gate manifest has duplicate optional skip tests: {', '.join(optional_duplicates)}")

    missing_optional = sorted(set(optional_skip_tests) - set(required_tests))
    if missing_optional:
        raise ValueError(
            "gate manifest optional skip tests must also be required tests: "
            + ", ".join(missing_optional)
        )

    required_environment = raw.get("requiredEnvironment", [])
    if not isinstance(required_environment, list):
        raise ValueError("gate manifest field 'requiredEnvironment' must be a list")
    for index, entry in enumerate(required_environment):
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError(f"gate manifest field 'requiredEnvironment'[{index}] must have a name")

    groups: dict[str, tuple[str, ...]] = {}
    raw_groups = raw.get("groups", {})
    if not isinstance(raw_groups, dict):
        raise ValueError("gate manifest field 'groups' must be an object")
    known_tests = set(required_tests)
    for group_name, group_tests in raw_groups.items():
        if not isinstance(group_name, str) or not group_name.strip():
            raise ValueError("gate manifest group names must be non-empty strings")
        values = _string_list(group_tests, f"groups.{group_name}")
        unknown = sorted(set(values) - known_tests)
        if unknown:
            raise ValueError(
                f"gate manifest group {group_name!r} references unknown tests: {', '.join(unknown)}"
            )
        groups[group_name] = tuple(values)

    return GateManifest(
        path=resolved,
        version=version,
        required_tests=tuple(required_tests),
        optional_skip_tests=frozenset(optional_skip_tests),
        runner_self_test=runner_self_test,
        required_environment=tuple(required_environment),
        groups=groups,
    )


def gate_manifest_payload(manifest: GateManifest) -> dict:
    return {
        "version": manifest.version,
        "path": str(manifest.path),
        "runnerSelfTest": manifest.runner_self_test,
        "requiredTests": list(manifest.required_tests),
        "optionalSkipTests": sorted(manifest.optional_skip_tests),
        "requiredEnvironment": list(manifest.required_environment),
        "groups": {name: list(values) for name, values in manifest.groups.items()},
    }


def git_output(args: list[str]) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=str(repository_root()),
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


def git_head() -> str:
    return git_output(["rev-parse", "--short", "HEAD"]) or "unknown"


def git_dirty() -> bool | None:
    output = git_output(["status", "--short"])
    if output:
        return True
    probe = git_output(["rev-parse", "--is-inside-work-tree"])
    if probe:
        return False
    return None


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


def missing_required_tests(all_tests: list[str], manifest: GateManifest) -> list[str]:
    available = set(all_tests)
    return [name for name in manifest.required_tests if name not in available]


def required_skip_results(results: list[TestResult], manifest: GateManifest) -> list[TestResult]:
    return [
        result
        for result in results
        if result.status == "skipped" and result.name not in manifest.optional_skip_tests
    ]


def manifest_check_errors(all_tests: list[str], manifest: GateManifest) -> list[str]:
    errors: list[str] = []
    for name in missing_required_tests(all_tests, manifest):
        errors.append(f"required gate missing from CTest: {name}")
    if manifest.runner_self_test not in set(all_tests):
        errors.append(f"runner self-test missing from CTest: {manifest.runner_self_test}")
    return errors


def build_summary_payload(
    results: list[TestResult],
    *,
    all_tests: list[str],
    selected: list[str],
    build_dir: Path,
    ctest: str,
    manifest: GateManifest,
    full_gate_enforced: bool,
    environment: dict[str, str] | os._Environ[str] = os.environ,
) -> dict:
    optional_names = set(manifest.optional_skip_tests)
    required_names = set(manifest.required_tests) - optional_names
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
        "requiredMissing": missing_required_tests(all_tests, manifest),
        "manifestErrors": manifest_check_errors(all_tests, manifest),
        "manifestPath": str(manifest.path),
        "manifestVersion": manifest.version,
        "requiredGateCount": len(manifest.required_tests),
        "optionalGateCount": len(manifest.optional_skip_tests),
        "runnerSelfTest": manifest.runner_self_test,
        "runnerSelfTestPresent": manifest.runner_self_test in set(all_tests),
        "fullGateEnforced": full_gate_enforced,
        "gitHead": git_head(),
        "gitDirty": git_dirty(),
        "requiredTests": list(manifest.required_tests),
        "optionalSkipTests": sorted(manifest.optional_skip_tests),
        "requiredEnvironment": list(manifest.required_environment),
        "gateGroups": {name: list(values) for name, values in manifest.groups.items()},
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
    manifest: GateManifest,
    full_gate_enforced: bool,
) -> None:
    payload = build_summary_payload(
        results,
        all_tests=all_tests,
        selected=selected,
        build_dir=build_dir,
        ctest=ctest,
        manifest=manifest,
        full_gate_enforced=full_gate_enforced,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/dev", type=Path)
    parser.add_argument("--ctest", default=None)
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST_PATH, type=Path)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--logs-dir", default=None, type=Path)
    parser.add_argument("--summary", default=None, type=Path)
    parser.add_argument("--include-perf", action="store_true", help="accepted for command readability")
    parser.add_argument("--print-manifest", action="store_true")
    parser.add_argument("--check-manifest-only", action="store_true")
    args = parser.parse_args()

    try:
        manifest = load_gate_manifest(args.manifest)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if args.print_manifest:
        print(json.dumps(gate_manifest_payload(manifest), indent=2))
        return 0

    build_dir = args.build_dir.resolve()
    ctest = find_ctest(args.ctest)
    full_gate_enforced = not args.only and not args.exclude
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
    manifest_errors = manifest_check_errors(all_tests, manifest)
    if args.check_manifest_only:
        write_summary(
            [],
            summary_path,
            all_tests=all_tests,
            selected=[],
            build_dir=build_dir,
            ctest=ctest,
            manifest=manifest,
            full_gate_enforced=True,
        )
        if manifest_errors:
            for error in manifest_errors:
                print(f"MANIFEST-ERROR {error}", file=sys.stderr)
            return 1
        emit_status(
            "suite",
            "manifest-ok",
            suite_started_at,
            f"required={len(manifest.required_tests)} optional={len(manifest.optional_skip_tests)}",
            jsonl_path,
        )
        return 0

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
        manifest=manifest,
        full_gate_enforced=full_gate_enforced,
    )
    failed = [result for result in results if result.status == "failed"]
    required_skips = required_skip_results(results, manifest)
    required_missing = missing_required_tests(all_tests, manifest)
    detail = (
        f"passed={sum(1 for result in results if result.status == 'passed')} "
        f"failed={len(failed)} skipped={sum(1 for result in results if result.status == 'skipped')} "
        f"requiredMissing={len(required_missing)} "
        f"summary={summary_path}"
    )
    emit_status("suite", "summary", suite_started_at, detail, jsonl_path)

    if failed or required_skips or (full_gate_enforced and manifest_errors):
        for result in failed:
            print(f"FAILED {result.name}: log={result.log_path}", file=sys.stderr)
        for result in required_skips:
            print(
                f"REQUIRED-SKIP {result.name}: {result.skip_reason} log={result.log_path}",
                file=sys.stderr,
            )
        if full_gate_enforced:
            for name in required_missing:
                print(f"REQUIRED-MISSING {name}", file=sys.stderr)
            for error in manifest_errors:
                print(f"MANIFEST-ERROR {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
