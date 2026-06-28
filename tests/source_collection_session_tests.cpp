#include "domain/sample_labeling.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/source_collection_session.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
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

struct LoadedSourceSnapshot {
    std::filesystem::path path;
    std::size_t index = 0;
};

struct SourceFixture {
    std::filesystem::path path;
    std::size_t sample_count = 0;
};

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "specforge_source_collection_session_";
    path += std::to_string(now);
    path += std::string(suffix);
    return path;
}

void TouchFile(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    Require(stream.good(), "could not create source fixture file");
}

void WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::trunc);
    Require(stream.good(), "could not open text file for writing");
    stream << contents;
    Require(stream.good(), "could not write text file");
}

void SaveLabelResultFixture(
    const std::filesystem::path& path,
    std::string task_id,
    std::string task_name,
    std::vector<int> values,
    specforge::SampleLabelSet label_set,
    bool write_metadata)
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(std::move(task_id), std::move(task_name), values.size());
    task.values = std::move(values);
    task.label_set = std::move(label_set);
    std::string error;
    Require(
        specforge::SaveSampleLabelResultNpy(path, task, &error),
        error.empty() ? "label result fixture NPY should save" : error);
    if (write_metadata) {
        error.clear();
        Require(
            specforge::SaveSampleLabelResultMetadataSidecar(path, task, nullptr, &error),
            error.empty() ? "label result fixture metadata should save" : error);
    }
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

specforge::SourceCollectionSession MakeMultiSourceSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& first_source_path,
    std::size_t first_sample_count,
    const std::filesystem::path& second_source_path,
    std::size_t second_sample_count)
{
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    return specforge::SourceCollectionSession(
        [&loaded_snapshots, first_source_path, first_sample_count, second_source_path, second_sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == first_source_path || path == second_source_path,
                "multi-source session should reload a known source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            if (path == first_source_path) {
                return MakeSnapshot(first_source_path, first_sample_count, spectrum_index);
            }
            return MakeSnapshot(second_source_path, second_sample_count, spectrum_index);
        },
        navigation_cache,
        labeling_cache);
}

specforge::SourceCollectionSession MakePersistentMultiSourceSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& source_session_cache,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    const std::filesystem::path& first_source_path,
    std::size_t first_sample_count,
    const std::filesystem::path& second_source_path,
    std::size_t second_sample_count)
{
    return specforge::SourceCollectionSession(
        [&loaded_snapshots, first_source_path, first_sample_count, second_source_path, second_sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(
                path == first_source_path || path == second_source_path,
                "persistent session should reload a known source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            if (path == first_source_path) {
                return MakeSnapshot(first_source_path, first_sample_count, spectrum_index);
            }
            return MakeSnapshot(second_source_path, second_sample_count, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache);
}

specforge::SourceCollectionSession MakePersistentSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& source_session_cache,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    std::vector<SourceFixture> fixtures)
{
    return specforge::SourceCollectionSession(
        [&loaded_snapshots, fixtures = std::move(fixtures)](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            const auto match = std::find_if(fixtures.begin(), fixtures.end(), [&path](const SourceFixture& fixture) {
                return fixture.path == path;
            });
            Require(match != fixtures.end(), "persistent session should reload a known source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(match->path, match->sample_count, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache);
}

specforge::SourceCollectionSessionResult Submit(
    specforge::SourceCollectionSession& session,
    specforge::SourceCollectionSessionIntent intent)
{
    return session.Submit(std::move(intent));
}

specforge::SourceCollectionSessionIntent OpenSourceCollection(
    std::filesystem::path path,
    std::size_t spectrum_index = 0)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::Open(std::move(path), spectrum_index));
}

specforge::SourceCollectionSessionIntent SwitchSourceCollection(std::size_t source_index)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::SwitchActive(source_index));
}

specforge::SourceCollectionSessionIntent RemoveSourceCollection(std::size_t source_index)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::Remove(source_index));
}

specforge::SourceCollectionSessionIntent AddReadOnlyAnnotation(std::filesystem::path path)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::AddReadOnlyAnnotationResult(std::move(path)));
}

specforge::SourceCollectionSessionIntent RemoveReadOnlyAnnotation(std::filesystem::path path)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::RemoveReadOnlyAnnotationResult(std::move(path)));
}

specforge::SourceCollectionSessionIntent MoveSampleNavigation(specforge::SampleNavigationRequest request)
{
    return specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
        specforge::SampleNavigationIntent::Move(std::move(request)));
}

specforge::SourceCollectionSessionIntent SetSampleNameQuery(std::string query)
{
    return specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
        specforge::SampleNavigationIntent::SetSampleNameQuery(std::move(query)));
}

specforge::SourceCollectionSessionIntent CreateDefaultLabelingTask()
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::CreateDefaultLabelingTask());
}

specforge::SourceCollectionSessionIntent CreateLabelingTask(std::string task_name)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::CreateLabelingTask(std::move(task_name)));
}

