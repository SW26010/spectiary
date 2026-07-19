#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"
#include "domain/sample_labeling.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
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

std::string Utf8(std::u8string_view value)
{
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
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

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "could not open text file for reading");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteUnicodeNameNpy(
    const std::filesystem::path& path,
    std::initializer_list<std::string_view> values,
    std::size_t code_units)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open name NPY fixture for writing");

    std::string header = "{'descr': '<U";
    header += std::to_string(code_units);
    header += "', 'fortran_order': False, 'shape': (";
    header += std::to_string(values.size());
    header += ",), }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    Require(header.size() <= std::numeric_limits<std::uint16_t>::max(), "test NPY header is too large");

    constexpr unsigned char kMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic), static_cast<std::streamsize>(sizeof(kMagic)));
    constexpr char kVersion[] = {1, 0};
    stream.write(kVersion, static_cast<std::streamsize>(sizeof(kVersion)));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const char length_bytes[] = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes, static_cast<std::streamsize>(sizeof(length_bytes)));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (std::string_view value : values) {
        for (std::size_t index = 0; index < code_units; ++index) {
            const std::uint32_t code_point =
                index < value.size() ? static_cast<unsigned char>(value[index]) : 0U;
            const char bytes[] = {
                static_cast<char>(code_point & 0xffU),
                static_cast<char>((code_point >> 8U) & 0xffU),
                static_cast<char>((code_point >> 16U) & 0xffU),
                static_cast<char>((code_point >> 24U) & 0xffU),
            };
            stream.write(bytes, static_cast<std::streamsize>(sizeof(bytes)));
        }
    }
    Require(stream.good(), "could not write name NPY fixture");
}

std::filesystem::path CompanionNamePath(const std::filesystem::path& source_path)
{
    return source_path.parent_path() / (source_path.stem().string() + "_name.npy");
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string AnnotationSourceId(const std::filesystem::path& path)
{
    return "annotation:" + PathToUtf8(path);
}

bool HasSortSource(
    const specforge::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    return std::any_of(view.sources.begin(), view.sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
}

bool HasAvailableSortSource(
    const specforge::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    return std::any_of(view.available_sources.begin(), view.available_sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
}

const specforge::SourceCollectionSampleSortSourceView* FindSortSource(
    const specforge::SourceCollectionSampleSortingView& view,
    std::string_view source_id)
{
    const auto match = std::find_if(view.sources.begin(), view.sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
    return match == view.sources.end() ? nullptr : &*match;
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

specforge::SourceCollectionSession MakeWorkflowPersistentSession(
    std::vector<LoadedSourceSnapshot>& loaded_snapshots,
    const std::filesystem::path& navigation_cache,
    const std::filesystem::path& labeling_cache,
    const std::filesystem::path& workflow_cache,
    const std::filesystem::path& source_path,
    std::size_t sample_count)
{
    return specforge::SourceCollectionSession(
        [&loaded_snapshots, source_path, sample_count](
            const std::filesystem::path& path,
            std::size_t spectrum_index) {
            Require(path == source_path, "workflow-persistent session should reload the active source path");
            loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, sample_count, spectrum_index);
        },
        std::filesystem::path{},
        navigation_cache,
        labeling_cache,
        workflow_cache);
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

specforge::SourceCollectionSessionIntent RenameAnnotationDisplayName(
    std::filesystem::path path,
    std::string display_name)
{
    return specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::RenameAnnotationResultDisplayName(
            std::move(path),
            std::move(display_name)));
}

specforge::SourceCollectionSessionIntent AddSampleFilterSource(std::string source_id)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::AddSource(std::move(source_id)));
}

specforge::SourceCollectionSessionIntent RemoveSampleFilterSource(std::string source_id)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::RemoveSource(std::move(source_id)));
}

std::filesystem::path AddPlainIntegerFilterAnnotation(
    specforge::SourceCollectionSession& session,
    std::vector<int> values,
    std::string_view suffix = "_filter.npy")
{
    const std::filesystem::path annotation_path = UniqueTempPath(suffix);
    SaveLabelResultFixture(
        annotation_path,
        "filter",
        "Filter",
        std::move(values),
        specforge::SampleLabelSet{},
        false);
    const specforge::SourceCollectionSessionResult result =
        Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "plain integer filter annotation should load");
    return annotation_path;
}

std::string AddPlainIntegerSampleFilterSource(
    specforge::SourceCollectionSession& session,
    std::vector<int> values,
    std::string_view suffix = "_filter.npy")
{
    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, std::move(values), suffix);
    const std::string source_id = AnnotationSourceId(annotation_path);
    const specforge::SourceCollectionSessionResult result =
        Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "sample filter source should be explicitly added");
    Require(session.View().filter.sources[0].id == source_id, "added filter source should use the annotation id");
    return source_id;
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

specforge::SourceCollectionSessionIntent StartOrResumeTemporaryLabelingTask()
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask());
}

specforge::SourceCollectionSessionIntent ActivateLabelingTaskFromAnnotation(std::filesystem::path annotation_path)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(std::move(annotation_path)));
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

specforge::SourceCollectionSessionIntent UpdateActiveLabel(
    int original_code,
    specforge::SampleLabelDefinition label,
    bool allow_used_code_change)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::UpdateActiveLabel(
            original_code,
            std::move(label),
            allow_used_code_change));
}

specforge::SourceCollectionSessionIntent RemoveActiveLabel(int code)
{
    return specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        specforge::ActiveSampleWorkflowIntent::RemoveActiveLabel(code));
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

specforge::SourceCollectionSessionIntent ClearSampleSorting()
{
    return specforge::SourceCollectionSessionIntent::ApplySampleSorting(
        specforge::SampleSortingIntent::Clear());
}

specforge::SourceCollectionSessionIntent AddSampleSortSource(std::string source_id)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleSorting(
        specforge::SampleSortingIntent::AddSource(std::move(source_id)));
}

specforge::SourceCollectionSessionIntent RemoveSampleSortSource(std::string source_id)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleSorting(
        specforge::SampleSortingIntent::RemoveSource(std::move(source_id)));
}

specforge::SourceCollectionSessionIntent SetSampleSortSource(std::string source_id)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleSorting(
        specforge::SampleSortingIntent::SetSortSource(std::move(source_id)));
}

specforge::SourceCollectionSessionIntent SetSampleSortDirection(
    specforge::SampleNavigationSortDirection direction)
{
    return specforge::SourceCollectionSessionIntent::ApplySampleSorting(
        specforge::SampleSortingIntent::SetSortDirection(direction));
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
    Require(session.View().sources.size() == 1, "opening a source should add one source entry");
    Require(session.View().snapshot->collection.current_index == 0, "opened snapshot should start at requested index");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "active source should accept a labeling task");

    const specforge::SourceCollectionSessionResult next_result =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::Next()));
    Require(next_result.navigation.target_found, "next navigation should find a target");
    Require(next_result.navigation.current_index == 1, "next navigation should move to row 1");
    Require(next_result.action.snapshot_changed, "moving to another sample should reload the snapshot");
    Require(next_result.action.navigation_inputs_changed, "moving should refresh navigation inputs");
    Require(session.View().snapshot->collection.current_index == 1, "session should expose the reloaded snapshot");

    const specforge::SourceCollectionLabelingView active_labeling = session.View().labeling;
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

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
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
    Require(session.View().snapshot->collection.current_index == 1, "auto-advance should move to row 1");

    Require(session.View().labeling.has_active_task, "task should remain active after auto-advance");
    const specforge::SourceCollectionSessionResult locate_result =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::LocateRow(0)));
    Require(session.View().labeling.current_code == 1, "current sample label should be written before advance");
    Require(
        loaded_indices == std::vector<std::size_t>({0, 1, 0}),
        "session should load only the opened, auto-advanced, and verified sample snapshots");
}

