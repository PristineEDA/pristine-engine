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


def notify(process: subprocess.Popen[bytes], method: str, params: dict | None) -> None:
    message = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        message["params"] = params
    write_message(process, message)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lsp_core_smoke.py <pristine-engine>", file=sys.stderr)
        return 2

    server_path = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="pristine-lsp-e2e-") as temp_dir:
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
            "module child; endmodule\n"
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
        leaf_uri = leaf.resolve().as_uri()
        child_uri = child.resolve().as_uri()
        typed_uri = typed.resolve().as_uri()
        module_context_uri = module_context.resolve().as_uri()
        macro_context_uri = macro_context.resolve().as_uri()
        port_filter_uri = port_filter.resolve().as_uri()
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
            labels = {item["label"] for item in completions}
            assert "child" in labels
            assert "child_i" in labels
            child_completion = next(item for item in completions if item["label"] == "child")
            assert child_completion["data"]["source"] == "semantic"
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
            rst_completion = next(item for item in port_completions if item["label"] == "rst_n")
            assert rst_completion["data"]["source"] == "port"
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
            assert module_context_completions[0]["label"] == "child"
            assert module_context_completions[1]["label"] == "chip"

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
            feature_completion = next(item for item in macro_completions if item["label"] == "FEATURE")
            assert feature_completion["detail"] == "Macro"
            assert feature_completion["data"]["source"] == "macro"
            resolved_feature = request(
                process,
                31,
                "completionItem/resolve",
                feature_completion,
            )["result"]
            assert resolved_feature["insertText"] == "FEATURE"
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
            assert inlay_hints == [
                {
                    "position": {"line": 3, "character": 15},
                    "label": ": child",
                    "kind": 1,
                    "tooltip": "child(input logic clk, output logic rst_n)",
                }
            ]

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
            assert code_actions[0]["edit"]["documentChanges"][0] == {
                "kind": "create",
                "uri": missing_uri,
                "options": {"ignoreIfExists": True},
            }

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