specforge::SourceCollectionSessionIntent ActivateLabelingTaskFromAnnotation(std::filesystem::path annotation_path)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(std::move(annotation_path)));
}

specforge::SourceCollectionSessionIntent RenameActiveLabelingTask(std::string task_name)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::RenameActiveLabelingTask(std::move(task_name)));
}

specforge::SourceCollectionSessionIntent DeleteActiveLabelingTask()
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::DeleteActiveLabelingTask());
}

specforge::SourceCollectionSessionIntent SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::SetActiveLabelingOutputPath(std::move(output_path)));
}

specforge::SourceCollectionSessionIntent UpsertActiveLabel(specforge::SampleLabelDefinition label)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::UpsertActiveLabel(std::move(label)));
}

specforge::SourceCollectionSessionIntent SetActiveLabelingAutoAdvance(bool enabled)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(enabled));
}

specforge::SourceCollectionSessionIntent DeactivateActiveLabelingTask()
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::DeactivateActiveLabelingTask());
}

specforge::SourceCollectionSessionIntent AssignActiveLabelToCurrentSample(int code)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(code));
}

specforge::SourceCollectionSessionIntent SetActiveLabelingFilterSourceSelected(bool selected)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::SetActiveLabelingSourceSelected(selected));
}

specforge::SourceCollectionSessionIntent SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::SetFilterValueSelected(
            std::move(source_id),
            std::move(value_key),
            selected));
}

void TestNavigationReloadsSnapshotAndRemembersLabelingPosition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);

    const specforge::SourceCollectionSessionResult open_result =
        Submit(session, OpenSourceCollection(source_path, 0));
    const specforge::SourceCollectionSessionAction& open_action = open_result.action;
    Require(open_action.snapshot_changed, "opening a source should change the displayed snapshot");
    Require(open_action.workflow_changed, "opening a source should activate a workflow identity");
    Require(open_action.navigation_inputs_changed, "opening a source should refresh navigation inputs");
    Require(open_result.view.sources.size() == 1, "opening a source should add one source entry");
    Require(open_result.view.snapshot->collection.current_index == 0, "opened snapshot should start at requested index");

    (void)Submit(session, CreateDefaultLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");

    const specforge::SourceCollectionSessionResult next_result =
        Submit(session, MoveSampleNavigation(
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
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, CreateDefaultLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "bad label should be accepted");
    (void)Submit(session, SetActiveLabelingAutoAdvance(true));

    const specforge::SourceCollectionSessionResult assign_result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    const specforge::SourceCollectionSessionAction& assign_action = assign_result.action;
    Require(assign_action.snapshot_changed, "auto-advance should load the next sample snapshot");
    Require(assign_action.navigation_inputs_changed, "auto-advance should refresh navigation inputs");
    Require(assign_result.view.snapshot->collection.current_index == 1, "auto-advance should move to row 1");

    Require(assign_result.view.labeling.has_active_task, "task should remain active after auto-advance");
    const specforge::SourceCollectionSessionResult locate_result =
        Submit(session, MoveSampleNavigation(
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
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, CreateDefaultLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "bad label should be accepted");
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{2, "good", 'g'}))
            .changed,
        "good label should be accepted");
    specforge::SourceCollectionSessionResult result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(result.view.labeling.current_code == 1, "sample 0 should be labeled bad");
    (void)Submit(session, MoveSampleNavigation(
                              specforge::SampleNavigationRequest::LocateRow(1)));
    result = Submit(session, AssignActiveLabelToCurrentSample(2));
    Require(result.view.labeling.current_code == 2, "sample 1 should be labeled good");
    (void)Submit(session, MoveSampleNavigation(
                              specforge::SampleNavigationRequest::LocateRow(2)));
    result = Submit(session, AssignActiveLabelToCurrentSample(2));
    Require(result.view.labeling.current_code == 2, "sample 2 should be labeled good");
    (void)Submit(session, MoveSampleNavigation(
                              specforge::SampleNavigationRequest::LocateRow(0)));

    result = Submit(session, SetActiveLabelingFilterSourceSelected(true));
    specforge::SourceCollectionFilterView filter_view = result.view.filter;
    Require(filter_view.active_labeling_filter_source_selected, "labeling filter source should be selected");
    Require(filter_view.sources.size() == 1, "selected labeling task should be the only filter source in this fixture");
    Require(filter_view.sources[0].id == "labeling:manual-labeling", "labeling filter source should use the task id");
    Require(filter_view.sources[0].options.size() == 3, "labeling filter view should expose label and unlabeled options");
    Require(
        filter_view.sources[0].options[0].key == "1" && filter_view.sources[0].options[0].sample_count == 1,
        "labeling filter view should reuse source counts for the bad label");
    Require(
        filter_view.sources[0].options[1].key == "2" && filter_view.sources[0].options[1].sample_count == 2,
        "labeling filter view should reuse source counts for the good label");
    Require(
        filter_view.sources[0].options[2].key == "-1" && filter_view.sources[0].options[2].sample_count == 0,
        "labeling filter view should keep zero-count unlabeled option from the source builder");

    result = Submit(
        session,
        SetFilterValueSelected("labeling:manual-labeling", "2", true));
    specforge::SourceCollectionNavigationView navigation_view = result.view.navigation;
    Require(navigation_view.filter_active, "labeling condition should activate navigation filtering");
    Require(navigation_view.filtered_sample_count == 2, "filter should include the two good samples");
    Require(!navigation_view.current_sample_in_filter, "current bad sample should be outside the filter");

    const specforge::SourceCollectionSessionResult locate_action =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::LocateRow(1)));
    Require(locate_action.navigation.target_found, "locating the first included sample should resolve");
    Require(locate_action.navigation.current_index == 1, "locate should move to the first good sample");

    const specforge::SourceCollectionSessionResult next_action =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::Next()));
    Require(next_action.navigation.target_found, "filtered next should find a visible target");
    Require(next_action.navigation.current_index == 2, "filtered next should move to the next good sample");

    result = Submit(session, SetActiveLabelingFilterSourceSelected(false));
    Require(!result.view.navigation.filter_active, "deselecting labeling source should clear its navigation filter");
}

