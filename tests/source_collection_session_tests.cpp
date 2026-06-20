#include "domain/sample_labeling.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_session.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "specforge_source_collection_session_";
    path += std::to_string(now);
    path += std::string(suffix);
    return path;
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_count,
    std::size_t current_index)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.id = "source";
    snapshot->source.display_name = "source";
    snapshot->source.path = path;
    snapshot->source.metadata.push_back(specforge::SpectrumMetadataEntry{"format", "test", "test"});
    snapshot->collection.spectrum_count = spectrum_count;
    snapshot->collection.current_index = current_index;
    snapshot->collection.can_move_previous = current_index > 0;
    snapshot->collection.can_move_next = current_index + 1 < spectrum_count;
    snapshot->current_spectrum.name = "sample-" + std::to_string(current_index + 1);
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = spectrum_count > 1;
    return snapshot;
}

specforge::SourceCollectionSession MakeSession(
    std::vector<std::size_t>& loaded_indices,
    const std::filesystem::path& source_path,
    std::size_t sample_count)
{
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    return specforge::SourceCollectionSession(
        [&loaded_indices, source_path, sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(path == source_path, "session should reload the active source path");
            loaded_indices.push_back(spectrum_index);
            return MakeSnapshot(source_path, sample_count, spectrum_index);
        },
        navigation_cache,
        labeling_cache);
}

specforge::SourceCollectionSessionResult Submit(
    specforge::SourceCollectionSession& session,
    specforge::SourceCollectionSessionCommand command)
{
    return session.Submit(std::move(command));
}

void TestNavigationReloadsSnapshotAndRemembersLabelingPosition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);

    const specforge::SourceCollectionSessionResult open_result =
        Submit(session, specforge::SourceCollectionSessionCommand::OpenSource(source_path, 0));
    const specforge::SourceCollectionSessionAction& open_action = open_result.action;
    Require(open_action.snapshot_changed, "opening a source should change the displayed snapshot");
    Require(open_action.workflow_changed, "opening a source should activate a workflow identity");
    Require(open_action.navigation_inputs_changed, "opening a source should refresh navigation inputs");
    Require(open_result.view.sources.size() == 1, "opening a source should add one source entry");
    Require(open_result.view.snapshot->collection.current_index == 0, "opened snapshot should start at requested index");

    (void)Submit(session, specforge::SourceCollectionSessionCommand::CreateDefaultLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");

    const specforge::SourceCollectionSessionResult next_result =
        Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                            specforge::SampleNavigationRequest::Next()));
    Require(next_result.navigation.target_found, "next navigation should find a target");
    Require(next_result.navigation.current_index == 1, "next navigation should move to row 1");
    Require(next_result.action.snapshot_changed, "moving to another sample should reload the snapshot");
    Require(next_result.action.navigation_inputs_changed, "moving should refresh navigation inputs");
    Require(next_result.view.snapshot->collection.current_index == 1, "session should expose the reloaded snapshot");

    const specforge::SourceCollectionLabelingView active_labeling = next_result.view.labeling;
    Require(active_labeling.has_active_task, "labeling task should remain active after navigation");
    Require(
        active_labeling.remembered_position && *active_labeling.remembered_position == 1,
        "session navigation should sync the labeling remembered position");
    Require(
        loaded_indices == std::vector<std::size_t>({0, 1}),
        "session should load only the opened and navigated sample snapshots");
}

void TestAssigningLabelAutoAdvancesInsideSession()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, specforge::SourceCollectionSessionCommand::OpenSource(source_path, 0));

    (void)Submit(session, specforge::SourceCollectionSessionCommand::CreateDefaultLabelingTask());
    Require(
        Submit(
            session,
            specforge::SourceCollectionSessionCommand::UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "bad label should be accepted");
    (void)Submit(session, specforge::SourceCollectionSessionCommand::SetActiveLabelingAutoAdvance(true));

    const specforge::SourceCollectionSessionResult assign_result =
        Submit(session, specforge::SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(1));
    const specforge::SourceCollectionSessionAction& assign_action = assign_result.action;
    Require(assign_action.snapshot_changed, "auto-advance should load the next sample snapshot");
    Require(assign_action.navigation_inputs_changed, "auto-advance should refresh navigation inputs");
    Require(assign_result.view.snapshot->collection.current_index == 1, "auto-advance should move to row 1");

    Require(assign_result.view.labeling.has_active_task, "task should remain active after auto-advance");
    const specforge::SourceCollectionSessionResult locate_result =
        Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                            specforge::SampleNavigationRequest::LocateRow(0)));
    Require(locate_result.view.labeling.current_code == 1, "current sample label should be written before advance");
    Require(
        loaded_indices == std::vector<std::size_t>({0, 1, 0}),
        "session should load only the opened, auto-advanced, and verified sample snapshots");
}

