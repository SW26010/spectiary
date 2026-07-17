#include "ui/sample_workflow_coordinator.h"

#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kWorkflowStateSaveDebounce = 500ms;
constexpr auto kWorkflowStateSaveRetry = 2s;

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool PathExists(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool ShouldRememberLabelingPosition(SampleNavigationRequestKind kind)
{
    return kind == SampleNavigationRequestKind::Previous || kind == SampleNavigationRequestKind::Next ||
           kind == SampleNavigationRequestKind::LabelAdvance;
}

SampleNavigationRequest BuildAutoAdvanceRequest(const SampleLabelingTask& task)
{
    if (!task.skip_labeled_on_advance) {
        return SampleNavigationRequest::LabelAdvance();
    }

    std::vector<bool> eligible_samples;
    eligible_samples.reserve(task.values.size());
    for (int value : task.values) {
        eligible_samples.push_back(value == kUnlabeledSampleLabelCode);
    }
    return SampleNavigationRequest::LabelAdvanceToEligible(std::move(eligible_samples));
}

std::optional<SampleLabelResultMetadata> LoadVerifiedLabelMetadataForAnnotation(
    const SampleAnnotationResult& annotation)
{
    if (annotation.label_metadata) {
        return annotation.label_metadata;
    }
    if (annotation.kind != SampleAnnotationKind::CategoricalInteger || annotation.path.empty()) {
        return std::nullopt;
    }

    SampleLabelResultMetadataLoadResult metadata =
        LoadSampleLabelResultMetadataForResult(annotation.path, annotation.values.size(), annotation.dtype_name);
    if (!metadata.warning.empty() || !metadata.metadata) {
        return std::nullopt;
    }
    return std::move(metadata.metadata);
}

SourceCollectionAnnotationValueView BuildAnnotationValueView(
    const SampleAnnotationResult& annotation,
    std::size_t current_index,
    const SampleLabelingTask* local_task,
    std::string display_name)
{
    SourceCollectionAnnotationValueView view;
    view.name = std::move(display_name);
    view.path = local_task != nullptr && local_task->output_path ? *local_task->output_path : annotation.path;
    view.relationship = local_task == nullptr
        ? annotation.relationship
        : SampleAnnotationWorkflowRelationship::LocalLabelingTask;
    view.relationship_label = std::string{SampleAnnotationWorkflowRelationshipLabel(view.relationship)};
    view.message = annotation.metadata_warning;
    if (local_task != nullptr && current_index < local_task->values.size()) {
        view.display_text = FormatSampleLabelValue(local_task->label_set, local_task->values[current_index]);
    } else if (current_index < annotation.values.size()) {
        view.display_text = annotation.values[current_index].display_text;
    } else {
        view.missing = true;
    }
    view.can_activate_labeling = local_task != nullptr || annotation.kind == SampleAnnotationKind::CategoricalInteger;
    view.can_filter_samples = local_task != nullptr || annotation.kind != SampleAnnotationKind::ContinuousFloat;
    view.can_sort_samples = local_task == nullptr &&
                            annotation.relationship == SampleAnnotationWorkflowRelationship::PlainAnnotation &&
                            !annotation.label_metadata;
    view.can_rename_annotation = !view.path.empty();
    view.can_remove_annotation = local_task == nullptr;
    if (local_task != nullptr && local_task->output_path) {
        view.output_missing = !PathExists(*local_task->output_path);
        view.metadata_missing = !PathExists(SampleLabelResultMetadataPathForResult(*local_task->output_path));
    }
    return view;
}

SourceCollectionAnnotationValueView BuildLocalTaskAnnotationValueView(
    const SampleLabelingTask& task,
    std::size_t current_index,
    std::string display_name)
{
    SourceCollectionAnnotationValueView view;
    view.name = std::move(display_name);
    view.path = task.output_path.value_or(std::filesystem::path{});
    view.relationship = SampleAnnotationWorkflowRelationship::LocalLabelingTask;
    view.relationship_label = std::string{SampleAnnotationWorkflowRelationshipLabel(view.relationship)};
    if (current_index < task.values.size()) {
        view.display_text = FormatSampleLabelValue(task.label_set, task.values[current_index]);
    } else {
        view.missing = true;
    }
    view.can_activate_labeling = task.output_path.has_value();
    view.can_filter_samples = task.output_path.has_value();
    view.can_sort_samples = false;
    view.can_rename_annotation = task.output_path.has_value();
    view.can_remove_annotation = false;
    if (task.output_path) {
        view.output_missing = !PathExists(*task.output_path);
        view.metadata_missing = !PathExists(SampleLabelResultMetadataPathForResult(*task.output_path));
    }
    return view;
}

}  // namespace

