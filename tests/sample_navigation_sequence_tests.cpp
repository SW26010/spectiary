#include "ui/sample_navigation_sequence.h"

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

void RequireRows(
    const std::vector<std::size_t>& actual,
    const std::vector<std::size_t>& expected,
    std::string_view message)
{
    Require(actual == expected, message);
}

void TestSourceOrderSequence()
{
    const std::vector<std::string> names = {"alpha", "beta", "gamma"};
    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 3;
    input.sample_names = names;
    input.current_source_row = 1;
    input.sample_name_query = "a";

    const spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);

    Require(!sequence.active, "source order without filters or sorting should not be active");
    Require(!sequence.empty, "source order sequence should not be empty");
    Require(sequence.row_location_available, "source order row location should be available");
    RequireRows(sequence.ordered_rows, {0, 1, 2}, "source order should keep all rows");
    Require(sequence.current_sequence_position && *sequence.current_sequence_position == 1, "current row should map to sequence position 1");
    Require(sequence.previous_target && *sequence.previous_target == 0, "previous target should be row 0");
    Require(sequence.next_target && *sequence.next_target == 2, "next target should be row 2");
    RequireRows(sequence.sample_name_matches, {0, 1, 2}, "sample-name matches should follow source order");
    Require(sequence.LocateSourceRow(2) && *sequence.LocateSourceRow(2) == 2, "row locate should resolve in source order");
    Require(
        sequence.LocateSequencePosition(2) &&
            *sequence.LocateSequencePosition(2) == 2,
        "sequence-position locate should resolve in source order");
    Require(
        !sequence.LocateSequencePosition(3),
        "sequence-position locate should reject a source-order position outside the sequence");
}

void TestFilteredSequence()
{
    const std::vector<std::string> names = {"alpha", "beta", "gamma", "beta"};
    const std::vector<bool> included = {false, true, false, true};
    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 4;
    input.sample_names = names;
    input.filter_active = true;
    input.included_samples = &included;
    input.current_source_row = 1;
    input.sample_name_query = "a";

    const spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);

    Require(sequence.active, "filtering should create an active sequence");
    Require(!sequence.empty, "filtered sequence should not be empty when rows are included");
    Require(!sequence.row_location_available, "row-index location should be unavailable when filtering changes source order");
    RequireRows(sequence.ordered_rows, {1, 3}, "filtered order should contain included rows in source order");
    Require(sequence.current_sequence_position && *sequence.current_sequence_position == 0, "current filtered row should have sequence position");
    Require(sequence.previous_target && *sequence.previous_target == 1, "previous at first filtered row should stay current");
    Require(sequence.next_target && *sequence.next_target == 3, "next target should skip excluded rows");
    RequireRows(sequence.sample_name_matches, {1, 3}, "sample-name matches should be scoped to included rows");
    Require(!sequence.LocateSourceRow(3), "row locate should be disabled while filtered order differs");
    Require(
        sequence.LocateSequencePosition(0) &&
            *sequence.LocateSequencePosition(0) == 1,
        "sequence position 0 should resolve the first included source row");
    Require(
        sequence.LocateSequencePosition(1) &&
            *sequence.LocateSequencePosition(1) == 3,
        "sequence position 1 should resolve the second included source row");
    Require(
        !sequence.LocateSequencePosition(2),
        "sequence-position locate should reject a position outside the filtered sequence");
    Require(sequence.LocateSourceRowInSequence(3) && *sequence.LocateSourceRowInSequence(3) == 3, "sequence-scoped source row locate should allow included rows");
    Require(!sequence.LocateSourceRowInSequence(2), "sequence-scoped source row locate should not bypass the active sequence");
    Require(sequence.LocateSampleName(names, "beta") && *sequence.LocateSampleName(names, "beta") == 1, "sample-name locate should resolve the first sequence match");
    Require(sequence.LocateSampleNameMatch(names, 3, "beta") && *sequence.LocateSampleNameMatch(names, 3, "beta") == 3, "sample-name match selection should preserve the chosen row");
    Require(!sequence.LocateSampleNameMatch(names, 2, "gamma"), "sample-name match selection should not bypass the sequence");
}

