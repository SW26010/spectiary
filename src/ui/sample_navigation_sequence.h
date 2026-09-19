#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace spectiary {

enum class SampleNavigationSortDirection {
    Ascending,
    Descending,
};

using SampleNavigationSortValue =
    std::variant<std::int64_t, std::uint64_t, double, std::string>;

struct SampleNavigationSortChoice {
    bool active = false;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
    std::vector<SampleNavigationSortValue> values;
};

struct SampleNavigationSequenceInput {
    std::size_t source_row_count = 0;
    std::span<const std::string> sample_names;
    bool filter_active = false;
    bool materialize_source_order = true;
    const std::vector<bool>* included_samples = nullptr;
    const SampleNavigationSortChoice* sort_choice = nullptr;
    std::optional<std::size_t> current_source_row;
    std::string_view sample_name_query;
};

struct SampleNavigationSequenceProjection {
    std::optional<std::size_t> current_source_row;
    std::optional<std::size_t> current_sequence_position;
    std::optional<std::size_t> previous_target;
    std::optional<std::size_t> next_target;
};

struct SampleNavigationSequence {
    bool active = false;
    bool empty = false;
    bool row_location_available = true;
    std::size_t source_row_count = 0;
    std::vector<std::size_t> ordered_rows;
    std::vector<bool> included_rows;
    // Materialized orders cache row -> position; max<size_t> marks an excluded row.
    std::vector<std::size_t> source_row_positions;
    std::optional<std::size_t> current_source_row;
    std::optional<std::size_t> current_sequence_position;
    std::optional<std::size_t> previous_target;
    std::optional<std::size_t> next_target;
    std::vector<std::size_t> sample_name_matches;

    [[nodiscard]] bool ContainsSourceRow(std::size_t row) const;
    [[nodiscard]] std::optional<std::size_t> LocateSourceRow(std::size_t row) const;
    [[nodiscard]] std::optional<std::size_t> LocateSequencePosition(
        std::size_t position) const;
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
    [[nodiscard]] std::optional<std::size_t> LabelAdvanceTarget(
        const SampleNavigationSequenceProjection& projection,
        const std::vector<bool>& eligible_samples) const;
};

[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(std::int64_t value);
[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(std::uint64_t value);
[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(double value);
[[nodiscard]] SampleNavigationSortValue MakeSampleNavigationSortValue(std::string value);
[[nodiscard]] SampleNavigationSequenceProjection ProjectSampleNavigationSequence(
    const SampleNavigationSequence& sequence,
    std::optional<std::size_t> current_source_row);
void ApplySampleNavigationSequenceProjection(
    SampleNavigationSequence& sequence,
    const SampleNavigationSequenceProjection& projection);
[[nodiscard]] std::vector<std::size_t> FindSampleNameMatches(
    const SampleNavigationSequence& sequence,
    std::span<const std::string> sample_names,
    std::string_view query);
[[nodiscard]] std::vector<std::size_t> FindSampleNameMatches(
    const SampleNavigationSequence& sequence,
    std::span<const std::string> sample_names,
    std::string_view query,
    const std::function<void()>& cancellation_checkpoint);
[[nodiscard]] SampleNavigationSequence BuildSampleNavigationSequence(
    const SampleNavigationSequenceInput& input);
[[nodiscard]] SampleNavigationSequence BuildSampleNavigationSequence(
    const SampleNavigationSequenceInput& input,
    const std::function<void()>& cancellation_checkpoint);

}  // namespace spectiary