void TestAnnotationFilterSelectionAppliesToNavigation()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, {1, 2, 2}, "_quality.npy");
    const std::string source_id = AnnotationSourceId(annotation_path);

    specforge::SourceCollectionFilterView filter_view = session.View().filter;
    Require(filter_view.sources.empty(), "annotation filter sources should not be selected by default");
    Require(filter_view.available_sources.size() == 1, "filterable annotation should be available to add");
    Require(filter_view.available_sources[0].id == source_id, "available source should use the annotation id");

    specforge::SourceCollectionSessionResult result = Submit(session, AddSampleFilterSource(source_id));
    filter_view = session.View().filter;
    Require(filter_view.sources.size() == 1, "added annotation should become a sample filter source");
    Require(filter_view.available_sources.empty(), "added annotation should leave the add-source list");
    Require(filter_view.sources[0].id == source_id, "selected filter source should use the annotation id");
    Require(filter_view.sources[0].options.size() == 2, "annotation filter view should expose observed values");
    Require(
        filter_view.sources[0].options[0].key == "1" && filter_view.sources[0].options[0].sample_count == 1,
        "annotation filter view should count the first value");
    Require(
        filter_view.sources[0].options[1].key == "2" && filter_view.sources[0].options[1].sample_count == 2,
        "annotation filter view should count the second value");

    result = Submit(
        session,
        SetFilterValueSelected(source_id, "2", true));
    filter_view = session.View().filter;
    specforge::SourceCollectionNavigationView navigation_view = session.View().navigation;
    Require(navigation_view.filter_active, "annotation condition should activate navigation filtering");
    Require(navigation_view.filtered_sample_count == 2, "filter should include the two matching samples");
    Require(navigation_view.sequence_active, "filter should expose an active navigation sequence");
    Require(navigation_view.sequence_count == 2, "sequence should include the two matching samples");
    Require(
        navigation_view.current_index && *navigation_view.current_index == 1,
        "filter should move navigation to the first included sample");
    Require(
        navigation_view.current_sequence_position && *navigation_view.current_sequence_position == 0,
        "first included sample should be sequence position 0");
    Require(navigation_view.current_sample_in_filter, "reconciled current sample should be inside the filter");
    Require(!navigation_view.row_location_available, "row-index location should be disabled for filtered sequence");
    Require(result.action.snapshot_changed, "filter should load the first included sample snapshot");
    Require(session.View().snapshot->collection.current_index == 1, "filter should display the first included sample");

    const specforge::SourceCollectionSessionResult locate_action =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::LocateRow(0)));
    Require(locate_action.navigation.blocked_by_filter, "row locate should be blocked while filtering changes order");
    Require(!locate_action.navigation.target_found, "blocked row locate should not resolve");
    Require(locate_action.navigation.current_index == 1, "blocked row locate should keep the sequence current row");

    const specforge::SourceCollectionSessionResult next_action =
        Submit(session, MoveSampleNavigation(
                            specforge::SampleNavigationRequest::Next()));
    Require(next_action.navigation.target_found, "filtered next should find a visible target");
    Require(next_action.navigation.current_index == 2, "filtered next should move to the next matching sample");

    result = Submit(session, RemoveSampleFilterSource(source_id));
    Require(!session.View().navigation.filter_active, "removing the sample filter source should clear navigation filtering");
    Require(session.View().filter.sources.empty(), "removed source should leave no selected sample filters");
    Require(session.View().filter.available_sources.size() == 1, "removed source should return to the add-source list");
}

void TestLocalLabelingAnnotationCanBeSampleFilterSource()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "bad label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{2, "good", 'g'})).changed,
        "good label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(1));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(2)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(2));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(0)));

    specforge::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(output_path));
    const std::string source_id = "labeling:temporary-labeling-task";
    Require(
        session.View().navigation.current_annotations.size() == 1 &&
            session.View().navigation.current_annotations[0].relationship ==
                specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "local labeling task should appear as an annotation row");
    Require(
        session.View().navigation.current_annotations[0].can_filter_samples,
        "local labeling annotation row should be draggable to sample filters");
    Require(session.View().filter.sources.empty(), "local labeling source should not be selected by default");
    Require(session.View().filter.available_sources.size() == 1, "local labeling source should be available to add");
    Require(session.View().filter.available_sources[0].id == source_id, "local labeling filter source should use task id");

    result = Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "local labeling source should be explicitly addable");
    Require(session.View().filter.sources[0].options.size() == 3, "labeling source should expose labels and unlabeled");

    result = Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.filter_active, "local labeling sample filter should affect navigation");
    Require(session.View().navigation.sequence_count == 2, "local labeling filter should include the good samples");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "local labeling filter should move to the first matching sample");
}

void TestRemovingLabelSelectedBySampleFilterReloadsReconciledSnapshot()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(0)));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id = "labeling:temporary-labeling-task";
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));
    (void)Submit(session, SetFilterValueSelected(source_id, "4", true));
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 0,
        "test should start on the sample using the label to remove");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 0,
        "test snapshot should start on the sample using the label to remove");

    const specforge::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "removing the label selected by a sample filter should change the task");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "removing the label currently selected by sample filtering should reconcile navigation to the "
        "remaining match");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 1,
        "removing the label currently selected by sample filtering should reload the spectrum snapshot");
    Require(
        session.View().current_sample_snapshot &&
            session.View().current_sample_snapshot->collection.current_index == 1,
        "the current sample snapshot should match reconciled navigation");
    Require(result.action.snapshot_changed, "reloading the reconciled sample should report a snapshot change");
}

void TestRemovingLabelPrunesItsSampleFilterValue()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id = "labeling:temporary-labeling-task";
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));
    (void)Submit(session, SetFilterValueSelected(source_id, "4", true));

    const specforge::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "removing the selected label should change the task");
    Require(session.View().filter.sources.size() == 1, "the labeling sample-filter source should stay selected");
    const std::unordered_set<std::string> selected_keys =
        session.View().filter.sources[0].selected_value_keys;
    Require(
        selected_keys.find("3") == selected_keys.end(),
        "removing a label should remove its code from the active sample-filter condition");
    Require(
        selected_keys.find("4") != selected_keys.end(),
        "removing a label should preserve other selected values in the same sample-filter condition");
    Require(session.View().navigation.filter_active, "the remaining selected value should keep sample filtering active");
    Require(session.View().navigation.sequence_count == 1, "the remaining selected label should keep one sample");
}

void TestChangingUsedLabelCodeMigratesValuesAndSampleFilter()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "review label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{4, "keep", 'k'})).changed,
        "keep label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(1)));
    (void)Submit(session, AssignActiveLabelToCurrentSample(4));
    (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(0)));
    (void)Submit(session, SetActiveLabelingOutputPath(output_path));

    const std::string source_id = "labeling:temporary-labeling-task";
    (void)Submit(session, AddSampleFilterSource(source_id));
    (void)Submit(session, SetFilterValueSelected(source_id, "3", true));

    specforge::SourceCollectionSessionResult result = Submit(
        session,
        UpdateActiveLabel(3, specforge::SampleLabelDefinition{7, "accepted", 'a'}, false));
    Require(!result.changed, "changing a used label code should be rejected without confirmation");
    Require(session.View().labeling.current_code == 3, "rejected recode should keep the current sample value");

    result = Submit(
        session,
        UpdateActiveLabel(3, specforge::SampleLabelDefinition{7, "accepted", 'a'}, true));
    Require(result.changed, "confirmed used label recode should flow through the session intent");
    Require(session.View().labeling.current_code == 7, "confirmed recode should migrate the current sample value");
    Require(
        specforge::FindSampleLabel(session.View().labeling.label_set, 3) == nullptr &&
            specforge::FindSampleLabel(session.View().labeling.label_set, 7) != nullptr,
        "confirmed recode should atomically replace the label definition");
    Require(session.View().filter.sources.size() == 1, "confirmed recode should keep the sample filter source");
    const std::unordered_set<std::string> selected_keys =
        session.View().filter.sources[0].selected_value_keys;
    Require(selected_keys.find("3") == selected_keys.end(), "sample filtering should drop the old label code");
    Require(selected_keys.find("7") != selected_keys.end(), "sample filtering should follow the new label code");
    Require(session.View().navigation.sequence_count == 1, "migrated sample filtering should keep one match");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 0,
        "migrated sample filtering should keep the same matching sample current");
    Require(
        session.View().snapshot && session.View().snapshot->collection.current_index == 0,
        "migrated sample filtering should keep the spectrum snapshot aligned");

    std::string load_error;
    const std::optional<std::vector<int>> persisted =
        specforge::LoadSampleLabelResultNpy(output_path, 3, &load_error);
    Require(persisted.has_value(), load_error.empty() ? "recode output should load" : load_error);
    Require(*persisted == std::vector<int>({7, 4, -1}), "confirmed recode should persist migrated values");
}

void TestLabelingViewCodeConflictIncludesUndefinedSampleValues()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_incomplete_metadata.npy");
    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(specforge::SampleLabelDefinition{5, "defined", 'd'});
    SaveLabelResultFixture(
        annotation_path,
        "incomplete-metadata",
        "Incomplete metadata",
        {5, 9, -1},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    Require(Submit(session, AddReadOnlyAnnotation(annotation_path)).loaded, "fixture annotation should load");
    (void)Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));

    const specforge::SourceCollectionLabelingView view = session.View().labeling;
    Require(
        specforge::FindSampleLabel(view.label_set, 9) == nullptr,
        "fixture metadata should intentionally omit sample value code 9");
    Require(view.label_usage_counts.at(9) == 1, "the labeling view should count undefined sample value code 9");

    Require(
        view.HasConflictingLabelCode(5, 9),
        "a code present in sample values should conflict even when its metadata definition is missing");
    Require(
        !Submit(
             session,
             UpdateActiveLabel(5, specforge::SampleLabelDefinition{9, "collision", 'c'}, true))
             .changed,
        "the session should reject recoding a label to an undefined code already present in sample values");
}

void TestResumeLocateRespectsActiveFilterSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});

    specforge::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.sequence_active, "test should activate the filtered sequence");
    Require(session.View().navigation.sequence_count == 2, "test should include only the two matching samples");
    Require(session.View().navigation.sequence_rows.empty(), "active sequence rows should stay out of the per-frame view");
    Require(
        session.View().navigation.current_index && *session.View().navigation.current_index == 1,
        "filter should reconcile to the first included row");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(2)));
    Require(!result.navigation.target_found, "ordinary row locate remains blocked when sequence order differs");
    Require(result.navigation.blocked_by_filter, "ordinary row locate should report the active filter block");
    Require(result.navigation.current_index == 1, "blocked ordinary locate should keep the current row");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateSourceRowInSequence(2)));
    Require(result.navigation.target_found, "resume locate should allow a remembered row inside the sequence");
    Require(result.navigation.current_index == 2, "resume locate should jump to the remembered in-sequence row");
    Require(session.View().snapshot->collection.current_index == 2, "resume locate should load the remembered row snapshot");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateSourceRowInSequence(0)));
    Require(!result.navigation.target_found, "resume locate should not bypass the active sequence");
    Require(result.navigation.blocked_by_filter, "out-of-sequence resume locate should report the filter block");
    Require(result.navigation.current_index == 2, "blocked resume locate should keep the current sequence row");
}

void TestSourceOrderNavigationViewDoesNotMaterializeSequenceRows()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    TouchFile(source_path);
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 4);

    const specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    const specforge::SourceCollectionNavigationView navigation = session.View().navigation;

    Require(!navigation.sequence_active, "source order should not expose an active sequence");
    Require(navigation.sequence_count == 4, "source-order sequence count should still match sample count");
    Require(navigation.filtered_sample_count == 4, "source-order filtered count should still match sample count");
    Require(navigation.sequence_rows.empty(), "source-order view should not materialize rows");
    Require(
        navigation.current_sequence_position && *navigation.current_sequence_position == 0,
        "source-order current position should remain available");
    Require(navigation.row_location_available, "source-order row location should remain available");
}

void TestRememberedPositionResumableTracksActiveSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});

    specforge::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "2", true));
    Require(session.View().navigation.sequence_count == 2, "annotation filter should include rows 1 and 2");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::Next()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "next should remember row 2");
    Require(
        session.View().labeling.remembered_position && *session.View().labeling.remembered_position == 2,
        "next navigation should remember row 2");
    Require(session.View().labeling.remembered_position_resumable, "remembered in-sequence row should be resumable");

    (void)Submit(session, SetFilterValueSelected(source_id, "2", false));
    result = Submit(session, SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.sequence_count == 1, "second annotation filter should include only row 0");
    Require(
        session.View().labeling.remembered_position && *session.View().labeling.remembered_position == 2,
        "remembered row should survive filter changes");
    Require(
        !session.View().labeling.remembered_position_resumable,
        "remembered out-of-sequence row should not be resumable");
}

void TestSampleSortingIntentAppliesNavigationSequence()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);

    specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    Require(session.View().sorting.has_active_source, "sorting view should attach to the active source");
    Require(HasSortSource(session.View().sorting, "sample-name"), "sample names should be available as a sort source");

    result = Submit(session, SetSampleSortSource("sample-name"));
    Require(session.View().sorting.active, "selecting sample-name sorting should activate sorting view state");
    Require(session.View().navigation.sequence_count == 3, "sample-name sorting should keep all rows in the sequence");
    Require(session.View().navigation.sequence_rows.empty(), "sorted rows should stay out of the per-frame view");
    Require(!session.View().navigation.row_location_available, "sorted sequence should disable ordinary row locate");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "current row should keep selection and update its sorted sequence position");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::Previous()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "previous should follow sorted order");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateRow(1)));
    Require(!result.navigation.target_found, "ordinary row locate should be blocked for sorted sequence");
    Require(result.navigation.current_index == 2, "blocked row locate should keep the sorted current row");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::LocateSourceRowInSequence(1)));
    Require(result.navigation.target_found && result.navigation.current_index == 1, "sequence locate should allow sorted in-sequence rows");

    result = Submit(session, SetSampleSortDirection(specforge::SampleNavigationSortDirection::Descending));
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "descending sample-name sorting should place alpha after gamma and beta");
    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::Previous()));
    Require(result.navigation.target_found && result.navigation.current_index == 2, "previous should follow descending sorted order");

    result = Submit(session, ClearSampleSorting());
    Require(!session.View().sorting.active, "clearing sorting should return sorting view to source order");
    Require(
        session.View().sorting.direction == specforge::SampleNavigationSortDirection::Ascending,
        "clearing sorting should restore ascending source order");
    Require(!session.View().navigation.sequence_active, "clearing sorting without filters should deactivate sequence state");
    Require(session.View().navigation.row_location_available, "source-order navigation should allow ordinary row locate again");

    result = Submit(session, SetSampleSortDirection(specforge::SampleNavigationSortDirection::Descending));
    result = Submit(session, SetSampleSortSource("source-order"));
    Require(session.View().sorting.active, "descending source-order sorting should activate sorting view state");
    Require(
        session.View().sorting.active_source_id == "source-order",
        "source-order sorting should use the source-order source id");
    Require(session.View().navigation.sequence_active, "descending source-order sorting should activate navigation sequence");
    Require(!session.View().navigation.row_location_available, "descending source order should disable ordinary row locate");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 0,
        "descending source order should place row 2 at the first sequence position");

    result = Submit(session, SetSampleSortSource("sample-name"));
    result = Submit(session, SetSampleSortDirection(specforge::SampleNavigationSortDirection::Ascending));
    Require(
        session.View().sorting.source_order_direction == specforge::SampleNavigationSortDirection::Descending,
        "inactive source-order sorting should retain its own descending direction");

    result = Submit(session, SetSampleSortSource("source-order"));
    Require(
        session.View().sorting.direction == specforge::SampleNavigationSortDirection::Descending,
        "reactivating source-order sorting should use its cached direction");

    result = Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::Next()));
    Require(
        result.navigation.target_found && result.navigation.current_index == 1,
        "next should follow descending source order");
}

void TestSampleSortingSourcesRequireExplicitAddition()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    const std::string rank_source_id = AnnotationSourceId(rank_path);
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {2, 1, 1},
        specforge::SampleLabelSet{},
        false);

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
    Require(result.loaded, "plain integer annotation should load");
    specforge::SourceCollectionSessionView view = session.View();
    Require(HasSortSource(view.sorting, "sample-name"), "sample-name should be a default sort entry");
    Require(!HasSortSource(view.sorting, rank_source_id), "annotation sorting should not be selected by default");
    Require(
        HasAvailableSortSource(view.sorting, rank_source_id),
        "plain annotation sorting should be available to add");

    result = Submit(session, AddSampleSortSource(rank_source_id));
    view = session.View();
    Require(!view.sorting.active, "adding a sort entry should not activate sorting by itself");
    Require(HasSortSource(view.sorting, rank_source_id), "added annotation sorting should enter the visible list");
    Require(
        !HasAvailableSortSource(view.sorting, rank_source_id),
        "added annotation sorting should leave the addable list");
    Require(view.sorting.sources.back().removable, "added annotation sorting should be removable");

    result = Submit(session, SetSampleSortSource("sample-name"));
    result = Submit(session, SetSampleSortDirection(specforge::SampleNavigationSortDirection::Descending));
    view = session.View();
    const specforge::SourceCollectionSampleSortSourceView* rank_before_activation =
        FindSortSource(view.sorting, rank_source_id);
    Require(rank_before_activation != nullptr, "added annotation sorting should remain visible");
    Require(
        rank_before_activation->direction == specforge::SampleNavigationSortDirection::Ascending,
        "inactive annotation sorting should keep its own ascending direction");

    result = Submit(session, SetSampleSortSource(rank_source_id));
    view = session.View();
    Require(view.sorting.active, "activating an added sort entry should turn sorting on");
    Require(
        view.sorting.active_source_id == rank_source_id,
        "the added annotation should become the active sort source");
    Require(
        view.sorting.direction == specforge::SampleNavigationSortDirection::Ascending,
        "activating annotation sorting should use its cached direction");
    const specforge::SourceCollectionSampleSortSourceView* sample_name_after_activation =
        FindSortSource(view.sorting, "sample-name");
    Require(sample_name_after_activation != nullptr, "sample-name sorting should stay visible");
    Require(
        sample_name_after_activation->direction == specforge::SampleNavigationSortDirection::Descending,
        "inactive sample-name sorting should retain its own descending direction");
    Require(view.navigation.sequence_active, "active annotation sorting should reorder navigation");

    result = Submit(session, RemoveSampleSortSource(rank_source_id));
    view = session.View();
    Require(!HasSortSource(view.sorting, rank_source_id), "removed annotation sorting should leave the visible list");
    Require(
        HasAvailableSortSource(view.sorting, rank_source_id),
        "removed annotation sorting should become available to add again");
    Require(!view.sorting.active, "removing the active sort entry should clear active sorting");
    Require(!view.navigation.sequence_active, "removing active sorting should restore source-order navigation");
}

