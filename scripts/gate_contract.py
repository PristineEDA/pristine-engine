#!/usr/bin/env python3
"""Shared provenance and summary helpers for pristine-engine validation gates."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import uuid
from datetime import datetime, timezone
from pathlib import Path


SUMMARY_SCHEMA_VERSION = 4
RUN_CONTEXT_SCHEMA_VERSION = 1


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _git_bytes(args: list[str], cwd: Path) -> bytes:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(f"unable to run git {' '.join(args)}: {exc}") from exc
    if completed.returncode != 0:
        error = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {error}")
    return completed.stdout


def git_head(cwd: Path) -> str:
    return _git_bytes(["rev-parse", "HEAD"], cwd).decode("ascii", errors="replace").strip()


def git_dirty(cwd: Path) -> bool:
    return bool(_git_bytes(["status", "--porcelain=v1", "--untracked-files=all"], cwd))


def _hash_record(digest: "hashlib._Hash", label: bytes, payload: bytes) -> None:
    digest.update(len(label).to_bytes(8, "big"))
    digest.update(label)
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)


def source_fingerprint(workspace: Path) -> str:
    """Hash HEAD plus tracked changes and non-ignored untracked source files."""

    workspace = workspace.resolve()
    digest = hashlib.sha256()
    _hash_record(digest, b"head", git_head(workspace).encode("ascii"))
    _hash_record(
        digest,
        b"tracked-diff",
        _git_bytes(["diff", "--binary", "--no-ext-diff", "HEAD", "--"], workspace),
    )

    raw_paths = _git_bytes(
        ["ls-files", "--others", "--exclude-standard", "-z"],
        workspace,
    )
    for raw_path in sorted(path for path in raw_paths.split(b"\0") if path):
        relative = raw_path.decode("utf-8", errors="surrogateescape")
        candidate = workspace / Path(relative)
        if not candidate.is_file():
            raise RuntimeError(f"untracked source file disappeared while hashing: {relative}")
        _hash_record(digest, b"untracked-path", raw_path)
        _hash_record(digest, b"untracked-content", candidate.read_bytes())
    return digest.hexdigest()


def create_run_context(
    workspace: Path,
    manifest_path: Path,
    *,
    run_id: str | None = None,
) -> dict:
    workspace = workspace.resolve()
    manifest_path = manifest_path.resolve()
    return {
        "schemaVersion": RUN_CONTEXT_SCHEMA_VERSION,
        "runId": run_id or str(uuid.uuid4()),
        "createdAt": utc_timestamp(),
        "workspace": str(workspace),
        "gitHead": git_head(workspace),
        "gitDirty": git_dirty(workspace),
        "sourceFingerprint": source_fingerprint(workspace),
        "manifestPath": str(manifest_path),
        "manifestHash": file_sha256(manifest_path),
    }


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_run_context(path: Path, context: dict) -> None:
    write_json(path, context)


def load_json(path: Path, label: str) -> dict:
    if not path.is_file():
        raise RuntimeError(f"{label} is missing: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{label} is not valid JSON: {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"{label} root must be an object: {path}")
    return payload


def load_run_context(path: Path) -> dict:
    context = load_json(path, "gate run context")
    if context.get("schemaVersion") != RUN_CONTEXT_SCHEMA_VERSION:
        raise RuntimeError(
            "unsupported gate run context schema: "
            f"{context.get('schemaVersion')!r}, expected {RUN_CONTEXT_SCHEMA_VERSION}"
        )
    required = (
        "runId",
        "workspace",
        "gitHead",
        "sourceFingerprint",
        "manifestPath",
        "manifestHash",
    )
    missing = [name for name in required if not str(context.get(name, "")).strip()]
    if missing:
        raise RuntimeError(f"gate run context is missing fields: {', '.join(missing)}")
    return context


def validate_run_context(
    context: dict,
    workspace: Path,
    *,
    manifest_path: Path | None = None,
) -> list[str]:
    workspace = workspace.resolve()
    errors: list[str] = []
    expected_workspace = Path(str(context.get("workspace", ""))).resolve()
    if os.path.normcase(str(expected_workspace)) != os.path.normcase(str(workspace)):
        errors.append(
            f"run context workspace mismatch: expected={expected_workspace} actual={workspace}"
        )

    current_head = git_head(workspace)
    if context.get("gitHead") != current_head:
        errors.append(
            f"run context git head mismatch: expected={context.get('gitHead')} actual={current_head}"
        )

    current_fingerprint = source_fingerprint(workspace)
    if context.get("sourceFingerprint") != current_fingerprint:
        errors.append(
            "run context source fingerprint mismatch: "
            f"expected={context.get('sourceFingerprint')} actual={current_fingerprint}"
        )

    if manifest_path is not None:
        manifest_path = manifest_path.resolve()
        current_manifest_hash = file_sha256(manifest_path)
        if context.get("manifestHash") != current_manifest_hash:
            errors.append(
                "run context manifest hash mismatch: "
                f"expected={context.get('manifestHash')} actual={current_manifest_hash}"
            )
    return errors


def summary_provenance(context: dict, workspace: Path) -> dict:
    workspace = workspace.resolve()
    observed_fingerprint = source_fingerprint(workspace)
    expected_fingerprint = str(context.get("sourceFingerprint", ""))
    return {
        "runId": context.get("runId", ""),
        "workspace": str(workspace),
        "gitHead": git_head(workspace),
        "gitDirty": git_dirty(workspace),
        "sourceFingerprint": expected_fingerprint,
        "observedSourceFingerprint": observed_fingerprint,
        "sourceStable": bool(expected_fingerprint) and expected_fingerprint == observed_fingerprint,
        "manifestHash": context.get("manifestHash", ""),
    }


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


def _find_vs_cmake_tool(name: str) -> str | None:
    roots = [
        Path("C:/Program Files/Microsoft Visual Studio"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio"),
    ]
    suffix = Path(f"Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{name}.exe")
    candidates: list[Path] = []
    for root in roots:
        if root.is_dir():
            candidates.extend(root.glob(f"*/*/{suffix.as_posix()}"))
    existing = [candidate for candidate in candidates if candidate.is_file()]
    if not existing:
        return None
    return str(max(existing, key=lambda candidate: (_cmake_tool_version(candidate), str(candidate))))


def _cmake_tool_version(path: Path) -> tuple[int, int, int]:
    try:
        completed = subprocess.run(
            [str(path), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return (-1, -1, -1)
    if completed.returncode != 0:
        return (-1, -1, -1)
    match = re.search(r"\b(?:cmake|ctest) version (\d+)\.(\d+)\.(\d+)", completed.stdout)
    if not match:
        return (-1, -1, -1)
    return tuple(int(part) for part in match.groups())


def find_cmake(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    if os.environ.get("CMAKE"):
        return os.environ["CMAKE"]
    if shutil.which("cmake"):
        return str(shutil.which("cmake"))
    if os.name == "nt":
        candidate = _find_vs_cmake_tool("cmake")
        if candidate:
            return candidate
    return "cmake"


def find_ctest(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    if os.environ.get("CTEST"):
        return os.environ["CTEST"]
    if shutil.which("ctest"):
        return str(shutil.which("ctest"))
    if os.name == "nt":
        candidate = _find_vs_cmake_tool("ctest")
        if candidate:
            return candidate
    return "ctest"


def compiler_info(build_dir: Path) -> dict:
    build_dir = build_dir.resolve()
    cache_path = build_dir / "CMakeCache.txt"
    compiler_path = ""
    if cache_path.is_file():
        cache_text = cache_path.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"^CMAKE_CXX_COMPILER(?::[^=]+)?=(.+)$", cache_text, re.MULTILINE)
        if match:
            compiler_path = match.group(1).strip()

    compiler_id = ""
    compiler_version = ""
    compiler_files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if compiler_files:
        text = compiler_files[-1].read_text(encoding="utf-8", errors="replace")
        id_match = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text)
        version_match = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text)
        compiler_id = id_match.group(1) if id_match else ""
        compiler_version = version_match.group(1) if version_match else ""
    return {
        "id": compiler_id,
        "version": compiler_version,
        "path": compiler_path,
    }


def engine_binary_path(build_dir: Path) -> Path:
    return build_dir.resolve() / ("pristine-engine.exe" if os.name == "nt" else "pristine-engine")


def build_metadata(build_dir: Path, *, preset: str, build_type: str) -> dict:
    binary = engine_binary_path(build_dir)
    return {
        "preset": preset,
        "type": build_type,
        "buildDir": str(build_dir.resolve()),
        "compiler": compiler_info(build_dir),
        "binaryPath": str(binary),
        "binarySha256": file_sha256(binary) if binary.is_file() else "",
    }


def required_environment_errors(
    manifest_path: Path,
    environment: dict[str, str] | os._Environ[str] = os.environ,
) -> list[str]:
    manifest = load_json(manifest_path, "gate manifest")
    errors: list[str] = []
    entries = manifest.get("requiredEnvironment", [])
    if not isinstance(entries, list):
        return ["gate manifest requiredEnvironment must be a list"]
    for entry in entries:
        if not isinstance(entry, dict):
            errors.append("gate manifest requiredEnvironment entry must be an object")
            continue
        name = str(entry.get("name", "")).strip()
        expected = str(entry.get("recommendedValue", "")).strip()
        actual = str(environment.get(name, "")).strip()
        if not name:
            errors.append("gate manifest requiredEnvironment entry is missing name")
        elif expected and actual != expected:
            errors.append(f"required environment {name} expected={expected} actual={actual!r}")
        elif not expected and not actual:
            errors.append(f"required environment {name} must be non-empty")
    return errors
