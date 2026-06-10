#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


FILE_COUNT = 300
TOP_MODULE = "large_top"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def generate_corpus(root: Path) -> None:
    write_text(
        root / "large_defs.svh",
        "`ifndef LARGE_DEFS_SVH\n"
        "`define LARGE_DEFS_SVH\n"
        "`define LARGE_E2E_WIDTH 8\n"
        "`endif\n",
    )
    write_text(
        root / "large_pkg.sv",
        "package large_pkg;\n"
        "  typedef enum logic [1:0] {IDLE, BUSY, DONE} state_t;\n"
        "  localparam int LARGE_PARAM = 8;\n"
        "endpackage\n",
    )
    write_text(
        root / "large_leaf.sv",
        "module large_leaf #(parameter int Width = `LARGE_E2E_WIDTH)(\n"
        "  input logic clk,\n"
        "  input logic [Width-1:0] data_i,\n"
        "  output logic [Width-1:0] data_o\n"
        ");\n"
        "  assign data_o = data_i;\n"
        "endmodule\n",
    )
    write_text(
        root / "large_mid.sv",
        "`include \"large_defs.svh\"\n"
        "module large_mid(input logic clk, input logic [`LARGE_E2E_WIDTH-1:0] data_i, output logic [`LARGE_E2E_WIDTH-1:0] data_o);\n"
        "  large_leaf #(.Width(`LARGE_E2E_WIDTH)) u_leaf(.clk(clk), .data_i(data_i), .data_o(data_o));\n"
        "endmodule\n",
    )
    write_text(
        root / "large_top.sv",
        "`include \"large_defs.svh\"\n"
        "import large_pkg::*;\n"
        "module large_top(input logic clk, input logic [`LARGE_E2E_WIDTH-1:0] data_i, output logic [`LARGE_E2E_WIDTH-1:0] data_o);\n"
        "  state_t state_q;\n"
        "  logic [3:0] narrow;\n"
        "  large_mid u_mid_a(.clk(clk), .data_i(data_i), .data_o(data_o));\n"
        "  large_mid u_mid_b(.clk(clk), .data_i(data_o), .data_o());\n"
        "  assign narrow = 16'hcafe;\n"
        "  assign state_q = IDLE;\n"
        "endmodule\n",
    )

    for index in range(FILE_COUNT - 5):
        write_text(
            root / "fillers" / f"large_filler_{index:03d}.sv",
            f"module large_filler_{index:03d}(input logic clk, output logic q);\n"
            f"  logic local_q_{index:03d};\n"
            f"  assign local_q_{index:03d} = clk;\n"
            f"  assign q = local_q_{index:03d};\n"
            "endmodule\n",
        )


def tail_text(text: str, limit: int = 4000) -> str:
    if len(text) <= limit:
        return text
    return text[-limit:]


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: run_lsp_large_workspace_baseline.py <client> <pristine-engine> <binary-dir>",
            file=sys.stderr,
        )
        return 2

    client = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve()
    binary_dir = Path(sys.argv[3]).resolve()

    with tempfile.TemporaryDirectory(prefix="pristine-rtl-e2e-large-") as temp:
        root = Path(temp) / "workspace"
        generate_corpus(root)

        log_dir = binary_dir / "rtl-e2e-large-workspace-logs"
        trace_file = log_dir / "lsp-protocol-trace.jsonl"
        debug_trace_file = log_dir / "server-debug-trace.jsonl"
        command = [
            str(client),
            "--server",
            str(server),
            "--root",
            str(root),
            "--log-dir",
            str(log_dir),
            "--mode",
            "large-workspace",
            "--corpus",
            "synthetic-large-workspace",
            "--top",
            TOP_MODULE,
            "--max-depth",
            "16",
            "--trace-file",
            str(trace_file),
        ]
        env = os.environ.copy()
        env["PRISTINE_DEBUG_TRACE"] = "1"
        env["PRISTINE_DEBUG_TRACE_FILE"] = str(debug_trace_file)
        env["PRISTINE_DEBUG_SUPPRESS_ABORT_DIALOG"] = "1"
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(tail_text(completed.stderr), file=sys.stderr)
            print(f"debugTracePath={debug_trace_file}", file=sys.stderr)
            return completed.returncode

        summary = json.loads(completed.stdout.strip().splitlines()[-1])
        assert summary["mode"] == "large-workspace"
        assert summary["corpusName"] == "synthetic-large-workspace"
        assert summary["fileCount"] == FILE_COUNT
        assert summary["topModule"] == TOP_MODULE
        assert summary["openedSourcePath"].endswith("large_top.sv")
        assert summary["syntaxDiagnosticsPublished"] is True
        assert "semanticDiagnosticsPublished" in summary
        assert "backgroundDiagnosticsSkippedReason" in summary
        assert summary["outlineRootCount"] >= 1
        assert summary["outlineItemCount"] >= 5
        assert summary["outlineMicros"] >= 0
        assert summary["hoverMicros"] >= 0
        assert summary["moduleHierarchyColdMicros"] >= 0
        assert summary["schematicMicros"] >= 0
        assert summary["hierarchyRootCount"] >= 1
        assert summary["schematicModuleCount"] >= 1
        assert summary["serverExitCode"] == 0
        assert summary["stderrTail"] == ""
        assert Path(summary["debugTracePath"]) == debug_trace_file.resolve()
        assert summary["traceEnabled"] is True
        assert Path(summary["tracePath"]) == trace_file.resolve()
        assert trace_file.exists()
        assert debug_trace_file.exists()

        trace_lines = trace_file.read_text(encoding="utf-8").splitlines()
        assert any('"method":"systemverilog/outline"' in line for line in trace_lines)
        assert any('"method":"textDocument/hover"' in line for line in trace_lines)
        assert sum(1 for line in trace_lines if '"method":"systemverilog/moduleHierarchy"' in line) == 2
        assert sum(1 for line in trace_lines if '"method":"systemverilog/schematic"' in line) == 1
        assert not any('"method":"workspace/symbol"' in line for line in trace_lines)
        assert not any('"method":"systemverilog/backwardCone"' in line for line in trace_lines)
        assert not any('"method":"textDocument/completion"' in line for line in trace_lines)

        print(json.dumps(summary, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