void TestSampleWorkflowStateRestoresFiltersAndSorting()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path annotation_path = UniqueTempPath("_quality.npy");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    TouchFile(source_path);
    WriteUnicodeNameNpy(CompanionNamePath(source_path), {"gamma", "alpha", "beta"}, 6);
    SaveLabelResultFixture(
        annotation_path,
        "quality",
        "Quality",
        {1, 2, 2},
        specforge::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        specforge::SourceCollectionSession session = MakeWorkflowPersistentSession(
            loaded_snapshots,
            navigation_cache,
            labeling_cache,
            workflow_cache,
            source_path,
            3);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
        Require(result.loaded, "test annotation should load before selecting sample filters");
        result = Submit(session, AddSampleFilterSource(annotation_source_id));
        Require(session.View().filter.sources.size() == 1, "test should add the annotation sample filter source");
        result = Submit(session, SetFilterValueSelected(annotation_source_id, "2", true));
        Require(session.View().navigation.filter_active, "test should activate an annotation-value sample filter");
        Require(session.View().navigation.sequence_count == 2, "test should keep only the two matching samples");

        result = Submit(session, SetSampleSortSource("sample-name"));
        Require(session.View().sorting.active, "test should activate sample-name sorting");
        result = Submit(session, SetSampleSortDirection(specforge::SampleNavigationSortDirection::Descending));
        Require(
            session.View().sorting.direction == specforge::SampleNavigationSortDirection::Descending,
            "test should switch sorting to descending");
        Require(
            session.View().navigation.current_sequence_position &&
                *session.View().navigation.current_sequence_position == 1,
            "descending filtered sequence should place alpha after beta");

        Require(session.FlushStateCaches(), "session flush should save workflow state");
        Require(std::filesystem::exists(workflow_cache), "workflow state flush should create the workflow cache");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = MakeWorkflowPersistentSession(
        restored_loads,
        navigation_cache,
        labeling_cache,
        workflow_cache,
        source_path,
        3);

    (void)Submit(restored, OpenSourceCollection(source_path, 0));
    Require(restored.View().filter.sources.empty(), "restored workflow should wait for its annotation source to load");
    (void)Submit(restored, AddReadOnlyAnnotation(annotation_path));
    const specforge::SourceCollectionSessionView& view = restored.View();
    Require(view.filter.sources.size() == 1, "restored workflow should expose the annotation sample filter source");
    Require(
        view.filter.sources[0].id == annotation_source_id,
        "restored workflow should select the annotation sample filter source");
    Require(
        view.filter.sources[0].selected_value_keys.find("2") != view.filter.sources[0].selected_value_keys.end(),
        "restored workflow should preserve the selected annotation value");
    Require(view.navigation.filter_active, "restored workflow should reactivate navigation filtering");
    Require(view.navigation.sequence_count == 2, "restored filter should still include two samples");
    Require(view.sorting.active, "restored workflow should reactivate sample sorting");
    Require(
        view.sorting.direction == specforge::SampleNavigationSortDirection::Descending,
        "restored workflow should preserve descending sorting direction");
    Require(
        std::any_of(view.sorting.sources.begin(), view.sorting.sources.end(), [](const auto& source) {
            return source.id == "sample-name" && source.selected;
        }),
        "restored workflow should preserve the sample-name sort source");
    Require(
        view.navigation.current_index && *view.navigation.current_index == 1,
        "restored workflow should reconcile to the first filtered sample");
    Require(
        view.navigation.current_sequence_position && *view.navigation.current_sequence_position == 1,
        "restored descending sequence should keep alpha at position 1");
    Require(
        view.snapshot->collection.current_index == 1,
        "restored source should load the reconciled filtered sample snapshot");

    const specforge::SourceCollectionSessionResult previous_result =
        Submit(restored, MoveSampleNavigation(specforge::SampleNavigationRequest::Previous()));
    Require(
        previous_result.navigation.target_found && previous_result.navigation.current_index == 2,
        "restored previous navigation should follow the descending filtered sequence");
}

void TestAnnotationDisplayNameCustomizesWorkflowSurfacesAndPersists()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path annotation_path = UniqueTempPath("_rank.npy");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);
    const std::string utf8_display_name = Utf8(u8"\u8d28\u91cf\u8bc4\u5206");
    TouchFile(source_path);
    SaveLabelResultFixture(
        annotation_path,
        "rank",
        "Rank",
        {2, 1, 3},
        specforge::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        specforge::SourceCollectionSession session = MakeWorkflowPersistentSession(
            loaded_snapshots,
            navigation_cache,
            labeling_cache,
            workflow_cache,
            source_path,
            3);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(annotation_path));
        Require(result.loaded, "test annotation should load before renaming");
        Require(session.View().navigation.current_annotations.size() == 1, "loaded annotation should be visible");
        Require(session.View().navigation.current_annotations[0].name != "Quality score", "test should start from the default name");
        const std::string default_annotation_name = session.View().navigation.current_annotations[0].name;

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, "  Quality score  "));
        Require(result.action.workflow_changed, "renaming an annotation display name should report workflow change");
        specforge::SourceCollectionSessionView view = session.View();
        Require(
            view.navigation.current_annotations[0].name == "Quality score",
            "annotation row should use the custom display name");
        Require(
            view.filter.available_sources.size() == 1 &&
                view.filter.available_sources[0].name == "Quality score",
            "sample filters should show the custom annotation display name");
        Require(
            HasAvailableSortSource(view.sorting, annotation_source_id),
            "plain integer annotation should be available for sorting");
        Require(
            std::any_of(view.sorting.available_sources.begin(), view.sorting.available_sources.end(), [](const auto& source) {
                return source.name == "Quality score";
            }),
            "sample sorting add-source list should show the custom display name");

        result = Submit(session, AddSampleFilterSource(annotation_source_id));
        view = session.View();
        Require(view.filter.sources.size() == 1, "renamed annotation should still be addable as a filter source");
        Require(view.filter.sources[0].name == "Quality score", "selected sample filter source should keep the custom name");

        result = Submit(session, SetSampleSortSource(annotation_source_id));
        view = session.View();
        const specforge::SourceCollectionSampleSortSourceView* sort_source =
            FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "renamed annotation should still be selectable as a sort source");
        Require(sort_source->name == "Quality score", "selected sample sort source should keep the custom name");

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, "   "));
        Require(result.action.workflow_changed, "clearing an annotation display name should report workflow change");
        view = session.View();
        Require(
            view.navigation.current_annotations[0].name == default_annotation_name,
            "cleared annotation display name should restore the default annotation name");
        Require(
            view.filter.sources.size() == 1 &&
                view.filter.sources[0].name == default_annotation_name,
            "selected sample filter source should restore the default annotation name");
        sort_source = FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "cleared annotation should remain the selected sort source");
        Require(
            sort_source->name == default_annotation_name,
            "selected sample sort source should restore the default annotation name");

        result = Submit(session, RenameAnnotationDisplayName(annotation_path, utf8_display_name));
        view = session.View();
        Require(
            view.navigation.current_annotations[0].name == utf8_display_name,
            "annotation row should support UTF-8 display names after clearing the override");
        Require(
            view.filter.sources.size() == 1 &&
                view.filter.sources[0].name == utf8_display_name,
            "selected sample filter source should keep the UTF-8 annotation display name");
        sort_source = FindSortSource(view.sorting, annotation_source_id);
        Require(sort_source != nullptr, "UTF-8 renamed annotation should remain the selected sort source");
        Require(
            sort_source->name == utf8_display_name,
            "selected sample sort source should keep the UTF-8 annotation display name");

        Require(session.FlushStateCaches(), "session flush should save the custom annotation display name");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = MakeWorkflowPersistentSession(
        restored_loads,
        navigation_cache,
        labeling_cache,
        workflow_cache,
        source_path,
        3);

    (void)Submit(restored, OpenSourceCollection(source_path, 0));
    (void)Submit(restored, AddReadOnlyAnnotation(annotation_path));
    const specforge::SourceCollectionSessionView restored_view = restored.View();
    Require(
        restored_view.navigation.current_annotations[0].name == utf8_display_name,
        "restored workflow should keep the UTF-8 annotation display name");
    Require(
        restored_view.filter.sources.size() == 1 &&
            restored_view.filter.sources[0].name == utf8_display_name,
        "restored selected sample filter source should keep the UTF-8 annotation display name");
    Require(
        std::any_of(
            restored_view.sorting.available_sources.begin(),
            restored_view.sorting.available_sources.end(),
            [&utf8_display_name](const auto& source) { return source.name == utf8_display_name; }),
        "restored available sample sort source should keep the UTF-8 annotation display name");
}

void TestAnnotationSortingSourcesRequireComparablePlainValues()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    const std::filesystem::path label_result_path = UniqueTempPath("_quality.npy");
    TouchFile(source_path);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {2, 1, 1},
        specforge::SampleLabelSet{},
        false);

    specforge::SampleLabelSet label_set;
    label_set.labels.push_back(specforge::SampleLabelDefinition{1, "bad", 'b'});
    label_set.labels.push_back(specforge::SampleLabelDefinition{2, "good", 'g'});
    SaveLabelResultFixture(
        label_result_path,
        "quality",
        "Quality",
        {2, 1, 1},
        label_set,
        true);

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
    Require(result.loaded, "plain integer annotation should load");
    Require(
        !HasSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "plain integer annotation should not enter the visible sort list by default");
    Require(
        HasAvailableSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "plain integer annotation should be available to add as a sort source");

    result = Submit(session, AddReadOnlyAnnotation(label_result_path));
    Require(result.loaded, "metadata-backed label result annotation should load");
    Require(
        !HasSortSource(session.View().sorting, AnnotationSourceId(label_result_path)),
        "label-result integer annotation should not enter the visible sort list");
    Require(
        !HasAvailableSortSource(session.View().sorting, AnnotationSourceId(label_result_path)),
        "label-result integer annotation should not be available to add as a sort source");

    result = Submit(session, SetSampleSortSource(AnnotationSourceId(rank_path)));
    Require(session.View().sorting.active, "plain annotation sort source should be selectable");
    Require(
        HasSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "activating plain annotation sorting should add it to the visible list");
    Require(
        session.View().navigation.current_sequence_position &&
            *session.View().navigation.current_sequence_position == 2,
        "plain annotation sorting should use numeric values with source-order tie break");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    result = Submit(session, SetActiveLabelingOutputPath(rank_path));
    Require(
        !HasSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "annotation should stop being a sort source after it becomes the active local task output");
    Require(
        !HasAvailableSortSource(session.View().sorting, AnnotationSourceId(rank_path)),
        "local task output annotation should not remain addable for sorting");
    Require(!session.View().sorting.active, "invalidated annotation sorting should be cleared");
    Require(!session.View().navigation.sequence_active, "cleared sorting should remove the active sorting sequence");
    Require(session.View().navigation.row_location_available, "cleared sorting should restore ordinary row location");
}

