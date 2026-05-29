#include "pristine/analysis/SemanticEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace pristine::analysis {
namespace {

namespace fs = std::filesystem;

struct SemanticGoldenCase {
    std::string_view name;
    std::string_view uri;
    std::string_view text;
    int line = 0;
    int character = 0;
    std::string_view expected_symbol;
    size_t expected_references = 0;
};

struct SemanticDiagnosticGoldenCase {
    std::string_view name;
    std::string_view uri;
    std::string_view text;
    std::string_view code;
    std::string_view message_fragment;
    bool expected_present = true;
    int expected_severity = 0;
    int expected_start_line = -1;
};

constexpr SemanticGoldenCase kGoldenCases[] = {
    SemanticGoldenCase{.name = "local signal references",
                       .uri = "file:///golden/local.sv",
                       .text = "module top;\n"
                               "  logic ready;\n"
                               "  assign ready = ready;\n"
                               "endmodule\n",
                       .line = 1,
                       .character = 9,
                       .expected_symbol = "ready",
                       .expected_references = 3},
    SemanticGoldenCase{.name = "module identifier lookup",
                       .uri = "file:///golden/module.sv",
                       .text = "module child;\n"
                               "endmodule\n"
                               "module top;\n"
                               "  child u_child();\n"
                               "endmodule\n",
                       .line = 3,
                       .character = 4,
                       .expected_symbol = "child",
                       .expected_references = 2},
    SemanticGoldenCase{.name = "shadowed locals stay scoped",
                       .uri = "file:///golden/shadow.sv",
                       .text = "module first;\n"
                               "  logic value;\n"
                               "  assign value = value;\n"
                               "endmodule\n"
                               "module second;\n"
                               "  logic value;\n"
                               "  assign value = value;\n"
                               "endmodule\n",
                       .line = 1,
                       .character = 9,
                       .expected_symbol = "value",
                       .expected_references = 3},
    SemanticGoldenCase{.name = "typedef value lookup",
                       .uri = "file:///golden/type.sv",
                       .text = "module top;\n"
                               "  typedef logic [7:0] byte_t;\n"
                               "  byte_t data;\n"
                               "  assign data = '0;\n"
                               "endmodule\n",
                       .line = 2,
                       .character = 10,
                       .expected_symbol = "data",
                       .expected_references = 2},
};

constexpr SemanticDiagnosticGoldenCase kDiagnosticGoldenCases[] = {
    SemanticDiagnosticGoldenCase{.name = "duplicate symbol diagnostic",
                                 .uri = "file:///golden/diagnostics/duplicate.sv",
                                 .text = "module top;\n"
                                         "  logic ready;\n"
                                         "  logic ready;\n"
                                         "endmodule\n",
                                 .code = "duplicateSymbol",
                                 .message_fragment = "Duplicate symbol 'ready'",
                                 .expected_severity = 1,
                                 .expected_start_line = 2},
    SemanticDiagnosticGoldenCase{.name = "unresolved package diagnostic",
                                 .uri = "file:///golden/diagnostics/missing-package.sv",
                                 .text = "module top;\n"
                                         "  import missing_pkg::*;\n"
                                         "endmodule\n",
                                 .code = "unresolvedPackage",
                                 .message_fragment = "missing_pkg",
                                 .expected_severity = 1,
                                 .expected_start_line = 1},
    SemanticDiagnosticGoldenCase{.name = "assignment width mismatch diagnostic",
                                 .uri = "file:///golden/diagnostics/width.sv",
                                 .text = "module top;\n"
                                         "  logic [3:0] lhs;\n"
                                         "  logic [7:0] rhs;\n"
                                         "  assign lhs = rhs;\n"
                                         "endmodule\n",
                                 .code = "widthMismatch",
                                 .message_fragment = "assigning 8-bit 'rhs' to 4-bit 'lhs'",
                                 .expected_severity = 2,
                                 .expected_start_line = 3},
    SemanticDiagnosticGoldenCase{.name = "local symbol does not shadow package resolution",
                                 .uri = "file:///golden/diagnostics/package-shadow.sv",
                                 .text = "package defs; endpackage\n"
                                         "module top;\n"
                                         "  logic defs;\n"
                                         "  import defs::*;\n"
                                         "endmodule\n",
                                 .code = "unresolvedPackage",
                                 .message_fragment = "defs",
                                 .expected_present = false},
};

std::string readTextFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

fs::path repositoryRoot() {
    auto current = fs::current_path();
    while (!current.empty()) {
        if (fs::exists(current / "CMakeLists.txt") && fs::exists(current / "tests")) {
            return current;
        }
        current = current.parent_path();
    }
    return fs::current_path();
}

std::vector<fs::path> semanticFixturePaths() {
    const auto root = repositoryRoot() / "tests" / "golden" / "semantic";
    std::vector<fs::path> result;
    if (!fs::exists(root)) {
        return result;
    }
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

void loadSources(SemanticEngine& engine, const nlohmann::json& fixture) {
    for (const auto& source : fixture.at("sources")) {
        engine.updateDocument(source.at("uri").get<std::string>(),
                              source.at("text").get<std::string>(),
                              SemanticEngineDocumentState{.version = source.value("version", 1),
                                                          .is_open = source.value("isOpen", true),
                                                          .dirty = source.value("dirty", false)});
    }
}

void runLookupFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.lookupAt(request.at("uri").get<std::string>(),
                                        request.at("line").get<int>(),
                                        request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("symbol")) {
        REQUIRE(result.symbol.has_value());
        CHECK(result.symbol->name == expected.at("symbol").get<std::string>());
    }
}

void runReferencesFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.referencesAt(request.at("uri").get<std::string>(),
                                            request.at("line").get<int>(),
                                            request.at("character").get<int>(),
                                            request.value("includeDeclaration", true));
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("count")) {
        CHECK(result.locations.size() == expected.at("count").get<size_t>());
    }
    if (expected.contains("allBeforeLine")) {
        const auto line = expected.at("allBeforeLine").get<int>();
        CHECK(std::all_of(result.locations.begin(), result.locations.end(), [line](const SemanticLocation& location) {
            return location.range.start_line < line;
        }));
    }
}

void runDiagnosticsFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& expected = fixture.at("expected");
    const auto diagnostics = engine.diagnosticsFor(expected.at("uri").get<std::string>());
    for (const auto& item : expected.at("diagnostics")) {
        const auto code = item.at("code").get<std::string>();
        const auto present = item.value("present", true);
        const auto found = std::find_if(diagnostics.begin(),
                                        diagnostics.end(),
                                        [&](const SemanticEngineDiagnostic& diagnostic) {
                                            return diagnostic.code == code;
                                        });
        if (!present) {
            CHECK(found == diagnostics.end());
            continue;
        }
        REQUIRE(found != diagnostics.end());
        if (item.contains("severity")) {
            CHECK(found->severity == item.at("severity").get<int>());
        }
        if (item.contains("startLine")) {
            CHECK(found->range.start_line == item.at("startLine").get<int>());
        }
    }
}

} // namespace

TEST_CASE("Semantic golden cases exercise first-batch query contracts",
          "[analysis][golden][semantic]") {
    for (const auto& test_case : kGoldenCases) {
        CAPTURE(test_case.name);
        SemanticEngine engine;
        engine.updateDocument(test_case.uri,
                              test_case.text,
                              SemanticEngineDocumentState{.version = 1, .is_open = true});

        const auto lookup = engine.lookupAt(test_case.uri, test_case.line, test_case.character);
        REQUIRE_FALSE(lookup.unresolved);
        REQUIRE(lookup.symbol.has_value());
        CHECK(lookup.symbol->name == test_case.expected_symbol);

        const auto references = engine.referencesAt(test_case.uri,
                                                    test_case.line,
                                                    test_case.character,
                                                    true);
        REQUIRE_FALSE(references.unresolved);
        CHECK(references.locations.size() == test_case.expected_references);

        const auto hover = engine.hoverAt(test_case.uri, test_case.line, test_case.character);
        REQUIRE_FALSE(hover.unresolved);
        CHECK(hover.contents.find(test_case.expected_symbol) != std::string::npos);

        const auto rename = engine.renameAt(test_case.uri,
                                           test_case.line,
                                           test_case.character,
                                           "renamed_symbol");
        REQUIRE_FALSE(rename.unresolved);
        CHECK(rename.edits.size() == test_case.expected_references);
    }
}

TEST_CASE("Semantic diagnostic golden cases exercise engine-owned diagnostics",
          "[analysis][golden][semantic][diagnostics]") {
    for (const auto& test_case : kDiagnosticGoldenCases) {
        CAPTURE(test_case.name);
        SemanticEngine engine;
        engine.updateDocument(test_case.uri,
                              test_case.text,
                              SemanticEngineDocumentState{.version = 1, .is_open = true});

        const auto diagnostics = engine.diagnosticsFor(test_case.uri);
        const auto diagnostic = std::find_if(diagnostics.begin(),
                                             diagnostics.end(),
                                             [&](const SemanticEngineDiagnostic& item) {
                                                 return item.code == test_case.code &&
                                                        item.message.find(test_case.message_fragment) !=
                                                            std::string::npos;
                                             });

        if (!test_case.expected_present) {
            CHECK(diagnostic == diagnostics.end());
            continue;
        }

        REQUIRE(diagnostic != diagnostics.end());
        CHECK(diagnostic->severity == test_case.expected_severity);
        CHECK(diagnostic->range.start_line == test_case.expected_start_line);
    }
}

TEST_CASE("JSON semantic golden fixtures exercise stable request shapes",
          "[analysis][golden][semantic][json]") {
    const auto fixtures = semanticFixturePaths();
    REQUIRE_FALSE(fixtures.empty());

    for (const auto& path : fixtures) {
        CAPTURE(path.string());
        const auto fixture = nlohmann::json::parse(readTextFile(path));
        SemanticEngine engine;
        loadSources(engine, fixture);

        const auto kind = fixture.at("request").at("kind").get<std::string>();
        if (kind == "lookup") {
            runLookupFixture(engine, fixture);
        }
        else if (kind == "references") {
            runReferencesFixture(engine, fixture);
        }
        else if (kind == "diagnostics") {
            runDiagnosticsFixture(engine, fixture);
        }
        else {
            FAIL("Unsupported semantic golden request kind: " << kind);
        }
    }
}

} // namespace pristine::analysis
