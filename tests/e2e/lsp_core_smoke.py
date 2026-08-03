import json
import pathlib
import subprocess
import sys
import tempfile


def write_message(process: subprocess.Popen[bytes], message: dict) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    assert process.stdin is not None
    process.stdin.write(header + payload)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes]) -> dict:
    assert process.stdout is not None
    content_length = None

    while True:
        line = process.stdout.readline()
        if line == b"":
            raise RuntimeError("LSP server stdout closed while reading headers")
        if line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode("ascii").partition(":")
        if name.lower() == "content-length":
            content_length = int(value.strip())

    if content_length is None:
        raise RuntimeError("LSP message is missing Content-Length")

    payload = process.stdout.read(content_length)
    if len(payload) != content_length:
        raise RuntimeError("LSP server stdout closed while reading payload")
    return json.loads(payload.decode("utf-8"))


def request(process: subprocess.Popen[bytes], request_id: int, method: str, params: dict | None) -> dict:
    message = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)

    while True:
        response = read_message(process)
        if response.get("id") == request_id:
            if "error" in response:
                raise AssertionError(f"{method} failed: {response['error']}")
            return response


def completion_items(result: object) -> list[dict]:
    assert isinstance(result, dict)
    assert isinstance(result.get("isIncomplete"), bool)
    items = result.get("items")
    assert isinstance(items, list)
    assert all(isinstance(item, dict) for item in items)
    return items


def notify(process: subprocess.Popen[bytes], method: str, params: dict | None) -> None:
    message = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)


def cancel_request(process: subprocess.Popen[bytes], request_id: int, method: str, params: dict) -> None:
    message = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
    write_message(process, message)
    notify(process, "$/cancelRequest", {"id": request_id})
    while True:
        response = read_message(process)
        if response.get("id") != request_id:
            continue
        error = response.get("error")
        if not isinstance(error, dict) or error.get("code") != -32800:
            raise AssertionError(f"{method} cancellation returned unexpected response: {response}")
        return


