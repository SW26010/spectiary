#include "ui/sample_workflow_coordinator.h"

#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_sorting_sources.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace specforge {
namespace {

constexpr std::uint64_t kWorkflowStateSaveDebounceFrames = 30;
constexpr std::uint64_t kWorkflowStateSaveRetryFrames = 120;

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string TrimAscii(std::string_view value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string AnnotationDisplayNameKeyFromPath(const std::filesystem::path& path)
{
    return path.empty() ? std::string{} : "annotation:" + PathToUtf8(path);
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

SampleFilterEvaluation EvaluateFilterSources(
    const SampleFilterController& filters,
    const std::vector<SampleFilterSource>& filter_sources,
    std::size_t sample_count)
{
    if (sample_count == 0) {
        return {};
    }
    if (filters.conditions().empty()) {
        SampleFilterEvaluation evaluation;
        evaluation.included_count = sample_count;
        return evaluation;
    }
    return filters.Evaluate(filter_sources, sample_count);
}

SourceCollectionFilterSourceView BuildFilterSourceView(
    const SampleFilterSource& source,
    const SampleFilterController& filters,
    std::filesystem::path annotation_path = {})
{
    SourceCollectionFilterSourceView view;
    view.id = source.id;
    view.name = source.name;
    view.annotation_path = std::move(annotation_path);
    view.filterable = source.filterable;
    view.options = source.options;
    if (const SampleFilterCondition* condition = filters.FindCondition(source.id)) {
        view.selected_value_keys = condition->allowed_value_keys;
    }
    return view;
}

bool PathExists(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code equivalent_error;
    if (PathExists(left) && PathExists(right) &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

const SampleAnnotationResult* FindAnnotationByPath(
    const SourceCollectionManifest& context,
    const std::filesystem::path& path)
{
    const auto match = std::find_if(context.annotations.begin(), context.annotations.end(), [&path](const auto& annotation) {
        return PathsReferToSameFile(annotation.path, path);
    });
    return match == context.annotations.end() ? nullptr : &*match;
}

const SampleLabelingTask* FindLocalTaskByOutputPath(
    const std::vector<SampleLabelingTask>* tasks,
    const std::filesystem::path& path)
{
    if (tasks == nullptr || path.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(tasks->begin(), tasks->end(), [&path](const SampleLabelingTask& task) {
        return task.output_path && PathsReferToSameFile(*task.output_path, path);
    });
    return match == tasks->end() ? nullptr : &*match;
}

bool IsAnnotationFilterSourceId(std::string_view source_id)
{
    constexpr std::string_view kPrefix = "annotation:";
    return source_id.size() > kPrefix.size() && source_id.substr(0, kPrefix.size()) == kPrefix;
}

bool IsLabelingFilterSourceId(std::string_view source_id)
{
    constexpr std::string_view kPrefix = "labeling:";
    return source_id.size() > kPrefix.size() && source_id.substr(0, kPrefix.size()) == kPrefix;
}

bool HasSelectedSourceId(const std::vector<std::string>& source_ids, std::string_view source_id)
{
    return std::any_of(source_ids.begin(), source_ids.end(), [source_id](const std::string& selected_source_id) {
        return selected_source_id == source_id;
    });
}

bool IsDefaultSampleSortSourceId(std::string_view source_id)
{
    return source_id == "source-order" || source_id == "sample-name";
}

bool AddSelectedSourceId(std::vector<std::string>& source_ids, std::string source_id)
{
    if (source_id.empty() || HasSelectedSourceId(source_ids, source_id)) {
        return false;
    }
    source_ids.push_back(std::move(source_id));
    return true;
}

bool RemoveSelectedSourceId(std::vector<std::string>& source_ids, std::string_view source_id)
{
    const auto old_size = source_ids.size();
    source_ids.erase(
        std::remove_if(
            source_ids.begin(),
            source_ids.end(),
            [source_id](const std::string& selected_source_id) {
                return selected_source_id == source_id;
            }),
        source_ids.end());
    return source_ids.size() != old_size;
}

bool IsAnnotationSampleFilterCandidate(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    const SampleAnnotationResult& annotation,
    std::size_t sample_count)
{
    if (annotation.values.size() != sample_count ||
        annotation.kind == SampleAnnotationKind::ContinuousFloat) {
        return false;
    }
    return FindLocalTaskForLoadedAnnotation(active_source_tasks, annotation) == nullptr;
}

bool IsLabelingSampleFilterCandidate(
    const SampleLabelingTask& task,
    std::size_t sample_count)
{
    return task.values.size() == sample_count;
}

const SampleLabelingTask* FindLabelingFilterSourceById(
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    if (active_source_tasks == nullptr) {
        return nullptr;
    }
    for (const SampleLabelingTask& task : *active_source_tasks) {
        if (BuildLabelingFilterSourceId(task) == source_id &&
            IsLabelingSampleFilterCandidate(task, sample_count)) {
            return &task;
        }
    }
    return nullptr;
}

bool HasAnnotationFilterSourceById(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    if (context == nullptr || !IsAnnotationFilterSourceId(source_id)) {
        return false;
    }
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (BuildAnnotationFilterSourceId(annotation) == source_id &&
            IsAnnotationSampleFilterCandidate(active_source_tasks, annotation, sample_count)) {
            return true;
        }
    }
    return false;
}

bool IsSampleFilterSourceAvailable(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    return HasAnnotationFilterSourceById(context, active_source_tasks, sample_count, source_id) ||
           FindLabelingFilterSourceById(active_source_tasks, sample_count, source_id) != nullptr;
}

bool IsSampleSortSourceAvailable(
    const SourceCollectionManifest* context,
    const std::vector<SampleLabelingTask>* active_source_tasks,
    std::size_t sample_count,
    std::string_view source_id)
{
    return BuildSampleSortingSource(context, active_source_tasks, sample_count, source_id).has_value();
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
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounceFrames, kWorkflowStateSaveRetryFrames)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounceFrames, kWorkflowStateSaveRetryFrames)
{
}

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path,
    std::filesystem::path workflow_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      workflow_state_cache_path_(std::move(workflow_state_cache_path)),
      workflow_state_save_scheduler_(kWorkflowStateSaveDebounceFrames, kWorkflowStateSaveRetryFrames)
{
}

std::string SampleWorkflowCoordinator::AnnotationDisplayName(
    const SampleAnnotationResult& annotation,
    const SampleLabelingTask* local_task) const
{
    const std::string key = AnnotationDisplayNameKeyFromPath(
        local_task != nullptr && local_task->output_path ? *local_task->output_path : annotation.path);
    const auto override = annotation_display_names_.find(key);
    if (override != annotation_display_names_.end()) {
        return override->second;
    }
    return local_task == nullptr ? annotation.name : local_task->task_name;
}

std::string SampleWorkflowCoordinator::LocalTaskAnnotationDisplayName(
    const SampleLabelingTask& task) const
{
    const std::string key = task.output_path ? AnnotationDisplayNameKeyFromPath(*task.output_path) : std::string{};
    const auto override = annotation_display_names_.find(key);
    if (override != annotation_display_names_.end()) {
        return override->second;
    }
    return task.task_name;
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
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
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
        InvalidateSampleSortingSourceCache();
        InvalidateSampleFilterViewCache();
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
        if (const SampleAnnotationResult* annotation = FindAnnotationByPath(*context, path)) {
            removed_source_id = BuildAnnotationFilterSourceId(*annotation);
        }
    }
    const bool removed_annotation = navigation_.RemoveReadOnlyAnnotationFromActiveSource(path);
    if (!removed_annotation) {
        return action;
    }

    InvalidateSampleSortingSourceCache();
    if (removed_source_id) {
        filters_.ClearCondition(*removed_source_id);
        (void)RemoveSelectedSourceId(selected_filter_source_ids_, *removed_source_id);
        (void)RemoveSelectedSampleSortSource(*removed_source_id);
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
    display_name = TrimAscii(display_name);
    if (path.empty()) {
        return action;
    }

    const SourceCollectionManifest* context = navigation_.active_context();
    const SampleAnnotationResult* annotation = context == nullptr ? nullptr : FindAnnotationByPath(*context, path);
    const SampleLabelingTask* local_task = annotation == nullptr
        ? FindLocalTaskByOutputPath(labeling_.active_source_tasks(), path)
        : FindLocalTaskForLoadedAnnotation(labeling_.active_source_tasks(), *annotation);
    if (annotation == nullptr && local_task == nullptr) {
        return action;
    }

    const std::string key = AnnotationDisplayNameKeyFromPath(
        local_task != nullptr && local_task->output_path ? *local_task->output_path : path);
    if (key.empty()) {
        return action;
    }
    const std::string default_display_name =
        local_task != nullptr ? local_task->task_name : annotation->name;

    bool changed = false;
    if (display_name.empty() || display_name == default_display_name) {
        changed = annotation_display_names_.erase(key) > 0;
    } else {
        auto [entry, inserted] = annotation_display_names_.emplace(key, display_name);
        if (!inserted && entry->second != display_name) {
            entry->second = std::move(display_name);
            changed = true;
        } else {
            changed = inserted;
        }
    }

    if (!changed) {
        return action;
    }
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

bool SampleWorkflowCoordinator::RestoreReadOnlyAnnotationsForActiveSource(
    const std::vector<std::filesystem::path>& paths)
{
    const bool restored = navigation_.RestoreReadOnlyAnnotationsForActiveSource(paths);
    if (restored) {
        InvalidateSampleSortingSourceCache();
        InvalidateSampleFilterViewCache();
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
        InvalidateSampleSortingSourceCache();
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
        InvalidateSampleSortingSourceCache();
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

    const SampleAnnotationResult* annotation = FindAnnotationByPath(*context, annotation_path);
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
            InvalidateSampleSortingSourceCache();
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
    InvalidateSampleSortingSourceCache();
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
        InvalidateSampleFilterViewCache();
        InvalidateSampleSortingSourceCache();
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
        filters_.ClearCondition(deleted_task_source_id);
        (void)RemoveSelectedSourceId(selected_filter_source_ids_, deleted_task_source_id);
    }
    MarkActiveWorkflowStateDirty();
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
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
            if (const SampleAnnotationResult* annotation = FindAnnotationByPath(*context, selected_output_path)) {
                const std::string source_id = BuildAnnotationFilterSourceId(*annotation);
                filters_.ClearCondition(source_id);
                (void)RemoveSelectedSourceId(selected_filter_source_ids_, source_id);
                (void)RemoveSelectedSampleSortSource(source_id);
            }
        }
        MarkActiveWorkflowStateDirty();
        InvalidateSampleFilterViewCache();
        InvalidateSampleSortingSourceCache();
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
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
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
    filters_.Clear();
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
    if (!IsSampleFilterSourceAvailable(
            navigation_.active_context(),
            labeling_.active_source_tasks(),
            ActiveSampleCount(snapshot),
            source_id)) {
        return action;
    }

    if (!AddSelectedSourceId(selected_filter_source_ids_, std::move(source_id))) {
        return action;
    }

    InvalidateSampleFilterViewCache();
    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveFilterSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    const bool removed_source = RemoveSelectedSourceId(selected_filter_source_ids_, source_id);
    const bool had_condition = filters_.FindCondition(source_id) != nullptr;
    filters_.ClearCondition(source_id);
    if (!removed_source && !had_condition) {
        return action;
    }

    InvalidateSampleFilterViewCache();
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
    if (!IsSelectedFilterSource(source_id)) {
        return action;
    }
    if (!IsSampleFilterSourceAvailable(
            navigation_.active_context(),
            labeling_.active_source_tasks(),
            ActiveSampleCount(snapshot),
            source_id)) {
        return action;
    }

    std::unordered_set<std::string> allowed_value_keys;
    if (const SampleFilterCondition* condition = filters_.FindCondition(source_id)) {
        allowed_value_keys = condition->allowed_value_keys;
    }
    if (selected) {
        allowed_value_keys.insert(std::move(value_key));
    } else {
        allowed_value_keys.erase(value_key);
    }
    filters_.SetCondition(std::move(source_id), std::move(allowed_value_keys));
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
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
    SetSampleSortSourceDirection("source-order", SampleNavigationSortDirection::Ascending);
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
    if (IsDefaultSampleSortSourceId(source_id) ||
        !IsSampleSortSourceAvailable(
            navigation_.active_context(),
            labeling_.active_source_tasks(),
            ActiveSampleCount(snapshot),
            source_id)) {
        return action;
    }

    if (!AddSelectedSourceId(selected_sample_sort_source_ids_, std::move(source_id))) {
        return action;
    }

    SetSampleSortSourceDirection(selected_sample_sort_source_ids_.back(), SampleNavigationSortDirection::Ascending);
    MarkActiveWorkflowStateDirty();
    action.workflow_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveSampleSortSource(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id)
{
    SourceCollectionSessionAction action;
    if (IsDefaultSampleSortSourceId(source_id) || !RemoveSelectedSampleSortSource(source_id)) {
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
    if (!IsSampleSortSourceAvailable(
            navigation_.active_context(),
            labeling_.active_source_tasks(),
            ActiveSampleCount(snapshot),
            source_id)) {
        return action;
    }

    if (!IsDefaultSampleSortSourceId(source_id)) {
        (void)AddSelectedSourceId(selected_sample_sort_source_ids_, source_id);
    }
    selected_sample_sort_direction_ = SampleSortSourceDirection(source_id);
    selected_sample_sort_source_id_ = std::move(source_id);
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
    selected_sample_sort_direction_ = direction;
    if (selected_sample_sort_source_id_) {
        SetSampleSortSourceDirection(*selected_sample_sort_source_id_, direction);
    } else {
        SetSampleSortSourceDirection("source-order", direction);
    }
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
                    AnnotationDisplayName(annotation, local_task)));
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
                            return task.output_path && PathsReferToSameFile(annotation.path, *task.output_path);
                        });
                    if (!already_loaded) {
                        view.current_annotations.push_back(BuildLocalTaskAnnotationValueView(
                            task,
                            current_index,
                            LocalTaskAnnotationDisplayName(task)));
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
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    SourceCollectionFilterView view = CachedSampleFilterView(sample_count);
    view.sample_count = sample_count;
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    return view;
}

SourceCollectionSampleSortingView SampleWorkflowCoordinator::SortingView(
    const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionSampleSortingView view;
    view.has_active_source = snapshot && !snapshot->source.path.empty() && ActiveSampleCount(snapshot) > 0;
    view.direction = selected_sample_sort_direction_;
    view.source_order_direction = SampleSortSourceDirection("source-order");
    const std::vector<SourceCollectionSampleSortSourceView>& all_sources =
        CachedSampleSortingSourceViews(ActiveSampleCount(snapshot));
    view.sources.reserve(selected_sample_sort_source_ids_.size() + 1);
    view.available_sources.reserve(all_sources.size());

    const auto find_source = [&all_sources](std::string_view source_id) {
        return std::find_if(
            all_sources.begin(),
            all_sources.end(),
            [source_id](const SourceCollectionSampleSortSourceView& source) {
                return source.id == source_id;
            });
    };
    const auto add_source_view = [this, &view](SourceCollectionSampleSortSourceView source_view) {
        source_view.selected = selected_sample_sort_source_id_ &&
                               *selected_sample_sort_source_id_ == source_view.id;
        source_view.removable = !IsDefaultSampleSortSourceId(source_view.id);
        source_view.direction = source_view.selected
            ? selected_sample_sort_direction_
            : SampleSortSourceDirection(source_view.id);
        view.active = view.active || source_view.selected;
        if (source_view.selected) {
            view.active_source_id = source_view.id;
        }
        view.sources.push_back(std::move(source_view));
    };

    const auto sample_name_source = find_source("sample-name");
    if (sample_name_source != all_sources.end()) {
        add_source_view(*sample_name_source);
    }

    for (const std::string& source_id : selected_sample_sort_source_ids_) {
        if (IsDefaultSampleSortSourceId(source_id)) {
            continue;
        }
        const auto source = find_source(source_id);
        if (source != all_sources.end()) {
            add_source_view(*source);
        }
    }

    for (const SourceCollectionSampleSortSourceView& source : all_sources) {
        if (IsDefaultSampleSortSourceId(source.id) || IsSelectedSampleSortSource(source.id)) {
            continue;
        }
        SourceCollectionSampleSortSourceView source_view = source;
        source_view.direction = SampleSortSourceDirection(source_view.id);
        view.available_sources.push_back(std::move(source_view));
    }
    if (selected_sample_sort_source_id_ && *selected_sample_sort_source_id_ == "source-order") {
        view.active = true;
        view.active_source_id = *selected_sample_sort_source_id_;
    }
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

void SampleWorkflowCoordinator::MaybeSaveStateCaches(std::uint64_t frame_index)
{
    labeling_.MaybeSaveStateCache(frame_index);
    if (!workflow_state_save_scheduler_.ShouldAttemptSave(frame_index)) {
        return;
    }
    if (SaveWorkflowStateCache()) {
        workflow_state_save_scheduler_.MarkSaveSucceeded();
    } else {
        workflow_state_save_scheduler_.MarkSaveFailed(frame_index);
    }
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
        InvalidateSampleFilterViewCache();
        InvalidateSampleSortingSourceCache();
        action.workflow_changed = true;
    } else if (
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint) {
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        labeling_.ActivateSource(identity);
        InvalidateSampleFilterViewCache();
        InvalidateSampleSortingSourceCache();
        action.workflow_changed = true;
    } else {
        labeling_.ActivateSource(identity);
    }
}

void SampleWorkflowCoordinator::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    filters_.Clear();
    active_sample_workflow_identity_.reset();
    active_sample_workflow_context_fingerprint_.reset();
    selected_filter_source_ids_.clear();
    selected_sample_sort_source_ids_.clear();
    annotation_display_names_.clear();
    sample_sort_source_directions_.clear();
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
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
    InvalidateSampleFilterViewCache();
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    if (sample_count == 0) {
        return navigation_.ClearSampleFilter();
    }
    if (filters_.conditions().empty()) {
        return navigation_.ClearSampleFilter();
    }

    const std::vector<SampleFilterSource> filter_sources = BuildSampleFilterSources();
    const SampleFilterEvaluation evaluation = filters_.Evaluate(filter_sources, sample_count);
    if (evaluation.active) {
        return navigation_.SetSampleFilter(evaluation.included_samples);
    }
    return navigation_.ClearSampleFilter();
}

std::optional<std::size_t> SampleWorkflowCoordinator::ApplySampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    if (sample_count == 0 || !selected_sample_sort_source_id_) {
        return navigation_.ClearSampleSorting();
    }

    std::optional<SampleSortingSource> source = BuildSampleSortingSource(
        navigation_.active_context(),
        labeling_.active_source_tasks(),
        sample_count,
        *selected_sample_sort_source_id_);
    if (!source) {
        if (!restoring_source_session_) {
            const std::string removed_source_id = *selected_sample_sort_source_id_;
            (void)RemoveSelectedSampleSortSource(removed_source_id);
            MarkActiveWorkflowStateDirty();
        }
        return navigation_.ClearSampleSorting();
    }

    SampleNavigationSortChoice sort_choice;
    sort_choice.active = true;
    sort_choice.direction = selected_sample_sort_direction_;
    sort_choice.values = std::move(source->values);
    return navigation_.SetSampleSorting(std::move(sort_choice));
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

SampleFilterEvaluation SampleWorkflowCoordinator::EvaluateSampleFilters(const SpectrumSnapshotHandle& snapshot) const
{
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    return EvaluateFilterSources(filters_, BuildSampleFilterSources(), sample_count);
}

bool SampleWorkflowCoordinator::IsSelectedFilterSource(std::string_view source_id) const
{
    return HasSelectedSourceId(selected_filter_source_ids_, source_id);
}

bool SampleWorkflowCoordinator::IsSelectedSampleSortSource(std::string_view source_id) const
{
    return HasSelectedSourceId(selected_sample_sort_source_ids_, source_id);
}

SampleNavigationSortDirection SampleWorkflowCoordinator::SampleSortSourceDirection(
    std::string_view source_id) const
{
    const auto match = sample_sort_source_directions_.find(std::string(source_id));
    if (match != sample_sort_source_directions_.end()) {
        return match->second;
    }
    return SampleNavigationSortDirection::Ascending;
}

void SampleWorkflowCoordinator::SetSampleSortSourceDirection(
    std::string_view source_id,
    SampleNavigationSortDirection direction)
{
    if (source_id.empty()) {
        return;
    }
    sample_sort_source_directions_[std::string(source_id)] = direction;
}

bool SampleWorkflowCoordinator::RemoveSelectedSampleSortSource(std::string_view source_id)
{
    bool changed = RemoveSelectedSourceId(selected_sample_sort_source_ids_, source_id);
    if (!IsDefaultSampleSortSourceId(source_id)) {
        sample_sort_source_directions_.erase(std::string(source_id));
    }
    if (selected_sample_sort_source_id_ && *selected_sample_sort_source_id_ == source_id) {
        selected_sample_sort_source_id_.reset();
        selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
        changed = true;
    }
    return changed;
}

std::vector<SampleFilterSource> SampleWorkflowCoordinator::BuildSampleFilterSources() const
{
    std::vector<SampleFilterSource> filter_sources;
    const SourceCollectionManifest* context = navigation_.active_context();
    const std::vector<SampleLabelingTask>* active_source_tasks = labeling_.active_source_tasks();
    const std::size_t sample_count = navigation_.spectrum_count().value_or(0);
    if (context == nullptr || sample_count == 0) {
        return filter_sources;
    }

    filter_sources.reserve(selected_filter_source_ids_.size());
    std::unordered_set<std::string> emitted_labeling_source_ids;
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (const SampleLabelingTask* local_task =
                FindLocalTaskForLoadedAnnotation(active_source_tasks, annotation)) {
            if (!IsLabelingSampleFilterCandidate(*local_task, sample_count)) {
                continue;
            }
            SampleFilterSource source = BuildLabelingFilterSource(*local_task);
            source.name = AnnotationDisplayName(annotation, local_task);
            emitted_labeling_source_ids.insert(source.id);
            if (IsSelectedFilterSource(source.id)) {
                filter_sources.push_back(std::move(source));
            }
            continue;
        }
        if (!IsAnnotationSampleFilterCandidate(labeling_.active_source_tasks(), annotation, sample_count)) {
            continue;
        }
        SampleFilterSource source = BuildAnnotationFilterSource(annotation);
        source.name = AnnotationDisplayName(annotation);
        if (IsSelectedFilterSource(source.id)) {
            filter_sources.push_back(std::move(source));
        }
    }
    if (active_source_tasks != nullptr) {
        for (const SampleLabelingTask& task : *active_source_tasks) {
            if (!task.output_path || !IsLabelingSampleFilterCandidate(task, sample_count)) {
                continue;
            }
            SampleFilterSource source = BuildLabelingFilterSource(task);
            source.name = LocalTaskAnnotationDisplayName(task);
            if (emitted_labeling_source_ids.find(source.id) != emitted_labeling_source_ids.end()) {
                continue;
            }
            emitted_labeling_source_ids.insert(source.id);
            if (IsSelectedFilterSource(source.id)) {
                filter_sources.push_back(std::move(source));
            }
        }
    }
    return filter_sources;
}

const SourceCollectionFilterView& SampleWorkflowCoordinator::CachedSampleFilterView(std::size_t sample_count) const
{
    const SourceCollectionManifest* context = navigation_.active_context();
    if (!sample_filter_view_cache_valid_ ||
        sample_filter_view_cache_context_ != context ||
        sample_filter_view_cache_sample_count_ != sample_count) {
        SourceCollectionFilterView view;
        view.sample_count = sample_count;
        const std::vector<SampleFilterSource> filter_sources = BuildSampleFilterSources();
        view.evaluation = EvaluateFilterSources(filters_, filter_sources, sample_count);
        view.evaluation.included_samples.clear();
        if (context != nullptr && sample_count > 0) {
            const std::vector<SampleLabelingTask>* active_source_tasks = labeling_.active_source_tasks();
            std::unordered_set<std::string> emitted_labeling_source_ids;
            for (const SampleAnnotationResult& annotation : context->annotations) {
                if (const SampleLabelingTask* local_task =
                        FindLocalTaskForLoadedAnnotation(active_source_tasks, annotation)) {
                    if (!IsLabelingSampleFilterCandidate(*local_task, sample_count)) {
                        continue;
                    }
                    SampleFilterSource source = BuildLabelingFilterSource(*local_task);
                    source.name = AnnotationDisplayName(annotation, local_task);
                    emitted_labeling_source_ids.insert(source.id);
                    SourceCollectionFilterSourceView source_view =
                        BuildFilterSourceView(source, filters_, annotation.path);
                    if (IsSelectedFilterSource(source.id)) {
                        view.sources.push_back(std::move(source_view));
                    } else {
                        view.available_sources.push_back(std::move(source_view));
                    }
                    continue;
                }
                if (!IsAnnotationSampleFilterCandidate(
                        active_source_tasks,
                        annotation,
                        sample_count)) {
                    continue;
                }
                SampleFilterSource source = BuildAnnotationFilterSource(annotation);
                source.name = AnnotationDisplayName(annotation);
                SourceCollectionFilterSourceView source_view =
                    BuildFilterSourceView(source, filters_, annotation.path);
                if (IsSelectedFilterSource(source.id)) {
                    view.sources.push_back(std::move(source_view));
                } else {
                    view.available_sources.push_back(std::move(source_view));
                }
            }
            if (active_source_tasks != nullptr) {
                for (const SampleLabelingTask& task : *active_source_tasks) {
                    if (!task.output_path || !IsLabelingSampleFilterCandidate(task, sample_count)) {
                        continue;
                    }
                    SampleFilterSource source = BuildLabelingFilterSource(task);
                    if (emitted_labeling_source_ids.find(source.id) != emitted_labeling_source_ids.end()) {
                        continue;
                    }
                    emitted_labeling_source_ids.insert(source.id);
                    source.name = LocalTaskAnnotationDisplayName(task);
                    SourceCollectionFilterSourceView source_view =
                        BuildFilterSourceView(source, filters_, *task.output_path);
                    if (IsSelectedFilterSource(source.id)) {
                        view.sources.push_back(std::move(source_view));
                    } else {
                        view.available_sources.push_back(std::move(source_view));
                    }
                }
            }
        }
        sample_filter_view_cache_ = std::move(view);
        sample_filter_view_cache_context_ = context;
        sample_filter_view_cache_sample_count_ = sample_count;
        sample_filter_view_cache_valid_ = true;
    }
    return sample_filter_view_cache_;
}

void SampleWorkflowCoordinator::InvalidateSampleFilterViewCache()
{
    sample_filter_view_cache_valid_ = false;
}

const std::vector<SourceCollectionSampleSortSourceView>&
SampleWorkflowCoordinator::CachedSampleSortingSourceViews(std::size_t sample_count) const
{
    const SourceCollectionManifest* context = navigation_.active_context();
    if (!sample_sorting_source_cache_valid_ ||
        sample_sorting_source_cache_context_ != context ||
        sample_sorting_source_cache_sample_count_ != sample_count) {
        sample_sorting_source_cache_ =
            BuildSampleSortingSourceViews(context, labeling_.active_source_tasks(), sample_count);
        if (context != nullptr) {
            for (SourceCollectionSampleSortSourceView& source_view : sample_sorting_source_cache_) {
                if (source_view.annotation_path.empty()) {
                    continue;
                }
                if (const SampleAnnotationResult* annotation =
                        FindAnnotationByPath(*context, source_view.annotation_path)) {
                    source_view.name = AnnotationDisplayName(*annotation);
                }
            }
        }
        sample_sorting_source_cache_context_ = context;
        sample_sorting_source_cache_sample_count_ = sample_count;
        sample_sorting_source_cache_valid_ = true;
    }
    return sample_sorting_source_cache_;
}

void SampleWorkflowCoordinator::InvalidateSampleSortingSourceCache()
{
    sample_sorting_source_cache_valid_ = false;
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

    filters_.Clear();
    selected_filter_source_ids_.clear();
    selected_sample_sort_source_ids_.clear();
    annotation_display_names_.clear();
    sample_sort_source_directions_.clear();
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;

    const auto match = workflow_state_cache_.sources_by_identity.find(std::string(source_identity));
    if (match == workflow_state_cache_.sources_by_identity.end()) {
        return;
    }

    for (const std::string& source_id : match->second.selected_filter_source_ids) {
        if (IsAnnotationFilterSourceId(source_id) || IsLabelingFilterSourceId(source_id)) {
            (void)AddSelectedSourceId(selected_filter_source_ids_, source_id);
        }
    }
    for (const SampleFilterCondition& condition : match->second.filter_conditions) {
        if (IsAnnotationFilterSourceId(condition.source_id) ||
            IsLabelingFilterSourceId(condition.source_id)) {
            filters_.SetCondition(condition.source_id, condition.allowed_value_keys);
            (void)AddSelectedSourceId(selected_filter_source_ids_, condition.source_id);
        }
    }
    for (const std::string& source_id : match->second.selected_sample_sort_source_ids) {
        if (!IsDefaultSampleSortSourceId(source_id)) {
            (void)AddSelectedSourceId(selected_sample_sort_source_ids_, source_id);
        }
    }
    for (const SampleAnnotationDisplayNameOverride& display_name : match->second.annotation_display_names) {
        if (IsAnnotationFilterSourceId(display_name.source_id) && !display_name.display_name.empty()) {
            annotation_display_names_[display_name.source_id] = display_name.display_name;
        }
    }
    selected_sample_sort_source_id_ = match->second.selected_sample_sort_source_id;
    if (selected_sample_sort_source_id_ && !IsDefaultSampleSortSourceId(*selected_sample_sort_source_id_)) {
        (void)AddSelectedSourceId(selected_sample_sort_source_ids_, *selected_sample_sort_source_id_);
    }
    selected_sample_sort_direction_ = match->second.selected_sample_sort_direction;
    if (selected_sample_sort_source_id_) {
        SetSampleSortSourceDirection(*selected_sample_sort_source_id_, selected_sample_sort_direction_);
    }
}

void SampleWorkflowCoordinator::StoreActiveWorkflowState()
{
    if (!active_sample_workflow_identity_ || active_sample_workflow_identity_->empty()) {
        return;
    }
    EnsureWorkflowStateCacheLoaded();

    SampleWorkflowSourceState state;
    state.filter_conditions = filters_.conditions();
    state.selected_filter_source_ids = selected_filter_source_ids_;
    state.selected_sample_sort_source_ids = selected_sample_sort_source_ids_;
    state.annotation_display_names.reserve(annotation_display_names_.size());
    for (const auto& [source_id, display_name] : annotation_display_names_) {
        if (IsAnnotationFilterSourceId(source_id) && !display_name.empty()) {
            state.annotation_display_names.push_back(
                SampleAnnotationDisplayNameOverride{source_id, display_name});
        }
    }
    state.selected_sample_sort_source_id = selected_sample_sort_source_id_;
    state.selected_sample_sort_direction = selected_sample_sort_direction_;

    const bool has_state =
        !state.filter_conditions.empty() ||
        !state.selected_filter_source_ids.empty() ||
        !state.selected_sample_sort_source_ids.empty() ||
        !state.annotation_display_names.empty() ||
        (state.selected_sample_sort_source_id && !state.selected_sample_sort_source_id->empty()) ||
        state.selected_sample_sort_direction != SampleNavigationSortDirection::Ascending;
    if (has_state) {
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
    workflow_state_save_scheduler_.MarkSaveFailed(0);
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