void TestDeactivatingLabelingTaskClearsActiveTaskFilter()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, CreateDefaultLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    specforge::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingFilterSourceSelected(true));
    Require(result.view.filter.active_labeling_filter_source_selected, "test should select active task filter");
    result = Submit(session, SetFilterValueSelected("labeling:manual-labeling", "1", true));
    Require(result.view.navigation.filter_active, "test should activate the label filter");
    Require(result.view.labeling.can_deactivate_task, "draft-only active task should be closable");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(result.action.workflow_changed, "deactivating active task should report workflow change");
    Require(!result.view.labeling.has_active_task, "deactivation should leave no active task");
    Require(!result.view.filter.has_active_labeling_task, "filter view should no longer expose an active task");
    Require(!result.view.filter.active_labeling_filter_source_selected, "deactivation should clear selected task filter source");
    Require(!result.view.navigation.filter_active, "deactivation should clear navigation filtering from the active task");

    result = Submit(session, CreateDefaultLabelingTask());
    Require(result.view.labeling.has_active_task, "task record should remain available after deactivation");
    Require(result.view.labeling.current_code == 1, "reactivated task should keep its label result");
}

void TestCreateLabelingTaskUsesCustomName()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result =
        Submit(session, CreateLabelingTask("  Quality review  "));
    Require(result.view.labeling.has_active_task, "custom labeling task should become active");
    Require(result.view.labeling.task_name == "Quality review", "custom task name should be trimmed and exposed");

    result = Submit(session, SetActiveLabelingFilterSourceSelected(true));
    Require(result.view.filter.sources.size() == 1, "custom task should be available as a filter source");
    Require(result.view.filter.sources[0].id == "labeling:quality-review", "custom task id should derive from name");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "custom task should accept labels");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    (void)Submit(session, DeactivateActiveLabelingTask());
    result = Submit(session, CreateLabelingTask("Quality review"));
    Require(result.view.labeling.has_active_task, "same custom task should reactivate");
    Require(result.view.labeling.current_code == 3, "reactivated custom task should keep its draft values");
    result = Submit(session, SetActiveLabelingFilterSourceSelected(true));
    Require(
        result.view.filter.sources[0].id == "labeling:quality-review",
        "reactivated custom task should keep the same filter source id");
}

void TestRenameAndDeleteActiveLabelingTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, CreateLabelingTask("Quality review"));
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "task should accept a label before rename/delete");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    specforge::SourceCollectionSessionResult result =
        Submit(session, RenameActiveLabelingTask(" Reviewed set "));
    Require(result.view.labeling.has_active_task, "renamed task should remain active");
    Require(result.view.labeling.task_name == "Reviewed set", "rename should trim and expose the new task name");
    result = Submit(session, SetActiveLabelingFilterSourceSelected(true));
    Require(
        result.view.filter.sources[0].id == "labeling:quality-review",
        "rename should keep the stable task id");
    Require(
        result.view.filter.sources[0].name == "Reviewed set",
        "rename should update the filter source display name");

    result = Submit(session, DeleteActiveLabelingTask());
    Require(result.action.workflow_changed, "delete should report workflow change");
    Require(!result.view.labeling.has_active_task, "delete should clear the active task");
    Require(!result.view.filter.has_active_labeling_task, "deleted task should no longer be a filter source");

    result = Submit(session, CreateLabelingTask("Quality review"));
    Require(result.view.labeling.has_active_task, "creating after delete should create a fresh task");
    Require(
        result.view.labeling.current_code == specforge::kUnlabeledSampleLabelCode,
        "deleted draft values should not come back");
}