void TestSourceSessionRestoresAnnotationSortingState()
{
    const std::filesystem::path source_session_cache = UniqueTempPath("_sources.json");
    const std::filesystem::path navigation_cache = UniqueTempPath("_navigation.json");
    const std::filesystem::path labeling_cache = UniqueTempPath("_labeling.json");
    const std::filesystem::path workflow_cache = UniqueTempPath("_workflow.json");
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path rank_path = UniqueTempPath("_rank.npy");
    TouchFile(source_path);
    SaveLabelResultFixture(
        rank_path,
        "rank",
        "Rank",
        {3, 1, 2},
        specforge::SampleLabelSet{},
        false);

    {
        std::vector<LoadedSourceSnapshot> loaded_snapshots;
        specforge::SourceCollectionSession session = specforge::SourceCollectionSession(
            [&loaded_snapshots, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
                Require(path == source_path, "session should reload the source fixture");
                loaded_snapshots.push_back(LoadedSourceSnapshot{path, spectrum_index});
                return MakeSnapshot(source_path, 3, spectrum_index);
            },
            source_session_cache,
            navigation_cache,
            labeling_cache,
            workflow_cache);

        (void)Submit(session, OpenSourceCollection(source_path, 0));
        specforge::SourceCollectionSessionResult result = Submit(session, AddReadOnlyAnnotation(rank_path));
        Require(result.loaded, "plain integer annotation should load before selecting ordering");
        result = Submit(session, SetSampleSortSource(AnnotationSourceId(rank_path)));
        Require(session.View().sorting.active, "annotation ordering should be active before saving");
        Require(
            session.View().navigation.current_sequence_position &&
                *session.View().navigation.current_sequence_position == 2,
            "annotation ordering should put row 0 last before saving");
        Require(session.FlushStateCaches(), "session flush should save source and workflow state");
    }

    std::vector<LoadedSourceSnapshot> restored_loads;
    specforge::SourceCollectionSession restored = specforge::SourceCollectionSession(
        [&restored_loads, source_path](const std::filesystem::path& path, std::size_t spectrum_index) {
            Require(path == source_path, "restored session should reload the source fixture");
            restored_loads.push_back(LoadedSourceSnapshot{path, spectrum_index});
            return MakeSnapshot(source_path, 3, spectrum_index);
        },
        source_session_cache,
        navigation_cache,
        labeling_cache,
        workflow_cache);

    const specforge::SourceCollectionSessionView view = restored.View();
    Require(view.sources.size() == 1, "restored session should restore the source entry");
    Require(
        view.navigation.current_annotations.size() == 1 &&
            view.navigation.current_annotations[0].path == rank_path,
        "restored session should restore the annotation used for ordering");
    Require(
        HasSortSource(view.sorting, AnnotationSourceId(rank_path)),
        "restored sorting view should expose the annotation ordering source");
    Require(view.sorting.active, "restored workflow should keep annotation ordering active");
    Require(
        view.sorting.active_source_id == AnnotationSourceId(rank_path),
        "restored workflow should keep the annotation ordering source selected");
    Require(view.navigation.sequence_active, "restored annotation ordering should activate the navigation sequence");
    Require(
        view.navigation.current_sequence_position && *view.navigation.current_sequence_position == 2,
        "restored annotation ordering should put row 0 last");

    const specforge::SourceCollectionSessionResult previous_result =
        Submit(restored, MoveSampleNavigation(specforge::SampleNavigationRequest::Previous()));
    Require(
        previous_result.navigation.target_found && previous_result.navigation.current_index == 2,
        "restored previous navigation should follow the annotation ordering");
}

void TestEmptyFilterSequenceDoesNotLoadFallbackSnapshot()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {2, 2, 2});

    const specforge::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "1", true));
    const specforge::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.filter_active, "zero-match condition should still activate navigation filtering");
    Require(navigation.sequence_active && navigation.sequence_empty, "zero-match condition should expose empty sequence");
    Require(navigation.sequence_count == 0, "empty sequence should expose zero sequence rows");
    Require(!navigation.current_index, "empty sequence should not expose a current index");
    Require(!navigation.current_source_row, "empty sequence should not expose a source row");
    Require(!navigation.current_sequence_position, "empty sequence should not expose a sequence position");
    Require(navigation.current_sample_display_name.empty(), "empty sequence should not display the stale snapshot sample");
    Require(navigation.current_annotations.empty(), "empty sequence should not display stale current annotations");
    Require(
        session.View().snapshot != nullptr,
        "empty sequence should keep the source snapshot available to source-management views");
    Require(
        session.View().current_sample_snapshot == nullptr,
        "empty sequence should suppress the stale snapshot for sample displays");
    Require(!session.View().labeling.current_index, "labeling should not receive a fallback current row");
    Require(!result.action.snapshot_changed, "empty sequence should not load a fallback sample snapshot");
    Require(loaded_indices == std::vector<std::size_t>({0}), "empty sequence should not call LoadActiveSourceAt");
}

void TestDeactivatingLabelingTaskKeepsAnnotationFilter()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    const std::string source_id = AddPlainIntegerSampleFilterSource(session, {1, 2, 2});
    specforge::SourceCollectionSessionResult result =
        Submit(session, SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.filter_active, "annotation sample filter should affect navigation");
    Require(session.View().navigation.filtered_sample_count == 1, "filter should include the one matching sample");

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{1, "bad", 'b'})).changed,
        "label should be accepted");
    result = Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(session.View().labeling.can_deactivate_task, "draft-only active task should be closable");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(result.action.workflow_changed, "deactivating active task should report workflow change");
    Require(!session.View().labeling.has_active_task, "deactivation should leave no active task");
    Require(session.View().filter.sources.size() == 1, "deactivation should keep the annotation sample filter source");
    Require(session.View().filter.sources[0].id == source_id, "deactivation should keep the annotation source id");
    Require(session.View().navigation.filter_active, "deactivation should keep annotation navigation filtering active");

    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "task record should remain available after deactivation");
    Require(session.View().labeling.current_code == 1, "reactivated task should keep its label result");
}

void TestTemporaryLabelingTaskUsesDefaultNameAndResumes()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    specforge::SourceCollectionSessionResult result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "temporary labeling task should become active");
    Require(session.View().labeling.has_temporary_task, "active draft should be exposed as the temporary task");
    Require(session.View().labeling.active_task_is_temporary, "new labeling task should remain temporary");
    Require(
        session.View().labeling.task_name == specforge::kTemporarySampleLabelingTaskName,
        "temporary task should use the fixed default name");
    const std::string temporary_task_id = session.View().labeling.task_id;
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "temporary task should accept labels");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    (void)Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_active_task, "paused temporary task should not remain active");
    Require(session.View().labeling.has_temporary_task, "paused temporary task should remain resumable");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "temporary task should resume");
    Require(session.View().labeling.task_id == temporary_task_id, "resumed task should keep its stable id");
    Require(session.View().labeling.current_code == 3, "resumed task should keep its draft values");
}

void TestLabelingViewAndIntentClearValuesWhenRemovingUsedLabel()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "used", 'u'})).changed,
        "used label should be accepted");
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{4, "unused", 'n'})).changed,
        "unused label should be accepted");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    const specforge::SourceCollectionLabelingView labeling = session.View().labeling;
    Require(labeling.label_usage_counts.at(3) == 1, "view should expose the used label count");
    Require(
        labeling.label_usage_counts.find(4) == labeling.label_usage_counts.end(),
        "view should omit zero-count labels from its usage map");

    specforge::SourceCollectionSessionResult result = Submit(session, RemoveActiveLabel(4));
    Require(result.changed, "unused label removal should flow through the session intent");
    Require(
        specforge::FindSampleLabel(session.View().labeling.label_set, 4) == nullptr,
        "unused label should disappear from the view");

    result = Submit(session, RemoveActiveLabel(3));
    Require(result.changed, "used label removal should flow through the session intent");
    Require(
        specforge::FindSampleLabel(session.View().labeling.label_set, 3) == nullptr,
        "used label definition should disappear from the view");
    Require(
        session.View().labeling.current_code == specforge::kUnlabeledSampleLabelCode,
        "removing a used label should clear the current sample value");
    Require(session.View().labeling.labeled_count == 0, "cleared values should update labeling progress");
}

