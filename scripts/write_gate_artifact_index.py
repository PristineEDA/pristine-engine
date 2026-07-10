#!/usr/bin/env python3
"""Validate required gate summaries and write one provenance-safe artifact index."""

from __future__ import annotations

import argparse
import platform
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import gate_contract


INDEX_SCHEMA_VERSION = 2


def _summary_contract_errors(summary: dict, gate_type: str, label: str) -> list[str]:
    errors: list[str] = []
    if summary.get("schemaVersion") != gate_contract.SUMMARY_SCHEMA_VERSION:
        errors.append(
            f"{label} schema is {summary.get('schemaVersion')!r}, "
            f"expected {gate_contract.SUMMARY_SCHEMA_VERSION}"
        )
    if summary.get("gateType") != gate_type:
        errors.append(f"{label} gateType is {summary.get('gateType')!r}, expected {gate_type!r}")
    if summary.get("status") != "passed":
        errors.append(f"{label} status is {summary.get('status')!r}")
    return errors


def _provenance_errors(context: dict, summary: dict, label: str) -> list[str]:
    errors: list[str] = []
    provenance = summary.get("provenance")
    if not isinstance(provenance, dict):
        return [f"{label} provenance is missing"]
    expected_fields = {
        "runId": context.get("runId"),
        "workspace": context.get("workspace"),
        "gitHead": context.get("gitHead"),
        "sourceFingerprint": context.get("sourceFingerprint"),
        "manifestHash": context.get("manifestHash"),
    }
    for name, expected in expected_fields.items():
        if provenance.get(name) != expected:
            errors.append(
                f"{label} provenance {name} mismatch: "
                f"expected={expected!r} actual={provenance.get(name)!r}"
            )
    if provenance.get("sourceStable") is not True:
        errors.append(f"{label} sourceStable is not true")
    if provenance.get("observedSourceFingerprint") != context.get("sourceFingerprint"):
        errors.append(f"{label} observed source fingerprint does not match the run context")
    return errors


def _full_debug_errors(summary: dict) -> list[str]:
    errors: list[str] = []
    if summary.get("fullGateEnforced") is not True:
        errors.append("full Debug summary did not enforce the complete gate manifest")
    if int(summary.get("failed", -1)) != 0:
        errors.append("full Debug summary contains failed tests")
    if int(summary.get("requiredSkipped", -1)) != 0:
        errors.append("full Debug summary contains required skips")
    for field in ("manifestErrors", "unclassifiedTests", "missingRequiredEnvironment", "gateErrors"):
        if summary.get(field):
            errors.append(f"full Debug summary field {field} is not empty")
    return errors


def _release_errors(summary: dict) -> list[str]:
    errors: list[str] = []
    preparation = summary.get("cleanPreparation")
    if not isinstance(preparation, dict):
        errors.append("clean Release summary is missing cleanPreparation")
    else:
        if preparation.get("status") not in {"deleted", "alreadyAbsent"}:
            errors.append(
                "clean Release preparation status must be deleted or alreadyAbsent, "
                f"got {preparation.get('status')!r}"
            )
        build_dir = str(summary.get("buildDir", "")).strip()
        if not build_dir or str(preparation.get("path", "")).strip() != build_dir:
            errors.append("clean Release preparation path does not match buildDir")
    if not str(summary.get("releaseVersionOutput", "")).strip():
        errors.append("clean Release summary is missing releaseVersionOutput")
    if summary.get("gateErrors"):
        errors.append("clean Release summary contains gate errors")
    return errors


def _compiler_errors(summary: dict, expected_id: str, *, require_clang_cl: bool = False) -> list[str]:
    build = summary.get("build")
    compiler = build.get("compiler") if isinstance(build, dict) else None
    if not isinstance(compiler, dict):
        return ["compiler metadata is missing"]
    errors: list[str] = []
    if compiler.get("id") != expected_id:
        errors.append(f"compiler id is {compiler.get('id')!r}, expected {expected_id!r}")
    if require_clang_cl and "clang-cl" not in Path(str(compiler.get("path", ""))).name.lower():
        errors.append(f"compiler path is not clang-cl: {compiler.get('path')!r}")
    if not str(build.get("binarySha256", "")).strip():
        errors.append("gate binary SHA256 is missing")
    return errors


def _ci_compiler_errors(summary: dict, system_name: str, label: str) -> list[str]:
    normalized = system_name.lower()
    if normalized == "linux":
        errors = _compiler_errors(summary, "Clang")
        compiler = summary.get("build", {}).get("compiler", {})
        version = str(compiler.get("version", ""))
        try:
            major = int(version.split(".", 1)[0])
        except ValueError:
            major = 0
        if major < 16:
            errors.append(f"Linux Clang version is {version!r}, expected 16 or newer")
        return [f"{label}: {error}" for error in errors]
    if normalized == "windows":
        errors = _compiler_errors(summary, "MSVC")
        return [f"{label}: {error}" for error in errors]
    if normalized in {"darwin", "macos"}:
        errors = _compiler_errors(summary, "AppleClang")
        return [f"{label}: {error}" for error in errors]
    return [f"unsupported CI platform for compiler validation: {system_name}"]


