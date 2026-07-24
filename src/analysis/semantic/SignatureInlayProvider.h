#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct SignatureInlaySymbol {
    SemanticSymbolIdentity identity;
    std::string type_display;
    std::string value_display;
};

struct SignatureInlayConnection {
    std::string port_name;
    std::string parameter_signature;
    ParseRange range;
    bool module_port = true;
};

struct SignatureInlayModuleInstance {
    std::string module_name;
    std::string instance_name;
    std::string type_display;
    ParseRange range;
    ParseRange selection_range;
    std::vector<SignatureInlayConnection> connections;
};

struct CallableInvocationFact {
    std::string target_stable_id;
    std::string name;
    std::string kind;
    std::string return_type;
    std::string receiver_type;
    ParseRange range;
    ParseRange selection_range;
    std::vector<std::string> parameters;
    std::vector<ParseRange> argument_ranges;
    bool resolved = true;
};

struct MacroInvocationFact {
    std::string name;
    std::string definition_uri;
    MacroDefinition definition;
    ParseRange range;
    ParseRange selection_range;
    std::vector<std::string> arguments;
    std::vector<ParseRange> argument_ranges;
    std::string expansion_text;
    bool function_like = false;
    bool resolved = false;
};

struct SignatureInlayContext {
    std::uint64_t generation = 0;
    std::string document_uri;
    const std::unordered_map<std::string, ModuleDefinition>* modules_by_name = nullptr;
    std::vector<SignatureInlaySymbol> symbols;
    std::vector<SignatureInlayModuleInstance> module_instances;
    std::vector<CallableInvocationFact> callable_invocations;
    std::vector<MacroInvocationFact> macro_invocations;
    bool snapshot_available = false;
};

[[nodiscard]] constexpr std::string_view signatureInlayProviderName() {
    return "SignatureInlayProvider";
}

[[nodiscard]] SemanticSignatureHelpResult signatureHelpAt(const SignatureInlayContext& context,
                                                          int line,
                                                          int character);

[[nodiscard]] SemanticInlayHintResult inlayHints(const SignatureInlayContext& context,
                                                 ParseRange range);

} // namespace pristine::analysis::semantic
