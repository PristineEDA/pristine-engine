#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace pristine::lsp {

using Json = nlohmann::json;

Json makeInitializeResult(std::string_view server_name, std::string_view server_version);

} // namespace pristine::lsp