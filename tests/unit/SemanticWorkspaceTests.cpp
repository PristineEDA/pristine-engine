#include "pristine/analysis/SemanticWorkspace.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace pristine::analysis {

TEST_CASE("SemanticWorkspace resolves cross-file module definitions", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/child.sv", "module child; endmodule\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  child child_i();\n"
                             "endmodule\n");

    const auto definitions = workspace.definitionsAt("file:///workspace/top.sv", 1, 3);

    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().name == "child");
    CHECK(definitions.front().location.uri == "file:///workspace/child.sv");
    CHECK(definitions.front().scope_path == "$root");
}

TEST_CASE("SemanticWorkspace keeps same-name references scoped", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/shadowed.sv",
                             "module first;\n"
                             "  logic ready;\n"
                             "  assign ready = ready;\n"
                             "endmodule\n"
                             "module second;\n"
                             "  logic ready;\n"
                             "  assign ready = ready;\n"
                             "endmodule\n");

    const auto first_references = workspace.referencesAt("file:///workspace/shadowed.sv", 1, 9, false);
    REQUIRE(first_references.size() == 2);
    CHECK(std::all_of(first_references.begin(), first_references.end(), [](const SemanticReference& reference) {
        return reference.location.range.start_line == 2;
    }));

    const auto second_references = workspace.referencesAt("file:///workspace/shadowed.sv", 5, 9, false);
    REQUIRE(second_references.size() == 2);
    CHECK(std::all_of(second_references.begin(), second_references.end(), [](const SemanticReference& reference) {
        return reference.location.range.start_line == 6;
    }));

    const auto* document = workspace.document("file:///workspace/shadowed.sv");
    REQUIRE(document != nullptr);
    const auto declaration_it = std::find_if(document->references.begin(), document->references.end(),
                                            [](const SemanticReference& reference) {
                                                return reference.name == "ready" && reference.is_declaration &&
                                                       reference.location.range.start_line == 1;
                                            });
    REQUIRE(declaration_it != document->references.end());
    REQUIRE(declaration_it->target_symbol_id.has_value());
    CHECK(std::all_of(first_references.begin(), first_references.end(), [&](const SemanticReference& reference) {
        return reference.target_symbol_id == declaration_it->target_symbol_id;
    }));
}

TEST_CASE("SemanticWorkspace resolves imported package symbols", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/defs.sv",
                             "package defs;\n"
                             "  typedef logic [7:0] word_t;\n"
                             "endpackage\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  import defs::*;\n"
                             "  word_t value;\n"
                             "endmodule\n");

    const auto definitions = workspace.findTypeDefinitionsAt("file:///workspace/top.sv", 2, 3);

    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().name == "word_t");
    CHECK(definitions.front().scope_path == "$root::defs");
    CHECK(definitions.front().location.uri == "file:///workspace/defs.sv");
}

TEST_CASE("SemanticWorkspace evaluates parameterized widths and hover metadata", "[analysis][semantic][types]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/typed.sv",
                             "module top #(parameter int WIDTH = 8) (\n"
                             "  input logic [WIDTH-1:0] data\n"
                             ");\n"
                             "endmodule\n");

    const auto* document = workspace.document("file:///workspace/typed.sv");
    REQUIRE(document != nullptr);

    const auto width = std::find_if(document->symbols.begin(), document->symbols.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "WIDTH";
    });
    REQUIRE(width != document->symbols.end());
    REQUIRE(width->constant_value.has_value());
    CHECK(*width->constant_value == 8);

    const auto data = std::find_if(document->symbols.begin(), document->symbols.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "data";
    });
    REQUIRE(data != document->symbols.end());
    CHECK(data->direction == "input");
    REQUIRE(data->type.has_value());
    REQUIRE(data->type->bit_width.has_value());
    CHECK(*data->type->bit_width == 8);

    const auto hover = workspace.hoverAt("file:///workspace/typed.sv",
                                         data->selection_range.start_line,
                                         data->selection_range.start_character);
    REQUIRE(hover.has_value());
    CHECK(hover->contents.find("data") != std::string::npos);
    CHECK(hover->contents.find("Direction: `input`") != std::string::npos);
    CHECK(hover->contents.find("Width: `8 bits`") != std::string::npos);
}

TEST_CASE("SemanticWorkspace resolves typedef aliases into hover metadata", "[analysis][semantic][types]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/alias.sv",
                             "typedef logic [7:0] byte_t;\n"
                             "module top;\n"
                             "  byte_t value;\n"
                             "endmodule\n");

    const auto* document = workspace.document("file:///workspace/alias.sv");
    REQUIRE(document != nullptr);
    const auto value = std::find_if(document->symbols.begin(), document->symbols.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "value";
    });
    REQUIRE(value != document->symbols.end());
    REQUIRE(value->type.has_value());
    CHECK(value->type->kind == SemanticTypeKind::Alias);
    CHECK(value->type->alias_target.find("logic") != std::string::npos);
    REQUIRE(value->type->bit_width.has_value());
    CHECK(*value->type->bit_width == 8);

    const auto hover = workspace.hoverAt("file:///workspace/alias.sv",
                                         value->selection_range.start_line,
                                         value->selection_range.start_character);
    REQUIRE(hover.has_value());
    CHECK(hover->contents.find("value: byte_t") != std::string::npos);
    CHECK(hover->contents.find("Alias: `logic [7:0]`") != std::string::npos);
}