void TestLabelingFilterSelectionAppliesToNavigation()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, specforge::SourceCollectionSessionCommand::OpenSource(source_path, 0));

    (void)Submit(session, specforge::SourceCollectionSessionCommand::CreateDefaultLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");
    Require(
        Submit(
            session,
            specforge::SourceCollectionSessionCommand::UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "bad label should be accepted");
    Require(
        Submit(
            session,
            specforge::SourceCollectionSessionCommand::UpsertActiveLabel(
                specforge::SampleLabelDefinition{2, "good", 'g'}))
            .changed,
        "good label should be accepted");
    specforge::SourceCollectionSessionResult result =
        Submit(session, specforge::SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(1));
    Require(result.view.labeling.current_code == 1, "sample 0 should be labeled bad");
    (void)Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                              specforge::SampleNavigationRequest::LocateRow(1)));
    result = Submit(session, specforge::SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(2));
    Require(result.view.labeling.current_code == 2, "sample 1 should be labeled good");
    (void)Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                              specforge::SampleNavigationRequest::LocateRow(2)));
    result = Submit(session, specforge::SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(2));
    Require(result.view.labeling.current_code == 2, "sample 2 should be labeled good");
    (void)Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                              specforge::SampleNavigationRequest::LocateRow(0)));

    result = Submit(session, specforge::SourceCollectionSessionCommand::SetActiveLabelingFilterSourceSelected(true));
    specforge::SourceCollectionFilterView filter_view = result.view.filter;
    Require(filter_view.active_labeling_filter_source_selected, "labeling filter source should be selected");
    Require(filter_view.sources.size() == 1, "selected labeling task should be the only filter source in this fixture");
    Require(filter_view.sources[0].id == "labeling:manual-labeling", "labeling filter source should use the task id");

    result = Submit(
        session,
        specforge::SourceCollectionSessionCommand::SetFilterValueSelected("labeling:manual-labeling", "2", true));
    specforge::SourceCollectionNavigationView navigation_view = result.view.navigation;
    Require(navigation_view.filter_active, "labeling condition should activate navigation filtering");
    Require(navigation_view.filtered_sample_count == 2, "filter should include the two good samples");
    Require(!navigation_view.current_sample_in_filter, "current bad sample should be outside the filter");

    const specforge::SourceCollectionSessionResult locate_action =
        Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                            specforge::SampleNavigationRequest::LocateRow(1)));
    Require(locate_action.navigation.target_found, "locating the first included sample should resolve");
    Require(locate_action.navigation.current_index == 1, "locate should move to the first good sample");

    const specforge::SourceCollectionSessionResult next_action =
        Submit(session, specforge::SourceCollectionSessionCommand::NavigateSample(
                            specforge::SampleNavigationRequest::Next()));
    Require(next_action.navigation.target_found, "filtered next should find a visible target");
    Require(next_action.navigation.current_index == 2, "filtered next should move to the next good sample");

    result = Submit(session, specforge::SourceCollectionSessionCommand::SetActiveLabelingFilterSourceSelected(false));
    Require(!result.view.navigation.filter_active, "deselecting labeling source should clear its navigation filter");
}

}  // namespace

int main()
{
    TestNavigationReloadsSnapshotAndRemembersLabelingPosition();
    TestAssigningLabelAutoAdvancesInsideSession();
    TestLabelingFilterSelectionAppliesToNavigation();
    return 0;
}
