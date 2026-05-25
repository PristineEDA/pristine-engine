#include "pristine/lsp/Protocol.h"

namespace pristine::lsp {

Json makeInitializeResult(std::string_view server_name, std::string_view server_version) {
    return Json{
        {"capabilities",
         Json{{"positionEncoding", "utf-16"},
              {"textDocumentSync",
               Json{{"openClose", true}, {"change", 2}, {"save", Json{{"includeText", false}}}}}}},
        {"serverInfo", Json{{"name", server_name}, {"version", server_version}}},
    };
}

} // namespace pristine::lsp