void TestActivatingExternalAnnotationResultCreatesLocalLabelingTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_quality.npy");
    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(specforge::SampleLabelDefinition{5, "bad", 'b'});
    label_set.labels.push_back(specforge::SampleLabelDefinition{9, "good", 'g'});
    SaveLabelResultFixture(
        annotation_path,
        "quality-review",
        "Quality review",
        {5, -1, 9},
        label_set,
        true);
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "external label result annotation should load");
    Require(result.view.navigation.current_annotations.size() == 1, "loaded annotation should appear in navigation");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "metadata-backed annotation should start as external");
    Require(
        result.view.navigation.current_annotations[0].can_activate_labeling,
        "categorical annotation should be draggable into labeling");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(result.view.labeling.has_active_task, "annotation activation should create an active task");
    Require(result.view.labeling.task_name == "Quality review", "annotation metadata task name should be reused");
    Require(result.view.labeling.current_code == 5, "labeling task should reuse annotation values");
    Require(result.view.labeling.output_path && *result.view.labeling.output_path == annotation_path, "task should bind output path");
    Require(
        result.view.labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "metadata-backed annotation activation should be clean");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "activated annotation should be shown as a local labeling task");

    result = Submit(session, RemoveReadOnlyAnnotation(annotation_path));
    Require(!result.action.workflow_changed, "removing an annotation should not delete the local task");
    Require(result.view.navigation.current_annotations.size() == 1, "local task should remain visible after annotation removal");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "remaining row should come from the local task record");
    Require(result.view.labeling.has_active_task, "removing an annotation should not remove the active local task");
}

void TestAnnotationActivationRequiresCurrentTaskToBeClosed()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_blocked_activation.npy");
    const std::filesystem::path blocked_output_path = UniqueTempPath("_blocked_output");
    std::filesystem::create_directories(blocked_output_path);

    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(specforge::SampleLabelDefinition{5, "bad", 'b'});
    SaveLabelResultFixture(
        annotation_path,
        "external-task",
        "External task",
        {5, -1, 5},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    specforge::SourceCollectionSessionResult result = Submit(session, CreateLabelingTask("Current task"));
    Require(result.view.labeling.has_active_task, "current task should be active before activation attempt");
    Require(result.view.labeling.task_id == "current-task", "test should start with the current task");
    result = Submit(session, SetActiveLabelingOutputPath(blocked_output_path));
    Require(
        result.view.labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "current task should have a failed save guard");

    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "external annotation should load before blocked activation");
    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(result.view.labeling.has_active_task, "blocked activation should keep the active task");
    Require(result.view.labeling.task_id == "current-task", "annotation activation must not switch active tasks");
    Require(
        result.view.labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "blocked activation should preserve the failed save state");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "blocked activation should leave the annotation external");
}

void TestActivatingPlainIntegerAnnotationCreatesMetadataSidecar()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_plain.npy");
    SaveLabelResultFixture(
        annotation_path,
        "plain-fixture",
        "Plain fixture",
        {7, -1, 5},
        {},
        false);
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "plain integer annotation should load");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "annotation without metadata should start as plain");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(result.view.labeling.has_active_task, "plain annotation activation should create an active task");
    Require(result.view.labeling.current_code == 7, "plain annotation values should become editable label values");
    Require(result.view.labeling.label_set.labels.size() == 2, "plain annotation unique values should become labels");
    Require(result.view.labeling.label_set.labels[0].code == 5, "plain annotation labels should include value 5");
    Require(result.view.labeling.label_set.labels[1].code == 7, "plain annotation labels should include value 7");
    Require(
        result.view.labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "plain annotation activation should write its new metadata sidecar");
    Require(
        std::filesystem::exists(specforge::SampleLabelResultMetadataPathForResult(annotation_path)),
        "plain annotation activation should create metadata sidecar");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "plain annotation should become local after activation");
}

void TestLoadedLocalTaskAnnotationStaysLocalWhenMetadataSidecarIsMissing()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_missing_metadata.npy");
    SaveLabelResultFixture(
        annotation_path,
        "plain-fixture",
        "Plain fixture",
        {7, -1, 5},
        {},
        false);
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, AddReadOnlyAnnotation(annotation_path));

    specforge::SourceCollectionSessionResult result =
        Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(result.view.labeling.has_active_task, "plain annotation should become a local task");
    Require(
        std::filesystem::exists(specforge::SampleLabelResultMetadataPathForResult(annotation_path)),
        "test should start with a converted metadata sidecar");

    std::filesystem::remove(specforge::SampleLabelResultMetadataPathForResult(annotation_path));
    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(
        result.view.navigation.current_annotations.size() == 1,
        "same loaded output path should render as one annotation row");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "loaded output path owned by a local task should stay local when metadata is missing");
    Require(
        result.view.navigation.current_annotations[0].metadata_missing,
        "missing local metadata sidecar should be surfaced in the annotation row");
    Require(
        result.view.navigation.current_annotations[0].display_text == "7 (7)",
        "local task values should drive the row after missing metadata fallback");
}

