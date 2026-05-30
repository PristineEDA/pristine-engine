#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace pristine::analysis::semantic {

struct SignatureInlaySymbol {
    SemanticSymbolIdentity identity;
    std::string type_display;
};

struct SignatureInlayModuleInstance {
    std::string module_name;
    std::string instance_name;
    ParseRange range;
    ParseRange selection_range;
    std::vector<SchematicConnection> connections;
};

struct SignatureInlayContext {
    std::uint64_t generation = 0;
    std::string document_uri;
    const std::string* document_text = nullptr;
    const std::unordered_map<std::string, ModuleDefinition>* modules_by_name = nullptr;
    std::vector<SignatureInlaySymbol> symbols;
    std::vector<SignatureInlayModuleInstance> module_instances;
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
