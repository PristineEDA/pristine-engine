#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: run_lsp_framework_client_smoke.py <client> <pristine-engine> <binary-dir>",
            file=sys.stderr,
        )
        return 2

    client = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve()
    binary_dir = Path(sys.argv[3]).resolve()

    with tempfile.TemporaryDirectory(prefix="pristine-lsp-framework-smoke-") as temp:
        root = Path(temp)
        (root / "placeholder.sv").write_text(
            "// The probe source is sent via didOpen; this file only makes the workspace non-empty.\n",
            encoding="utf-8",
        )

        def run_client(log_dir: Path, *extra: str, top: str | None = "top_a") -> dict:
            top_args = [] if top is None else ["--top", top]
            completed = subprocess.run(
                [
                    str(client),
                    "--server",
                    str(server),
                    "--root",
                    str(root),
                    "--log-dir",
                    str(log_dir),
                    "--corpus",
                    "smoke",
                    *top_args,
                    "--max-depth",
                    "8",
                    *extra,
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if completed.returncode != 0:
                print(completed.stdout)
                print(completed.stderr, file=sys.stderr)
                raise SystemExit(completed.returncode)
            return json.loads(completed.stdout.strip().splitlines()[-1])

        no_trace_dir = binary_dir / "lsp-framework-smoke-logs" / "no-trace"
        no_trace_summary = run_client(no_trace_dir, top=None)
        assert no_trace_summary["fileCount"] == 1
        assert no_trace_summary["corpusName"] == "smoke"
        assert no_trace_summary["topModule"] == "rtl_e2e_probe_top_a"
        assert no_trace_summary["hierarchyRootCount"] == 2
        assert no_trace_summary["schematicModuleCount"] >= 1
        assert no_trace_summary["schematicCellCount"] >= 1
        assert no_trace_summary["schematicNetCount"] >= 1
        assert no_trace_summary["traceEnabled"] is False
        assert no_trace_summary["tracePath"] == ""
        assert (no_trace_dir / "summary.json").exists()
        assert (no_trace_dir / "operations.jsonl").exists()
        assert not (no_trace_dir / "lsp-trace.jsonl").exists()

        trace_dir = binary_dir / "lsp-framework-smoke-logs" / "trace"
        trace_file = trace_dir / "lsp-protocol-trace.jsonl"
        trace_summary = run_client(
            trace_dir,
            "--trace-file",
            str(trace_file),
            top="top_a",
        )
        assert trace_summary["fileCount"] == 1
        assert trace_summary["corpusName"] == "smoke"
        assert trace_summary["topModule"] == "top_a"
        assert trace_summary["hierarchyRootCount"] == 1
        assert trace_summary["schematicModuleCount"] >= 1
        assert trace_summary["traceEnabled"] is True
        assert Path(trace_summary["tracePath"]) == trace_file.resolve()
        assert trace_file.exists()
        trace_lines = trace_file.read_text(encoding="utf-8").splitlines()
        assert any('"direction":"client->server"' in line for line in trace_lines)
        assert any('"direction":"server->client"' in line for line in trace_lines)
        assert any('"method":"initialize"' in line for line in trace_lines)
        assert any('"method":"systemverilog/moduleHierarchy"' in line for line in trace_lines)
        assert any('"method":"systemverilog/schematic"' in line for line in trace_lines)

        print(json.dumps(trace_summary, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