void TestAnnotationLocalMatchRequiresSidecarTaskId()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_task_id_mismatch.npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    TouchFile(source_path);

    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(specforge::SampleLabelDefinition{5, "bad", 'b'});
    SaveLabelResultFixture(
        annotation_path,
        "external-task",
        "External task",
        {5, -1},
        label_set,
        true);

    const specforge::SourceCollectionIdentity identity =
        specforge::BuildSourceCollectionIdentity(*MakeSnapshot(source_path, 2, 0));
    specforge::SampleLabelingTask local_task =
        specforge::CreateSampleLabelingTask("local-task", "Local task", 2);
    local_task.output_path = annotation_path;
    local_task.values = {5, -1};

    specforge::SampleLabelingSourceState source_state;
    source_state.sample_count = identity.spectrum_count;
    source_state.source_name = identity.source_name;
    source_state.source_fingerprint = identity.source_fingerprint;
    source_state.context_fingerprint = identity.context_fingerprint;
    source_state.tasks.push_back(std::move(local_task));
    source_state.active_task_id = "local-task";

    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(identity.id, std::move(source_state));
    Require(specforge::SaveSampleLabelingStateCache(labeling_cache, cache), "labeling cache fixture should save");

    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    specforge::SourceCollectionSession session(
        [&loaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
            Require(path == source_path, "mismatch fixture should load the source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, 2, spectrum_index);
        },
        navigation_cache,
        labeling_cache);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "metadata-backed annotation should load");
    Require(result.view.labeling.has_active_task, "local cache fixture should restore the local task");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "same path and sample count should not make a local match without matching sidecar task id");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(
        result.view.labeling.task_id == "local-task",
        "activation should not switch to a same-path task when the sidecar task id differs");
    Require(
        result.view.navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "mismatched sidecar task id should keep the annotation external");
}

void TestSwitchingSourceCollectionRestoresWorkflowAndClearsFilters()
{
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    specforge::SourceCollectionSession session = MakeMultiSourceSession(
        loaded_snapshots,
        first_source_path,
        3,
        second_source_path,
        2);

    (void)Submit(session, OpenSourceCollection(first_source_path, 0));
    (void)Submit(session, CreateDefaultLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "first source collection should accept a label definition");
    specforge::SourceCollectionSessionResult result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(result.view.labeling.current_code == 1, "first sample should be labeled before filtering");
    result = Submit(session, SetActiveLabelingFilterSourceSelected(true));
    Require(
        result.view.filter.active_labeling_filter_source_selected,
        "active sample workflow should be available as a filter source");
    result = Submit(
        session,
        SetFilterValueSelected("labeling:manual-labeling", "1", true));
    Require(result.view.navigation.filter_active, "labeling filter should affect sample navigation");
    Require(result.view.navigation.filtered_sample_count == 1, "filter should include the one labeled sample");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(result.action.workflow_changed, "opening another source collection should change the active workflow");
    Require(!result.view.labeling.has_active_task, "second source collection should not inherit the first workflow");
    Require(!result.view.filter.active_labeling_filter_source_selected, "source switch should clear selected filter source");
    Require(!result.view.navigation.filter_active, "source switch should clear navigation filtering");

    result = Submit(session, SwitchSourceCollection(0));
    Require(result.action.workflow_changed, "switching back should reactivate the first workflow");
    Require(result.view.labeling.has_active_task, "first source collection workflow should be restored");
    Require(result.view.labeling.current_code == 1, "first source collection label result should be restored");
    Require(!result.view.filter.active_labeling_filter_source_selected, "workflow restore should not restore old filter source");
    Require(!result.view.navigation.filter_active, "workflow restore should leave sample filtering cleared");
}

void TestNavigationViewSeparatesSampleNameFromDisplayName()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);

    specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    specforge::SourceCollectionNavigationView navigation = result.view.navigation;
    Require(navigation.current_sample_display_name == "sample-1", "view should expose the current display name");
    Require(navigation.current_sample_name.empty(), "unnamed samples should not expose a searchable sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_display_name));
    navigation = result.view.navigation;
    Require(!navigation.exact_sample_name_match, "fallback display names should not be exact sample-name matches");
    Require(navigation.sample_name_matches.empty(), "fallback display names should not be sample-name search matches");
}

void TestNavigationViewExposesSourceProvidedSampleName()
{
    const std::filesystem::path source_path = UniqueTempPath("_folder");
    std::filesystem::create_directories(source_path);
    TouchFile(source_path / "alpha.csv");
    TouchFile(source_path / "beta.csv");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 2);

    specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    specforge::SourceCollectionNavigationView navigation = result.view.navigation;
    Require(navigation.has_sample_names, "folder source should expose source-provided sample names");
    Require(navigation.current_sample_name == "alpha.csv", "view should expose the current source-provided sample name");
    Require(navigation.current_sample_display_name == "sample-1", "display text should remain separate from sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_name));
    navigation = result.view.navigation;
    Require(
        navigation.exact_sample_name_match && *navigation.exact_sample_name_match == 0,
        "source-provided sample names should remain searchable");
}

