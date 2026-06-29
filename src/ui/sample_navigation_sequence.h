#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace specforge {

enum class SampleNavigationSortDirection {
    Ascending,
    Descending,
};

using SampleNavigationSortValue = std::variant<double, std::string>;

struct SampleNavigationSortChoice {
    bool active = false;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
    std::vector<SampleNavigationSortValue> values;
};

struct SampleNavigationSequenceInput {
    std::size_t source_row_count = 0;
    std::span<const std::string> sample_names;
    bool filter_active = false;
    const std::vector<bool>* included_samples = nullptr;
    const SampleNavigationSortChoice* sort_choice = nullptr;
    std::optional<std::size_t> current_source_row;
    std::string_view sample_name_query;
};

struct SampleNavigationSequence {
    bool active = false;
    bool empty = false;
    bool row_location_available = true;
    std::vector<std::size_t> ordered_rows;
    std::optional<std::size_t> current_source_row;
    std::optional<std::size_t> current_sequence_position;
    std::optional<std::size_t> previous_target;
    std::optional<std::size_t> next_target;
    std::vector<std::size_t> sample_name_matches;

    [[nodiscard]] bool ContainsSourceRow(std::size_t row) const;
    [[nodiscard]] std::optional<std::size_t> LocateSourceRow(std::size_t row) const;
    [[nodiscard]] std::optional<std::size_t> LocateSourceRowInSequence(std::size_t row) const;
    [[nodiscard]] std::optional<std::size_t> LocateSampleName(
        std::span<const std::string> sample_names,
        std::string_view sample_name) const;
    [[nodiscard]] std::optional<std::size_t> LocateSampleNameMatch(
        std::span<const std::string> sample_names,
        std::size_t row,
        std::string_view sample_name) const;
    [[nodiscard]] std::optional<std::size_t> LabelAdvanceTarget(
        const std::vector<bool>& eligible_samples) const;
};

[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(double value);
[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(std::string value);
[[nodiscard]] SampleNavigationSequence BuildSampleNavigationSequence(
    const SampleNavigationSequenceInput& input);

}  // namespace specforge
