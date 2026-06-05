#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path


def truthy_env(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def truthy_env_with_legacy(name: str, legacy_name: str) -> bool:
    if os.environ.get(name) is not None:
        return truthy_env(name)
    return truthy_env(legacy_name)


def env_value(name: str, legacy_name: str | None = None, default: str | None = None) -> str | None:
    if name in os.environ:
        value = os.environ.get(name, "")
        return value.strip() or default
    if legacy_name is not None:
        legacy_value = os.environ.get(legacy_name)
        if legacy_value is not None and legacy_value.strip():
            return legacy_value.strip()
    return default


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: run_rtl_e2e_lsp_stress.py <client> <pristine-engine> <binary-dir>",
            file=sys.stderr,
        )
        return 2

    client = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve()
    binary_dir = Path(sys.argv[3]).resolve()
    repo_root = Path(__file__).resolve().parents[2]
    prepare = repo_root / "tests" / "perf" / "prepare_rtl_e2e_corpus.py"

    try:
        prepared = subprocess.run(
            [sys.executable, str(prepare)],
            stderr=subprocess.STDOUT,
            stdout=subprocess.PIPE,
            text=True,
            check=False,
        )
        if prepared.returncode != 0:
            print(prepared.stdout)
            return prepared.returncode
        root = prepared.stdout.strip().splitlines()[-1]
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    log_dir = Path(
        env_value("RTL_E2E_LSP_LOG_DIR", "RETROSOC_LSP_LOG_DIR", str(binary_dir / "rtl-e2e-lsp-logs"))
    ).resolve()
    command = [
        str(client),
        "--server",
        str(server),
        "--root",
        root,
        "--log-dir",
        str(log_dir),
        "--mode",
        env_value("RTL_E2E_LSP_MODE", "RETROSOC_LSP_MODE", "probe"),
        "--corpus",
        env_value("RTL_E2E_CORPUS", None, "retrosoc"),
        "--max-depth",
        env_value("RTL_E2E_MAX_DEPTH", "RETROSOC_MAX_DEPTH", "64"),
    ]
    top = env_value("RTL_E2E_TOP", "RETROSOC_TOP")
    if top:
        command.extend(["--top", top])
    trace_file = env_value("RTL_E2E_LSP_TRACE_FILE", "RETROSOC_LSP_TRACE_FILE")
    if trace_file:
        command.extend(["--trace-file", str(Path(trace_file).resolve())])
    elif truthy_env_with_legacy("RTL_E2E_LSP_TRACE", "RETROSOC_LSP_TRACE"):
        command.append("--trace")

    print("RUN:", " ".join(command), flush=True)
    child_env = os.environ.copy()
    server_trace = env_value("RTL_E2E_SERVER_DEBUG_TRACE", "RETROSOC_SERVER_DEBUG_TRACE")
    if server_trace is not None:
        child_env["PRISTINE_DEBUG_TRACE"] = server_trace
    server_trace_file = env_value("RTL_E2E_SERVER_DEBUG_TRACE_FILE", "RETROSOC_SERVER_DEBUG_TRACE_FILE")
    if server_trace_file:
        child_env["PRISTINE_DEBUG_TRACE_FILE"] = str(Path(server_trace_file).resolve())
    suppress_abort_dialog = env_value(
        "RTL_E2E_SERVER_SUPPRESS_ABORT_DIALOG",
        "RETROSOC_SERVER_SUPPRESS_ABORT_DIALOG",
    )
    if suppress_abort_dialog is not None:
        child_env["PRISTINE_DEBUG_SUPPRESS_ABORT_DIALOG"] = suppress_abort_dialog
    completed = subprocess.run(command, check=False, env=child_env)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