TEST_CASE("SemanticWorkspace scopes named generate blocks", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/generate.sv",
                             "module top;\n"
                             "  logic ready;\n"
                             "  if (1) begin : g\n"
                             "    logic ready;\n"
                             "    assign ready = ready;\n"
                             "  end\n"
                             "endmodule\n");

    const auto definitions = workspace.findDefinitionsAt("file:///workspace/generate.sv", 4, 11);
    const auto references = workspace.findReferencesAt("file:///workspace/generate.sv", 3, 11, false);

    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().name == "ready");
    CHECK(definitions.front().scope_path == "$root::top::g");
    REQUIRE(references.size() == 2);
    CHECK(std::all_of(references.begin(), references.end(), [](const SemanticReference& reference) {
        return reference.location.range.start_line == 4;
    }));
}

TEST_CASE("SemanticWorkspace reports visible symbols from nearest scope outward", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/visible.sv",
                             "module visible;\n"
                             "  logic local_ready;\n"
                             "  function void sample();\n"
                             "  endfunction\n"
                             "endmodule\n");

    const auto visible = workspace.visibleSymbolsAt("file:///workspace/visible.sv", 3, 4, "");

    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "local_ready";
    }));
    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "sample";
    }));
    CHECK(std::any_of(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "visible";
    }));

    const auto local_it = std::find_if(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "local_ready";
    });
    const auto root_it = std::find_if(visible.begin(), visible.end(), [](const SemanticSymbol& symbol) {
        return symbol.name == "visible";
    });
    REQUIRE(local_it != visible.end());
    REQUIRE(root_it != visible.end());
    CHECK(local_it < root_it);
}

TEST_CASE("SemanticWorkspace reports duplicate symbols in the same scope",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/duplicates.sv",
                             "module top;\n"
                             "  logic ready;\n"
                             "  logic ready;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/duplicates.sv");

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().code == "duplicateSymbol");
    CHECK(diagnostics.front().message == "Duplicate symbol 'ready' in the same scope.");
    CHECK(diagnostics.front().range.start_line == 2);
    CHECK(diagnostics.front().range.start_character == 8);
}

TEST_CASE("SemanticWorkspace reports ambiguous imported references",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/pkg_a.sv",
                             "package pkg_a;\n"
                             "  typedef logic [7:0] word_t;\n"
                             "endpackage\n");
    workspace.updateDocument("file:///workspace/pkg_b.sv",
                             "package pkg_b;\n"
                             "  typedef logic [15:0] word_t;\n"
                             "endpackage\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  import pkg_a::*;\n"
                             "  import pkg_b::*;\n"
                             "  word_t value;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/top.sv");

    const auto diagnostic = std::find_if(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "ambiguousReference";
    });
    REQUIRE(diagnostic != diagnostics.end());
    CHECK(diagnostic->message == "Symbol 'word_t' has 2 possible definitions in scope.");
    CHECK(diagnostic->range.start_line == 3);
    CHECK(diagnostic->range.start_character == 2);
    CHECK(diagnostic->severity == 2);
}

TEST_CASE("SemanticWorkspace does not report local shadowing as ambiguous",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/shadowing.sv",
                             "module top;\n"
                             "  logic flag;\n"
                             "  if (1) begin : g\n"
                             "    logic flag;\n"
                             "    assign flag = flag;\n"
                             "  end\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/shadowing.sv");

    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "ambiguousReference";
    }));
}

TEST_CASE("SemanticWorkspace reports unresolved package imports",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/missing-package.sv",
                             "module top;\n"
                             "  import missing_pkg::*;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/missing-package.sv");

    const auto diagnostic = std::find_if(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "unresolvedPackage";
    });
    REQUIRE(diagnostic != diagnostics.end());
    CHECK(diagnostic->message == "Package 'missing_pkg' could not be resolved.");
    CHECK(diagnostic->range.start_line == 1);
    CHECK(diagnostic->range.start_character == 9);
    CHECK(diagnostic->severity == 1);
}

TEST_CASE("SemanticWorkspace resolves packages despite local same-name symbols",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/defs.sv", "package defs; endpackage\n");
    workspace.updateDocument("file:///workspace/top.sv",
                             "module top;\n"
                             "  logic defs;\n"
                             "  import defs::*;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/top.sv");

    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "unresolvedPackage";
    }));
}

