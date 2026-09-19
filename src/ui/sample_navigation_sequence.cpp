#include "ui/sample_navigation_sequence.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace spectiary {
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
    if (const std::int64_t* left_number = std::get_if<std::int64_t>(&left)) {
        const std::int64_t right_number = std::get<std::int64_t>(right);
        if (*left_number < right_number) {
            return -1;
        }
        if (*left_number > right_number) {
            return 1;
        }
        return 0;
    }
    if (const std::uint64_t* left_number = std::get_if<std::uint64_t>(&left)) {
        const std::uint64_t right_number = std::get<std::uint64_t>(right);
        if (*left_number < right_number) {
            return -1;
        }
        if (*left_number > right_number) {
            return 1;
        }
        return 0;
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

bool UsesImplicitSourceOrder(const SampleNavigationSequence& sequence)
{
    return !sequence.active && sequence.ordered_rows.empty();
}

constexpr std::size_t kMissingSequencePosition = std::numeric_limits<std::size_t>::max();

}  // namespace

SampleNavigationSortValue MakeSampleNavigationSortValue(std::int64_t value)
{
    return SampleNavigationSortValue{value};
}

SampleNavigationSortValue MakeSampleNavigationSortValue(std::uint64_t value)
{
    return SampleNavigationSortValue{value};
}

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
    if (UsesImplicitSourceOrder(*this)) {
        return row < source_row_count;
    }
    if (row < included_rows.size()) {
        return included_rows[row];
    }
    return std::find(ordered_rows.begin(), ordered_rows.end(), row) != ordered_rows.end();
}

std::optional<std::size_t> SampleNavigationSequence::LocateSourceRow(std::size_t row) const
{
    if (!row_location_available || !ContainsSourceRow(row)) {
        return std::nullopt;
    }
    return row;
}