void TestEmptySequence()
{
    const std::vector<bool> included = {false, false, false};
    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 3;
    input.filter_active = true;
    input.included_samples = &included;
    input.current_source_row = 1;

    const spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);

    Require(sequence.active, "empty filtered result should still be an active sequence");
    Require(sequence.empty, "sequence should expose empty state");
    Require(sequence.ordered_rows.empty(), "empty sequence should not expose rows");
    Require(!sequence.current_source_row, "empty sequence should not expose a current source row");
    Require(!sequence.current_sequence_position, "empty sequence should not expose a current sequence position");
    Require(!sequence.previous_target && !sequence.next_target, "empty sequence should not expose movement targets");
    Require(
        !sequence.LocateSequencePosition(0),
        "empty sequence should not resolve a sequence position");
}

void TestCurrentRowExcluded()
{
    const std::vector<bool> included = {false, true, true};
    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 3;
    input.filter_active = true;
    input.included_samples = &included;
    input.current_source_row = 0;

    const spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);

    RequireRows(sequence.ordered_rows, {1, 2}, "filtered sequence should still expose included rows");
    Require(!sequence.current_source_row, "excluded current row should not remain current inside the sequence");
    Require(!sequence.current_sequence_position, "excluded current row should not have a sequence position");
}

void TestSortingStableTieBreak()
{
    spectiary::SampleNavigationSortChoice sort;
    sort.active = true;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(2.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
        spectiary::MakeSampleNavigationSortValue(3.0),
    };

    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 4;
    input.sort_choice = &sort;
    input.current_source_row = 1;

    spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);
    Require(sequence.active, "sorting should create an active sequence");
    Require(!sequence.row_location_available, "row location should be unavailable when sorting changes order");
    RequireRows(sequence.ordered_rows, {1, 2, 0, 3}, "ascending sort should keep source order for equal values");

    sort.direction = spectiary::SampleNavigationSortDirection::Descending;
    sequence = spectiary::BuildSampleNavigationSequence(input);
    RequireRows(sequence.ordered_rows, {3, 0, 1, 2}, "descending sort should keep source order for equal values");
}

void TestSortedTopologySupportsIndependentCursorProjections()
{
    spectiary::SampleNavigationSortChoice sort;
    sort.active = true;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(2.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
        spectiary::MakeSampleNavigationSortValue(3.0),
    };

    spectiary::SampleNavigationSequenceInput input;
    input.source_row_count = 4;
    input.sort_choice = &sort;
    input.current_source_row = std::nullopt;
    const spectiary::SampleNavigationSequence sequence =
        spectiary::BuildSampleNavigationSequence(input);

    const spectiary::SampleNavigationSequenceProjection row_zero =
        spectiary::ProjectSampleNavigationSequence(sequence, 0);
    Require(
        row_zero.current_sequence_position && *row_zero.current_sequence_position == 2,
        "row 0 should project to its cached sorted position");
    Require(
        row_zero.previous_target && *row_zero.previous_target == 2 &&
            row_zero.next_target && *row_zero.next_target == 3,
        "row 0 projection should resolve adjacent sorted targets");
    Require(
        sequence.LabelAdvanceTarget(row_zero, {false, false, false, true}) == 3,
        "label eligibility should be evaluated against each projected cursor on demand");

    const spectiary::SampleNavigationSequenceProjection row_one =
        spectiary::ProjectSampleNavigationSequence(sequence, 1);
    Require(
        row_one.current_sequence_position && *row_one.current_sequence_position == 0 &&
            row_one.previous_target && *row_one.previous_target == 1 &&
            row_one.next_target && *row_one.next_target == 2,
        "the same topology should independently project a different cursor");
}

}  // namespace

int main()
{
    TestSourceOrderSequence();
    TestFilteredSequence();
    TestEmptySequence();
    TestCurrentRowExcluded();
    TestSortingStableTieBreak();
    TestSortedTopologySupportsIndependentCursorProjections();
    return 0;
}
