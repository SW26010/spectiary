#include "ui/sample_name_autocomplete.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestExactMatchSubmitsWithoutPartialDropdown()
{
    const std::vector<std::string> sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SampleNameAutocompleteEvaluation evaluation =
        spectiary::EvaluateSampleNameAutocomplete(sample_names, {1}, "BETA");

    Require(evaluation.exact_match && *evaluation.exact_match == 1, "exact match should resolve case-insensitively");
    Require(!evaluation.has_partial_matches, "exact match should not also request a partial-match dropdown");
}

void TestPartialMatchKeepsDropdownOpenWithoutSubmitting()
{
    const std::vector<std::string> sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SampleNameAutocompleteEvaluation evaluation =
        spectiary::EvaluateSampleNameAutocomplete(sample_names, {0, 2}, "a");

    Require(!evaluation.exact_match, "partial query should not submit a sample");
    Require(evaluation.has_partial_matches, "partial query should request the dropdown");
}

void TestNoMatchDoesNotOpenDropdown()
{
    const std::vector<std::string> sample_names = {"alpha", "beta", "gamma"};
    const spectiary::SampleNameAutocompleteEvaluation evaluation =
        spectiary::EvaluateSampleNameAutocomplete(sample_names, {}, "missing");

    Require(!evaluation.exact_match, "missing query should not submit a sample");
    Require(!evaluation.has_partial_matches, "missing query should not request the dropdown");
}

void TestFailedSearchRestoresOnlyAfterInteractionEnds()
{
    Require(
        !spectiary::ShouldRestoreSampleNameSearch(true, false, true, false),
        "active input should keep the failed search pending");
    Require(
        !spectiary::ShouldRestoreSampleNameSearch(true, false, false, true),
        "dropdown interaction should keep the failed search pending");
    Require(
        !spectiary::ShouldRestoreSampleNameSearch(true, true, false, false),
        "committed search should not restore");
    Require(
        spectiary::ShouldRestoreSampleNameSearch(true, false, false, false),
        "uncommitted search should restore after input and dropdown interaction end");
}

}  // namespace

int main()
{
    TestExactMatchSubmitsWithoutPartialDropdown();
    TestPartialMatchKeepsDropdownOpenWithoutSubmitting();
    TestNoMatchDoesNotOpenDropdown();
    TestFailedSearchRestoresOnlyAfterInteractionEnds();
    return 0;
}