void TestRemovingActiveSourceActivatesNextSourceWorkflow()
{
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    specforge::SourceCollectionSession session = MakeMultiSourceSession(
        loaded_snapshots,
        first_source_path,
        3,
        second_source_path,
        2);

    specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(first_source_path, 1));
    Require(result.view.current_source_index && *result.view.current_source_index == 0, "first source should be active");
    Require(result.view.snapshot->source.path == first_source_path, "first source snapshot should be visible");
    Require(result.view.snapshot->collection.current_index == 1, "first source should open at requested row");
    (void)Submit(session, CreateDefaultLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{10, "first-label", 'f'}))
            .changed,
        "first source should accept its own active label");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(result.view.current_source_index && *result.view.current_source_index == 1, "second source should be active");
    Require(result.view.snapshot->source.path == second_source_path, "second source snapshot should be visible");
    (void)Submit(session, CreateDefaultLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{20, "second-label", 's'}))
            .changed,
        "second source should accept its own active label");

    result = Submit(session, SwitchSourceCollection(0));
    Require(result.action.snapshot_changed, "activating first source should swap to its cached snapshot");
    Require(
        result.view.current_source_index && *result.view.current_source_index == 0,
        "first source should be active again");
    Require(result.view.snapshot->source.path == first_source_path, "reactivated snapshot should be the first source");
    Require(result.view.snapshot->collection.current_index == 1, "reactivated first source should keep its cached row");
    Require(result.view.labeling.has_active_task, "first source labeling task should be restored");
    Require(result.view.labeling.label_set.labels.size() == 1, "first source should expose its own label set");
    Require(result.view.labeling.label_set.labels[0].code == 10, "first source label set should not come from second source");

    const std::size_t loaded_count_before_remove = loaded_snapshots.size();
    result = Submit(session, RemoveSourceCollection(0));
    Require(result.action.snapshot_changed, "removing active first source should activate the next source snapshot");
    Require(result.action.workflow_changed, "removing active first source should resync the workflow");
    Require(result.action.navigation_inputs_changed, "removing active first source should refresh navigation inputs");
    Require(result.view.sources.size() == 1, "removing first source should leave one source");
    Require(result.view.sources[0].path == second_source_path, "remaining source should be the second source");
    Require(
        result.view.current_source_index && *result.view.current_source_index == 0,
        "second source should become index 0");
    Require(result.view.snapshot->source.path == second_source_path, "second source snapshot should be visible after removal");
    Require(result.view.snapshot->collection.current_index == 0, "second source cached row should be preserved");
    Require(result.view.labeling.has_active_task, "second source labeling task should be restored after removal");
    Require(result.view.labeling.label_set.labels.size() == 1, "second source should expose its own label set");
    Require(result.view.labeling.label_set.labels[0].code == 20, "second source label set should survive first removal");
    Require(
        loaded_snapshots.size() == loaded_count_before_remove,
        "removing active source should activate the next cached source without reloading");
}

