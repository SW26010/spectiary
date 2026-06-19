#include "ui/sample_name_autocomplete.h"

#include <algorithm>
#include <cctype>

namespace specforge {
namespace {

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

}  // namespace

SampleNameAutocompleteEvaluation EvaluateSampleNameAutocomplete(
    const std::vector<std::string>& sample_names,
    const std::vector<std::size_t>& matches,
    std::string_view query)
{
    SampleNameAutocompleteEvaluation evaluation;
    if (query.empty() || sample_names.empty() || matches.empty()) {
        return evaluation;
    }

    const std::string target = LowerAscii(std::string(query));
    for (const std::size_t row : matches) {
        if (row >= sample_names.size()) {
            continue;
        }
        if (LowerAscii(sample_names[row]) == target) {
            evaluation.exact_match = row;
            return evaluation;
        }
    }
    evaluation.has_partial_matches = true;
    return evaluation;
}

bool ShouldRestoreSampleNameSearch(
    bool search_active,
    bool search_committed,
    bool input_active,
    bool dropdown_interacting)
{
    return search_active && !search_committed && !input_active && !dropdown_interacting;
}

}  // namespace specforge
