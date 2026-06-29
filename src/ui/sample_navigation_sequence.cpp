#include "ui/sample_navigation_sequence.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace specforge {
namespace {

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool SortApplies(const SampleNavigationSequenceInput& input)
{
    return input.sort_choice != nullptr && input.sort_choice->active &&
           input.sort_choice->values.size() == input.source_row_count;
}

bool IncludeRow(const SampleNavigationSequenceInput& input, std::size_t row)
{
    if (!input.filter_active) {
        return true;
    }
    return input.included_samples != nullptr && input.included_samples->size() == input.source_row_count &&
           (*input.included_samples)[row];
}

int CompareSortValues(const SampleNavigationSortValue& left, const SampleNavigationSortValue& right)
{
    if (left.index() != right.index()) {
        return left.index() < right.index() ? -1 : 1;
    }
    if (const double* left_number = std::get_if<double>(&left)) {
        const double right_number = std::get<double>(right);
        if (*left_number < right_number) {
            return -1;
        }
        if (*left_number > right_number) {
            return 1;
        }
        return 0;
    }

    const std::string& left_text = std::get<std::string>(left);
    const std::string& right_text = std::get<std::string>(right);
    if (left_text < right_text) {
        return -1;
    }
    if (left_text > right_text) {
        return 1;
    }
    return 0;
}

bool IsFullSourceOrder(const std::vector<std::size_t>& ordered_rows, std::size_t source_row_count)
{
    if (ordered_rows.size() != source_row_count) {
        return false;
    }
    for (std::size_t index = 0; index < ordered_rows.size(); ++index) {
        if (ordered_rows[index] != index) {
            return false;
        }
    }
    return true;
}

}  // namespace

SampleNavigationSortValue MakeSampleNavigationSortValue(double value)
{
    return SampleNavigationSortValue{value};
}

SampleNavigationSortValue MakeSampleNavigationSortValue(std::string value)
{
    return SampleNavigationSortValue{std::move(value)};
}

bool SampleNavigationSequence::ContainsSourceRow(std::size_t row) const
{
    return std::find(ordered_rows.begin(), ordered_rows.end(), row) != ordered_rows.end();
}

std::optional<std::size_t> SampleNavigationSequence::LocateSourceRow(std::size_t row) const
{
    if (!row_location_available || !ContainsSourceRow(row)) {
        return std::nullopt;
    }
    return row;
}

std::optional<std::size_t> SampleNavigationSequence::LocateSourceRowInSequence(std::size_t row) const
{
    if (!ContainsSourceRow(row)) {
        return std::nullopt;
    }
    return row;
}

std::optional<std::size_t> SampleNavigationSequence::LocateSampleName(
    std::span<const std::string> sample_names,
    std::string_view sample_name) const
{
    if (sample_name.empty() || sample_names.empty()) {
        return std::nullopt;
    }

    const std::string target = LowerAscii(std::string(sample_name));
    for (const std::size_t row : ordered_rows) {
        if (row < sample_names.size() && LowerAscii(sample_names[row]) == target) {
            return row;
        }
    }
    for (const std::size_t row : ordered_rows) {
        if (row < sample_names.size() && LowerAscii(sample_names[row]).find(target) != std::string::npos) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> SampleNavigationSequence::LocateSampleNameMatch(
    std::span<const std::string> sample_names,
    std::size_t row,
    std::string_view sample_name) const
{
    if (sample_name.empty() || row >= sample_names.size() || !ContainsSourceRow(row)) {
        return std::nullopt;
    }

    const std::string target = LowerAscii(std::string(sample_name));
    return LowerAscii(sample_names[row]) == target ? std::optional<std::size_t>{row} : std::nullopt;
}

std::optional<std::size_t> SampleNavigationSequence::LabelAdvanceTarget(
    const std::vector<bool>& eligible_samples) const
{
    if (!current_sequence_position || !current_source_row) {
        return std::nullopt;
    }

    const auto eligible = [&eligible_samples](std::size_t row) {
        return eligible_samples.empty() || (row < eligible_samples.size() && eligible_samples[row]);
    };
    for (std::size_t position = *current_sequence_position + 1; position < ordered_rows.size(); ++position) {
        const std::size_t row = ordered_rows[position];
        if (eligible(row)) {
            return row;
        }
    }
    return current_source_row;
}

SampleNavigationSequence BuildSampleNavigationSequence(const SampleNavigationSequenceInput& input)
{
    SampleNavigationSequence sequence;
    const bool sort_applies = SortApplies(input);
    sequence.active = input.filter_active || sort_applies;

    sequence.ordered_rows.reserve(input.source_row_count);
    for (std::size_t row = 0; row < input.source_row_count; ++row) {
        if (IncludeRow(input, row)) {
            sequence.ordered_rows.push_back(row);
        }
    }

    if (sort_applies) {
        const SampleNavigationSortChoice& sort_choice = *input.sort_choice;
        std::stable_sort(
            sequence.ordered_rows.begin(),
            sequence.ordered_rows.end(),
            [&sort_choice](std::size_t left, std::size_t right) {
                const int comparison = CompareSortValues(sort_choice.values[left], sort_choice.values[right]);
                if (comparison == 0) {
                    return left < right;
                }
                if (sort_choice.direction == SampleNavigationSortDirection::Ascending) {
                    return comparison < 0;
                }
                return comparison > 0;
            });
    }

    sequence.empty = sequence.ordered_rows.empty();
    sequence.row_location_available = IsFullSourceOrder(sequence.ordered_rows, input.source_row_count);

    if (input.current_source_row && *input.current_source_row < input.source_row_count) {
        const auto current = std::find(
            sequence.ordered_rows.begin(),
            sequence.ordered_rows.end(),
            *input.current_source_row);
        if (current != sequence.ordered_rows.end()) {
            sequence.current_source_row = *current;
            sequence.current_sequence_position =
                static_cast<std::size_t>(std::distance(sequence.ordered_rows.begin(), current));
        }
    }

    if (sequence.current_sequence_position && sequence.current_source_row) {
        const std::size_t position = *sequence.current_sequence_position;
        sequence.previous_target = position > 0 ? sequence.ordered_rows[position - 1] : *sequence.current_source_row;
        sequence.next_target = position + 1 < sequence.ordered_rows.size() ? sequence.ordered_rows[position + 1]
                                                                           : *sequence.current_source_row;
    }

    if (!input.sample_name_query.empty() && !input.sample_names.empty()) {
        const std::string query = LowerAscii(std::string(input.sample_name_query));
        for (const std::size_t row : sequence.ordered_rows) {
            if (row < input.sample_names.size() &&
                LowerAscii(input.sample_names[row]).find(query) != std::string::npos) {
                sequence.sample_name_matches.push_back(row);
            }
        }
    }

    return sequence;
}

}  // namespace specforge