void TestSourceSessionRestoresSourcesAndActiveIndex()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path first_source_path = UniqueTempPath("_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_second.npy");
    const std::filesystem::path first_annotation_path = UniqueTempPath("_first_annotation.npy");
    const std::filesystem::path second_annotation_path = UniqueTempPath("_second_annotation.npy");
    TouchFile(first_source_path);
    TouchFile(second_source_path);
    SaveLabelResultFixture(first_annotation_path, "first-annotation", "First annotation", {1, 2, 3}, {}, false);
    SaveLabelResultFixture(second_annotation_path, "second-annotation", "Second annotation", {4, 5}, {}, false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        specforge::SourceCollectionSession session = MakePersistentMultiSourceSession(
            loaded_snapshots,
            source_session_cache,
            navigation_cache,
            labeling_cache,
            first_source_path,
            3,
            second_source_path,
            2);

        (void)Submit(session, OpenSourceCollection(first_source_path, 1));
        (void)Submit(session, AddReadOnlyAnnotation(first_annotation_path));
        (void)Submit(session, OpenSourceCollection(second_source_path, 0));
        (void)Submit(session, AddReadOnlyAnnotation(second_annotation_path));
        (void)Submit(session, SwitchSourceCollection(0));
        specforge::SourceCollectionSessionResult navigate_result =
            Submit(session, MoveSampleNavigation(
                                specforge::SampleNavigationRequest::Next()));
        Require(navigate_result.view.snapshot->source.path == first_source_path, "first source should be active");
        Require(navigate_result.view.snapshot->collection.current_index == 2, "first source should reach row 2");
        Require(session.FlushStateCaches(), "session state caches should flush");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = MakePersistentMultiSourceSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        first_source_path,
        3,
        second_source_path,
        2);

    const specforge::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 2, "restored session should reload both source entries");
    Require(view.sources[0].path == first_source_path, "first restored source should keep its path");
    Require(view.sources[1].path == second_source_path, "second restored source should keep its path");
    Require(view.current_source_index && *view.current_source_index == 0, "restored active source should be first");
    Require(view.snapshot->source.path == first_source_path, "restored snapshot should be the active first source");
    Require(view.snapshot->collection.current_index == 2, "restored first source should keep its last sample index");
    Require(view.navigation.current_annotations.size() == 1, "restored first source should restore annotations");
    Require(
        view.navigation.current_annotations[0].path == first_annotation_path,
        "restored first source annotation path should come from source session state");
    Require(restored_loads.size() == 2, "restoring two cached sources should load each source once");
    Require(restored_loads[0].path == first_source_path && restored_loads[0].index == 2, "first source should restore row 2");
    Require(restored_loads[1].path == second_source_path && restored_loads[1].index == 0, "second source should restore row 0");

    std::filesystem::remove(source_session_cache);
    Require(restored.FlushStateCaches(), "flush after restore should succeed without a dirty source session");
    Require(
        !std::filesystem::exists(source_session_cache),
        "restore should not mark the source session cache dirty immediately");

    const specforge::SourceCollectionSessionResult second_result =
        Submit(restored, SwitchSourceCollection(1));
    Require(
        second_result.view.navigation.current_annotations.size() == 1,
        "restored second source should restore its own annotations");
    Require(
        second_result.view.navigation.current_annotations[0].path == second_annotation_path,
        "restored second source annotation path should come from source session state");
}

void TestSourceSessionStateCacheRoundTrip()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_sources.json");
    const std::filesystem::path first_source_path = UniqueTempPath("_adapter_first.npy");
    const std::filesystem::path second_source_path = UniqueTempPath("_adapter_second.npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_adapter_annotation.npy");

    specforge::SourceCollectionSessionStateCache cache;
    specforge::SourceCollectionSavedSource first_source{first_source_path, 2};
    first_source.annotation_paths.push_back(annotation_path);
    cache.sources = {first_source, specforge::SourceCollectionSavedSource{second_source_path, 0}};
    cache.active_source_index = 1;
    Require(
        specforge::SaveSourceCollectionSessionStateCache(source_session_cache, cache),
        "source session cache should save");

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(loaded.sources.size() == 2, "source session cache should restore all sources");
    Require(loaded.sources[0].path == first_source_path, "first source path should round-trip");
    Require(loaded.sources[0].last_spectrum_index == 2, "first source index should round-trip");
    Require(loaded.sources[0].annotation_paths.size() == 1, "first source annotation paths should round-trip");
    Require(
        loaded.sources[0].annotation_paths[0] == annotation_path,
        "first source annotation path should round-trip");
    Require(loaded.sources[1].path == second_source_path, "second source path should round-trip");
    Require(
        loaded.active_source_index && *loaded.active_source_index == 1,
        "active source index should round-trip");
}

void TestSourceSessionStateCacheIgnoresCorruptJson()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_corrupt_sources.json");
    WriteTextFile(source_session_cache, "{ invalid json");

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(loaded.sources.empty(), "corrupt source session cache should be ignored");
    Require(!loaded.active_source_index, "corrupt source session cache should not restore an active source");
}

void TestSourceSessionStateCacheIgnoresUnsupportedSchema()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_adapter_schema_sources.json");
    WriteTextFile(
        source_session_cache,
        "{\n"
        "  \"format_kind\": \"specforge.source_collection_session.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"active_source_index\": 0,\n"
        "  \"sources\": []\n"
        "}\n");

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(loaded.sources.empty(), "unsupported source session cache schema should be ignored");
    Require(
        !loaded.active_source_index,
        "unsupported source session cache schema should not restore an active source");
}

void TestSourceSessionSkipsMissingSourcePathsOnRestore()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path missing_source_path = UniqueTempPath("_missing.npy");
    const std::filesystem::path existing_source_path = UniqueTempPath("_existing.npy");
    TouchFile(existing_source_path);

    specforge::SourceCollectionSessionStateCache saved_state;
    saved_state.sources = {
        specforge::SourceCollectionSavedSource{missing_source_path, 3},
        specforge::SourceCollectionSavedSource{existing_source_path, 1},
    };
    saved_state.active_source_index = 1;
    Require(
        specforge::SaveSourceCollectionSessionStateCache(source_session_cache, saved_state),
        "source session fixture should save");

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {SourceFixture{existing_source_path, 3}});

    const specforge::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 1, "restore should skip missing source paths");
    Require(view.sources[0].path == existing_source_path, "existing source should remain after missing source skip");
    Require(view.current_source_index && *view.current_source_index == 0, "remaining source should become active");
    Require(view.snapshot->source.path == existing_source_path, "active snapshot should use the existing source");
    Require(view.snapshot->collection.current_index == 1, "existing source should restore its last row");
    Require(restored_loads.size() == 1, "missing source should not be loaded");
}