SampleWorkflowCoordinator::SampleWorkflowCoordinator()
    : workflow_state_cache_path_(DefaultSampleWorkflowStateCachePath()),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      workflow_state_cache_path_(std::move(workflow_state_cache_path)),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SyncActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionSessionAction action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return action;
    }

    const SourceCollectionIdentity identity = BuildSourceCollectionIdentity(*snapshot);
    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint;

    navigation_.ActivateSource(std::move(*source_key), snapshot);
    SyncSampleWorkflowSession(snapshot, action);
    if (workflow_identity_changed || workflow_context_changed) {
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ClearActiveWorkflow()
{
    SourceCollectionSessionAction action;
    ClearSampleWorkflow(action);
    return action;
}

void SampleWorkflowCoordinator::BeginRestoringSourceSession()
{
    restoring_source_session_ = true;
}

void SampleWorkflowCoordinator::EndRestoringSourceSession()
{
    restoring_source_session_ = false;
}

void SampleWorkflowCoordinator::RemoveSource(std::string_view source_key)
{
    navigation_.RemoveSource(source_key);
    workflow_sources_.InvalidateFilterViewCache();
    workflow_sources_.InvalidateSortingSourceCache();
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    const SpectrumSnapshotHandle& snapshot)
{
    SampleWorkflowCommandResult result;
    result.navigation = navigation_.Navigate(request);
    if (result.navigation.has_active_source && result.navigation.target_found &&
        ShouldRememberLabelingPosition(request.kind)) {
        (void)labeling_.RememberActivePosition(result.navigation.current_index);
    }
    if (result.navigation.has_active_source && result.navigation.target_found) {
        if (!snapshot || snapshot->collection.current_index != result.navigation.current_index) {
            result.snapshot_index_to_load = result.navigation.current_index;
        } else {
            result.action.navigation_inputs_changed = true;
        }
    }
    return result;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::AddReadOnlyAnnotationToActiveSource(
    const std::filesystem::path& path,
    bool* loaded,
    std::string* message)
{
    SourceCollectionSessionAction action;
    const bool annotation_loaded = navigation_.AddReadOnlyAnnotationToActiveSource(path, message);
    if (loaded != nullptr) {
        *loaded = annotation_loaded;
    }
    if (annotation_loaded) {
        workflow_sources_.InvalidateSortingSourceCache();
        workflow_sources_.InvalidateFilterViewCache();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveReadOnlyAnnotationFromActiveSource(
    const std::filesystem::path& path)
{
    SourceCollectionSessionAction action;
    std::optional<std::string> removed_source_id;
    if (const SourceCollectionManifest* context = navigation_.active_context()) {
        if (const SampleAnnotationResult* annotation = FindSampleWorkflowAnnotationByPath(*context, path)) {
            removed_source_id = BuildAnnotationFilterSourceId(*annotation);
        }
    }
    const bool removed_annotation = navigation_.RemoveReadOnlyAnnotationFromActiveSource(path);
    if (!removed_annotation) {
        return action;
    }

    workflow_sources_.InvalidateSortingSourceCache();
    if (removed_source_id) {
        (void)workflow_sources_.RemoveFilterSource(*removed_source_id);
        (void)workflow_sources_.RemoveSampleSortSource(*removed_source_id);
    }
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RenameAnnotationDisplayNameForActiveSource(
    std::filesystem::path path,
    std::string display_name)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.RenameAnnotationDisplayName(
            SourcePolicyContext(nullptr),
            path,
            std::move(display_name))) {
        return action;
    }
    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

bool SampleWorkflowCoordinator::RestoreReadOnlyAnnotationsForActiveSource(
    const std::vector<std::filesystem::path>& paths)
{
    const bool restored = navigation_.RestoreReadOnlyAnnotationsForActiveSource(paths);
    if (restored) {
        workflow_sources_.InvalidateSortingSourceCache();
        workflow_sources_.InvalidateFilterViewCache();
        (void)ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true});
    }
    return restored;
}

std::unordered_map<std::string, std::vector<std::filesystem::path>>
SampleWorkflowCoordinator::AnnotationPathsBySourceKey() const
{
    return navigation_.AnnotationPathsBySourceKey();
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetSampleNameQuery(std::string query)
{
    navigation_.SetSampleNameQuery(std::move(query));
    return {};
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name,
    const SpectrumSnapshotHandle& snapshot)
{
    navigation_.SetSampleNameQuery(std::move(matched_name));
    std::string query{navigation_.sample_name_query()};
    return RequestSampleNavigation(
        SampleNavigationRequest::LocateSampleNameMatch(target_row, std::move(query)),
        snapshot);
}

SourceCollectionSessionAction SampleWorkflowCoordinator::CreateDefaultLabelingTask()
{
    SourceCollectionSessionAction action;
    if (labeling_.CreateTask("manual-labeling", "Manual labeling") != nullptr) {
        workflow_sources_.InvalidateSortingSourceCache();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::CreateLabelingTask(std::string task_name)
{
    SourceCollectionSessionAction action;
    task_name = DefaultedSampleLabelingTaskName(std::move(task_name));
    const std::string task_id =
        TaskIdForCreatedSampleLabelingTask(task_name, labeling_.active_source_tasks());
    if (labeling_.CreateTask(task_id, std::move(task_name)) != nullptr) {
        workflow_sources_.InvalidateSortingSourceCache();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{
                    .workflow_changed = true,
                    .filters_changed = true,
                    .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ActivateLabelingTaskFromAnnotation(
    std::filesystem::path annotation_path)
{
    SourceCollectionSessionAction action;
    const SourceCollectionManifest* context = navigation_.active_context();
    if (context == nullptr || annotation_path.empty()) {
        return action;
    }

    const SampleAnnotationResult* annotation = FindSampleWorkflowAnnotationByPath(*context, annotation_path);
    if (annotation == nullptr) {
        return action;
    }

    const std::optional<SampleLabelResultMetadata> metadata =
        LoadVerifiedLabelMetadataForAnnotation(*annotation);
    const SampleLabelingTask* active_task = labeling_.active_task();
    SampleAnnotationLabelingActivationPlan plan = PlanSampleAnnotationLabelingActivation(
        SampleAnnotationLabelingActivationRequest{
            .annotation = annotation,
            .active_task = active_task,
            .active_source_tasks = labeling_.active_source_tasks(),
            .metadata = metadata ? &*metadata : nullptr});
    if (plan.kind == SampleAnnotationLabelingActivationKind::None) {
        return action;
    }

    if (plan.kind == SampleAnnotationLabelingActivationKind::ActivateExistingTask) {
        if (labeling_.ActivateTask(plan.task_id)) {
            workflow_sources_.InvalidateSortingSourceCache();
            ApplyNavigationInputEffects(
                action,
                ReconcileNavigationInputs(
                    nullptr,
                    NavigationInputReconcileRequest{
                        .workflow_changed = true,
                        .filters_changed = true,
                        .sorting_changed = true}));
        }
        return action;
    }

    SampleLabelingTask* task = labeling_.CreateTaskFromAnnotation(
        std::move(plan.task_id),
        std::move(plan.task_name),
        std::move(plan.label_set),
        std::move(plan.values),
        annotation->path,
        plan.metadata_clean);
    if (task == nullptr) {
        return action;
    }
    if (!plan.metadata_clean && task->output_path) {
        const bool metadata_saved = labeling_.PersistActiveTask();
        if (!metadata_saved) {
            (void)labeling_.MarkActiveOutputSaveFailed("Could not write converted annotation metadata.");
        } else {
            (void)navigation_.AddReadOnlyAnnotationToActiveSource(annotation->path);
        }
    }
    workflow_sources_.InvalidateSortingSourceCache();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RenameActiveLabelingTask(std::string task_name)
{
    SourceCollectionSessionAction action;
    if (labeling_.RenameActiveTask(DefaultedSampleLabelingTaskName(std::move(task_name)))) {
        (void)labeling_.PersistActiveTask();
        workflow_sources_.InvalidateFilterViewCache();
        workflow_sources_.InvalidateSortingSourceCache();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{
                    .workflow_changed = true,
                    .filters_changed = true,
                    .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::DeleteActiveLabelingTask()
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.active_task();
    const std::string deleted_task_source_id = task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*task);
    if (!labeling_.DeleteActiveTask()) {
        return action;
    }

    if (!deleted_task_source_id.empty()) {
        (void)workflow_sources_.RemoveFilterSource(deleted_task_source_id);
    }
    MarkActiveWorkflowStateDirty();
    workflow_sources_.InvalidateFilterViewCache();
    workflow_sources_.InvalidateSortingSourceCache();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::UpsertActiveLabel(SampleLabelDefinition label, bool* changed)
{
    SourceCollectionSessionAction action;
    const bool label_changed = labeling_.UpsertActiveLabel(std::move(label));
    if (changed != nullptr) {
        *changed = label_changed;
    }
    if (label_changed) {
        if (const SampleLabelingTask* task = labeling_.active_task(); task != nullptr && task->output_path) {
            (void)labeling_.PersistActiveTask();
        }
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingAutoAdvance(bool enabled)
{
    SourceCollectionSessionAction action;
    SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr || task->auto_advance == enabled) {
        return action;
    }

    task->auto_advance = enabled;
    (void)labeling_.PersistActiveTaskRecord();
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    SourceCollectionSessionAction action;
    SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr || task->skip_labeled_on_advance == enabled) {
        return action;
    }

    task->skip_labeled_on_advance = enabled;
    (void)labeling_.PersistActiveTaskRecord();
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    SourceCollectionSessionAction action;
    const std::filesystem::path selected_output_path = output_path;
    if (labeling_.SetActiveTaskOutputPath(std::move(output_path))) {
        (void)labeling_.PersistActiveTask();
        if (const SourceCollectionManifest* context = navigation_.active_context()) {
            if (const SampleAnnotationResult* annotation =
                    FindSampleWorkflowAnnotationByPath(*context, selected_output_path)) {
                const std::string source_id = BuildAnnotationFilterSourceId(*annotation);
                (void)workflow_sources_.RemoveFilterSource(source_id);
                (void)workflow_sources_.RemoveSampleSortSource(source_id);
            }
        }
        MarkActiveWorkflowStateDirty();
        workflow_sources_.InvalidateFilterViewCache();
        workflow_sources_.InvalidateSortingSourceCache();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::DeactivateActiveLabelingTask()
{
    SourceCollectionSessionAction action;
    if (!labeling_.DeactivateActiveTask()) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    workflow_sources_.InvalidateFilterViewCache();
    workflow_sources_.InvalidateSortingSourceCache();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            nullptr,
            NavigationInputReconcileRequest{
                .workflow_changed = true,
                .filters_changed = true,
                .sorting_changed = true}));
    return action;
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::AssignActiveLabelToCurrentSample(
    const SpectrumSnapshotHandle& snapshot,
    int code)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    return ApplyLabelWriteResult(snapshot, labeling_.AssignLabel(*sample_index, code));
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::ClearActiveLabelForCurrentSample(
    const SpectrumSnapshotHandle& snapshot)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    return ApplyLabelWriteResult(snapshot, labeling_.ClearLabel(*sample_index));
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ClearFilters(const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionSessionAction action;
    workflow_sources_.ClearFilters();
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::AddFilterSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.AddFilterSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveFilterSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.RemoveFilterSource(source_id)) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetFilterValueSelected(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.SetFilterValueSelected(
            SourcePolicyContext(snapshot),
            std::move(source_id),
            std::move(value_key),
            selected)) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.filters_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ClearSampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionSessionAction action;
    workflow_sources_.ClearSampleSorting();
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::AddSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.AddSampleSortSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (!workflow_sources_.RemoveSampleSortSource(source_id)) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (source_id.empty()) {
        return ClearSampleSorting(snapshot);
    }
    if (!workflow_sources_.SetSampleSortSource(SourcePolicyContext(snapshot), std::move(source_id))) {
        return action;
    }

    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetSampleSortDirection(
    const SpectrumSnapshotHandle& snapshot,
    SampleNavigationSortDirection direction)
{
    SourceCollectionSessionAction action;
    workflow_sources_.SetSampleSortDirection(direction);
    MarkActiveWorkflowStateDirty();
    ApplyNavigationInputEffects(
        action,
        ReconcileNavigationInputs(
            snapshot,
            NavigationInputReconcileRequest{.sorting_changed = true}));
    return action;
}

SourceCollectionNavigationView SampleWorkflowCoordinator::NavigationView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionNavigationView view;
    const SampleNavigationSequence& sequence = navigation_.current_sequence();
    view.current_index = navigation_.current_index();
    view.current_source_row = sequence.current_source_row;
    view.current_sequence_position = sequence.current_sequence_position;
    view.sample_count = navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.can_move_previous = sequence.previous_target && sequence.current_source_row &&
                             *sequence.previous_target != *sequence.current_source_row;
    view.can_move_next = sequence.next_target && sequence.current_source_row &&
                         *sequence.next_target != *sequence.current_source_row;
    view.filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    view.sequence_active = sequence.active;
    view.sequence_empty = sequence.empty;
    view.sequence_count = sequence.active ? sequence.ordered_rows.size() : view.sample_count;
    view.filtered_sample_count = view.sequence_count;
    view.row_location_available = sequence.row_location_available;
    if (snapshot && view.current_index && snapshot->collection.current_index == *view.current_index) {
        view.current_sample_display_name = snapshot->current_spectrum.name;
    }
    if (const SourceCollectionManifest* context = navigation_.active_context()) {
        view.has_sample_names = !context->sample_names.empty();
        view.annotation_messages = context->messages;
        if (view.current_index && *view.current_index < context->sample_names.size()) {
            view.current_sample_name = context->sample_names[*view.current_index];
        }

        if (view.current_index) {
            const std::size_t current_index = *view.current_index;
            view.current_annotations.reserve(context->annotations.size());
            for (const SampleAnnotationResult& annotation : context->annotations) {
                const SampleLabelingTask* local_task =
                    FindLocalTaskForLoadedAnnotation(labeling_.active_source_tasks(), annotation);
                view.current_annotations.push_back(BuildAnnotationValueView(
                    annotation,
                    current_index,
                    local_task,
                    workflow_sources_.AnnotationDisplayName(annotation, local_task)));
            }

            if (const std::vector<SampleLabelingTask>* tasks = labeling_.active_source_tasks()) {
                for (const SampleLabelingTask& task : *tasks) {
                    if (!task.output_path) {
                        continue;
                    }
                    const bool already_loaded = std::any_of(
                        context->annotations.begin(),
                        context->annotations.end(),
                        [&task](const SampleAnnotationResult& annotation) {
                            return task.output_path &&
                                   SampleWorkflowPathsReferToSameFile(annotation.path, *task.output_path);
                        });
                    if (!already_loaded) {
                        view.current_annotations.push_back(BuildLocalTaskAnnotationValueView(
                            task,
                            current_index,
                            workflow_sources_.LocalTaskAnnotationDisplayName(task)));
                    }
                }
            }
        }

        const std::string_view query = navigation_.sample_name_query();
        const std::vector<std::size_t>& matches = navigation_.sample_name_matches();
        if (!query.empty() && view.has_sample_names && !matches.empty()) {
            const std::string target = LowerAscii(std::string{query});
            view.sample_name_matches.reserve(matches.size());
            for (const std::size_t row : matches) {
                if (row >= context->sample_names.size()) {
                    continue;
                }
                const std::string& name = context->sample_names[row];
                if (!view.exact_sample_name_match && LowerAscii(name) == target) {
                    view.exact_sample_name_match = row;
                    view.exact_sample_name = name;
                }
                view.sample_name_matches.push_back(SourceCollectionSampleNameMatchView{row, name});
            }
            view.has_partial_sample_name_matches = !view.exact_sample_name_match && !view.sample_name_matches.empty();
        }
    }
    return view;
}

SourceCollectionLabelingView SampleWorkflowCoordinator::LabelingView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionLabelingView view;
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    view.current_index = ActiveSampleIndex(snapshot);
    if (const SampleLabelingTask* task = labeling_.active_task()) {
        view.has_active_task = true;
        view.task_id = task->task_id;
        view.task_name = task->task_name;
        view.label_set = task->label_set;
        view.labeled_count = CountLabeledSamples(*task);
        view.sample_count = task->values.size();
        if (view.current_index && *view.current_index < task->values.size()) {
            view.current_code = task->values[*view.current_index];
        }
        view.auto_advance = task->auto_advance;
        view.skip_labeled_on_advance = task->skip_labeled_on_advance;
        view.remembered_position = task->remembered_position;
        if (view.remembered_position && *view.remembered_position < view.sample_count) {
            const SampleNavigationSequence& sequence = navigation_.current_sequence();
            view.remembered_position_resumable =
                !sequence.active || (!sequence.empty && sequence.ContainsSourceRow(*view.remembered_position));
        }
        view.output_path = task->output_path;
        view.save_state = task->save_state;
        view.can_deactivate_task = labeling_.CanDeactivateActiveTask();
        view.can_delete_task = labeling_.CanDeleteActiveTask();
    }
    view.state_save_failed = labeling_.state_save_failed();
    view.state_save_error = std::string{labeling_.state_save_error()};
    view.state_load_warning = std::string{labeling_.state_load_warning()};
    return view;
}

SourceCollectionFilterView SampleWorkflowCoordinator::FilterView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionFilterView view = workflow_sources_.BuildFilterView(SourcePolicyContext(snapshot));
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    return view;
}

SourceCollectionSampleSortingView SampleWorkflowCoordinator::SortingView(
    const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionSampleSortingView view = workflow_sources_.BuildSortingView(SourcePolicyContext(snapshot));
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    return view;
}

bool SampleWorkflowCoordinator::can_add_read_only_annotation() const
{
    return navigation_.active_context() != nullptr && navigation_.spectrum_count().value_or(0) > 0;
}

std::optional<std::size_t> SampleWorkflowCoordinator::current_index() const
{
    return navigation_.current_index();
}

void SampleWorkflowCoordinator::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    labeling_.RunMaintenance(now);
    if (!workflow_state_save_scheduler_.ShouldAttemptSave(now)) {
        return;
    }
    if (SaveWorkflowStateCache()) {
        workflow_state_save_scheduler_.MarkSaveSucceeded();
    } else {
        workflow_state_save_scheduler_.MarkSaveFailed();
    }
}

std::optional<LocalUserStateSaveScheduler::TimePoint> SampleWorkflowCoordinator::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline = labeling_.NextMaintenanceDeadline();
    const std::optional<LocalUserStateSaveScheduler::TimePoint> workflow_deadline =
        workflow_state_save_scheduler_.next_attempt_time();
    if (workflow_deadline && (!deadline || *workflow_deadline < *deadline)) {
        deadline = workflow_deadline;
    }
    return deadline;
}

bool SampleWorkflowCoordinator::FlushStateCaches()
{
    const bool labeling_saved = labeling_.FlushStateCache();
    const bool workflow_saved = FlushWorkflowStateCache();
    return labeling_saved && workflow_saved;
}

void SampleWorkflowCoordinator::SyncSampleWorkflowSession(
    const SpectrumSnapshotHandle& snapshot,
    SourceCollectionSessionAction& action)
{
    if (!snapshot || snapshot->source.path.empty() || snapshot->collection.spectrum_count == 0) {
        ClearSampleWorkflow(action);
        return;
    }

    const SourceCollectionIdentity identity = BuildSourceCollectionIdentity(*snapshot);
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        active_sample_workflow_identity_ = identity.id;
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        labeling_.ActivateSource(identity);
        RestoreActiveWorkflowState(identity.id);
        workflow_sources_.InvalidateFilterViewCache();
        workflow_sources_.InvalidateSortingSourceCache();
        action.workflow_changed = true;
    } else if (
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint) {
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        labeling_.ActivateSource(identity);
        workflow_sources_.InvalidateFilterViewCache();
        workflow_sources_.InvalidateSortingSourceCache();
        action.workflow_changed = true;
    } else {
        labeling_.ActivateSource(identity);
    }
}

void SampleWorkflowCoordinator::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    workflow_sources_.Clear();
    active_sample_workflow_identity_.reset();
    active_sample_workflow_context_fingerprint_.reset();
    action.workflow_changed = true;
}

SampleWorkflowCoordinator::NavigationInputReconcileEffects
SampleWorkflowCoordinator::ReconcileNavigationInputs(
    const SpectrumSnapshotHandle& snapshot,
    NavigationInputReconcileRequest request)
{
    NavigationInputReconcileEffects effects;
    effects.workflow_changed = request.workflow_changed;
    effects.navigation_inputs_changed = request.filters_changed || request.sorting_changed;
    if (request.filters_changed) {
        if (const std::optional<std::size_t> target = ApplySampleFilters(snapshot)) {
            effects.snapshot_index_to_load = target;
        }
    }
    if (request.sorting_changed) {
        if (const std::optional<std::size_t> target = ApplySampleSorting(snapshot)) {
            effects.snapshot_index_to_load = target;
        }
    }
    return effects;
}

void SampleWorkflowCoordinator::ApplyNavigationInputEffects(
    SourceCollectionSessionAction& action,
    const NavigationInputReconcileEffects& effects)
{
    action.workflow_changed = action.workflow_changed || effects.workflow_changed;
    action.navigation_inputs_changed =
        action.navigation_inputs_changed || effects.navigation_inputs_changed;
}

void SampleWorkflowCoordinator::ApplyNavigationInputEffects(
    SampleWorkflowCommandResult& result,
    const NavigationInputReconcileEffects& effects)
{
    ApplyNavigationInputEffects(result.action, effects);
    if (effects.snapshot_index_to_load) {
        result.snapshot_index_to_load = effects.snapshot_index_to_load;
    }
}

std::optional<std::size_t> SampleWorkflowCoordinator::ApplySampleFilters(const SpectrumSnapshotHandle& snapshot)
{
    workflow_sources_.InvalidateFilterViewCache();
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    if (sample_count == 0) {
        return navigation_.ClearSampleFilter();
    }
    if (!workflow_sources_.has_filter_conditions()) {
        return navigation_.ClearSampleFilter();
    }

    const SampleFilterEvaluation evaluation = workflow_sources_.EvaluateFilters(SourcePolicyContext(snapshot));
    if (evaluation.active) {
        return navigation_.SetSampleFilter(evaluation.included_samples);
    }
    return navigation_.ClearSampleFilter();
}

std::optional<std::size_t> SampleWorkflowCoordinator::ApplySampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    SampleWorkflowSortChoiceResult sort_choice =
        workflow_sources_.BuildSortChoice(SourcePolicyContext(snapshot), !restoring_source_session_);
    if (sort_choice.state_changed) {
        MarkActiveWorkflowStateDirty();
    }
    if (!sort_choice.choice) {
        return navigation_.ClearSampleSorting();
    }

    return navigation_.SetSampleSorting(std::move(*sort_choice.choice));
}

std::size_t SampleWorkflowCoordinator::ActiveSampleCount(const SpectrumSnapshotHandle& snapshot) const
{
    return navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
}

std::optional<std::size_t> SampleWorkflowCoordinator::ActiveSampleIndex(const SpectrumSnapshotHandle& snapshot) const
{
    if (navigation_.spectrum_count()) {
        return navigation_.current_index();
    }
    if (snapshot && !snapshot->source.path.empty() && snapshot->collection.spectrum_count > 0) {
        return snapshot->collection.current_index;
    }
    return std::nullopt;
}

SampleWorkflowSourceContext SampleWorkflowCoordinator::SourcePolicyContext(
    const SpectrumSnapshotHandle& snapshot) const
{
    return SampleWorkflowSourceContext{
        .collection = navigation_.active_context(),
        .labeling_tasks = labeling_.active_source_tasks(),
        .sample_count = ActiveSampleCount(snapshot)};
}

void SampleWorkflowCoordinator::EnsureWorkflowStateCacheLoaded()
{
    if (workflow_state_cache_loaded_) {
        return;
    }
    workflow_state_cache_loaded_ = true;
    workflow_state_cache_ = LoadSampleWorkflowStateCache(workflow_state_cache_path_);
}

void SampleWorkflowCoordinator::RestoreActiveWorkflowState(std::string_view source_identity)
{
    EnsureWorkflowStateCacheLoaded();

    workflow_sources_.Clear();
    const auto match = workflow_state_cache_.sources_by_identity.find(std::string(source_identity));
    if (match == workflow_state_cache_.sources_by_identity.end()) {
        return;
    }
    workflow_sources_.RestoreState(match->second);
}

void SampleWorkflowCoordinator::StoreActiveWorkflowState()
{
    if (!active_sample_workflow_identity_ || active_sample_workflow_identity_->empty()) {
        return;
    }
    EnsureWorkflowStateCacheLoaded();

    SampleWorkflowSourceState state = workflow_sources_.StoreState();
    if (workflow_sources_.HasState()) {
        workflow_state_cache_.sources_by_identity[*active_sample_workflow_identity_] = std::move(state);
    } else {
        workflow_state_cache_.sources_by_identity.erase(*active_sample_workflow_identity_);
    }
}

void SampleWorkflowCoordinator::MarkActiveWorkflowStateDirty()
{
    if (workflow_state_cache_path_.empty()) {
        return;
    }
    StoreActiveWorkflowState();
    workflow_state_save_scheduler_.MarkDirty();
}

bool SampleWorkflowCoordinator::SaveWorkflowStateCache()
{
    return SaveSampleWorkflowStateCache(workflow_state_cache_path_, workflow_state_cache_);
}

bool SampleWorkflowCoordinator::FlushWorkflowStateCache()
{
    if (!workflow_state_save_scheduler_.dirty()) {
        return true;
    }
    if (SaveWorkflowStateCache()) {
        workflow_state_save_scheduler_.MarkSaveSucceeded();
        return true;
    }
    workflow_state_save_scheduler_.MarkSaveFailed();
    return false;
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::ApplyLabelWriteResult(
    const SpectrumSnapshotHandle& snapshot,
    const SampleLabelWriteResult& result)
{
    SampleWorkflowCommandResult command_result;
    if (!result.changed) {
        return command_result;
    }

    SampleLabelingTask* task = labeling_.active_task();
    if (task != nullptr && task->output_path) {
        (void)labeling_.PersistActiveTask();
    }

    const NavigationInputReconcileEffects effects = ReconcileNavigationInputs(
        snapshot,
        NavigationInputReconcileRequest{.filters_changed = true});
    ApplyNavigationInputEffects(command_result, effects);

    if (result.advance_requested && task != nullptr) {
        const SampleWorkflowCommandResult navigation_result =
            RequestSampleNavigation(BuildAutoAdvanceRequest(*task), snapshot);
        MergeSourceCollectionSessionAction(command_result.action, navigation_result.action);
        command_result.navigation = navigation_result.navigation;
        command_result.snapshot_index_to_load = navigation_result.snapshot_index_to_load;
    }
    return command_result;
}

}  // namespace specforge