void TestDiscardingTemporaryLabelingTaskAllowsFreshStart()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{3, "review", 'r'})).changed,
        "temporary task should accept a label before discard");
    (void)Submit(session, AssignActiveLabelToCurrentSample(3));

    const std::string discarded_task_id = session.View().labeling.task_id;
    specforge::SourceCollectionSessionResult result = Submit(session, DeleteActiveLabelingTask());
    Require(result.action.workflow_changed, "delete should report workflow change");
    Require(!session.View().labeling.has_active_task, "delete should clear the active task");
    Require(!session.View().labeling.has_temporary_task, "delete should discard the temporary task record");

    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "starting after discard should create a fresh task");
    Require(session.View().labeling.task_id == discarded_task_id, "fresh temporary task may reuse the available id");
    Require(
        session.View().labeling.current_code == specforge::kUnlabeledSampleLabelCode,
        "deleted draft values should not come back");
}

void TestSavingTemporaryTaskCreatesNamedAnnotationAndAllowsFreshTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_quality.npy");
    const std::string saved_name = Utf8(output_path.stem().u8string());
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));

    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.active_task_is_temporary, "task should start as a local temporary draft");
    specforge::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(!session.View().labeling.has_temporary_task, "choosing output should formalize the temporary task");
    Require(!session.View().labeling.active_task_is_temporary, "saved task should become a formal annotation");
    Require(session.View().labeling.task_name == saved_name, "saved annotation name should derive from its filename");
    Require(session.View().navigation.current_annotations.size() == 1, "local task output should appear in annotations");
    Require(
        session.View().navigation.current_annotations[0].name == saved_name,
        "local labeling annotation should default to the saved filename");
    Require(
        session.View().filter.available_sources.size() == 1 &&
            session.View().filter.available_sources[0].name == saved_name,
        "local labeling filter source should default to the saved filename");

    result = Submit(session, RenameAnnotationDisplayName(output_path, "Hard cases"));
    Require(session.View().labeling.task_name == saved_name, "display-name customization should not rename metadata");
    Require(
        session.View().navigation.current_annotations[0].name == "Hard cases",
        "annotation row should use the custom local-task display name");
    Require(
        session.View().filter.available_sources[0].name == "Hard cases",
        "local labeling filter source should use the custom annotation display name");

    result = Submit(session, RenameAnnotationDisplayName(output_path, "   "));
    Require(result.action.workflow_changed, "clearing local-task annotation display name should report workflow change");
    Require(
        session.View().navigation.current_annotations[0].name == saved_name,
        "cleared local-task annotation display name should restore the saved filename");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_temporary_task, "closing a formal annotation should not create a draft");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.active_task_is_temporary, "a fresh temporary task should start after formal save");
    Require(
        session.View().navigation.current_annotations.size() == 1 &&
            session.View().navigation.current_annotations[0].name == saved_name,
        "starting a new temporary task should keep the formal annotation available");

    const std::string fresh_temporary_task_id = session.View().labeling.task_id;
    result = Submit(session, ActivateLabelingTaskFromAnnotation(output_path));
    Require(
        !session.View().labeling.active_task_is_temporary && session.View().labeling.output_path == output_path,
        "selecting a labeling annotation should safely switch away from the active draft");
    Require(session.View().labeling.has_temporary_task, "switching annotations should retain the paused draft");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        session.View().labeling.active_task_is_temporary &&
            session.View().labeling.task_id == fresh_temporary_task_id,
        "the persistent draft option should safely switch back from a formal annotation");
}

void TestFailedFirstOutputSaveKeepsRecoverableTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path blocked_output_path = UniqueTempPath("_blocked_output");
    const std::filesystem::path replacement_output_path = UniqueTempPath("_replacement.npy");
    std::filesystem::create_directories(blocked_output_path);

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    const std::string temporary_task_id = session.View().labeling.task_id;

    specforge::SourceCollectionSessionResult result =
        Submit(session, SetActiveLabelingOutputPath(blocked_output_path));
    Require(session.View().labeling.has_active_task, "failed first save should keep the draft active");
    Require(session.View().labeling.has_temporary_task, "failed first save should keep a resumable draft");
    Require(session.View().labeling.active_task_is_temporary, "failed first save must not formalize the task");
    Require(!session.View().labeling.output_path, "failed first save must not retain the rejected output target");
    Require(
        session.View().labeling.task_name == specforge::kTemporarySampleLabelingTaskName,
        "failed first save should retain the temporary task name");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "failed first save should surface the write failure");
    Require(session.View().labeling.can_deactivate_task, "failed first save should still allow pausing the draft");
    Require(session.View().labeling.can_delete_task, "failed first save should still allow deleting the draft");

    result = Submit(session, DeactivateActiveLabelingTask());
    Require(!session.View().labeling.has_active_task, "failed first-save draft should be pausable");
    Require(session.View().labeling.has_temporary_task, "paused failed draft should remain resumable");
    result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        session.View().labeling.task_id == temporary_task_id,
        "resuming after failed first save should preserve the draft identity");

    result = Submit(session, SetActiveLabelingOutputPath(replacement_output_path));
    Require(!session.View().labeling.active_task_is_temporary, "replacement output should formalize the draft");
    Require(
        session.View().labeling.output_path == replacement_output_path,
        "replacement output should become the formal task target");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "replacement output should save successfully");
}

void TestFailedFirstMetadataSaveKeepsRecoverableTemporaryTask()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path output_path = UniqueTempPath("_metadata_blocked.npy");
    const std::filesystem::path replacement_output_path = UniqueTempPath("_metadata_replacement.npy");
    std::filesystem::create_directories(specforge::SampleLabelResultMetadataPathForResult(output_path));

    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);
    (void)Submit(session, OpenSourceCollection(source_path, 0));
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());

    specforge::SourceCollectionSessionResult result = Submit(session, SetActiveLabelingOutputPath(output_path));
    Require(std::filesystem::exists(output_path), "metadata failure fixture should still write the label array");
    Require(session.View().labeling.has_temporary_task, "failed first metadata save should retain the draft");
    Require(
        session.View().labeling.active_task_is_temporary,
        "failed first metadata save must not formalize the task");
    Require(!session.View().labeling.output_path, "failed first metadata save must not bind the partial output");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "failed first metadata save should surface the sidecar failure");
    Require(
        session.View().labeling.can_deactivate_task && session.View().labeling.can_delete_task,
        "failed first metadata save should leave the draft recoverable");

    result = Submit(session, SetActiveLabelingOutputPath(replacement_output_path));
    Require(!session.View().labeling.active_task_is_temporary, "replacement target should formalize the draft");
    Require(
        session.View().labeling.output_path == replacement_output_path,
        "replacement target should replace the rejected partial output");
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
    Require(session.View().navigation.current_annotations.size() == 1, "loaded annotation should appear in navigation");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "metadata-backed annotation should start as external");
    Require(
        session.View().navigation.current_annotations[0].can_activate_labeling,
        "categorical annotation should be draggable into labeling");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "annotation activation should create an active task");
    Require(session.View().labeling.task_name == "Quality review", "annotation metadata task name should be reused");
    Require(session.View().labeling.current_code == 5, "labeling task should reuse annotation values");
    Require(session.View().labeling.output_path && *session.View().labeling.output_path == annotation_path, "task should bind output path");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "metadata-backed annotation activation should be clean");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "activated annotation should be shown as a local labeling task");

    result = Submit(session, RemoveReadOnlyAnnotation(annotation_path));
    Require(!result.action.workflow_changed, "removing an annotation should not delete the local task");
    Require(session.View().navigation.current_annotations.size() == 1, "local task should remain visible after annotation removal");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "remaining row should come from the local task record");
    Require(session.View().labeling.has_active_task, "removing an annotation should not remove the active local task");
}

