#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import test_status


EXPECTED_CORPUS = "retrosoc"


def tail_text(text: str, limit: int = 4000) -> str:
    if len(text) <= limit:
        return text
    return text[-limit:]


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: run_rtl_e2e_real_retrosoc.py <client> <pristine-engine> <binary-dir>",
            file=sys.stderr,
        )
        return 2

    client = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve()
    binary_dir = Path(sys.argv[3]).resolve()
    repo_root = Path(__file__).resolve().parents[2]
    prepare = repo_root / "tests" / "perf" / "prepare_rtl_e2e_corpus.py"

    prepare_env = os.environ.copy()
    prepare_env.setdefault("RTL_E2E_CORPUS", EXPECTED_CORPUS)
    prepare_env["RTL_E2E_REQUIRED"] = "1"
    test_status.emit("pristine_rtl_e2e_real_retrosoc", "prepare-corpus", EXPECTED_CORPUS)
    prepared = subprocess.run(
        [sys.executable, str(prepare)],
        stderr=subprocess.STDOUT,
        stdout=subprocess.PIPE,
        text=True,
        check=False,
        env=prepare_env,
    )
    if prepared.returncode != 0:
        test_status.emit("pristine_rtl_e2e_real_retrosoc", "prepare-failed", f"returncode={prepared.returncode}")
        print(prepared.stdout)
        return prepared.returncode
    root = prepared.stdout.strip().splitlines()[-1]

    log_dir = Path(os.environ.get("RTL_E2E_REAL_LOG_DIR", binary_dir / "rtl-e2e-real-retrosoc-logs")).resolve()
    trace_file = log_dir / "lsp-protocol-trace.jsonl"
    debug_trace_file = log_dir / "server-debug-trace.jsonl"
    command = [
        str(client),
        "--server",
        str(server),
        "--root",
        root,
        "--log-dir",
        str(log_dir),
        "--mode",
        "real",
        "--corpus",
        EXPECTED_CORPUS,
        "--max-depth",
        "64",
        "--trace-file",
        str(trace_file),
    ]
    top = os.environ.get("RTL_E2E_TOP") or os.environ.get("RETROSOC_TOP")
    if top:
        command.extend(["--top", top])

    child_env = os.environ.copy()
    child_env["PRISTINE_DEBUG_TRACE"] = child_env.get("RTL_E2E_SERVER_DEBUG_TRACE", "1")
    child_env["PRISTINE_DEBUG_TRACE_FILE"] = str(debug_trace_file)
    child_env.setdefault("PRISTINE_DEBUG_SUPPRESS_ABORT_DIALOG", "1")
    test_status.emit("pristine_rtl_e2e_real_retrosoc", "launch-client", f"root={root}")
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=child_env,
    )
    if completed.returncode != 0:
        test_status.emit("pristine_rtl_e2e_real_retrosoc", "failed", f"returncode={completed.returncode}")
        print(completed.stdout)
        print(tail_text(completed.stderr), file=sys.stderr)
        print(f"debugTracePath={debug_trace_file}", file=sys.stderr)
        return completed.returncode

    test_status.emit("pristine_rtl_e2e_real_retrosoc", "validate-summary")
    summary = json.loads(completed.stdout.strip().splitlines()[-1])
    assert summary["mode"] == "real"
    assert summary["corpusName"] == EXPECTED_CORPUS
    assert summary["fileCount"] > 1
    assert summary["openedSourcePath"]
    assert summary["syntaxDiagnosticsPublished"] is True
    assert "semanticDiagnosticsPublished" in summary
    assert "backgroundDiagnosticsSkippedReason" in summary
    assert summary["outlineMicros"] >= 0
    assert summary["hoverMicros"] >= 0
    assert summary["completionColdMicros"] >= 0
    assert summary["completionWarmMicros"] >= 0
    assert summary["completionResolveMicros"] >= 0
    assert summary["signatureHelpMicros"] >= 0
    assert summary["inlayHintMicros"] >= 0
    assert "signatureScannedInvocations" in summary
    assert "inlayScannedInvocations" in summary
    assert "macroScannedVisibleDefinitions" in summary
    assert summary["scannedGlobalSymbols"] == 0
    assert summary["moduleHierarchyColdMicros"] >= 0
    assert summary["schematicMicros"] >= 0
    assert summary["backwardConeMicros"] >= 0
    assert "backwardConeNodeCount" in summary
    assert summary["hierarchyRootCount"] >= 1
    assert summary["schematicModuleCount"] >= 1
    assert summary["serverExitCode"] == 0
    assert summary["stderrTail"] == ""
    assert summary["traceEnabled"] is True
    assert Path(summary["tracePath"]) == trace_file.resolve()
    assert Path(summary["debugTracePath"]) == debug_trace_file.resolve()
    assert trace_file.exists()
    assert debug_trace_file.exists()

    trace_lines = trace_file.read_text(encoding="utf-8").splitlines()
    assert any('"method":"systemverilog/outline"' in line for line in trace_lines)
    assert any('"method":"textDocument/hover"' in line for line in trace_lines)
    assert any('"method":"textDocument/completion"' in line for line in trace_lines)
    assert any('"method":"textDocument/signatureHelp"' in line for line in trace_lines)
    assert any('"method":"textDocument/inlayHint"' in line for line in trace_lines)
    assert any('"method":"systemverilog/moduleHierarchy"' in line for line in trace_lines)
    assert any('"method":"systemverilog/schematic"' in line for line in trace_lines)
    assert any('"method":"systemverilog/backwardCone"' in line for line in trace_lines)
    assert not any('"method":"workspace/symbol"' in line for line in trace_lines)

    test_status.emit(
        "pristine_rtl_e2e_real_retrosoc",
        "summary",
        f"files={summary['fileCount']} outline={summary['outlineMicros']}us "
        f"hover={summary['hoverMicros']}us hierarchy={summary['moduleHierarchyColdMicros']}us",
    )
    print(json.dumps(summary, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