std::optional<std::size_t> SampleNavigationSequence::LocateSequencePosition(
    std::size_t position) const
{
    if (UsesImplicitSourceOrder(*this)) {
        return position < source_row_count
            ? std::optional<std::size_t>{position}
            : std::nullopt;
    }
    return position < ordered_rows.size()
        ? std::optional<std::size_t>{ordered_rows[position]}
        : std::nullopt;
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
    if (UsesImplicitSourceOrder(*this)) {
        const std::size_t count = std::min(source_row_count, sample_names.size());
        for (std::size_t row = 0; row < count; ++row) {
            if (LowerAscii(sample_names[row]) == target) {
                return row;
            }
        }
        for (std::size_t row = 0; row < count; ++row) {
            if (LowerAscii(sample_names[row]).find(target) != std::string::npos) {
                return row;
            }
        }
    } else {
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
    return LabelAdvanceTarget(
        SampleNavigationSequenceProjection{
            .current_source_row = current_source_row,
            .current_sequence_position = current_sequence_position,
            .previous_target = previous_target,
            .next_target = next_target,
        },
        eligible_samples);
}

std::optional<std::size_t> SampleNavigationSequence::LabelAdvanceTarget(
    const SampleNavigationSequenceProjection& projection,
    const std::vector<bool>& eligible_samples) const
{
    if (!projection.current_source_row) {
        return std::nullopt;
    }

    const auto eligible = [&eligible_samples](std::size_t row) {
        return eligible_samples.empty() || (row < eligible_samples.size() && eligible_samples[row]);
    };

    if (UsesImplicitSourceOrder(*this)) {
        for (std::size_t row = *projection.current_source_row + 1; row < source_row_count; ++row) {
            if (eligible(row)) {
                return row;
            }
        }
        return projection.current_source_row;
    }

    if (!projection.current_sequence_position) {
        return std::nullopt;
    }
    for (std::size_t position = *projection.current_sequence_position + 1;
         position < ordered_rows.size();
         ++position) {
        const std::size_t row = ordered_rows[position];
        if (eligible(row)) {
            return row;
        }
    }
    return projection.current_source_row;
}

SampleNavigationSequenceProjection ProjectSampleNavigationSequence(
    const SampleNavigationSequence& sequence,
    std::optional<std::size_t> current_source_row)
{
    SampleNavigationSequenceProjection projection;
    if (!current_source_row || *current_source_row >= sequence.source_row_count) {
        return projection;
    }

    std::optional<std::size_t> current_sequence_position;
    if (UsesImplicitSourceOrder(sequence)) {
        current_sequence_position = *current_source_row;
    } else if (*current_source_row < sequence.source_row_positions.size()) {
        const std::size_t position = sequence.source_row_positions[*current_source_row];
        if (position != kMissingSequencePosition) {
            current_sequence_position = position;
        }
    }
    if (!current_sequence_position) {
        return projection;
    }

    projection.current_source_row = *current_source_row;
    projection.current_sequence_position = *current_sequence_position;
    const std::size_t position = *current_sequence_position;
    if (UsesImplicitSourceOrder(sequence)) {
        projection.previous_target = position > 0 ? position - 1 : *current_source_row;
        projection.next_target =
            position + 1 < sequence.source_row_count ? position + 1 : *current_source_row;
    } else {
        projection.previous_target =
            position > 0 ? sequence.ordered_rows[position - 1] : *current_source_row;
        projection.next_target = position + 1 < sequence.ordered_rows.size()
            ? sequence.ordered_rows[position + 1]
            : *current_source_row;
    }
    return projection;
}

void ApplySampleNavigationSequenceProjection(
    SampleNavigationSequence& sequence,
    const SampleNavigationSequenceProjection& projection)
{
    sequence.current_source_row = projection.current_source_row;
    sequence.current_sequence_position = projection.current_sequence_position;
    sequence.previous_target = projection.previous_target;
    sequence.next_target = projection.next_target;
}

std::vector<std::size_t> FindSampleNameMatches(
    const SampleNavigationSequence& sequence,
    std::span<const std::string> sample_names,
    std::string_view query)
{
    return FindSampleNameMatches(sequence, sample_names, query, {});
}

std::vector<std::size_t> FindSampleNameMatches(
    const SampleNavigationSequence& sequence,
    std::span<const std::string> sample_names,
    std::string_view query,
    const std::function<void()>& cancellation_checkpoint)
{
    std::vector<std::size_t> matches;
    if (query.empty() || sample_names.empty()) {
        return matches;
    }

    const std::string normalized_query = LowerAscii(std::string(query));
    const auto append_if_matching =
        [&matches, sample_names, &normalized_query](std::size_t row) {
            if (row < sample_names.size() &&
                LowerAscii(sample_names[row]).find(normalized_query) != std::string::npos) {
                matches.push_back(row);
            }
        };
    if (UsesImplicitSourceOrder(sequence)) {
        const std::size_t count = std::min(sequence.source_row_count, sample_names.size());
        for (std::size_t row = 0; row < count; ++row) {
            if ((row & 0xfffU) == 0U && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            append_if_matching(row);
        }
    } else {
        matches.reserve(sequence.ordered_rows.size());
        for (std::size_t position = 0; position < sequence.ordered_rows.size(); ++position) {
            if ((position & 0xfffU) == 0U && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            append_if_matching(sequence.ordered_rows[position]);
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return matches;
}

SampleNavigationSequence BuildSampleNavigationSequence(const SampleNavigationSequenceInput& input)
{
    return BuildSampleNavigationSequence(input, {});
}

SampleNavigationSequence BuildSampleNavigationSequence(
    const SampleNavigationSequenceInput& input,
    const std::function<void()>& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    SampleNavigationSequence sequence;
    sequence.source_row_count = input.source_row_count;
    const bool sort_applies = SortApplies(input);
    sequence.active = input.filter_active || sort_applies;
    const bool materialize_order = sequence.active || input.materialize_source_order;

    if (materialize_order) {
        sequence.ordered_rows.reserve(input.source_row_count);
        for (std::size_t row = 0; row < input.source_row_count; ++row) {
            if ((row & 0xfffU) == 0U && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            if (IncludeRow(input, row)) {
                sequence.ordered_rows.push_back(row);
            }
        }
        if (sequence.active) {
            sequence.included_rows.assign(input.source_row_count, false);
            for (std::size_t index = 0; index < sequence.ordered_rows.size(); ++index) {
                if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
                    cancellation_checkpoint();
                }
                const std::size_t row = sequence.ordered_rows[index];
                sequence.included_rows[row] = true;
            }
        }

        if (sort_applies) {
            const SampleNavigationSortChoice& sort_choice = *input.sort_choice;
            std::size_t comparison_count = 0;
            std::stable_sort(
                sequence.ordered_rows.begin(),
                sequence.ordered_rows.end(),
                [&sort_choice, &cancellation_checkpoint, &comparison_count](std::size_t left, std::size_t right) {
                    if (((++comparison_count) & 0xfffU) == 0U && cancellation_checkpoint) {
                        cancellation_checkpoint();
                    }
                    const int comparison = CompareSortValues(sort_choice.values[left], sort_choice.values[right]);
                    if (comparison == 0) {
                        return left < right;
                    }
                    if (sort_choice.direction == SampleNavigationSortDirection::Ascending) {
                        return comparison < 0;
                    }
                    return comparison > 0;
                });
            if (cancellation_checkpoint) {
                cancellation_checkpoint();
            }
        }

        sequence.source_row_positions.assign(
            input.source_row_count,
            kMissingSequencePosition);
        for (std::size_t position = 0; position < sequence.ordered_rows.size(); ++position) {
            if ((position & 0xfffU) == 0U && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            sequence.source_row_positions[sequence.ordered_rows[position]] = position;
        }
    }

    sequence.empty = materialize_order ? sequence.ordered_rows.empty() : input.source_row_count == 0;
    sequence.row_location_available = !materialize_order ||
                                      IsFullSourceOrder(sequence.ordered_rows, input.source_row_count);

    ApplySampleNavigationSequenceProjection(
        sequence,
        ProjectSampleNavigationSequence(sequence, input.current_source_row));

    sequence.sample_name_matches = FindSampleNameMatches(
        sequence,
        input.sample_names,
        input.sample_name_query,
        cancellation_checkpoint);

    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }

    return sequence;
}

}  // namespace spectiary