def read_notification(process: subprocess.Popen[bytes], method: str, uri: str) -> dict:
    while True:
        message = read_message(process)
        if message.get("method") != method:
            continue
        params = message.get("params", {})
        if params.get("uri") == uri:
            return message


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lsp_core_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="pristine-engine-e2e-") as temp_dir:
        root = pathlib.Path(temp_dir)
        rtl = root / "rtl"
        rtl.mkdir()
        leaf = rtl / "leaf.sv"
        child = rtl / "child.sv"
        top = rtl / "top.sv"
        typed = rtl / "typed.sv"
        module_context = rtl / "module_context.sv"
        macro_context = rtl / "macro_context.sv"
        port_filter = rtl / "port_filter.sv"
        port_quickfix = rtl / "port_quickfix.sv"
        unresolved_module = rtl / "unresolved_module.sv"
        unresolved_package = rtl / "unresolved_package.sv"
        unresolved_type = rtl / "unresolved_type.sv"
        width_mismatch = rtl / "width_mismatch.sv"
        cone = rtl / "cone.sv"
        duplicate_symbol = rtl / "duplicate_symbol.sv"
        ambiguous_pkg_a = rtl / "ambiguous_pkg_a.sv"
        ambiguous_pkg_b = rtl / "ambiguous_pkg_b.sv"
        ambiguous_top = rtl / "ambiguous_top.sv"
        defs = rtl / "defs.svh"
        missing = rtl / "missing.svh"
        leaf.write_text("module leaf; endmodule\n", encoding="utf-8")
        child.write_text(
            "module child(input logic clk, output logic rst_n);\n"
            "  leaf u_leaf();\n"
            "endmodule\n",
            encoding="utf-8",
        )
        defs_text = (
            "`define FEATURE 1\n"
            "`define ADD_ONE(value) ((value) + 1)\n"
        )
        defs.write_text(defs_text, encoding="utf-8")
        top_text = (
            "`include \"defs.svh\"\n"
            "`include \"missing.svh\"\n"
            "module top;\n"
            "  child child_i(.clk(), .rst_n());\n"
            "  logic ready;\n"
            "  assign ready = ready;\n"
            "endmodule\n"
        )
        top.write_text(top_text, encoding="utf-8")
        typed_text = (
            "module typed;\n"
            "  typedef logic [7:0] byte_t;\n"
            "  byte_t value;\n"
            "endmodule\n"
        )
        typed.write_text(typed_text, encoding="utf-8")
        module_context.write_text(
            "module chard; endmodule\n"
            "module chip; endmodule\n"
            "module top;\n"
            "  logic chip_count;\n"
            "  ch\n"
            "endmodule\n",
            encoding="utf-8",
        )
        macro_context_text = (
            "`include \"defs.svh\"\n"
            "module macro_top;\n"
            "  logic ready;\n"
            "  assign ready = `FE\n"
            "endmodule\n"
        )
        macro_context.write_text(macro_context_text, encoding="utf-8")
        port_filter.write_text(
            "module port_child(input logic clk, output logic rst_n, input logic data); endmodule\n"
            "module port_top;\n"
            "  logic sig;\n"
            "  port_child u_child(.clk(sig), .);\n"
            "endmodule\n",
            encoding="utf-8",
        )
        port_quickfix_text = (
            "module quick_child(input logic clk, output logic rst_n, input logic data); endmodule\n"
            "module quick_top;\n"
            "  logic sig;\n"
            "  quick_child u_child(.clk(sig));\n"
            "endmodule\n"
        )
        port_quickfix.write_text(port_quickfix_text, encoding="utf-8")
        unresolved_module_text = (
            "module unresolved_top;\n"
            "  missing_child u_missing();\n"
            "endmodule\n"
        )
        unresolved_module.write_text(unresolved_module_text, encoding="utf-8")
        unresolved_package_text = (
            "module unresolved_package_top;\n"
            "  import missing_pkg::*;\n"
            "endmodule\n"
        )
        unresolved_package.write_text(unresolved_package_text, encoding="utf-8")
        unresolved_type_text = (
            "module unresolved_type_top;\n"
            "  missing_t value;\n"
            "endmodule\n"
        )
        unresolved_type.write_text(unresolved_type_text, encoding="utf-8")
        width_mismatch_text = (
            "module width_mismatch_top;\n"
            "  logic [3:0] lhs;\n"
            "  logic [7:0] rhs;\n"
            "  assign lhs = rhs;\n"
            "endmodule\n"
        )
        width_mismatch.write_text(width_mismatch_text, encoding="utf-8")
        cone_text = (
            "module cone_top;\n"
            "  logic a;\n"
            "  logic b;\n"
            "  logic mid;\n"
            "  logic out;\n"
            "  assign mid = a & b;\n"
            "  assign out = mid;\n"
            "endmodule\n"
        )
        cone.write_text(cone_text, encoding="utf-8")
        duplicate_symbol_text = (
            "module duplicate_top;\n"
            "  logic ready;\n"
            "  logic ready;\n"
            "endmodule\n"
        )
        duplicate_symbol.write_text(duplicate_symbol_text, encoding="utf-8")
        ambiguous_pkg_a_text = (
            "package ambiguous_pkg_a;\n"
            "  typedef logic [7:0] word_t;\n"
            "endpackage\n"
        )
        ambiguous_pkg_a.write_text(ambiguous_pkg_a_text, encoding="utf-8")
        ambiguous_pkg_b_text = (
            "package ambiguous_pkg_b;\n"
            "  typedef logic [15:0] word_t;\n"
            "endpackage\n"
        )
        ambiguous_pkg_b.write_text(ambiguous_pkg_b_text, encoding="utf-8")
        ambiguous_top_text = (
            "module ambiguous_top;\n"
            "  import ambiguous_pkg_a::*;\n"
            "  import ambiguous_pkg_b::*;\n"
            "  word_t value;\n"
            "endmodule\n"
        )
        ambiguous_top.write_text(ambiguous_top_text, encoding="utf-8")
        leaf_uri = leaf.resolve().as_uri()
        child_uri = child.resolve().as_uri()
        typed_uri = typed.resolve().as_uri()
        module_context_uri = module_context.resolve().as_uri()
        macro_context_uri = macro_context.resolve().as_uri()
        port_filter_uri = port_filter.resolve().as_uri()
        port_quickfix_uri = port_quickfix.resolve().as_uri()
        unresolved_module_uri = unresolved_module.resolve().as_uri()
        unresolved_package_uri = unresolved_package.resolve().as_uri()
        unresolved_type_uri = unresolved_type.resolve().as_uri()
        width_mismatch_uri = width_mismatch.resolve().as_uri()
        cone_uri = cone.resolve().as_uri()
        duplicate_symbol_uri = duplicate_symbol.resolve().as_uri()
        ambiguous_pkg_a_uri = ambiguous_pkg_a.resolve().as_uri()
        ambiguous_pkg_b_uri = ambiguous_pkg_b.resolve().as_uri()
        ambiguous_top_uri = ambiguous_top.resolve().as_uri()
        defs_uri = defs.resolve().as_uri()
        missing_uri = missing.resolve().as_uri()
        top_uri = top.resolve().as_uri()

        process = subprocess.Popen(
            [str(server_path), "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            initialize = request(
                process,
                1,
                "initialize",
                {
                    "processId": None,
                    "rootUri": root.resolve().as_uri(),
                    "capabilities": {},
                },
            )
            capabilities = initialize["result"]["capabilities"]
            assert capabilities["definitionProvider"] is True
            assert capabilities["typeDefinitionProvider"] is True
            assert capabilities["implementationProvider"] is True
            assert capabilities["documentHighlightProvider"] is True
            assert capabilities["documentLinkProvider"]["resolveProvider"] is False
            assert capabilities["inlayHintProvider"]["resolveProvider"] is False
            assert capabilities["codeActionProvider"]["resolveProvider"] is False
            assert capabilities["foldingRangeProvider"] is True
            assert capabilities["semanticTokensProvider"]["full"] is True
            assert capabilities["selectionRangeProvider"] is True
            assert capabilities["signatureHelpProvider"]["triggerCharacters"] == ["(", ","]
            assert capabilities["callHierarchyProvider"] is True
            assert capabilities["referencesProvider"] is True
            assert capabilities["renameProvider"]["prepareProvider"] is True
            assert capabilities["workspaceSymbolProvider"] is True
            assert capabilities["completionProvider"]["resolveProvider"] is True

            notify(process, "initialized", {})
            cancel_request(
                process,
                900,
                "workspace/symbol",
                {"query": "", "workDoneToken": "cold-workspace-cancel"},
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": top_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": top_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": port_quickfix_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": port_quickfix_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": defs_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": defs_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": macro_context_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": macro_context_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": typed_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": typed_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": module_context_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": module_context.read_text(encoding="utf-8"),
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": port_filter_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": port_filter.read_text(encoding="utf-8"),
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": unresolved_module_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": unresolved_module_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": duplicate_symbol_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": duplicate_symbol_text,
                    }
                },
            )
            duplicate_diagnostics = read_notification(
                process,
                "textDocument/publishDiagnostics",
                duplicate_symbol_uri,
            )["params"]["diagnostics"]
            assert any(
                item["code"] == "duplicateSymbol"
                and item["message"] == "Duplicate symbol 'ready' in the same scope."
                and item["range"]["start"] == {"line": 2, "character": 8}
                for item in duplicate_diagnostics
            )

            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": ambiguous_pkg_a_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": ambiguous_pkg_a_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": ambiguous_pkg_b_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": ambiguous_pkg_b_text,
                    }
                },
            )
            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": ambiguous_top_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": ambiguous_top_text,
                    }
                },
            )
            ambiguous_diagnostics = read_notification(
                process,
                "textDocument/publishDiagnostics",
                ambiguous_top_uri,
            )["params"]["diagnostics"]
            assert any(
                item["code"] == "ambiguousReference"
                and item["severity"] == 2
                and item["message"] == "Symbol 'word_t' has 2 possible definitions in scope."
                and item["range"]["start"] == {"line": 3, "character": 2}
                for item in ambiguous_diagnostics
            )

            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": unresolved_package_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": unresolved_package_text,
                    }
                },
            )
            unresolved_package_diagnostics = read_notification(
                process,
                "textDocument/publishDiagnostics",
                unresolved_package_uri,
            )["params"]["diagnostics"]
            assert any(
                item["code"] == "unresolvedPackage"
                and item["severity"] == 1
                and item["message"] == "Package 'missing_pkg' could not be resolved."
                and item["range"]["start"] == {"line": 1, "character": 9}
                for item in unresolved_package_diagnostics
            )

            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": unresolved_type_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": unresolved_type_text,
                    }
                },
            )
            unresolved_type_diagnostics = read_notification(
                process,
                "textDocument/publishDiagnostics",
                unresolved_type_uri,
            )["params"]["diagnostics"]
            assert any(
                item["code"] == "unresolvedType"
                and item["severity"] == 1
                and item["message"] == "Type 'missing_t' could not be resolved."
                and item["range"]["start"] == {"line": 1, "character": 2}
                and item["range"]["end"] == {"line": 1, "character": 11}
                for item in unresolved_type_diagnostics
            )

            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": width_mismatch_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": width_mismatch_text,
                    }
                },
            )
            width_mismatch_diagnostics = read_notification(
                process,
                "textDocument/publishDiagnostics",
                width_mismatch_uri,
            )["params"]["diagnostics"]
            assert any(
                item["code"] == "widthMismatch"
                and item["severity"] == 2
                and item["message"] == "Width mismatch: assigning 8-bit 'rhs' to 4-bit 'lhs'."
                and item["range"]["start"] == {"line": 3, "character": 15}
                and item["range"]["end"] == {"line": 3, "character": 18}
                for item in width_mismatch_diagnostics
            )

            notify(
                process,
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": cone_uri,
                        "languageId": "systemverilog",
                        "version": 1,
                        "text": cone_text,
                    }
                },
            )

            outline = request(
                process,
                36,
                "systemverilog/outline",
                {
                    "textDocument": {"uri": top_uri},
                    "includeChildren": True,
                    "includeFlat": True,
                },
            )["result"]
            assert outline["uri"] == top_uri
            assert outline["version"] == 1
            assert outline["partial"] is False
            assert outline["truncated"] is False
            assert outline["messages"] == []
            assert len(outline["roots"]) == 1
            assert outline["roots"][0]["name"] == "top"
            assert outline["roots"][0]["kind"] == "module"
            outline_items = {item["name"]: item for item in outline["items"]}
            assert outline_items["top"]["id"] == "outline:0"
            assert outline_items["child_i"]["parentId"] == "outline:0"
            assert outline_items["child_i"]["detail"] == "child"
            assert outline_items["child_i"]["moduleName"] == "child"
            assert outline_items["ready"]["parentId"] == "outline:0"
            assert outline_items["ready"]["detail"] == "logic"
            assert outline_items["ready"]["type"] == "logic"

            definition = request(
                process,
                2,
                "textDocument/definition",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 3},
                },
            )["result"]
            assert len(definition) == 1
            assert definition[0]["uri"] == child_uri
            assert definition[0]["range"]["start"] == {"line": 0, "character": 7}

            implementations = request(
                process,
                3,
                "textDocument/implementation",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 3},
                },
            )["result"]
            assert len(implementations) == 1
            assert implementations[0]["uri"] == top_uri
            assert implementations[0]["range"]["start"] == {"line": 3, "character": 2}
            assert implementations[0]["range"]["end"] == {"line": 3, "character": 7}

            references = request(
                process,
                4,
                "textDocument/references",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 4, "character": 9},
                    "context": {"includeDeclaration": False},
                },
            )["result"]
            assert len(references) == 2
            assert all(item["range"]["start"]["line"] == 5 for item in references)

            workspace_symbols = request(
                process,
                5,
                "workspace/symbol",
                {"query": "ch"},
            )["result"]
            assert any(
                item["name"] == "child" and item["location"]["uri"] == child_uri
                for item in workspace_symbols
            )

            completions = request(
                process,
                6,
                "textDocument/completion",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 4},
                    "context": {"triggerKind": 1},
                },
            )["result"]
            completions = completion_items(completions)
            labels = {item["label"] for item in completions}
            assert "child" in labels
            assert "child_i" in labels
            child_completion = next(item for item in completions if item["label"] == "child")
            assert child_completion["data"]["source"] == "semanticEngine"
            resolved_child = request(
                process,
                25,
                "completionItem/resolve",
                child_completion,
            )["result"]
            assert resolved_child["detail"] == "child(input logic clk, output logic rst_n)"
            assert resolved_child["documentation"]["kind"] == "markdown"
            assert "Ports:" in resolved_child["documentation"]["value"]
            assert resolved_child["insertTextFormat"] == 2
            assert ".clk(${2:clk})" in resolved_child["insertText"]

            port_completions = request(
                process,
                26,
                "textDocument/completion",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 26},
                    "context": {"triggerKind": 1},
                },
            )["result"]
            port_completions = completion_items(port_completions)
            rst_completion = next(item for item in port_completions if item["label"] == "rst_n")
            assert rst_completion["data"]["source"] == "semanticEngine"
            resolved_port = request(
                process,
                27,
                "completionItem/resolve",
                rst_completion,
            )["result"]
            assert resolved_port["detail"] == "output logic rst_n"
            assert resolved_port["insertText"] == "rst_n(${1:rst_n})"

            module_context_completions = request(
                process,
                28,
                "textDocument/completion",
                {
                    "textDocument": {"uri": module_context_uri},
                    "position": {"line": 4, "character": 4},
                    "context": {"triggerKind": 1},
                },
            )["result"]
            module_context_completions = completion_items(module_context_completions)
            module_context_labels = {item["label"] for item in module_context_completions}
            assert "chard" in module_context_labels
            assert "chip" in module_context_labels

            filtered_port_completions = request(
                process,
                29,
                "textDocument/completion",
                {
                    "textDocument": {"uri": port_filter_uri},
                    "position": {"line": 3, "character": 33},
                    "context": {"triggerKind": 2, "triggerCharacter": "."},
                },
            )["result"]
            filtered_port_completions = completion_items(filtered_port_completions)
            filtered_labels = {item["label"] for item in filtered_port_completions}
            assert "clk" not in filtered_labels
            assert "rst_n" in filtered_labels
            assert "data" in filtered_labels

            macro_completions = request(
                process,
                30,
                "textDocument/completion",
                {
                    "textDocument": {"uri": macro_context_uri},
                    "position": {"line": 3, "character": 20},
                    "context": {"triggerKind": 1},
                },
            )["result"]
            macro_completions = completion_items(macro_completions)
            feature_completion = next(item for item in macro_completions if item["label"] == "FEATURE")
            assert feature_completion["detail"] == "Macro"
            assert feature_completion["data"]["source"] == "semanticEngine"
            resolved_feature = request(
                process,
                31,
                "completionItem/resolve",
                feature_completion,
            )["result"]
            assert resolved_feature["label"] == "FEATURE"
            assert "Body:" in resolved_feature["documentation"]["value"]
            assert "1" in resolved_feature["documentation"]["value"]

            highlights = request(
                process,
                7,
                "textDocument/documentHighlight",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 4, "character": 9},
                },
            )["result"]
            assert len(highlights) == 3
            assert highlights[0]["range"]["start"] == {"line": 4, "character": 8}
            assert highlights[1]["range"]["start"]["line"] == 5

            rename = request(
                process,
                8,
                "textDocument/rename",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 4, "character": 9},
                    "newName": "valid",
                },
            )["result"]
            assert set(rename["changes"].keys()) == {top_uri}
            assert len(rename["changes"][top_uri]) == 3
            assert all(edit["newText"] == "valid" for edit in rename["changes"][top_uri])

            links = request(
                process,
                9,
                "textDocument/documentLink",
                {"textDocument": {"uri": top_uri}},
            )["result"]
            assert links == [
                {
                    "range": {
                        "start": {"line": 0, "character": 10},
                        "end": {"line": 0, "character": 18},
                    },
                    "target": defs_uri,
                }
            ]

            inlay_hints = request(
                process,
                10,
                "textDocument/inlayHint",
                {
                    "textDocument": {"uri": top_uri},
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 7, "character": 0},
                    },
                },
            )["result"]
            assert any(
                item == {
                    "position": {"line": 3, "character": 15},
                    "label": ": child",
                    "kind": 1,
                    "tooltip": "child(input logic clk, output logic rst_n)",
                }
                for item in inlay_hints
            )

            code_actions = request(
                process,
                11,
                "textDocument/codeAction",
                {
                    "textDocument": {"uri": top_uri},
                    "range": {
                        "start": {"line": 1, "character": 10},
                        "end": {"line": 1, "character": 21},
                    },
                    "context": {"diagnostics": []},
                },
            )["result"]
            assert len(code_actions) == 1
            assert code_actions[0]["title"] == "Create include file 'missing.svh'"
            assert code_actions[0]["kind"] == "quickfix"
            assert code_actions[0]["diagnostics"][0]["code"] == "unknownInclude"
            assert (
                code_actions[0]["diagnostics"][0]["message"]
                == "Include file 'missing.svh' could not be resolved."
            )
            assert code_actions[0]["edit"]["documentChanges"][0] == {
                "kind": "create",
                "uri": missing_uri,
                "options": {"ignoreIfExists": True},
            }

            module_actions = request(
                process,
                32,
                "textDocument/codeAction",
                {
                    "textDocument": {"uri": unresolved_module_uri},
                    "range": {
                        "start": {"line": 1, "character": 2},
                        "end": {"line": 1, "character": 15},
                    },
                    "context": {"diagnostics": []},
                },
            )["result"]
            assert len(module_actions) == 1
            assert module_actions[0]["title"] == "Create stub module 'missing_child'"
            assert module_actions[0]["diagnostics"][0]["code"] == "unresolvedModule"
            assert module_actions[0]["edit"]["changes"][unresolved_module_uri][0] == {
                "range": {
                    "start": {"line": 3, "character": 0},
                    "end": {"line": 3, "character": 0},
                },
                "newText": "\nmodule missing_child;\nendmodule\n",
            }

            port_actions = request(
                process,
                33,
                "textDocument/codeAction",
                {
                    "textDocument": {"uri": port_quickfix_uri},
                    "range": {
                        "start": {"line": 3, "character": 14},
                        "end": {"line": 3, "character": 21},
                    },
                    "context": {"diagnostics": []},
                },
            )["result"]
            assert len(port_actions) == 1
            assert port_actions[0]["title"] == "Add missing port connections to 'u_child'"
            assert port_actions[0]["kind"] == "quickfix"
            assert port_actions[0]["edit"]["changes"][port_quickfix_uri][0] == {
                "range": {
                    "start": {"line": 3, "character": 31},
                    "end": {"line": 3, "character": 31},
                },
                "newText": ", .rst_n(rst_n), .data(data)",
            }

            type_actions = request(
                process,
                34,
                "textDocument/codeAction",
                {
                    "textDocument": {"uri": unresolved_type_uri},
                    "range": {
                        "start": {"line": 1, "character": 2},
                        "end": {"line": 1, "character": 11},
                    },
                    "context": {"diagnostics": []},
                },
            )["result"]
            assert len(type_actions) == 1
            assert type_actions[0]["title"] == "Create typedef 'missing_t'"
            assert type_actions[0]["diagnostics"][0]["code"] == "unresolvedType"
            assert type_actions[0]["edit"]["changes"][unresolved_type_uri][0] == {
                "range": {
                    "start": {"line": 3, "character": 0},
                    "end": {"line": 3, "character": 0},
                },
                "newText": "\ntypedef logic missing_t;\n",
            }

            cone_trace = request(
                process,
                35,
                "systemverilog/backwardCone",
                {
                    "textDocument": {"uri": cone_uri},
                    "position": {"line": 4, "character": 9},
                },
            )["result"]
            assert cone_trace["messages"] == []
            cone_nodes = {node["name"]: node for node in cone_trace["nodes"]}
            assert set(cone_nodes) == {"out", "mid", "a", "b"}
            assert cone_trace["rootSymbolId"] == cone_nodes["out"]["id"]
            cone_edges = {(edge["from"], edge["to"]) for edge in cone_trace["edges"]}
            assert (cone_nodes["out"]["id"], cone_nodes["mid"]["id"]) in cone_edges
            assert (cone_nodes["mid"]["id"], cone_nodes["a"]["id"]) in cone_edges
            assert (cone_nodes["mid"]["id"], cone_nodes["b"]["id"]) in cone_edges

            prepare_top = request(
                process,
                12,
                "textDocument/prepareCallHierarchy",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 2, "character": 8},
                },
            )["result"]
            assert len(prepare_top) == 1
            top_item = prepare_top[0]
            assert top_item["name"] == "top"
            assert top_item["uri"] == top_uri

            top_outgoing = request(
                process,
                13,
                "callHierarchy/outgoingCalls",
                {"item": top_item},
            )["result"]
            assert len(top_outgoing) == 1
            assert top_outgoing[0]["to"]["name"] == "child"
            assert top_outgoing[0]["to"]["uri"] == child_uri
            assert top_outgoing[0]["fromRanges"][0]["start"] == {"line": 3, "character": 2}

            prepare_child = request(
                process,
                14,
                "textDocument/prepareCallHierarchy",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 3},
                },
            )["result"]
            assert len(prepare_child) == 1
            child_item = prepare_child[0]
            assert child_item["name"] == "child"
            assert child_item["uri"] == child_uri

            child_incoming = request(
                process,
                15,
                "callHierarchy/incomingCalls",
                {"item": child_item},
            )["result"]
            assert len(child_incoming) == 1
            assert child_incoming[0]["from"]["name"] == "top"
            assert child_incoming[0]["from"]["uri"] == top_uri

            child_outgoing = request(
                process,
                16,
                "callHierarchy/outgoingCalls",
                {"item": child_item},
            )["result"]
            assert len(child_outgoing) == 1
            assert child_outgoing[0]["to"]["name"] == "leaf"
            assert child_outgoing[0]["to"]["uri"] == leaf_uri

            folding_ranges = request(
                process,
                17,
                "textDocument/foldingRange",
                {"textDocument": {"uri": top_uri}},
            )["result"]
            assert any(
                item["startLine"] == 2 and item["endLine"] == 6
                for item in folding_ranges
            )

            semantic_tokens = request(
                process,
                18,
                "textDocument/semanticTokens/full",
                {"textDocument": {"uri": top_uri}},
            )["result"]
            assert semantic_tokens["data"]
            assert len(semantic_tokens["data"]) % 5 == 0

            selection_ranges = request(
                process,
                19,
                "textDocument/selectionRange",
                {
                    "textDocument": {"uri": top_uri},
                    "positions": [{"line": 4, "character": 9}],
                },
            )["result"]
            assert len(selection_ranges) == 1
            selection_range = selection_ranges[0]
            assert selection_range["range"]["start"] == {"line": 4, "character": 8}
            assert selection_range["parent"]["range"]["start"] == {"line": 4, "character": 2}
            assert selection_range["parent"]["parent"]["range"]["start"] == {"line": 2, "character": 0}

            signature_help = request(
                process,
                20,
                "textDocument/signatureHelp",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 3, "character": 25},
                },
            )["result"]
            assert (
                signature_help["signatures"][0]["label"]
                == "child(input logic clk, output logic rst_n)"
            )
            assert signature_help["signatures"][0]["parameters"] == [
                {"label": "input logic clk"},
                {"label": "output logic rst_n"},
            ]
            assert signature_help["activeParameter"] == 1

            prepare_rename = request(
                process,
                21,
                "textDocument/prepareRename",
                {
                    "textDocument": {"uri": top_uri},
                    "position": {"line": 4, "character": 9},
                },
            )["result"]
            assert prepare_rename == {
                "range": {
                    "start": {"line": 4, "character": 8},
                    "end": {"line": 4, "character": 13},
                },
                "placeholder": "ready",
            }

            type_definition = request(
                process,
                22,
                "textDocument/typeDefinition",
                {
                    "textDocument": {"uri": typed_uri},
                    "position": {"line": 2, "character": 3},
                },
            )["result"]
            assert len(type_definition) == 1
            assert type_definition[0]["uri"] == typed_uri
            assert type_definition[0]["range"]["start"] == {"line": 1, "character": 22}
            assert type_definition[0]["range"]["end"] == {"line": 1, "character": 28}

            watched = rtl / "watched.sv"
            watched.write_text("module watched_live; endmodule\n", encoding="utf-8")
            watched_uri = watched.resolve().as_uri()
            notify(
                process,
                "workspace/didChangeWatchedFiles",
                {"changes": [{"uri": watched_uri, "type": 1}]},
            )
            watched_symbols = request(
                process,
                23,
                "workspace/symbol",
                {"query": "watched_live"},
            )["result"]
            assert any(
                item["name"] == "watched_live" and item["location"]["uri"] == watched_uri
                for item in watched_symbols
            )

            request(process, 24, "shutdown", None)
            notify(process, "exit", None)
            return_code = process.wait(timeout=5)
            assert return_code == 0
        finally:
            if process.poll() is None:
                process.kill()
            stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
            if process.returncode not in (0, None) and stderr:
                print(stderr, file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
