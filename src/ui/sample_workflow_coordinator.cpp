#include "ui/sample_workflow_coordinator.h"

#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_workflow_preparation.h"

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
constexpr std::size_t kMaxLabelUndoEntries = 256;

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
           kind == SampleNavigationRequestKind::LabelAdvance ||
           kind == SampleNavigationRequestKind::RestoreLabelUndoPosition;
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
      workflow_state_cache_loader_([](const std::filesystem::path& path) {
          return LoadSampleWorkflowStateCache(path);
      }),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      workflow_state_cache_loader_([](const std::filesystem::path& path) {
          return LoadSampleWorkflowStateCache(path);
      }),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path)
    : SampleWorkflowCoordinator(
          std::move(navigation_state_cache_path),
          std::move(labeling_state_cache_path),
          std::move(workflow_state_cache_path),
          [](const std::filesystem::path& path) { return LoadSampleLabelingStateCache(path); },
          [](const std::filesystem::path& path) { return LoadSampleWorkflowStateCache(path); })
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path,
    SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
    WorkflowStateCacheLoader workflow_state_cache_loader)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path), std::move(labeling_state_cache_loader)),
      workflow_state_cache_path_(std::move(workflow_state_cache_path)),
      workflow_state_cache_loader_(std::move(workflow_state_cache_loader)),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounce, kWorkflowStateSaveRetry)
{
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SyncActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot)
{
    DiscardPreparedViewCaches();
    SourceCollectionSessionAction action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return action;
    }

    return SyncActiveSourceWithContext(
        std::move(source_key),
        snapshot,
        LoadSourceCollectionContext(*snapshot),
        std::nullopt);
}

PreparedSampleWorkflowActivationResult SampleWorkflowCoordinator::SyncPreparedActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    SourceCollectionContext context,
    PreparedSampleWorkflowState prepared_workflow)
{
    PreparedSampleWorkflowActivationResult result;
    SourceCollectionSessionAction& action = result.action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return result;
    }

    const SourceCollectionIdentity identity = context.identity;
    AdoptPreparedCache(prepared_workflow.preparation_cache, result.background_retirement);
    if (active_sample_workflow_identity_ && *active_sample_workflow_identity_ != identity.id) {
        StoreActiveWorkflowState();
    }
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        ClearLabelUndoHistory();
    }
    if (BackgroundRetirementHandle retired_labeling = labeling_.ActivatePreparedSource(
            identity,
            std::move(prepared_workflow.labeling_source_state),
            std::move(prepared_workflow.labeling_state_warning))) {
        result.background_retirement.push_back(std::move(retired_labeling));
    }
    result.background_retirement.push_back(
        MakeBackgroundRetirementHandle(std::move(workflow_sources_)));
    workflow_sources_ = SampleWorkflowSourcePolicy{};
    workflow_sources_.RestoreState(prepared_workflow.workflow_source_state);
    active_sample_workflow_identity_ = identity.id;
    active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
    if (prepared_filter_view_) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(*prepared_filter_view_)));
    }
    if (prepared_sorting_view_) {
        result.background_retirement.push_back(
            MakeBackgroundRetirementHandle(std::move(*prepared_sorting_view_)));
    }
    prepared_filter_view_.emplace(std::move(prepared_workflow.filter_view));
    prepared_sorting_view_.emplace(std::move(prepared_workflow.sorting_view));
    if (BackgroundRetirementHandle retired_navigation = navigation_.ActivatePreparedSource(
            std::move(*source_key),
            snapshot,
            identity,
            std::move(context.manifest),
            std::move(prepared_workflow))) {
        result.background_retirement.push_back(std::move(retired_navigation));
    }
    action.workflow_changed = true;
    action.navigation_inputs_changed = true;
    return result;
}