void TestAnnotationActivationRequiresCurrentTaskToBeClosed()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    const std::filesystem::path annotation_path = UniqueTempPath("_blocked_activation.npy");
    const std::filesystem::path formal_output_path = UniqueTempPath("_formal_output.npy");

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
    specforge::SourceCollectionSessionResult result = Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(session.View().labeling.has_active_task, "current task should be active before activation attempt");
    result = Submit(session, SetActiveLabelingOutputPath(formal_output_path));
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "current task should be formal before its later autosave failure");
    const std::string current_task_id = session.View().labeling.task_id;
    std::filesystem::remove(formal_output_path);
    std::filesystem::create_directories(formal_output_path);
    result = Submit(session, UpsertActiveLabel(specforge::SampleLabelDefinition{7, "review", 'r'}));
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "formal task should have a failed autosave guard");

    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(result.loaded, "external annotation should load before blocked activation");
    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "blocked activation should keep the active task");
    Require(session.View().labeling.task_id == current_task_id, "annotation activation must not switch active tasks");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::Failed,
        "blocked activation should preserve the failed save state");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
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
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::PlainAnnotation,
        "annotation without metadata should start as plain");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(session.View().labeling.has_active_task, "plain annotation activation should create an active task");
    Require(session.View().labeling.current_code == 7, "plain annotation values should become editable label values");
    Require(session.View().labeling.label_set.labels.size() == 2, "plain annotation unique values should become labels");
    Require(session.View().labeling.label_set.labels[0].code == 5, "plain annotation labels should include value 5");
    Require(session.View().labeling.label_set.labels[1].code == 7, "plain annotation labels should include value 7");
    Require(
        session.View().labeling.save_state.kind == specforge::SampleLabelSaveStateKind::AutosavedToOutput,
        "plain annotation activation should write its new metadata sidecar");
    Require(
        std::filesystem::exists(specforge::SampleLabelResultMetadataPathForResult(annotation_path)),
        "plain annotation activation should create metadata sidecar");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
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
    Require(session.View().labeling.has_active_task, "plain annotation should become a local task");
    Require(
        std::filesystem::exists(specforge::SampleLabelResultMetadataPathForResult(annotation_path)),
        "test should start with a converted metadata sidecar");

    std::filesystem::remove(specforge::SampleLabelResultMetadataPathForResult(annotation_path));
    result = Submit(session, AddReadOnlyAnnotation(annotation_path));
    Require(
        session.View().navigation.current_annotations.size() == 1,
        "same loaded output path should render as one annotation row");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::LocalLabelingTask,
        "loaded output path owned by a local task should stay local when metadata is missing");
    Require(
        session.View().navigation.current_annotations[0].metadata_missing,
        "missing local metadata sidecar should be surfaced in the annotation row");
    Require(
        session.View().navigation.current_annotations[0].display_text == "7 (7)",
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
    Require(!session.View().labeling.has_active_task, "mismatch fixture should start without an active task");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
            specforge::SampleAnnotationWorkflowRelationship::ExternalLabelResult,
        "same path and sample count should not make a local match without matching sidecar task id");

    result = Submit(session, ActivateLabelingTaskFromAnnotation(annotation_path));
    Require(
        !session.View().labeling.has_active_task,
        "activation must not bind an inactive same-path task when the sidecar task id differs");
    Require(
        session.View().navigation.current_annotations[0].relationship ==
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
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{1, "bad", 'b'}))
            .changed,
        "first source collection should accept a label definition");
    specforge::SourceCollectionSessionResult result =
        Submit(session, AssignActiveLabelToCurrentSample(1));
    Require(session.View().labeling.current_code == 1, "first sample should be labeled before filtering");
    const std::filesystem::path annotation_path =
        AddPlainIntegerFilterAnnotation(session, {1, 2, 2}, "_first_filter.npy");
    const std::string source_id = AnnotationSourceId(annotation_path);
    result = Submit(session, AddSampleFilterSource(source_id));
    Require(session.View().filter.sources.size() == 1, "annotation sample filter should be explicitly selected");
    result = Submit(
        session,
        SetFilterValueSelected(source_id, "1", true));
    Require(session.View().navigation.filter_active, "annotation sample filter should affect sample navigation");
    Require(session.View().navigation.filtered_sample_count == 1, "filter should include the one matching sample");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(result.action.workflow_changed, "opening another source collection should change the active workflow");
    Require(!session.View().labeling.has_active_task, "second source collection should not inherit the first workflow");
    Require(session.View().filter.sources.empty(), "source switch should clear selected filter sources");
    Require(!session.View().navigation.filter_active, "source switch should clear navigation filtering");

    result = Submit(session, SwitchSourceCollection(0));
    Require(result.action.workflow_changed, "switching back should reactivate the first workflow");
    Require(session.View().labeling.has_active_task, "first source collection workflow should be restored");
    Require(session.View().labeling.current_code == 1, "first source collection label result should be restored");
    Require(session.View().filter.sources.empty(), "workflow restore should not restore old filter sources");
    Require(session.View().filter.available_sources.size() == 1, "restored annotation should be available to add again");
    Require(!session.View().navigation.filter_active, "workflow restore should leave sample filtering cleared");
}

