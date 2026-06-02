#include "pristine/analysis/SemanticEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
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

void configureEngine(SemanticEngine& engine, const nlohmann::json& fixture) {
    if (!fixture.contains("config")) {
        return;
    }

    const auto& config_json = fixture.at("config");
    SemanticEngineConfig config;
    if (config_json.contains("workspaceRootUri")) {
        config.workspace_root_uri = config_json.at("workspaceRootUri").get<std::string>();
    }
    if (config_json.contains("build")) {
        config.build = config_json.at("build").get<std::string>();
    }
    if (config_json.contains("buildPattern")) {
        config.build_pattern = config_json.at("buildPattern").get<std::string>();
    }
    if (config_json.contains("buildRelativePaths")) {
        config.build_relative_paths = config_json.at("buildRelativePaths").get<bool>();
    }
    if (config_json.contains("flags")) {
        config.flags = config_json.at("flags").get<std::string>();
    }
    if (config_json.contains("topModules")) {
        for (const auto& top_module : config_json.at("topModules")) {
            config.top_modules.push_back(top_module.get<std::string>());
        }
    }
    engine.configure(std::move(config));
}

ParseRange parseRangeFromJson(const nlohmann::json& range_json) {
    return ParseRange{.start_line = range_json.at("startLine").get<int>(),
                      .start_character = range_json.at("startCharacter").get<int>(),
                      .end_line = range_json.at("endLine").get<int>(),
                      .end_character = range_json.at("endCharacter").get<int>()};
}

bool locationMatchesJson(const SemanticLocation& location, const nlohmann::json& expected) {
    if (expected.contains("uri") && location.uri != expected.at("uri").get<std::string>()) {
        return false;
    }
    if (expected.contains("startLine") &&
        location.range.start_line != expected.at("startLine").get<int>()) {
        return false;
    }
    if (expected.contains("startCharacter") &&
        location.range.start_character != expected.at("startCharacter").get<int>()) {
        return false;
    }
    if (expected.contains("endLine") &&
        location.range.end_line != expected.at("endLine").get<int>()) {
        return false;
    }
    if (expected.contains("endCharacter") &&
        location.range.end_character != expected.at("endCharacter").get<int>()) {
        return false;
    }
    return true;
}

bool rangeMatchesJson(const ParseRange& range, const nlohmann::json& expected) {
    if (expected.contains("startLine") && range.start_line != expected.at("startLine").get<int>()) {
        return false;
    }
    if (expected.contains("startCharacter") &&
        range.start_character != expected.at("startCharacter").get<int>()) {
        return false;
    }
    if (expected.contains("endLine") && range.end_line != expected.at("endLine").get<int>()) {
        return false;
    }
    if (expected.contains("endCharacter") &&
        range.end_character != expected.at("endCharacter").get<int>()) {
        return false;
    }
    return true;
}

void checkLocations(const std::vector<SemanticLocation>& locations, const nlohmann::json& expected) {
    if (expected.contains("count")) {
        CHECK(locations.size() == expected.at("count").get<size_t>());
    }
    if (expected.contains("locations")) {
        for (const auto& expected_location : expected.at("locations")) {
            CAPTURE(expected_location.dump());
            CHECK(std::any_of(locations.begin(),
                              locations.end(),
                              [&](const SemanticLocation& location) {
                                  return locationMatchesJson(location, expected_location);
                              }));
        }
    }
}

void runSemanticTokensFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.semanticTokens(request.at("uri").get<std::string>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("count")) {
        CHECK(result.tokens.size() == expected.at("count").get<size_t>());
    }
    if (expected.contains("tokens")) {
        for (const auto& expected_token : expected.at("tokens")) {
            CAPTURE(expected_token.dump());
            CHECK(std::any_of(result.tokens.begin(),
                              result.tokens.end(),
                              [&](const SemanticToken& token) {
                                  if (expected_token.contains("type") &&
                                      token.token_type != expected_token.at("type").get<std::string>()) {
                                      return false;
                                  }
                                  if (expected_token.contains("modifier") &&
                                      token.token_modifier != expected_token.at("modifier").get<std::string>()) {
                                      return false;
                                  }
                                  return rangeMatchesJson(token.location.range, expected_token);
                              }));
        }
    }
}

void runSelectionRangeFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.selectionRangesAt(request.at("uri").get<std::string>(),
                                                request.at("line").get<int>(),
                                                request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("minCount")) {
        CHECK(result.ranges.size() >= expected.at("minCount").get<size_t>());
    }
    if (expected.contains("ranges")) {
        for (const auto& expected_range : expected.at("ranges")) {
            CAPTURE(expected_range.dump());
            CHECK(std::any_of(result.ranges.begin(),
                              result.ranges.end(),
                              [&](const SemanticSelectionRange& range) {
                                  return rangeMatchesJson(range.range, expected_range);
                              }));
        }
    }
    if (expected.contains("rootRange")) {
        REQUIRE_FALSE(result.ranges.empty());
        CHECK(rangeMatchesJson(result.ranges.back().range, expected.at("rootRange")));
        CHECK(result.ranges.back().parent == std::nullopt);
    }
}

bool hierarchyContains(const SemanticHierarchyNode& node,
                       std::string_view module_name,
                       std::optional<std::string_view> instance_name = std::nullopt) {
    if (node.module_name == module_name &&
        (!instance_name.has_value() || node.instance_name == *instance_name)) {
        return true;
    }
    return std::any_of(node.children.begin(),
                       node.children.end(),
                       [&](const SemanticHierarchyNode& child) {
                           return hierarchyContains(child, module_name, instance_name);
                       });
}

bool hierarchyContains(const std::vector<SemanticHierarchyNode>& roots,
                       std::string_view module_name,
                       std::optional<std::string_view> instance_name = std::nullopt) {
    return std::any_of(roots.begin(), roots.end(), [&](const SemanticHierarchyNode& root) {
        return hierarchyContains(root, module_name, instance_name);
    });
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

void runDefinitionFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.definitionsAt(request.at("uri").get<std::string>(),
                                             request.at("line").get<int>(),
                                             request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    checkLocations(result.locations, expected);
}

void runTypeDefinitionFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.typeDefinitionsAt(request.at("uri").get<std::string>(),
                                                 request.at("line").get<int>(),
                                                 request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    checkLocations(result.locations, expected);
}

void runHoverFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.hoverAt(request.at("uri").get<std::string>(),
                                       request.at("line").get<int>(),
                                       request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("contentsContains")) {
        for (const auto& expected_fragment : expected.at("contentsContains")) {
            const auto fragment = expected_fragment.get<std::string>();
            CAPTURE(fragment);
            CHECK(result.contents.find(fragment) != std::string::npos);
        }
    }
    if (expected.contains("contentsAbsent")) {
        for (const auto& absent_fragment : expected.at("contentsAbsent")) {
            const auto fragment = absent_fragment.get<std::string>();
            CAPTURE(fragment);
            CHECK(result.contents.find(fragment) == std::string::npos);
        }
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

void runRenameFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.renameAt(request.at("uri").get<std::string>(),
                                        request.at("line").get<int>(),
                                        request.at("character").get<int>(),
                                        request.at("newName").get<std::string>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("count")) {
        CHECK(result.edits.size() == expected.at("count").get<size_t>());
    }
    if (expected.contains("allBeforeLine")) {
        const auto line = expected.at("allBeforeLine").get<int>();
        CHECK(std::all_of(result.edits.begin(), result.edits.end(), [line](const SemanticTextEdit& edit) {
            return edit.location.range.start_line < line;
        }));
    }
    if (expected.contains("allNewText")) {
        const auto new_text = expected.at("allNewText").get<std::string>();
        CHECK(std::all_of(result.edits.begin(), result.edits.end(), [&](const SemanticTextEdit& edit) {
            return edit.new_text == new_text;
        }));
    }
}

void runCompletionFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.completionsAt(request.at("uri").get<std::string>(),
                                             request.at("line").get<int>(),
                                             request.at("character").get<int>(),
                                             request.value("prefix", ""));
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("labels")) {
        for (const auto& expected_label : expected.at("labels")) {
            const auto label = expected_label.get<std::string>();
            CAPTURE(label);
            CHECK(std::any_of(result.items.begin(),
                              result.items.end(),
                              [&](const SemanticCompletionItem& item) {
                                  return item.label == label;
                              }));
        }
    }
    if (expected.contains("absentLabels")) {
        for (const auto& absent_label : expected.at("absentLabels")) {
            const auto label = absent_label.get<std::string>();
            CAPTURE(label);
            CHECK(std::none_of(result.items.begin(),
                               result.items.end(),
                               [&](const SemanticCompletionItem& item) {
                                   return item.label == label;
                               }));
        }
    }
}

void runCompletionResolveFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto completions = engine.completionsAt(request.at("uri").get<std::string>(),
                                                  request.at("line").get<int>(),
                                                  request.at("character").get<int>(),
                                                  request.value("prefix", ""));
    const auto label = request.at("label").get<std::string>();
    const auto item = std::find_if(completions.items.begin(),
                                   completions.items.end(),
                                   [&](const SemanticCompletionItem& candidate) {
                                       return candidate.label == label;
                                   });
    REQUIRE(item != completions.items.end());

    const auto resolved = engine.resolveCompletion(item->stable_id, item->label);
    CHECK(resolved.unresolved == expected.value("unresolved", false));
    if (expected.contains("detailContains")) {
        CHECK(resolved.detail.find(expected.at("detailContains").get<std::string>()) !=
              std::string::npos);
    }
    if (expected.contains("documentationContains")) {
        CHECK(resolved.documentation.find(expected.at("documentationContains").get<std::string>()) !=
              std::string::npos);
    }
    if (expected.contains("insertTextContains")) {
        CHECK(resolved.insert_text.find(expected.at("insertTextContains").get<std::string>()) !=
              std::string::npos);
    }
}

void runSignatureHelpFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.signatureHelpAt(request.at("uri").get<std::string>(),
                                               request.at("line").get<int>(),
                                               request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("label")) {
        CHECK(result.label == expected.at("label").get<std::string>());
    }
    if (expected.contains("labelContains")) {
        CHECK(result.label.find(expected.at("labelContains").get<std::string>()) != std::string::npos);
    }
    if (expected.contains("activeParameter")) {
        CHECK(result.active_parameter == expected.at("activeParameter").get<int>());
    }
    if (expected.contains("parameters")) {
        for (const auto& expected_parameter : expected.at("parameters")) {
            const auto parameter = expected_parameter.get<std::string>();
            CAPTURE(parameter);
            CHECK(std::any_of(result.parameters.begin(),
                              result.parameters.end(),
                              [&](const std::string& candidate) {
                                  return candidate == parameter;
                              }));
        }
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

void runWorkspaceSymbolFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.workspaceSymbols(request.value("query", ""),
                                                request.value("limit", static_cast<size_t>(1000)));
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("names")) {
        for (const auto& expected_name : expected.at("names")) {
            const auto name = expected_name.get<std::string>();
            CAPTURE(name);
            CHECK(std::any_of(result.symbols.begin(),
                              result.symbols.end(),
                              [&](const SemanticWorkspaceSymbol& symbol) {
                                  return symbol.name == name;
                              }));
        }
    }
    if (expected.contains("absentNames")) {
        for (const auto& absent_name : expected.at("absentNames")) {
            const auto name = absent_name.get<std::string>();
            CAPTURE(name);
            CHECK(std::none_of(result.symbols.begin(),
                               result.symbols.end(),
                               [&](const SemanticWorkspaceSymbol& symbol) {
                                   return symbol.name == name;
                               }));
        }
    }
}

void runInlayHintFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto range_json = request.at("range");
    const auto result = engine.inlayHints(request.at("uri").get<std::string>(),
                                          ParseRange{.start_line = range_json.at("startLine").get<int>(),
                                                     .start_character =
                                                         range_json.at("startCharacter").get<int>(),
                                                     .end_line = range_json.at("endLine").get<int>(),
                                                     .end_character =
                                                         range_json.at("endCharacter").get<int>()});
    CHECK(result.unresolved == expected.value("unresolved", false));
    if (expected.contains("labels")) {
        for (const auto& expected_label : expected.at("labels")) {
            const auto label = expected_label.get<std::string>();
            CAPTURE(label);
            CHECK(std::any_of(result.hints.begin(),
                              result.hints.end(),
                              [&](const SemanticInlayHint& hint) {
                                  return hint.label == label;
                              }));
        }
    }
    if (expected.contains("tooltips")) {
        for (const auto& expected_tooltip : expected.at("tooltips")) {
            const auto tooltip = expected_tooltip.get<std::string>();
            CAPTURE(tooltip);
            CHECK(std::any_of(result.hints.begin(),
                              result.hints.end(),
                              [&](const SemanticInlayHint& hint) {
                                  return hint.tooltip == tooltip;
                              }));
        }
    }
}

void runModuleHierarchyFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    std::optional<std::string> module_name_storage;
    std::optional<std::string_view> module_name;
    if (request.contains("moduleName") && !request.at("moduleName").is_null()) {
        module_name_storage = request.at("moduleName").get<std::string>();
        module_name = *module_name_storage;
    }
    const auto result = engine.moduleHierarchy(module_name, request.value("maxDepth", 64));
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.partial == expected.value("partial", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("discoveryClosureUsed")) {
        CHECK(result.discovery_closure_used == expected.at("discoveryClosureUsed").get<bool>());
    }
    if (expected.contains("discoveryClosureRoot")) {
        CHECK(result.discovery_closure_root_name == expected.at("discoveryClosureRoot").get<std::string>());
    }
    if (expected.contains("discoveryClosureCandidateDocumentCount")) {
        CHECK(result.discovery_closure_candidate_document_count ==
              expected.at("discoveryClosureCandidateDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureDocumentCount")) {
        CHECK(result.discovery_closure_document_count ==
              expected.at("discoveryClosureDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureMissingCandidateCount")) {
        CHECK(result.discovery_closure_missing_candidate_count ==
              expected.at("discoveryClosureMissingCandidateCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureDedupedDocumentCount")) {
        CHECK(result.discovery_closure_deduped_document_count ==
              expected.at("discoveryClosureDedupedDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureCacheHit")) {
        CHECK(result.discovery_closure_cache_hit == expected.at("discoveryClosureCacheHit").get<bool>());
    }
    if (expected.contains("rootModules")) {
        for (const auto& expected_root : expected.at("rootModules")) {
            const auto module = expected_root.get<std::string>();
            CAPTURE(module);
            CHECK(std::any_of(result.roots.begin(),
                              result.roots.end(),
                              [&](const SemanticHierarchyNode& node) {
                                  return node.module_name == module;
                              }));
        }
    }
    if (expected.contains("modules")) {
        for (const auto& expected_module : expected.at("modules")) {
            const auto module = expected_module.get<std::string>();
            CAPTURE(module);
            CHECK(hierarchyContains(result.roots, module));
        }
    }
    if (expected.contains("messagesContain")) {
        for (const auto& expected_message : expected.at("messagesContain")) {
            const auto message = expected_message.get<std::string>();
            CAPTURE(message);
            CHECK(std::any_of(result.messages.begin(), result.messages.end(), [&](const std::string& item) {
                return item.find(message) != std::string::npos;
            }));
        }
    }
}

void runSchematicFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    std::optional<std::string> module_name_storage;
    std::optional<std::string_view> module_name;
    if (request.contains("moduleName") && !request.at("moduleName").is_null()) {
        module_name_storage = request.at("moduleName").get<std::string>();
        module_name = *module_name_storage;
    }
    const auto result = engine.schematic(module_name, request.value("maxDepth", 64));
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.partial == expected.value("partial", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("discoveryClosureUsed")) {
        CHECK(result.discovery_closure_used == expected.at("discoveryClosureUsed").get<bool>());
    }
    if (expected.contains("discoveryClosureRoot")) {
        CHECK(result.discovery_closure_root_name == expected.at("discoveryClosureRoot").get<std::string>());
    }
    if (expected.contains("discoveryClosureCandidateDocumentCount")) {
        CHECK(result.discovery_closure_candidate_document_count ==
              expected.at("discoveryClosureCandidateDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureDocumentCount")) {
        CHECK(result.discovery_closure_document_count ==
              expected.at("discoveryClosureDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureMissingCandidateCount")) {
        CHECK(result.discovery_closure_missing_candidate_count ==
              expected.at("discoveryClosureMissingCandidateCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureDedupedDocumentCount")) {
        CHECK(result.discovery_closure_deduped_document_count ==
              expected.at("discoveryClosureDedupedDocumentCount").get<size_t>());
    }
    if (expected.contains("discoveryClosureCacheHit")) {
        CHECK(result.discovery_closure_cache_hit == expected.at("discoveryClosureCacheHit").get<bool>());
    }
    if (expected.contains("rootModule")) {
        REQUIRE(result.root_module_id.has_value());
        CHECK(*result.root_module_id == expected.at("rootModule").get<std::string>());
    }
    if (expected.contains("modules")) {
        for (const auto& expected_module : expected.at("modules")) {
            const auto module = expected_module.get<std::string>();
            CAPTURE(module);
            CHECK(std::any_of(result.modules.begin(),
                              result.modules.end(),
                              [&](const SemanticSchematicModuleView& view) {
                                  return view.module.name == module;
                              }));
        }
    }
    if (expected.contains("cells")) {
        for (const auto& expected_cell : expected.at("cells")) {
            const auto module = expected_cell.at("module").get<std::string>();
            const auto name = expected_cell.at("name").get<std::string>();
            const auto type = expected_cell.at("type").get<std::string>();
            CAPTURE(module, name, type);
            const auto module_it = std::find_if(result.modules.begin(),
                                                result.modules.end(),
                                                [&](const SemanticSchematicModuleView& view) {
                                                    return view.module.name == module;
                                                });
            REQUIRE(module_it != result.modules.end());
            const auto cell_it = std::find_if(module_it->module.cells.begin(),
                                              module_it->module.cells.end(),
                                              [&](const SchematicCell& cell) {
                                                  return cell.name == name && cell.type == type;
                                              });
            REQUIRE(cell_it != module_it->module.cells.end());
            if (expected_cell.contains("connections")) {
                for (const auto& expected_port : expected_cell.at("connections")) {
                    const auto port = expected_port.get<std::string>();
                    CAPTURE(port);
                    CHECK(std::any_of(cell_it->connections.begin(),
                                      cell_it->connections.end(),
                                      [&](const SchematicConnection& connection) {
                                          return connection.port_name == port;
                                      }));
                }
            }
        }
    }
    if (expected.contains("nets")) {
        for (const auto& expected_net : expected.at("nets")) {
            const auto net = expected_net.get<std::string>();
            CAPTURE(net);
            CHECK(std::any_of(result.modules.begin(),
                              result.modules.end(),
                              [&](const SemanticSchematicModuleView& view) {
                                  return std::any_of(view.nets.begin(),
                                                     view.nets.end(),
                                                     [&](const SemanticSchematicNet& candidate) {
                                                         return candidate.name == net;
                                                    });
                              }));
        }
    }
    if (expected.contains("messagesContain")) {
        for (const auto& expected_message : expected.at("messagesContain")) {
            const auto message = expected_message.get<std::string>();
            CAPTURE(message);
            CHECK(std::any_of(result.messages.begin(), result.messages.end(), [&](const std::string& item) {
                return item.find(message) != std::string::npos;
            }));
        }
    }
}

void runBackwardConeFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.backwardConeAt(request.at("uri").get<std::string>(),
                                              request.at("line").get<int>(),
                                              request.at("character").get<int>());
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.partial == expected.value("partial", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("nodeCount")) {
        CHECK(result.nodes.size() == expected.at("nodeCount").get<size_t>());
    }
    if (expected.contains("edgeCount")) {
        CHECK(result.edges.size() == expected.at("edgeCount").get<size_t>());
    }
    if (expected.contains("nodes")) {
        for (const auto& expected_node : expected.at("nodes")) {
            const auto node = expected_node.get<std::string>();
            CAPTURE(node);
            CHECK(std::any_of(result.nodes.begin(), result.nodes.end(), [&](const SemanticConeNode& candidate) {
                return candidate.name == node;
            }));
        }
    }
    if (expected.contains("expressions")) {
        for (const auto& expected_expression : expected.at("expressions")) {
            const auto expression = expected_expression.get<std::string>();
            CAPTURE(expression);
            CHECK(std::any_of(result.edges.begin(), result.edges.end(), [&](const SemanticConeEdge& edge) {
                return edge.expression == expression;
            }));
        }
    }
}

void runCodeActionFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto result = engine.codeActionsAt(request.at("uri").get<std::string>(),
                                             parseRangeFromJson(request.at("range")));
    CHECK(result.unresolved == expected.value("unresolved", false));
    CHECK(result.partial == expected.value("partial", false));
    CHECK(result.truncated == expected.value("truncated", false));
    if (expected.contains("actionTitles")) {
        for (const auto& expected_title : expected.at("actionTitles")) {
            const auto title = expected_title.get<std::string>();
            CAPTURE(title);
            CHECK(std::any_of(result.actions.begin(),
                              result.actions.end(),
                              [&](const SemanticCodeAction& action) {
                                  return action.title == title;
                              }));
        }
    }
    if (expected.contains("actions")) {
        for (const auto& expected_action : expected.at("actions")) {
            const auto title = expected_action.at("title").get<std::string>();
            CAPTURE(title);
            const auto action_it = std::find_if(result.actions.begin(),
                                                result.actions.end(),
                                                [&](const SemanticCodeAction& action) {
                                                    return action.title == title;
                                                });
            REQUIRE(action_it != result.actions.end());
            if (expected_action.contains("diagnosticCodes")) {
                for (const auto& expected_code : expected_action.at("diagnosticCodes")) {
                    const auto code = expected_code.get<std::string>();
                    CAPTURE(code);
                    CHECK(std::any_of(action_it->diagnostics.begin(),
                                      action_it->diagnostics.end(),
                                      [&](const SemanticDiagnosticData& diagnostic) {
                                          return diagnostic.code == code;
                                      }));
                }
            }
            if (expected_action.contains("editContains")) {
                const auto fragment = expected_action.at("editContains").get<std::string>();
                CHECK(std::any_of(action_it->edits.begin(),
                                  action_it->edits.end(),
                                  [&](const SemanticCodeActionEdit& edit) {
                                      return edit.new_text.find(fragment) != std::string::npos;
                                  }));
            }
            if (expected_action.contains("createFiles")) {
                for (const auto& expected_uri : expected_action.at("createFiles")) {
                    const auto uri = expected_uri.get<std::string>();
                    CAPTURE(uri);
                    CHECK(std::any_of(action_it->create_files.begin(),
                                      action_it->create_files.end(),
                                      [&](const SemanticCodeActionCreateFile& create_file) {
                                          return create_file.uri == uri;
                                      }));
                }
            }
        }
    }
}

void runCallHierarchyFixture(SemanticEngine& engine, const nlohmann::json& fixture) {
    const auto& request = fixture.at("request");
    const auto& expected = fixture.at("expected");
    const auto prepared = engine.prepareCallHierarchy(request.at("uri").get<std::string>(),
                                                      request.at("line").get<int>(),
                                                      request.at("character").get<int>());
    CHECK(prepared.unresolved == expected.value("unresolved", false));
    if (expected.contains("preparedNames")) {
        for (const auto& expected_name : expected.at("preparedNames")) {
            const auto name = expected_name.get<std::string>();
            CAPTURE(name);
            CHECK(std::any_of(prepared.items.begin(),
                              prepared.items.end(),
                              [&](const SemanticCallHierarchyItem& item) {
                                  return item.name == name;
                              }));
        }
    }
    if (expected.contains("outgoingNames")) {
        REQUIRE_FALSE(prepared.items.empty());
        const auto outgoing = engine.outgoingCalls(prepared.items.front());
        REQUIRE_FALSE(outgoing.unresolved);
        for (const auto& expected_name : expected.at("outgoingNames")) {
            const auto name = expected_name.get<std::string>();
            CAPTURE(name);
            CHECK(std::any_of(outgoing.calls.begin(),
                              outgoing.calls.end(),
                              [&](const SemanticCallHierarchyCall& call) {
                                  return call.item.name == name;
                              }));
        }
    }
    if (expected.contains("incomingNames")) {
        REQUIRE_FALSE(prepared.items.empty());
        const auto incoming = engine.incomingCalls(prepared.items.front());
        REQUIRE_FALSE(incoming.unresolved);
        for (const auto& expected_name : expected.at("incomingNames")) {
            const auto name = expected_name.get<std::string>();
            CAPTURE(name);
            CHECK(std::any_of(incoming.calls.begin(),
                              incoming.calls.end(),
                              [&](const SemanticCallHierarchyCall& call) {
                                  return call.item.name == name;
                              }));
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
        configureEngine(engine, fixture);
        loadSources(engine, fixture);

        const auto kind = fixture.at("request").at("kind").get<std::string>();
        if (kind == "lookup") {
            runLookupFixture(engine, fixture);
        }
        else if (kind == "definition") {
            runDefinitionFixture(engine, fixture);
        }
        else if (kind == "typeDefinition") {
            runTypeDefinitionFixture(engine, fixture);
        }
        else if (kind == "hover") {
            runHoverFixture(engine, fixture);
        }
        else if (kind == "references") {
            runReferencesFixture(engine, fixture);
        }
        else if (kind == "rename") {
            runRenameFixture(engine, fixture);
        }
        else if (kind == "completion") {
            runCompletionFixture(engine, fixture);
        }
        else if (kind == "completionResolve") {
            runCompletionResolveFixture(engine, fixture);
        }
        else if (kind == "diagnostics") {
            runDiagnosticsFixture(engine, fixture);
        }
        else if (kind == "signatureHelp") {
            runSignatureHelpFixture(engine, fixture);
        }
        else if (kind == "workspaceSymbol") {
            runWorkspaceSymbolFixture(engine, fixture);
        }
        else if (kind == "semanticTokens") {
            runSemanticTokensFixture(engine, fixture);
        }
        else if (kind == "selectionRange") {
            runSelectionRangeFixture(engine, fixture);
        }
        else if (kind == "inlayHint") {
            runInlayHintFixture(engine, fixture);
        }
        else if (kind == "moduleHierarchy") {
            runModuleHierarchyFixture(engine, fixture);
        }
        else if (kind == "schematic") {
            runSchematicFixture(engine, fixture);
        }
        else if (kind == "backwardCone") {
            runBackwardConeFixture(engine, fixture);
        }
        else if (kind == "codeAction") {
            runCodeActionFixture(engine, fixture);
        }
        else if (kind == "callHierarchy") {
            runCallHierarchyFixture(engine, fixture);
        }
        else {
            FAIL("Unsupported semantic golden request kind: " << kind);
        }
    }
}

} // namespace pristine::analysis