bool SampleWorkflowCoordinator::CanReusePreparedKnownSource(
    std::optional<std::string> source_key,
    const SourceCollectionIdentity& identity) const
{
    if (!source_key) {
        return false;
    }
    const std::optional<SourceCollectionIdentity> known =
        navigation_.KnownSourceIdentity(*source_key);
    return known && known->id == identity.id &&
           known->context_fingerprint == identity.context_fingerprint &&
           known->spectrum_count == identity.spectrum_count;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SyncReusedPreparedKnownSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    const SourceCollectionIdentity& identity)
{
    if (!snapshot || !CanReusePreparedKnownSource(source_key, identity)) {
        return {};
    }
    return SyncKnownActiveSource(std::move(source_key), snapshot);
}

std::optional<SourceCollectionIdentity> SampleWorkflowCoordinator::ActiveSourceIdentity() const
{
    return navigation_.active_source_identity();
}

std::optional<SourceCollectionIdentity> SampleWorkflowCoordinator::KnownSourceIdentity(
    std::string_view source_key) const
{
    return navigation_.KnownSourceIdentity(source_key);
}

std::optional<std::size_t> SampleWorkflowCoordinator::KnownSourceCurrentIndex(
    std::string_view source_key) const
{
    return navigation_.KnownSourceCurrentIndex(source_key);
}

std::optional<SampleWorkflowSourceState> SampleWorkflowCoordinator::WorkflowStateForSourceIdentity(
    std::string_view source_identity)
{
    if (active_sample_workflow_identity_ && *active_sample_workflow_identity_ == source_identity) {
        return workflow_sources_.StoreState();
    }
    EnsureWorkflowStateCacheLoaded();
    const SampleWorkflowSourceState* state = CachedWorkflowState(source_identity);
    return state == nullptr ? std::nullopt : std::optional<SampleWorkflowSourceState>{*state};
}