void TestNavigationViewSeparatesSampleNameFromDisplayName()
{
    const std::filesystem::path source_path = UniqueTempPath(".npy");
    std::vector<std::size_t> loaded_indices;
    specforge::SourceCollectionSession session = MakeSession(loaded_indices, source_path, 3);

    specforge::SourceCollectionSessionResult result =
        Submit(session, OpenSourceCollection(source_path, 0));
    specforge::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.current_sample_display_name == "sample-1", "view should expose the current display name");
    Require(navigation.current_sample_name.empty(), "unnamed samples should not expose a searchable sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_display_name));
    navigation = session.View().navigation;
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
    specforge::SourceCollectionNavigationView navigation = session.View().navigation;
    Require(navigation.has_sample_names, "folder source should expose source-provided sample names");
    Require(navigation.current_sample_name == "alpha.csv", "view should expose the current source-provided sample name");
    Require(navigation.current_sample_display_name == "sample-1", "display text should remain separate from sample name");

    result = Submit(
        session,
        SetSampleNameQuery(navigation.current_sample_name));
    navigation = session.View().navigation;
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
    Require(session.View().current_source_index && *session.View().current_source_index == 0, "first source should be active");
    Require(session.View().snapshot->source.path == first_source_path, "first source snapshot should be visible");
    Require(session.View().snapshot->collection.current_index == 1, "first source should open at requested row");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
    Require(
        Submit(
            session,
            UpsertActiveLabel(
                specforge::SampleLabelDefinition{10, "first-label", 'f'}))
            .changed,
        "first source should accept its own active label");

    result = Submit(session, OpenSourceCollection(second_source_path, 0));
    Require(session.View().current_source_index && *session.View().current_source_index == 1, "second source should be active");
    Require(session.View().snapshot->source.path == second_source_path, "second source snapshot should be visible");
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());
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
        session.View().current_source_index && *session.View().current_source_index == 0,
        "first source should be active again");
    Require(session.View().snapshot->source.path == first_source_path, "reactivated snapshot should be the first source");
    Require(session.View().snapshot->collection.current_index == 1, "reactivated first source should keep its cached row");
    Require(session.View().labeling.has_active_task, "first source labeling task should be restored");
    Require(session.View().labeling.label_set.labels.size() == 1, "first source should expose its own label set");
    Require(session.View().labeling.label_set.labels[0].code == 10, "first source label set should not come from second source");

    const std::size_t loaded_count_before_remove = loaded_snapshots.size();
    result = Submit(session, RemoveSourceCollection(0));
    Require(result.action.snapshot_changed, "removing active first source should activate the next source snapshot");
    Require(result.action.workflow_changed, "removing active first source should resync the workflow");
    Require(result.action.navigation_inputs_changed, "removing active first source should refresh navigation inputs");
    Require(session.View().sources.size() == 1, "removing first source should leave one source");
    Require(session.View().sources[0].path == second_source_path, "remaining source should be the second source");
    Require(
        session.View().current_source_index && *session.View().current_source_index == 0,
        "second source should become index 0");
    Require(session.View().snapshot->source.path == second_source_path, "second source snapshot should be visible after removal");
    Require(session.View().snapshot->collection.current_index == 0, "second source cached row should be preserved");
    Require(session.View().labeling.has_active_task, "second source labeling task should be restored after removal");
    Require(session.View().labeling.label_set.labels.size() == 1, "second source should expose its own label set");
    Require(session.View().labeling.label_set.labels[0].code == 20, "second source label set should survive first removal");
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
        (void)Submit(session, MoveSampleNavigation(specforge::SampleNavigationRequest::Next()));
        Require(session.View().snapshot->source.path == first_source_path, "first source should be active");
        Require(session.View().snapshot->collection.current_index == 2, "first source should reach row 2");
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

    (void)Submit(restored, SwitchSourceCollection(1));
    Require(
        restored.View().navigation.current_annotations.size() == 1,
        "restored second source should restore its own annotations");
    Require(
        restored.View().navigation.current_annotations[0].path == second_annotation_path,
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

void TestSourceSessionStateCacheStoresPackageRelativePaths()
{
    if (specforge::BuildReleaseProfile() != specforge::ReleaseProfile::Portable) {
        return;
    }

    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    const std::filesystem::path package_test_root =
        runtime_paths.package_root / "package-relative-source-session-test";
    const std::filesystem::path source_path = package_test_root / "sources" / "source.npy";
    const std::filesystem::path annotation_path = package_test_root / "annotations" / "labels.npy";
    const std::filesystem::path source_session_cache = UniqueTempPath("_package_relative_sources.json");
    std::error_code cleanup_error;
    std::filesystem::remove_all(package_test_root, cleanup_error);
    std::filesystem::create_directories(source_path.parent_path());
    std::filesystem::create_directories(annotation_path.parent_path());
    TouchFile(source_path);
    TouchFile(annotation_path);

    specforge::SourceCollectionSessionStateCache cache;
    specforge::SourceCollectionSavedSource source{source_path, 7};
    source.annotation_paths.push_back(annotation_path);
    cache.sources = {source};
    cache.active_source_index = 0;
    Require(
        specforge::SaveSourceCollectionSessionStateCache(source_session_cache, cache),
        "package-relative source session cache should save");

    const std::string cache_text = ReadTextFile(source_session_cache);
    Require(
        cache_text.find("\"path_kind\": \"package_relative\"") != std::string::npos,
        "package-contained paths should be written as package-relative references");
    Require(
        cache_text.find("C:") == std::string::npos,
        "package-relative source session cache should not store a Windows absolute path");

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(loaded.sources.size() == 1, "package-relative source should load");
    Require(loaded.sources[0].path == source_path, "package-relative source should resolve under package root");
    Require(loaded.sources[0].annotation_paths.size() == 1, "package-relative annotation should load");
    Require(
        loaded.sources[0].annotation_paths[0] == annotation_path,
        "package-relative annotation should resolve under package root");

    std::filesystem::remove(source_session_cache, cleanup_error);
    std::filesystem::remove_all(package_test_root, cleanup_error);
}

void TestSourceSessionStateCacheRebasesLegacyMovedPortablePath()
{
    if (specforge::BuildReleaseProfile() != specforge::ReleaseProfile::Portable) {
        return;
    }

    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    const std::filesystem::path package_test_root =
        runtime_paths.package_root / "legacy-rebase-source-session-test";
    const std::filesystem::path source_path = package_test_root / "sources" / "source.npy";
    const std::filesystem::path source_session_cache = UniqueTempPath("_legacy_rebase_sources.json");
    const std::filesystem::path old_package_parent =
        std::filesystem::temp_directory_path() / "specforge_old_portable_parent";
    const std::filesystem::path old_package_root = old_package_parent / runtime_paths.package_root.filename();
    const std::filesystem::path old_source_path =
        old_package_root / "legacy-rebase-source-session-test" / "sources" / "source.npy";

    std::error_code cleanup_error;
    std::filesystem::remove_all(package_test_root, cleanup_error);
    std::filesystem::remove_all(old_package_parent, cleanup_error);
    std::filesystem::create_directories(source_path.parent_path());
    TouchFile(source_path);

    std::ofstream stream(source_session_cache, std::ios::trunc);
    Require(stream.good(), "could not open legacy source-session cache fixture");
    stream << "{\n"
           << "  \"format_kind\": \"specforge.source_collection_session.cache\",\n"
           << "  \"schema_version\": 1,\n"
           << "  \"active_source_index\": 0,\n"
           << "  \"sources\": [\n"
           << "    { \"path\": ";
    specforge::WriteJsonString(stream, PathToUtf8(old_source_path));
    stream << ", \"last_index\": 5 }\n"
           << "  ]\n"
           << "}\n";
    Require(stream.good(), "could not write legacy source-session cache fixture");
    stream.close();

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(source_session_cache);
    Require(loaded.sources.size() == 1, "legacy moved portable source should load");
    Require(
        loaded.sources[0].path == source_path,
        "legacy moved portable source should rebase under the current package root");
    Require(loaded.sources[0].last_spectrum_index == 5, "legacy moved portable source should keep the row index");

    std::filesystem::remove(source_session_cache, cleanup_error);
    std::filesystem::remove_all(package_test_root, cleanup_error);
    std::filesystem::remove_all(old_package_parent, cleanup_error);
}

void TestSampleWorkflowStateCacheStoresPackageRelativeAnnotationSourceIds()
{
    if (specforge::BuildReleaseProfile() != specforge::ReleaseProfile::Portable) {
        return;
    }

    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    const std::filesystem::path package_test_root =
        runtime_paths.package_root / "package-relative-workflow-state-test";
    const std::filesystem::path annotation_path = package_test_root / "annotations" / "quality.npy";
    const std::filesystem::path workflow_cache = UniqueTempPath("_package_relative_workflow.json");
    const std::string annotation_source_id = AnnotationSourceId(annotation_path);

    std::error_code cleanup_error;
    std::filesystem::remove(workflow_cache, cleanup_error);
    std::filesystem::remove_all(package_test_root, cleanup_error);
    std::filesystem::create_directories(annotation_path.parent_path());
    TouchFile(annotation_path);

    specforge::SampleWorkflowSourceState state;
    state.selected_filter_source_ids.push_back(annotation_source_id);
    specforge::SampleFilterCondition condition;
    condition.source_id = annotation_source_id;
    condition.allowed_value_keys.insert("good");
    state.filter_conditions.push_back(std::move(condition));
    state.selected_sample_sort_source_ids.push_back(annotation_source_id);
    state.selected_sample_sort_source_id = annotation_source_id;
    state.selected_sample_sort_direction = specforge::SampleNavigationSortDirection::Descending;
    state.annotation_display_names.push_back(
        specforge::SampleAnnotationDisplayNameOverride{annotation_source_id, "Quality"});

    specforge::SampleWorkflowStateCache cache;
    cache.sources_by_identity.emplace("source-identity", std::move(state));
    Require(
        specforge::SaveSampleWorkflowStateCache(workflow_cache, cache),
        "package-relative workflow cache should save");

    const std::string cache_text = ReadTextFile(workflow_cache);
    Require(
        cache_text.find("\"source_kind\": \"annotation_path\"") != std::string::npos,
        "annotation source ids should be stored as structured path references");
    Require(
        cache_text.find("\"path_kind\": \"package_relative\"") != std::string::npos,
        "package-contained annotation source ids should use package-relative paths");
    Require(
        cache_text.find(PathToUtf8(runtime_paths.package_root)) == std::string::npos,
        "package-relative workflow cache should not store the package root");

    const specforge::SampleWorkflowStateCache loaded =
        specforge::LoadSampleWorkflowStateCache(workflow_cache);
    const auto source = loaded.sources_by_identity.find("source-identity");
    Require(source != loaded.sources_by_identity.end(), "workflow source state should load");
    Require(
        source->second.selected_filter_source_ids == std::vector<std::string>{annotation_source_id},
        "selected filter source id should restore");
    Require(source->second.filter_conditions.size() == 1, "filter condition should restore");
    Require(
        source->second.filter_conditions[0].source_id == annotation_source_id,
        "filter condition source id should restore");
    Require(
        source->second.selected_sample_sort_source_ids == std::vector<std::string>{annotation_source_id},
        "selected sort source id should restore");
    Require(
        source->second.selected_sample_sort_source_id &&
            *source->second.selected_sample_sort_source_id == annotation_source_id,
        "active sort source id should restore");
    Require(
        source->second.annotation_display_names.size() == 1 &&
            source->second.annotation_display_names[0].source_id == annotation_source_id,
        "annotation display name source id should restore");

    std::filesystem::remove(workflow_cache, cleanup_error);
    std::filesystem::remove_all(package_test_root, cleanup_error);
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
    (void)Submit(session, StartOrResumeTemporaryLabelingTask());

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
        (void)Submit(reloaded, OpenSourceCollection(source_path, 0));
        Require(
            reloaded.View().labeling.has_active_task,
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

void RunAllTests()
{
    TestNavigationReloadsSnapshotAndRemembersLabelingPosition();
    TestAssigningLabelAutoAdvancesInsideSession();
    TestAnnotationFilterSelectionAppliesToNavigation();
    TestLocalLabelingAnnotationCanBeSampleFilterSource();
    TestRemovingLabelSelectedBySampleFilterReloadsReconciledSnapshot();
    TestRemovingLabelPrunesItsSampleFilterValue();
    TestChangingUsedLabelCodeMigratesValuesAndSampleFilter();
    TestLabelingViewCodeConflictIncludesUndefinedSampleValues();
    TestResumeLocateRespectsActiveFilterSequence();
    TestSourceOrderNavigationViewDoesNotMaterializeSequenceRows();
    TestRememberedPositionResumableTracksActiveSequence();
    TestSampleSortingIntentAppliesNavigationSequence();
    TestSampleSortingSourcesRequireExplicitAddition();
    TestSampleWorkflowStateRestoresFiltersAndSorting();
    TestAnnotationDisplayNameCustomizesWorkflowSurfacesAndPersists();
    TestAnnotationSortingSourcesRequireComparablePlainValues();
    TestSourceSessionRestoresAnnotationSortingState();
    TestEmptyFilterSequenceDoesNotLoadFallbackSnapshot();
    TestDeactivatingLabelingTaskKeepsAnnotationFilter();
    TestTemporaryLabelingTaskUsesDefaultNameAndResumes();
    TestLabelingViewAndIntentClearValuesWhenRemovingUsedLabel();
    TestDiscardingTemporaryLabelingTaskAllowsFreshStart();
    TestSavingTemporaryTaskCreatesNamedAnnotationAndAllowsFreshTemporaryTask();
    TestFailedFirstOutputSaveKeepsRecoverableTemporaryTask();
    TestFailedFirstMetadataSaveKeepsRecoverableTemporaryTask();
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
    TestSourceSessionStateCacheStoresPackageRelativePaths();
    TestSourceSessionStateCacheRebasesLegacyMovedPortablePath();
    TestSampleWorkflowStateCacheStoresPackageRelativeAnnotationSourceIds();
    TestSourceSessionStateCacheIgnoresCorruptJson();
    TestSourceSessionStateCacheIgnoresUnsupportedSchema();
    TestSourceSessionSkipsMissingSourcePathsOnRestore();
    TestSourceSessionRestoresAtMostThirtyTwoSources();
    TestSourceSessionFlushFailureKeepsDirtyState();
}

int main()
{
    try {
        RunAllTests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
