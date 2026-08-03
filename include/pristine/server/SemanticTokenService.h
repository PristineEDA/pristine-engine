#pragma once

#include "pristine/analysis/SemanticEngine.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace pristine::server {

class SemanticTokenService {
public:
    struct DeltaEdit {
        size_t start = 0;
        size_t delete_count = 0;
        std::vector<int> data;
    };

    [[nodiscard]] static std::vector<int> encode(const analysis::SemanticTokenResult& result);
    [[nodiscard]] static DeltaEdit singleDelta(const std::vector<int>& previous,
                                               const std::vector<int>& current);
    [[nodiscard]] static nlohmann::json fullResponse(std::string result_id,
                                                      const std::vector<int>& data);
    [[nodiscard]] static nlohmann::json rangeResponse(const std::vector<int>& data);
    [[nodiscard]] static nlohmann::json deltaResponse(std::string result_id,
                                                       const DeltaEdit& edit);
};

} // namespace pristine::server