std::optional<SampleLabelingSourceState> SampleWorkflowCoordinator::LabelingStateForSourceIdentity(
    std::string_view source_identity)
{
    return labeling_.SourceStateForIdentity(source_identity);
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SyncKnownActiveSource(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot)
{
    DiscardPreparedViewCaches();
    SourceCollectionSessionAction action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return action;
    }

    const std::optional<SourceCollectionIdentity> identity =
        navigation_.ActivateKnownSource(*source_key);
    if (!identity) {
        // A source first opened through the synchronous compatibility path has
        // no prepared context yet. Build it once, then reuse it thereafter.
        return SyncActiveSource(std::move(source_key), snapshot);
    }

    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity->id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity->context_fingerprint;
    if (workflow_identity_changed) {
        ClearLabelUndoHistory();
    }

    SyncSampleWorkflowSession(*identity, action);
    if (workflow_identity_changed || workflow_context_changed) {
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                snapshot,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SyncActiveSourceWithContext(
    std::optional<std::string> source_key,
    const SpectrumSnapshotHandle& snapshot,
    SourceCollectionContext context,
    std::optional<std::size_t> prepared_index)
{
    SourceCollectionSessionAction action;
    if (!source_key || !snapshot || snapshot->source.path.empty()) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return action;
    }

    const SourceCollectionIdentity& identity = context.identity;
    const bool workflow_identity_changed =
        !active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id;
    const bool workflow_context_changed =
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint;

    if (workflow_identity_changed) {
        ClearLabelUndoHistory();
    }

    navigation_.ActivateSource(
        std::move(*source_key),
        snapshot,
        identity,
        std::move(context.manifest),
        prepared_index);
    SyncSampleWorkflowSession(identity, action);
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

BackgroundRetirementHandle SampleWorkflowCoordinator::RemoveSource(std::string_view source_key)
{
    BackgroundRetirementHandle retired = navigation_.RemoveSource(source_key);
    workflow_sources_.InvalidateFilterViewCache();
    workflow_sources_.InvalidateSortingSourceCache();
    return retired;
}

void SampleWorkflowCoordinator::DiscardPreparedViewCaches()
{
    prepared_filter_view_.reset();
    prepared_sorting_view_.reset();
}

std::vector<BackgroundRetirementHandle> SampleWorkflowCoordinator::ReleaseBackgroundResourcesForShutdown()
{
    std::vector<BackgroundRetirementHandle> resources =
        labeling_.ReleaseBackgroundResourcesForShutdown();
    if (workflow_state_cache_snapshot_) {
        resources.push_back(std::move(workflow_state_cache_snapshot_));
    }
    if (!workflow_state_cache_.sources_by_identity.empty()) {
        resources.push_back(
            MakeBackgroundRetirementHandle(std::exchange(workflow_state_cache_, {})));
    }
    resources.push_back(MakeBackgroundRetirementHandle(std::move(workflow_sources_)));
    workflow_sources_ = SampleWorkflowSourcePolicy{};
    if (prepared_filter_view_) {
        resources.push_back(MakeBackgroundRetirementHandle(std::move(*prepared_filter_view_)));
        prepared_filter_view_.reset();
    }
    if (prepared_sorting_view_) {
        resources.push_back(MakeBackgroundRetirementHandle(std::move(*prepared_sorting_view_)));
        prepared_sorting_view_.reset();
    }
    return resources;
}

void SampleWorkflowCoordinator::SetDeferredSampleNavigation(bool enabled)
{
    deferred_sample_navigation_ = enabled;
    if (!enabled) {
        CancelDeferredSampleNavigation();
    }
}

bool SampleWorkflowCoordinator::CommitDeferredSampleNavigation(std::size_t spectrum_index)
{
    const bool remember_labeling_position =
        navigation_.pending_navigation_remembers_labeling_position();
    if (!navigation_.CommitDeferredNavigation(spectrum_index)) {
        return false;
    }
    if (remember_labeling_position) {
        (void)labeling_.RememberActivePosition(spectrum_index);
    }
    return true;
}

bool SampleWorkflowCoordinator::RetargetDeferredSampleNavigation(std::size_t spectrum_index)
{
    return navigation_.RetargetDeferredNavigation(spectrum_index);
}

void SampleWorkflowCoordinator::CompletePreparedDeferredSampleNavigation(
    const PendingSampleNavigation& pending)
{
    const std::optional<SourceCollectionIdentity> active_identity =
        navigation_.active_source_identity();
    if (active_identity && active_identity->id == pending.source_identity.id &&
        navigation_.current_index() == pending.spectrum_index &&
        pending.remember_labeling_position) {
        (void)labeling_.RememberActivePosition(pending.spectrum_index);
    }
}

void SampleWorkflowCoordinator::CancelDeferredSampleNavigation()
{
    navigation_.CancelDeferredNavigation();
}

std::optional<std::size_t> SampleWorkflowCoordinator::pending_sample_index() const
{
    return navigation_.pending_index();
}

std::optional<PendingSampleNavigation> SampleWorkflowCoordinator::pending_sample_navigation() const
{
    const std::optional<SourceCollectionIdentity> identity = navigation_.active_source_identity();
    const std::optional<std::size_t> index = navigation_.pending_index();
    if (!identity || !index) {
        return std::nullopt;
    }
    return PendingSampleNavigation{
        *identity,
        *index,
        navigation_.pending_navigation_remembers_labeling_position(),
    };
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    const SpectrumSnapshotHandle& snapshot,
    std::optional<std::size_t> deferred_base_index,
    NavigationTargetResolutionReport* target_resolution)
{
    SampleWorkflowCommandResult result;
    const std::optional<std::size_t> pending_index_before = navigation_.pending_index();
    result.navigation = deferred_sample_navigation_
        ? navigation_.NavigateDeferred(
              request,
              ShouldRememberLabelingPosition(request.kind),
              deferred_base_index,
              target_resolution)
        : navigation_.Navigate(request);
    if (result.navigation.has_active_source && result.navigation.target_found) {
        if (!deferred_sample_navigation_ && ShouldRememberLabelingPosition(request.kind)) {
            (void)labeling_.RememberActivePosition(result.navigation.current_index);
        }
    }
    if (!result.navigation.has_active_source || !result.navigation.target_found) {
        return result;
    }

    if (deferred_sample_navigation_) {
        const std::optional<std::size_t> pending_index_after = navigation_.pending_index();
        if (pending_index_after == pending_index_before) {
            return result;
        }
        if (pending_index_after &&
            (!snapshot || snapshot->collection.current_index != *pending_index_after)) {
            result.snapshot_index_to_load = pending_index_after;
        } else {
            result.action.navigation_inputs_changed = true;
        }
    } else if (!snapshot || snapshot->collection.current_index != result.navigation.current_index) {
        result.snapshot_index_to_load = result.navigation.current_index;
    } else {
        result.action.navigation_inputs_changed = true;
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

std::vector<std::filesystem::path> SampleWorkflowCoordinator::AnnotationPathsForSourceKey(
    std::string_view source_key) const
{
    return navigation_.AnnotationPathsForSourceKey(source_key);
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

SourceCollectionSessionAction SampleWorkflowCoordinator::StartOrResumeTemporaryLabelingTask()
{
    SourceCollectionSessionAction action;
    const SampleLabelingControllerView labeling_view = labeling_.View();
    const SampleLabelingTask* active_task = labeling_view.active_task;
    const SampleLabelingTask* temporary_task = labeling_view.temporary_task;
    if (active_task != nullptr && temporary_task != nullptr && active_task->task_id == temporary_task->task_id) {
        return action;
    }
    if (active_task != nullptr &&
        (!labeling_.CanDeactivateActiveTask() || !labeling_.DeactivateActiveTask().changed)) {
        return action;
    }

    bool started_or_resumed = false;
    if (temporary_task != nullptr) {
        started_or_resumed = labeling_.ActivateTask(temporary_task->task_id).accepted;
    } else {
        const std::vector<SampleLabelingTask>* tasks = labeling_view.active_source_tasks;
        const std::string task_id = TaskIdForNewSampleLabelingTask(kTemporarySampleLabelingTaskName, tasks);
        started_or_resumed =
            labeling_.CreateTask(task_id, std::string{kTemporarySampleLabelingTaskName}).accepted;
    }
    if (started_or_resumed) {
        ClearLabelUndoHistory();
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

    const std::vector<SampleLabelingTask>* active_source_tasks =
        labeling_.View().active_source_tasks;
    const SampleAnnotationResult* annotation = FindSampleWorkflowAnnotationByPath(*context, annotation_path);
    std::optional<SampleAnnotationResult> loaded_annotation;
    if (annotation == nullptr) {
        const std::size_t sample_count = navigation_.spectrum_count().value_or(0);
        std::string load_error;
        loaded_annotation = LoadSampleAnnotationResultFromPath(annotation_path, sample_count, &load_error);
        if (!loaded_annotation) {
            return action;
        }
        annotation = &*loaded_annotation;
    }

    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::optional<SampleLabelResultMetadata> metadata = LoadVerifiedLabelMetadataForAnnotation(*annotation);
    SampleAnnotationLabelingActivationPlan plan = PlanSampleAnnotationLabelingActivation(
        SampleAnnotationLabelingActivationRequest{
            .annotation = annotation,
            .active_source_tasks = active_source_tasks,
            .metadata = metadata ? &*metadata : nullptr});
    if (plan.kind == SampleAnnotationLabelingActivationKind::None) {
        return action;
    }
    if (active_task != nullptr && active_task->task_id == plan.task_id) {
        return action;
    }
    if (active_task != nullptr &&
        (!labeling_.CanDeactivateActiveTask() || !labeling_.DeactivateActiveTask().changed)) {
        return action;
    }

    if (plan.kind == SampleAnnotationLabelingActivationKind::ActivateExistingTask) {
        if (labeling_.ActivateTask(plan.task_id).accepted) {
            ClearLabelUndoHistory();
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

    const SampleLabelingOperationResult create_result = labeling_.CreateTaskFromAnnotation(
        std::move(plan.task_id),
        std::move(plan.task_name),
        std::move(plan.label_set),
        std::move(plan.values),
        annotation->path,
        plan.metadata_clean);
    if (!create_result.accepted) {
        return action;
    }
    ClearLabelUndoHistory();
    if (!plan.metadata_clean && create_result.output_saved) {
        (void)navigation_.AddReadOnlyAnnotationToActiveSource(annotation->path);
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

SourceCollectionSessionAction SampleWorkflowCoordinator::DeleteActiveLabelingTask()
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.View().active_task;
    const std::string deleted_task_source_id = task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*task);
    if (!labeling_.DeleteActiveTask().changed) {
        return action;
    }
    ClearLabelUndoHistory();

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
    const SampleLabelingOperationResult operation =
        labeling_.UpsertActiveLabel(std::move(label));
    const bool label_changed = operation.changed;
    if (changed != nullptr) {
        *changed = label_changed;
    }
    if (label_changed) {
        ClearLabelUndoHistory();
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change,
    bool* changed)
{
    SourceCollectionSessionAction action;
    const int updated_code = label.code;
    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::string sample_filter_source_id =
        active_task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*active_task);
    const SampleLabelingOperationResult operation =
        labeling_.UpdateActiveLabel(original_code, std::move(label), allow_used_code_change);
    const bool label_changed = operation.changed;
    if (changed != nullptr) {
        *changed = label_changed;
    }
    if (label_changed) {
        ClearLabelUndoHistory();
        if (!sample_filter_source_id.empty() &&
            workflow_sources_.ReplaceSampleFilterValue(
                sample_filter_source_id,
                std::to_string(original_code),
                std::to_string(updated_code))) {
            MarkActiveWorkflowStateDirty();
        }
        ApplyNavigationInputEffects(
            action,
            ReconcileNavigationInputs(
                nullptr,
                NavigationInputReconcileRequest{.filters_changed = true, .sorting_changed = true}));
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveActiveLabel(int code, bool* changed)
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* active_task = labeling_.View().active_task;
    const std::string sample_filter_source_id =
        active_task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*active_task);
    const SampleLabelingOperationResult operation = labeling_.RemoveActiveLabel(code);
    const bool label_changed = operation.changed;
    if (changed != nullptr) {
        *changed = label_changed;
    }
    if (label_changed) {
        ClearLabelUndoHistory();
        if (!sample_filter_source_id.empty() &&
            workflow_sources_.RemoveSampleFilterValue(
                sample_filter_source_id,
                std::to_string(code))) {
            MarkActiveWorkflowStateDirty();
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
    (void)labeling_.SetActiveAutoAdvance(enabled);
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    SourceCollectionSessionAction action;
    (void)labeling_.SetActiveSkipLabeledOnAdvance(enabled);
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    SourceCollectionSessionAction action;
    const std::filesystem::path selected_output_path = output_path;
    const SampleLabelingOperationResult operation =
        labeling_.SaveActiveTemporaryTaskToOutput(
            std::move(output_path),
            SampleLabelingTaskNameForOutputPath(selected_output_path));
    if (operation.output_saved) {
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
    if (!labeling_.DeactivateActiveTask().changed) {
        return action;
    }
    ClearLabelUndoHistory();

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
    int code,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    const SampleLabelingWriteOperationResult write =
        labeling_.AssignLabel(*sample_index, code);
    return ApplyLabelWriteResult(
        snapshot,
        write.write,
        target_resolution);
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::ClearActiveLabelForCurrentSample(
    const SpectrumSnapshotHandle& snapshot,
    NavigationTargetResolutionReport* target_resolution)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex(snapshot);
    if (!sample_index) {
        return {};
    }
    const SampleLabelingWriteOperationResult write =
        labeling_.ClearLabel(*sample_index);
    return ApplyLabelWriteResult(
        snapshot,
        write.write,
        target_resolution);
}

SampleWorkflowCommandResult SampleWorkflowCoordinator::UndoLastLabelWrite(
    const SpectrumSnapshotHandle& snapshot)
{
    if (!LabelUndoHistoryMatchesActiveTask() || label_undo_history_->entries.empty()) {
        return {};
    }

    const SampleLabelingTask* task = labeling_.View().active_task;
    const LabelUndoEntry entry = label_undo_history_->entries.back();
    if (task == nullptr || entry.sample_index >= task->values.size() ||
        task->values[entry.sample_index] != entry.current_code) {
        ClearLabelUndoHistory();
        return {};
    }

    SampleLabelingWriteOperationResult write_operation =
        entry.previous_code == kUnlabeledSampleLabelCode
        ? labeling_.ClearLabel(entry.sample_index)
        : labeling_.AssignLabel(entry.sample_index, entry.previous_code);
    SampleLabelWriteResult& write_result = write_operation.write;
    if (!write_result.changed) {
        ClearLabelUndoHistory();
        return {};
    }

    write_result.advance_requested = false;
    label_undo_history_->entries.pop_back();
    return ApplyLabelWriteResult(
        snapshot,
        write_result,
        nullptr,
        false,
        entry.sample_index);
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
    view.can_move_previous = navigation_.can_move_previous();
    view.can_move_next = navigation_.can_move_next();
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
                    FindLocalTaskForLoadedAnnotation(
                        labeling_.View().active_source_tasks,
                        annotation);
                view.current_annotations.push_back(BuildAnnotationValueView(
                    annotation,
                    current_index,
                    local_task,
                    workflow_sources_.AnnotationDisplayName(annotation, local_task)));
            }

            if (const std::vector<SampleLabelingTask>* tasks =
                    labeling_.View().active_source_tasks) {
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
    const SampleLabelingControllerView labeling_view = labeling_.View();
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    view.current_index = ActiveSampleIndex(snapshot);
    view.has_temporary_task = labeling_view.temporary_task != nullptr;
    if (const SampleLabelingTask* task = labeling_view.active_task) {
        view.has_active_task = true;
        view.active_task_is_temporary = !task->output_path;
        view.task_id = task->task_id;
        view.task_name = task->task_name;
        view.label_set = task->label_set;
        view.label_usage_counts = task->label_usage_counts;
        view.labeled_count = task->labeled_count;
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
    SourceCollectionFilterView view;
    if (prepared_filter_view_) {
        view = std::move(*prepared_filter_view_);
        prepared_filter_view_.reset();
    } else {
        view = workflow_sources_.BuildFilterView(SourcePolicyContext(snapshot));
    }
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    return view;
}

SourceCollectionSampleSortingView SampleWorkflowCoordinator::SortingView(
    const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionSampleSortingView view;
    if (prepared_sorting_view_) {
        view = std::move(*prepared_sorting_view_);
        prepared_sorting_view_.reset();
    } else {
        view = workflow_sources_.BuildSortingView(SourcePolicyContext(snapshot));
    }
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

std::vector<std::size_t> SampleWorkflowCoordinator::AdjacentNavigationRows(
    SampleNavigationDirection direction,
    SampleNavigationPrefetchPolicy policy) const
{
    return navigation_.AdjacentRows(direction, policy);
}

void SampleWorkflowCoordinator::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    navigation_.RunMaintenance(now);
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
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        navigation_.NextMaintenanceDeadline();
    const std::optional<LocalUserStateSaveScheduler::TimePoint> labeling_deadline =
        labeling_.NextMaintenanceDeadline();
    if (labeling_deadline && (!deadline || *labeling_deadline < *deadline)) {
        deadline = labeling_deadline;
    }
    const std::optional<LocalUserStateSaveScheduler::TimePoint> workflow_deadline =
        workflow_state_save_scheduler_.next_attempt_time();
    if (workflow_deadline && (!deadline || *workflow_deadline < *deadline)) {
        deadline = workflow_deadline;
    }
    return deadline;
}

bool SampleWorkflowCoordinator::FlushStateCaches()
{
    const bool navigation_saved = navigation_.FlushStateCache();
    const bool labeling_saved = labeling_.FlushStateCache();
    const bool workflow_saved = FlushWorkflowStateCache();
    return navigation_saved && labeling_saved && workflow_saved;
}

void SampleWorkflowCoordinator::SyncSampleWorkflowSession(
    const SourceCollectionIdentity& identity,
    SourceCollectionSessionAction& action)
{
    if (identity.id.empty() || identity.spectrum_count == 0) {
        ClearSampleWorkflow(action);
        return;
    }
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        StoreActiveWorkflowState();
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
    DiscardPreparedViewCaches();
    ClearLabelUndoHistory();
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
        return navigation_.ClearSampleFilter(deferred_sample_navigation_);
    }
    if (!workflow_sources_.has_filter_conditions()) {
        return navigation_.ClearSampleFilter(deferred_sample_navigation_);
    }

    const SampleFilterEvaluation evaluation = workflow_sources_.EvaluateFilters(SourcePolicyContext(snapshot));
    if (evaluation.active) {
        return navigation_.SetSampleFilter(evaluation.included_samples, deferred_sample_navigation_);
    }
    return navigation_.ClearSampleFilter(deferred_sample_navigation_);
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
        return navigation_.ClearSampleSorting(deferred_sample_navigation_);
    }

    return navigation_.SetSampleSorting(
        std::move(*sort_choice.choice),
        deferred_sample_navigation_);
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
        .labeling_tasks = labeling_.View().active_source_tasks,
        .sample_count = ActiveSampleCount(snapshot)};
}

void SampleWorkflowCoordinator::EnsureWorkflowStateCacheLoaded()
{
    if (workflow_state_cache_loaded_) {
        return;
    }
    workflow_state_cache_loaded_ = true;
    workflow_state_cache_ = workflow_state_cache_loader_(workflow_state_cache_path_);
}

void SampleWorkflowCoordinator::AdoptPreparedCache(
    const std::shared_ptr<const SampleWorkflowPreparationCacheBundle>& cache,
    std::vector<BackgroundRetirementHandle>& background_retirement)
{
    if (!cache) {
        return;
    }
    std::shared_ptr<const SampleWorkflowStateCache> workflow_cache(cache, &cache->workflow);
    if (workflow_cache != workflow_state_cache_snapshot_) {
        if (workflow_state_cache_snapshot_) {
            background_retirement.push_back(workflow_state_cache_snapshot_);
        }
        workflow_state_cache_snapshot_ = std::move(workflow_cache);
    }
    workflow_state_cache_loaded_ = true;

    std::shared_ptr<const SampleLabelingStateCacheLoadResult> labeling_cache(cache, &cache->labeling);
    if (BackgroundRetirementHandle retired =
            labeling_.AdoptPreparedStateCache(std::move(labeling_cache))) {
        background_retirement.push_back(std::move(retired));
    }
}

const SampleWorkflowSourceState* SampleWorkflowCoordinator::CachedWorkflowState(
    std::string_view source_identity) const
{
    const std::string identity{source_identity};
    if (workflow_state_tombstones_.contains(identity)) {
        return nullptr;
    }
    if (const auto state = workflow_state_cache_.sources_by_identity.find(identity);
        state != workflow_state_cache_.sources_by_identity.end()) {
        return &state->second;
    }
    if (!workflow_state_cache_snapshot_) {
        return nullptr;
    }
    const auto state = workflow_state_cache_snapshot_->sources_by_identity.find(identity);
    return state == workflow_state_cache_snapshot_->sources_by_identity.end() ? nullptr : &state->second;
}

void SampleWorkflowCoordinator::RestoreActiveWorkflowState(std::string_view source_identity)
{
    EnsureWorkflowStateCacheLoaded();

    workflow_sources_.Clear();
    const SampleWorkflowSourceState* state = CachedWorkflowState(source_identity);
    if (state == nullptr) {
        return;
    }
    workflow_sources_.RestoreState(*state);
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
        workflow_state_tombstones_.erase(*active_sample_workflow_identity_);
    } else {
        workflow_state_cache_.sources_by_identity.erase(*active_sample_workflow_identity_);
        workflow_state_tombstones_.insert(*active_sample_workflow_identity_);
    }
}

void SampleWorkflowCoordinator::MarkActiveWorkflowStateDirty()
{
    StoreActiveWorkflowState();
    if (!workflow_state_cache_path_.empty()) {
        workflow_state_save_scheduler_.MarkDirty();
    }
}

bool SampleWorkflowCoordinator::SaveWorkflowStateCache()
{
    SampleWorkflowStateCache merged = workflow_state_cache_snapshot_
        ? *workflow_state_cache_snapshot_
        : SampleWorkflowStateCache{};
    for (const auto& [identity, state] : workflow_state_cache_.sources_by_identity) {
        merged.sources_by_identity[identity] = state;
    }
    for (const std::string& identity : workflow_state_tombstones_) {
        merged.sources_by_identity.erase(identity);
    }
    return SaveSampleWorkflowStateCache(workflow_state_cache_path_, merged);
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
    const SampleLabelWriteResult& result,
    NavigationTargetResolutionReport* target_resolution,
    bool record_undo,
    std::optional<std::size_t> restore_sample_index)
{
    DiscardPreparedViewCaches();
    SampleWorkflowCommandResult command_result;
    if (!result.changed) {
        return command_result;
    }

    if (record_undo) {
        RecordLabelUndo(result);
    }

    const SampleLabelingTask* task = labeling_.View().active_task;

    const NavigationInputReconcileEffects effects = ReconcileNavigationInputs(
        snapshot,
        NavigationInputReconcileRequest{.filters_changed = true});
    ApplyNavigationInputEffects(command_result, effects);

    if (restore_sample_index) {
        const SampleWorkflowCommandResult navigation_result = RequestSampleNavigation(
            SampleNavigationRequest::RestoreLabelUndoPosition(*restore_sample_index),
            snapshot);
        MergeSourceCollectionSessionAction(command_result.action, navigation_result.action);
        command_result.navigation = navigation_result.navigation;
        command_result.snapshot_index_to_load = navigation_result.snapshot_index_to_load;
    } else if (result.advance_requested && task != nullptr) {
        const SampleWorkflowCommandResult navigation_result = RequestSampleNavigation(
            BuildAutoAdvanceRequest(*task),
            snapshot,
            result.sample_index,
            target_resolution);
        MergeSourceCollectionSessionAction(command_result.action, navigation_result.action);
        command_result.navigation = navigation_result.navigation;
        command_result.snapshot_index_to_load = navigation_result.snapshot_index_to_load;
    }
    return command_result;
}

void SampleWorkflowCoordinator::RecordLabelUndo(const SampleLabelWriteResult& result)
{
    const SampleLabelingTask* task = labeling_.View().active_task;
    if (!active_sample_workflow_identity_ || task == nullptr) {
        return;
    }
    if (!LabelUndoHistoryMatchesActiveTask()) {
        label_undo_history_ = LabelUndoHistory{
            .workflow_identity = *active_sample_workflow_identity_,
            .task_id = task->task_id};
    }
    if (label_undo_history_->entries.size() == kMaxLabelUndoEntries) {
        label_undo_history_->entries.erase(label_undo_history_->entries.begin());
    }
    label_undo_history_->entries.push_back(LabelUndoEntry{
        .sample_index = result.sample_index,
        .previous_code = result.previous_code,
        .current_code = result.current_code});
}

void SampleWorkflowCoordinator::ClearLabelUndoHistory()
{
    label_undo_history_.reset();
}

bool SampleWorkflowCoordinator::LabelUndoHistoryMatchesActiveTask() const
{
    const SampleLabelingTask* task = labeling_.View().active_task;
    return label_undo_history_ && active_sample_workflow_identity_ && task != nullptr &&
           label_undo_history_->workflow_identity == *active_sample_workflow_identity_ &&
           label_undo_history_->task_id == task->task_id;
}

}  // namespace specforge
