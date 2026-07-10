#!/usr/bin/env python3
"""Write a single audit index for required pristine-engine gate artifacts."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


INDEX_SCHEMA_VERSION = 1


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


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
    return git_output(["rev-parse", "--short", "HEAD"], cwd) or "unknown"


def git_dirty(cwd: Path) -> bool | None:
    status = git_output(["status", "--short"], cwd)
    if status:
        return True
    probe = git_output(["rev-parse", "--is-inside-work-tree"], cwd)
    if probe:
        return False
    return None


def load_json(path: Path, label: str) -> dict:
    if not path.is_file():
        raise RuntimeError(f"{label} summary is missing: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{label} summary is not valid JSON: {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"{label} summary root must be an object: {path}")
    return payload


def full_summary_passed(summary: dict) -> bool:
    return (
        int(summary.get("failed", -1)) == 0
        and int(summary.get("requiredSkipped", -1)) == 0
        and not summary.get("manifestErrors", [])
        and not summary.get("unclassifiedTests", [])
        and not summary.get("missingRequiredEnvironment", [])
    )


def validate_required_summaries(full_summary: dict, release_summary: dict) -> list[str]:
    errors: list[str] = []
    if not full_summary_passed(full_summary):
        errors.append("full Debug summary is not passed")
    if release_summary.get("status") != "passed":
        errors.append(f"clean Release summary status is {release_summary.get('status')!r}")
    if not str(release_summary.get("deletedPath", "")).strip():
        errors.append("clean Release summary does not prove build/release was deleted")
    if not str(release_summary.get("releaseVersionOutput", "")).strip():
        errors.append("clean Release summary is missing releaseVersionOutput")
    return errors


def infer_clang_status(clang_summary: dict | None, clang_log: Path | None) -> str:
    if clang_summary is not None:
        status = str(clang_summary.get("status", "")).strip()
        if status:
            return status
        if int(clang_summary.get("failed", 0)) == 0:
            return "passed"
        return "failed"
    if clang_log is not None:
        try:
            text = clang_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return "log-missing"
        if "100% tests passed" in text and "0 tests failed" in text:
            return "passed"
        if "***Failed" in text or "tests failed out of" in text:
            return "failed"
        return "log-only"
    return "not-provided"


def write_index(
    output: Path,
    *,
    workspace: Path,
    full_summary_path: Path,
    release_summary_path: Path,
    clang_summary_path: Path | None = None,
    clang_log_path: Path | None = None,
) -> dict:
    full_summary = load_json(full_summary_path, "full Debug")
    release_summary = load_json(release_summary_path, "clean Release")
    clang_summary = load_json(clang_summary_path, "clang-cl") if clang_summary_path else None

    validation_errors = validate_required_summaries(full_summary, release_summary)
    if validation_errors:
        raise RuntimeError("; ".join(validation_errors))

    payload = {
        "schemaVersion": INDEX_SCHEMA_VERSION,
        "createdAt": utc_timestamp(),
        "workspace": str(workspace),
        "gitHead": git_head(workspace),
        "gitDirty": git_dirty(workspace),
        "platform": {
            "system": platform.system().lower() or "unknown",
            "machine": platform.machine() or "unknown",
            "python": platform.python_version(),
        },
        "cmakePath": release_summary.get("cmakePath", ""),
        "ctestPath": full_summary.get("ctestPath", ""),
        "fullDebug": {
            "status": "passed",
            "summaryPath": str(full_summary_path),
            "artifactRoot": full_summary.get("artifactRoot", ""),
            "total": full_summary.get("total", 0),
            "passed": full_summary.get("passed", 0),
            "skipped": full_summary.get("skipped", 0),
            "optionalSkipped": full_summary.get("optionalSkipped", 0),
            "durationSeconds": full_summary.get("durationSeconds", 0),
            "manifestHash": full_summary.get("manifestHash", ""),
        },
        "cleanRelease": {
            "status": release_summary.get("status", ""),
            "summaryPath": str(release_summary_path),
            "deletedPath": release_summary.get("deletedPath", ""),
            "releaseVersionOutput": release_summary.get("releaseVersionOutput", ""),
            "failedStep": release_summary.get("failedStep", ""),
            "failedLogPath": release_summary.get("failedLogPath", ""),
        },
        "clangCl": {
            "status": infer_clang_status(clang_summary, clang_log_path),
            "summaryPath": str(clang_summary_path) if clang_summary_path else "",
            "logPath": str(clang_log_path) if clang_log_path else "",
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full-summary", required=True, type=Path)
    parser.add_argument("--release-summary", required=True, type=Path)
    parser.add_argument("--clang-summary", default=None, type=Path)
    parser.add_argument("--clang-log", default=None, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    workspace = repository_root()
    try:
        payload = write_index(
            args.output.resolve(),
            workspace=workspace,
            full_summary_path=args.full_summary.resolve(),
            release_summary_path=args.release_summary.resolve(),
            clang_summary_path=args.clang_summary.resolve() if args.clang_summary else None,
            clang_log_path=args.clang_log.resolve() if args.clang_log else None,
        )
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"wrote gate artifact index: {args.output.resolve()} status={payload['fullDebug']['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