TEST_CASE("SemanticWorkspace reports unresolved type references",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/unresolved-type.sv",
                             "module top;\n"
                             "  missing_t value;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/unresolved-type.sv");

    const auto diagnostic = std::find_if(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "unresolvedType";
    });
    REQUIRE(diagnostic != diagnostics.end());
    CHECK(diagnostic->message == "Type 'missing_t' could not be resolved.");
    CHECK(diagnostic->range.start_line == 1);
    CHECK(diagnostic->range.start_character == 2);
    CHECK(diagnostic->range.end_character == 11);
    CHECK(diagnostic->severity == 1);
}

TEST_CASE("SemanticWorkspace does not report resolved typedef references as unresolved",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/resolved-type.sv",
                             "typedef logic [7:0] byte_t;\n"
                             "module top;\n"
                             "  byte_t value;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/resolved-type.sv");

    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "unresolvedType";
    }));
}

TEST_CASE("SemanticWorkspace reports simple assignment width mismatches",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/width-mismatch.sv",
                             "module top;\n"
                             "  logic [3:0] lhs;\n"
                             "  logic [7:0] rhs;\n"
                             "  assign lhs = rhs;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/width-mismatch.sv");

    const auto diagnostic = std::find_if(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "widthMismatch";
    });
    REQUIRE(diagnostic != diagnostics.end());
    CHECK(diagnostic->message == "Width mismatch: assigning 8-bit 'rhs' to 4-bit 'lhs'.");
    CHECK(diagnostic->range.start_line == 3);
    CHECK(diagnostic->range.start_character == 15);
    CHECK(diagnostic->range.end_character == 18);
    CHECK(diagnostic->severity == 2);
}

TEST_CASE("SemanticWorkspace does not report matching assignment widths",
          "[analysis][semantic][diagnostics]") {
    SemanticWorkspace workspace;
    workspace.updateDocument("file:///workspace/width-match.sv",
                             "module top;\n"
                             "  logic [3:0] lhs;\n"
                             "  logic [3:0] rhs;\n"
                             "  assign lhs = rhs;\n"
                             "endmodule\n");

    const auto diagnostics = workspace.diagnosticsFor("file:///workspace/width-match.sv");

    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const SemanticDiagnostic& item) {
        return item.code == "widthMismatch";
    }));
}

TEST_CASE("SemanticWorkspace tracks include directives per document", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/includes.sv",
                             "`include \"defs.svh\"\n"
                             "`include \"rtl/types.svh\"\n"
                             "module top; endmodule\n",
                             SemanticDocumentState{.version = 7,
                                                   .is_open = true,
                                                   .dirty = true,
                                                   .invalidate_dependents = false});

    const auto* document = workspace.document("file:///workspace/includes.sv");

    REQUIRE(document != nullptr);
    CHECK(document->version == 7);
    CHECK(document->is_open);
    CHECK(document->dirty);
    REQUIRE(document->includes.size() == 2);
    CHECK(document->includes.front().target == "defs.svh");
    CHECK(document->includes.front().range.start_line == 0);
    REQUIRE(document->included_uris.size() == 2);
    CHECK(document->included_uris.at(0) == "file:///workspace/defs.svh");
    CHECK(document->included_uris.at(1) == "file:///workspace/rtl/types.svh");
}

TEST_CASE("SemanticWorkspace builds reverse include edges and marks dependents stale", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/rtl/top.sv",
                             "`include \"defs.svh\"\n"
                             "module top; endmodule\n");
    workspace.updateDocument("file:///workspace/rtl/defs.svh", "typedef logic bit_t;\n");

    const auto including_top = workspace.includingUris("file:///workspace/rtl/defs.svh");
    REQUIRE(including_top.size() == 1);
    CHECK(including_top.front() == "file:///workspace/rtl/top.sv");
    CHECK(workspace.staleDocumentUris().empty());

    workspace.updateDocument("file:///workspace/rtl/defs.svh", "typedef bit bit_t;\n",
                             SemanticDocumentState{.version = -1,
                                                   .is_open = false,
                                                   .dirty = false,
                                                   .invalidate_dependents = true});

    const auto stale_documents = workspace.staleDocumentUris();
    REQUIRE(stale_documents.size() == 1);
    CHECK(stale_documents.front() == "file:///workspace/rtl/top.sv");
}

TEST_CASE("SemanticWorkspace clears dependency graph when documents are removed", "[analysis][semantic]") {
    SemanticWorkspace workspace;
    workspace.setWorkspaceRoot("file:///workspace");
    workspace.updateDocument("file:///workspace/top.sv",
                             "`include \"defs.svh\"\n"
                             "module top; endmodule\n");

    REQUIRE_FALSE(workspace.includingUris("file:///workspace/defs.svh").empty());

    workspace.removeDocument("file:///workspace/top.sv");

    CHECK(workspace.includingUris("file:///workspace/defs.svh").empty());
    CHECK(workspace.documentCount() == 0);
}

} // namespace pristine::analysis