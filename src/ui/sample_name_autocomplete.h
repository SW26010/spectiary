#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {

struct SampleNameAutocompleteEvaluation {
    std::optional<std::size_t> exact_match;
    bool has_partial_matches = false;
};

[[nodiscard]] SampleNameAutocompleteEvaluation EvaluateSampleNameAutocomplete(
    const std::vector<std::string>& sample_names,
    const std::vector<std::size_t>& matches,
    std::string_view query);

[[nodiscard]] bool ShouldRestoreSampleNameSearch(
    bool search_active,
    bool search_committed,
    bool input_active,
    bool dropdown_interacting);

}  // namespace spectiary