void TestSourceSessionRestoresAtMostThirtyTwoSources()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    std::vector<SourceFixture> fixtures;
    specforge::SourceCollectionSessionStateCache saved_state;
    for (std::size_t index = 0; index < 35; ++index) {
        std::filesystem::path source_path = UniqueTempPath(std::string("_cap_") + std::to_string(index) + ".npy");
        TouchFile(source_path);
        fixtures.push_back(SourceFixture{source_path, 4});
        saved_state.sources.push_back(specforge::SourceCollectionSavedSource{source_path, index % 4});
    }
    saved_state.active_source_index = 34;
    Require(
        specforge::SaveSourceCollectionSessionStateCache(source_session_cache, saved_state),
        "source session cap fixture should save");

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = MakePersistentSession(
        restored_loads,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        fixtures);

    const specforge::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 32, "restore should cap the source list at 32 entries");
    Require(restored_loads.size() == 32, "restore should load only 32 source snapshots");
    Require(view.sources.back().path == saved_state.sources[31].path, "last restored source should be the 32nd entry");
    Require(view.current_source_index && *view.current_source_index == 31, "out-of-cap active source should not restore");
    Require(view.snapshot->source.path == saved_state.sources[31].path, "last restored source should remain active");
}

void TestSourceSessionFlushFailureKeepsDirtyState()
{
    const std::filesystem::path blocker = UniqueTempPath("_blocked");
    const std::filesystem::path source_session_cache = blocker / "source-session.json";
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path source_path = UniqueTempPath("_source.npy");
    TouchFile(blocker);
    TouchFile(source_path);

    std::vector<LoadedSourceSnapshot> loaded_snapshots;
    specforge::SourceCollectionSession session = MakePersistentSession(
        loaded_snapshots,
        source_session_cache,
        navigation_cache,
        labeling_cache,
        {SourceFixture{source_path, 3}});
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, CreateDefaultLabelingTask());

    Require(!session.FlushStateCaches(), "flush should fail when the source cache path is blocked by a file");
    {
        std::vector<LoadedSourceSnapshot> reloaded_snapshots;
        specforge::SourceCollectionSession reloaded = specforge::SourceCollectionSession(
            [&reloaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
                Require(path == source_path, "labeling cache reload should use the source fixture");
                reloaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
                return MakeSnapshot(source_path, 3, spectrum_index);
            },
            navigation_cache,
            labeling_cache);
        const specforge::SourceCollectionSessionResult reload_result =
            Submit(reloaded, OpenSourceCollection(source_path, 0));
        Require(
            reload_result.view.labeling.has_active_task,
            "workflow flush should save labeling state even when source cache flush fails");
    }

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(session.FlushStateCaches(), "flush should retry dirty source state after the path is fixed");

    const specforge::SourceCollectionSessionStateCache restored_state =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(restored_state.sources.size() == 1, "retry flush should write the source session cache");
    Require(restored_state.sources[0].path == source_path, "retry flush should persist the source path");
}

}  // namespace

int main()
{
    TestNavigationReloadsSnapshotAndRemembersLabelingPosition();
    TestAssigningLabelAutoAdvancesInsideSession();
    TestLabelingFilterSelectionAppliesToNavigation();
    TestDeactivatingLabelingTaskClearsActiveTaskFilter();
    TestCreateLabelingTaskUsesCustomName();
    TestRenameAndDeleteActiveLabelingTask();
    TestActivatingExternalAnnotationResultCreatesLocalLabelingTask();
    TestAnnotationActivationRequiresCurrentTaskToBeClosed();
    TestActivatingPlainIntegerAnnotationCreatesMetadataSidecar();
    TestLoadedLocalTaskAnnotationStaysLocalWhenMetadataSidecarIsMissing();
    TestAnnotationLocalMatchRequiresSidecarTaskId();
    TestSwitchingSourceCollectionRestoresWorkflowAndClearsFilters();
    TestNavigationViewSeparatesSampleNameFromDisplayName();
    TestNavigationViewExposesSourceProvidedSampleName();
    TestRemovingActiveSourceActivatesNextSourceWorkflow();
    TestSourceSessionRestoresSourcesAndActiveIndex();
    TestSourceSessionStateCacheRoundTrip();
    TestSourceSessionStateCacheIgnoresCorruptJson();
    TestSourceSessionStateCacheIgnoresUnsupportedSchema();
    TestSourceSessionSkipsMissingSourcePathsOnRestore();
    TestSourceSessionRestoresAtMostThirtyTwoSources();
    TestSourceSessionFlushFailureKeepsDirtyState();
    return 0;
}