def validate_required_summaries(
    context: dict,
    full_summary: dict,
    release_summary: dict,
    clang_summary: dict | None,
    *,
    profile: str,
    system_name: str,
) -> list[str]:
    errors: list[str] = []
    errors.extend(_summary_contract_errors(full_summary, "full-debug", "full Debug"))
    errors.extend(_provenance_errors(context, full_summary, "full Debug"))
    errors.extend(_full_debug_errors(full_summary))
    errors.extend(_summary_contract_errors(release_summary, "clean-release", "clean Release"))
    errors.extend(_provenance_errors(context, release_summary, "clean Release"))
    errors.extend(_release_errors(release_summary))

    if profile == "windows-local":
        if clang_summary is None:
            errors.append("Windows local profile requires a structured clang-cl summary")
        else:
            errors.extend(_summary_contract_errors(clang_summary, "windows-clang", "clang-cl"))
            errors.extend(_provenance_errors(context, clang_summary, "clang-cl"))
            errors.extend(_compiler_errors(clang_summary, "Clang", require_clang_cl=True))
            if clang_summary.get("gateErrors"):
                errors.append("clang-cl summary contains gate errors")
    elif profile == "ci-matrix":
        errors.extend(_ci_compiler_errors(full_summary, system_name, "full Debug"))
        errors.extend(_ci_compiler_errors(release_summary, system_name, "clean Release"))
    else:
        errors.append(f"unknown gate artifact profile: {profile}")
    return errors


def write_index(
    output: Path,
    *,
    workspace: Path,
    run_context_path: Path,
    full_summary_path: Path,
    release_summary_path: Path,
    clang_summary_path: Path | None = None,
    profile: str = "windows-local",
    system_name: str | None = None,
) -> dict:
    context = gate_contract.load_run_context(run_context_path)
    full_summary = gate_contract.load_json(full_summary_path, "full Debug summary")
    release_summary = gate_contract.load_json(release_summary_path, "clean Release summary")
    clang_summary = (
        gate_contract.load_json(clang_summary_path, "clang-cl summary")
        if clang_summary_path
        else None
    )
    current_errors = gate_contract.validate_run_context(
        context,
        workspace,
        manifest_path=Path(str(context["manifestPath"])),
    )
    validation_errors = current_errors + validate_required_summaries(
        context,
        full_summary,
        release_summary,
        clang_summary,
        profile=profile,
        system_name=system_name or platform.system(),
    )
    if validation_errors:
        raise RuntimeError("; ".join(validation_errors))

    payload = {
        "schemaVersion": INDEX_SCHEMA_VERSION,
        "status": "passed",
        "createdAt": gate_contract.utc_timestamp(),
        "profile": profile,
        "workspace": str(workspace.resolve()),
        "provenance": {
            "runId": context["runId"],
            "gitHead": context["gitHead"],
            "gitDirty": context.get("gitDirty"),
            "sourceFingerprint": context["sourceFingerprint"],
            "manifestHash": context["manifestHash"],
        },
        "platform": {
            "system": (system_name or platform.system()).lower(),
            "machine": platform.machine() or "unknown",
            "python": platform.python_version(),
        },
        "runContextPath": str(run_context_path),
        "fullDebug": {
            "status": full_summary["status"],
            "summaryPath": str(full_summary_path),
            "artifactRoot": full_summary.get("artifactRoot", ""),
            "total": full_summary.get("total", 0),
            "passed": full_summary.get("passed", 0),
            "skipped": full_summary.get("skipped", 0),
            "durationSeconds": full_summary.get("durationSeconds", 0),
            "build": full_summary.get("build", {}),
        },
        "cleanRelease": {
            "status": release_summary["status"],
            "summaryPath": str(release_summary_path),
            "cleanPreparation": release_summary.get("cleanPreparation", {}),
            "releaseVersionOutput": release_summary.get("releaseVersionOutput", ""),
            "build": release_summary.get("build", {}),
        },
        "clangCl": {
            "required": profile == "windows-local",
            "status": clang_summary.get("status", "not-required") if clang_summary else "not-required",
            "summaryPath": str(clang_summary_path) if clang_summary_path else "",
            "build": clang_summary.get("build", {}) if clang_summary else {},
        },
    }
    gate_contract.write_json(output, payload)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-context", required=True, type=Path)
    parser.add_argument("--full-summary", required=True, type=Path)
    parser.add_argument("--release-summary", required=True, type=Path)
    parser.add_argument("--clang-summary", default=None, type=Path)
    parser.add_argument("--profile", choices=("windows-local", "ci-matrix"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    workspace = gate_contract.repository_root()
    try:
        payload = write_index(
            args.output.resolve(),
            workspace=workspace,
            run_context_path=args.run_context.resolve(),
            full_summary_path=args.full_summary.resolve(),
            release_summary_path=args.release_summary.resolve(),
            clang_summary_path=args.clang_summary.resolve() if args.clang_summary else None,
            profile=args.profile,
        )
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        f"wrote gate artifact index: {args.output.resolve()} "
        f"status={payload['status']} runId={payload['provenance']['runId']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
