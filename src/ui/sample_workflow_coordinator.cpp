#include "ui/sample_workflow_coordinator.h"

#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <unordered_set>
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

std::string TrimAscii(std::string value)
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

std::string TaskNameOrDefault(std::string task_name)
{
    task_name = TrimAscii(std::move(task_name));
    return task_name.empty() ? "Manual labeling" : task_name;
}

std::string TaskIdFromName(std::string_view task_name)
{
    std::string task_id;
    bool previous_dash = false;
    for (const unsigned char character : task_name) {
        if (std::isalnum(character) != 0) {
            task_id.push_back(static_cast<char>(std::tolower(character)));
            previous_dash = false;
        } else if (!task_id.empty() && !previous_dash) {
            task_id.push_back('-');
            previous_dash = true;
        }
    }
    while (!task_id.empty() && task_id.back() == '-') {
        task_id.pop_back();
    }
    return task_id.empty() ? "labeling" : task_id;
}

bool TaskIdExists(const std::vector<SampleLabelingTask>& tasks, std::string_view task_id)
{
    return std::any_of(tasks.begin(), tasks.end(), [task_id](const SampleLabelingTask& task) {
        return task.task_id == task_id;
    });
}

bool TaskIdExists(const std::vector<SampleLabelingTask>* tasks, std::string_view task_id)
{
    return tasks != nullptr && TaskIdExists(*tasks, task_id);
}

std::string UniqueTaskIdFromName(
    std::string_view task_name,
    const std::vector<SampleLabelingTask>* active_source_tasks)
{
    const std::string base_task_id = TaskIdFromName(task_name);
    if (!TaskIdExists(active_source_tasks, base_task_id)) {
        return base_task_id;
    }

    for (std::size_t suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = base_task_id;
        candidate += '-';
        candidate += std::to_string(suffix);
        if (!TaskIdExists(*active_source_tasks, candidate)) {
            return candidate;
        }
    }
    return base_task_id + "-copy";
}

std::string TaskIdForCreatedTask(
    std::string_view task_name,
    const std::vector<SampleLabelingTask>* active_source_tasks)
{
    const std::string base_task_id = TaskIdFromName(task_name);
    if (active_source_tasks == nullptr) {
        return base_task_id;
    }
    const auto base_match = std::find_if(
        active_source_tasks->begin(),
        active_source_tasks->end(),
        [&base_task_id](const SampleLabelingTask& task) {
            return task.task_id == base_task_id;
        });
    if (base_match == active_source_tasks->end() || base_match->task_name == task_name) {
        return base_task_id;
    }
    return UniqueTaskIdFromName(task_name, active_source_tasks);
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
    const SampleFilterController& filters)
{
    SourceCollectionFilterSourceView view;
    view.id = source.id;
    view.name = source.name;
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

std::optional<std::vector<int>> AnnotationIntegerValues(const SampleAnnotationResult& annotation)
{
    if (annotation.kind != SampleAnnotationKind::CategoricalInteger) {
        return std::nullopt;
    }

    std::vector<int> values;
    values.reserve(annotation.values.size());
    for (const SampleAnnotationValue& value : annotation.values) {
        if (!value.integer_value) {
            return std::nullopt;
        }
        values.push_back(*value.integer_value);
    }
    return values;
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

SampleLabelSet LabelSetFromUniqueIntegerValues(const std::vector<int>& values)
{
    std::vector<int> unique_values;
    unique_values.reserve(values.size());
    for (int value : values) {
        if (value == kUnlabeledSampleLabelCode) {
            continue;
        }
        unique_values.push_back(value);
    }
    std::sort(unique_values.begin(), unique_values.end());
    unique_values.erase(std::unique(unique_values.begin(), unique_values.end()), unique_values.end());

    SampleLabelSet label_set;
    label_set.labels.reserve(unique_values.size());
    for (int value : unique_values) {
        label_set.labels.push_back(SampleLabelDefinition{value, std::to_string(value), '\0'});
    }
    return label_set;
}

SourceCollectionAnnotationValueView BuildAnnotationValueView(
    const SampleAnnotationResult& annotation,
    std::size_t current_index,
    const SampleLabelingTask* local_task)
{
    SourceCollectionAnnotationValueView view;
    view.name = local_task == nullptr ? annotation.name : local_task->task_name;
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
    view.can_remove_annotation = local_task == nullptr;
    if (local_task != nullptr && local_task->output_path) {
        view.output_missing = !PathExists(*local_task->output_path);
        view.metadata_missing = !PathExists(SampleLabelResultMetadataPathForResult(*local_task->output_path));
    }
    return view;
}

SourceCollectionAnnotationValueView BuildLocalTaskAnnotationValueView(
    const SampleLabelingTask& task,
    std::size_t current_index)
{
    SourceCollectionAnnotationValueView view;
    view.name = task.task_name;
    view.path = task.output_path.value_or(std::filesystem::path{});
    view.relationship = SampleAnnotationWorkflowRelationship::LocalLabelingTask;
    view.relationship_label = std::string{SampleAnnotationWorkflowRelationshipLabel(view.relationship)};
    if (current_index < task.values.size()) {
        view.display_text = FormatSampleLabelValue(task.label_set, task.values[current_index]);
    } else {
        view.missing = true;
    }
    view.can_activate_labeling = task.output_path.has_value();
    view.can_remove_annotation = false;
    if (task.output_path) {
        view.output_missing = !PathExists(*task.output_path);
        view.metadata_missing = !PathExists(SampleLabelResultMetadataPathForResult(*task.output_path));
    }
    return view;
}

const SampleLabelingTask* FindLocalTaskForLoadedAnnotation(
    const SampleLabelingController& labeling,
    const SampleAnnotationResult& annotation)
{
    if (annotation.label_metadata) {
        return labeling.FindActiveSourceTaskByOutputPath(
            annotation.path,
            annotation.label_metadata->task_id,
            annotation.values.size());
    }

    if (annotation.kind != SampleAnnotationKind::CategoricalInteger || annotation.path.empty()) {
        return nullptr;
    }

    const std::vector<SampleLabelingTask>* tasks = labeling.active_source_tasks();
    if (tasks == nullptr) {
        return nullptr;
    }

    const auto match = std::find_if(tasks->begin(), tasks->end(), [&annotation](const SampleLabelingTask& task) {
        return task.output_path && task.values.size() == annotation.values.size() &&
               PathsReferToSameFile(*task.output_path, annotation.path);
    });
    return match == tasks->end() ? nullptr : &*match;
}

struct SampleSortingSource {
    std::string id;
    std::string name;
    std::vector<SampleNavigationSortValue> values;
};

std::optional<double> ParseFiniteDouble(std::string_view text)
{
    std::string trimmed = TrimAscii(std::string{text});
    if (trimmed.empty()) {
        return std::nullopt;
    }

    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

SourceCollectionSampleSortSourceView MakeSampleSortSourceView(std::string id, std::string name)
{
    SourceCollectionSampleSortSourceView view;
    view.id = std::move(id);
    view.name = std::move(name);
    return view;
}

std::optional<SourceCollectionSampleSortSourceView> BuildSampleNameSortingSourceView(
    const SourceCollectionManifest& context,
    std::size_t sample_count)
{
    if (context.sample_names.size() != sample_count) {
        return std::nullopt;
    }
    return MakeSampleSortSourceView("sample-name", "Sample name");
}

std::optional<SampleSortingSource> BuildSampleNameSortingSource(
    const SourceCollectionManifest& context,
    std::size_t sample_count)
{
    if (context.sample_names.size() != sample_count) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = "sample-name";
    source.name = "Sample name";
    source.values.reserve(context.sample_names.size());
    for (const std::string& sample_name : context.sample_names) {
        source.values.push_back(MakeSampleNavigationSortValue(sample_name));
    }
    return source;
}

bool IsAnnotationSortingSourceCandidate(
    const SampleLabelingController& labeling,
    const SampleAnnotationResult& annotation,
    std::size_t sample_count)
{
    return annotation.values.size() == sample_count &&
           annotation.relationship == SampleAnnotationWorkflowRelationship::PlainAnnotation &&
           !annotation.label_metadata && FindLocalTaskForLoadedAnnotation(labeling, annotation) == nullptr;
}

std::optional<SampleSortingSource> BuildAnnotationSortingSource(
    const SampleLabelingController& labeling,
    const SampleAnnotationResult& annotation,
    std::size_t sample_count)
{
    if (!IsAnnotationSortingSourceCandidate(labeling, annotation, sample_count)) {
        return std::nullopt;
    }

    SampleSortingSource source;
    source.id = BuildAnnotationFilterSourceId(annotation);
    source.name = annotation.name;
    source.values.reserve(annotation.values.size());
    for (const SampleAnnotationValue& value : annotation.values) {
        switch (annotation.kind) {
        case SampleAnnotationKind::CategoricalInteger:
            if (!value.integer_value) {
                return std::nullopt;
            }
            source.values.push_back(
                MakeSampleNavigationSortValue(static_cast<double>(*value.integer_value)));
            break;
        case SampleAnnotationKind::ContinuousFloat:
            if (const std::optional<double> parsed = ParseFiniteDouble(value.display_text)) {
                source.values.push_back(MakeSampleNavigationSortValue(*parsed));
            } else {
                return std::nullopt;
            }
            break;
        case SampleAnnotationKind::Text:
            source.values.push_back(MakeSampleNavigationSortValue(value.display_text));
            break;
        }
    }
    return source;
}

std::vector<SourceCollectionSampleSortSourceView> BuildSampleSortingSourceViews(
    const SourceCollectionManifest* context,
    const SampleLabelingController& labeling,
    std::size_t sample_count)
{
    std::vector<SourceCollectionSampleSortSourceView> sources;
    if (context == nullptr || sample_count == 0) {
        return sources;
    }

    sources.reserve(context->annotations.size() + 1);
    if (std::optional<SourceCollectionSampleSortSourceView> sample_names =
            BuildSampleNameSortingSourceView(*context, sample_count)) {
        sources.push_back(std::move(*sample_names));
    }
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (std::optional<SampleSortingSource> annotation_source =
                BuildAnnotationSortingSource(labeling, annotation, sample_count)) {
            sources.push_back(MakeSampleSortSourceView(
                std::move(annotation_source->id),
                std::move(annotation_source->name)));
        }
    }
    return sources;
}

std::optional<SampleSortingSource> BuildSampleSortingSource(
    const SourceCollectionManifest* context,
    const SampleLabelingController& labeling,
    std::size_t sample_count,
    std::string_view source_id)
{
    if (context == nullptr || sample_count == 0) {
        return std::nullopt;
    }

    if (source_id == "sample-name") {
        return BuildSampleNameSortingSource(*context, sample_count);
    }
    for (const SampleAnnotationResult& annotation : context->annotations) {
        if (BuildAnnotationFilterSourceId(annotation) == source_id) {
            return BuildAnnotationSortingSource(labeling, annotation, sample_count);
        }
    }
    return std::nullopt;
}

}  // namespace

SampleWorkflowCoordinator::SampleWorkflowCoordinator() = default;

SampleWorkflowCoordinator::SampleWorkflowCoordinator(
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path))
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
        ApplySampleFilters(snapshot);
        ApplySampleSorting(snapshot);
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ClearActiveWorkflow()
{
    SourceCollectionSessionAction action;
    ClearSampleWorkflow(action);
    return action;
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
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RemoveReadOnlyAnnotationFromActiveSource(
    const std::filesystem::path& path)
{
    SourceCollectionSessionAction action;
    const bool removed_annotation = navigation_.RemoveReadOnlyAnnotationFromActiveSource(path);
    if (!removed_annotation) {
        return action;
    }

    InvalidateSampleSortingSourceCache();
    filters_.Clear();
    ApplySampleFilters(nullptr);
    ApplySampleSorting(nullptr);
    action.navigation_inputs_changed = true;
    return action;
}

bool SampleWorkflowCoordinator::RestoreReadOnlyAnnotationsForActiveSource(
    const std::vector<std::filesystem::path>& paths)
{
    const bool restored = navigation_.RestoreReadOnlyAnnotationsForActiveSource(paths);
    if (restored) {
        InvalidateSampleSortingSourceCache();
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
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
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::CreateLabelingTask(std::string task_name)
{
    SourceCollectionSessionAction action;
    task_name = TaskNameOrDefault(std::move(task_name));
    const std::string task_id = TaskIdForCreatedTask(task_name, labeling_.active_source_tasks());
    if (labeling_.CreateTask(task_id, std::move(task_name)) != nullptr) {
        InvalidateSampleSortingSourceCache();
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
        action.navigation_inputs_changed = true;
        action.workflow_changed = true;
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
    if (active_task != nullptr) {
        if (!metadata || active_task->task_id != metadata->task_id) {
            return action;
        }
    }
    if (metadata) {
        if (const SampleLabelingTask* task = labeling_.FindActiveSourceTaskByOutputPath(
                annotation->path,
                metadata->task_id,
                annotation->values.size())) {
            if (labeling_.ActivateTask(task->task_id)) {
                InvalidateSampleSortingSourceCache();
                ApplySampleFilters(nullptr);
                ApplySampleSorting(nullptr);
                action.workflow_changed = true;
                action.navigation_inputs_changed = true;
            }
            return action;
        }
    }
    if (active_task != nullptr) {
        return action;
    }

    std::optional<std::vector<int>> values = AnnotationIntegerValues(*annotation);
    if (!values) {
        return action;
    }

    std::string task_name = metadata && !metadata->task_name.empty()
        ? metadata->task_name
        : annotation->name;
    task_name = TaskNameOrDefault(std::move(task_name));
    const std::string preferred_task_id =
        metadata && !metadata->task_id.empty()
        ? metadata->task_id
        : UniqueTaskIdFromName(task_name, labeling_.active_source_tasks());
    const std::string task_id = TaskIdExists(labeling_.active_source_tasks(), preferred_task_id)
        ? UniqueTaskIdFromName(task_name, labeling_.active_source_tasks())
        : preferred_task_id;
    SampleLabelSet label_set = metadata
        ? metadata->label_set
        : LabelSetFromUniqueIntegerValues(*values);
    const bool metadata_clean = metadata.has_value();
    SampleLabelingTask* task = labeling_.CreateTaskFromAnnotation(
        task_id,
        std::move(task_name),
        std::move(label_set),
        std::move(*values),
        annotation->path,
        metadata_clean);
    if (task == nullptr) {
        return action;
    }
    if (!metadata_clean && task->output_path) {
        const bool metadata_saved = labeling_.PersistActiveTask();
        if (!metadata_saved) {
            (void)labeling_.MarkActiveOutputSaveFailed("Could not write converted annotation metadata.");
        } else {
            (void)navigation_.AddReadOnlyAnnotationToActiveSource(annotation->path);
        }
    }
    InvalidateSampleSortingSourceCache();
    ApplySampleFilters(nullptr);
    ApplySampleSorting(nullptr);
    action.workflow_changed = true;
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::RenameActiveLabelingTask(std::string task_name)
{
    SourceCollectionSessionAction action;
    if (labeling_.RenameActiveTask(TaskNameOrDefault(std::move(task_name)))) {
        (void)labeling_.PersistActiveTask();
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
        action.workflow_changed = true;
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::DeleteActiveLabelingTask()
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.active_task();
    const std::string active_labeling_source_id = task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*task);
    if (!labeling_.DeleteActiveTask()) {
        return action;
    }

    if (!active_labeling_source_id.empty()) {
        filters_.ClearCondition(active_labeling_source_id);
    }
    selected_labeling_filter_source_id_.reset();
    InvalidateSampleSortingSourceCache();
    ApplySampleFilters(nullptr);
    ApplySampleSorting(nullptr);
    action.workflow_changed = true;
    action.navigation_inputs_changed = true;
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
        ApplySampleFilters(nullptr);
        ApplySampleSorting(nullptr);
        action.navigation_inputs_changed = true;
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
    if (labeling_.SetActiveTaskOutputPath(std::move(output_path))) {
        (void)labeling_.PersistActiveTask();
        InvalidateSampleSortingSourceCache();
        ApplySampleSorting(nullptr);
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::DeactivateActiveLabelingTask()
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.active_task();
    const std::string active_labeling_source_id = task == nullptr ? std::string{} : BuildLabelingFilterSourceId(*task);
    if (!labeling_.DeactivateActiveTask()) {
        return action;
    }

    if (!active_labeling_source_id.empty()) {
        filters_.ClearCondition(active_labeling_source_id);
    }
    selected_labeling_filter_source_id_.reset();
    InvalidateSampleSortingSourceCache();
    ApplySampleFilters(nullptr);
    ApplySampleSorting(nullptr);
    action.workflow_changed = true;
    action.navigation_inputs_changed = true;
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
    ApplySampleFilters(snapshot);
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetFilterValueSelected(
    const SpectrumSnapshotHandle& snapshot,
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SourceCollectionSessionAction action;
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
    ApplySampleFilters(snapshot);
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetActiveLabelingFilterSourceSelected(
    const SpectrumSnapshotHandle& snapshot,
    bool selected)
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr) {
        if (selected_labeling_filter_source_id_) {
            filters_.ClearCondition(*selected_labeling_filter_source_id_);
        }
        selected_labeling_filter_source_id_.reset();
        ApplySampleFilters(snapshot);
        action.navigation_inputs_changed = true;
        return action;
    }

    const std::string active_labeling_source_id = BuildLabelingFilterSourceId(*task);
    if (selected_labeling_filter_source_id_ && *selected_labeling_filter_source_id_ != active_labeling_source_id) {
        filters_.ClearCondition(*selected_labeling_filter_source_id_);
    }

    if (selected) {
        selected_labeling_filter_source_id_ = active_labeling_source_id;
    } else {
        filters_.ClearCondition(active_labeling_source_id);
        selected_labeling_filter_source_id_.reset();
    }
    ApplySampleFilters(snapshot);
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::ClearSampleSorting(
    const SpectrumSnapshotHandle& snapshot)
{
    SourceCollectionSessionAction action;
    selected_sample_sort_source_id_.reset();
    (void)navigation_.ClearSampleSorting();
    (void)snapshot;
    action.navigation_inputs_changed = true;
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

    selected_sample_sort_source_id_ = std::move(source_id);
    (void)ApplySampleSorting(snapshot);
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SampleWorkflowCoordinator::SetSampleSortDirection(
    const SpectrumSnapshotHandle& snapshot,
    SampleNavigationSortDirection direction)
{
    SourceCollectionSessionAction action;
    selected_sample_sort_direction_ = direction;
    (void)ApplySampleSorting(snapshot);
    action.navigation_inputs_changed = true;
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
                const SampleLabelingTask* local_task = FindLocalTaskForLoadedAnnotation(labeling_, annotation);
                view.current_annotations.push_back(BuildAnnotationValueView(annotation, current_index, local_task));
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
                        view.current_annotations.push_back(BuildLocalTaskAnnotationValueView(task, current_index));
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
    view.has_active_labeling_task = labeling_.active_task() != nullptr;
    view.active_labeling_filter_source_selected = active_labeling_filter_source_selected();
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
    const std::vector<SourceCollectionSampleSortSourceView>& sources =
        CachedSampleSortingSourceViews(ActiveSampleCount(snapshot));
    view.sources.reserve(sources.size());
    for (const SourceCollectionSampleSortSourceView& source : sources) {
        SourceCollectionSampleSortSourceView source_view = source;
        source_view.selected = selected_sample_sort_source_id_ &&
                               *selected_sample_sort_source_id_ == source_view.id;
        view.active = view.active || source_view.selected;
        if (source_view.selected) {
            view.active_source_id = source_view.id;
        }
        view.sources.push_back(std::move(source_view));
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
}

bool SampleWorkflowCoordinator::FlushStateCaches()
{
    return labeling_.FlushStateCache();
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
        filters_.Clear();
        selected_labeling_filter_source_id_.reset();
        selected_sample_sort_source_id_.reset();
        selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
        active_sample_workflow_identity_ = identity.id;
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        InvalidateSampleSortingSourceCache();
        action.workflow_changed = true;
    } else if (
        !active_sample_workflow_context_fingerprint_ ||
        *active_sample_workflow_context_fingerprint_ != identity.context_fingerprint) {
        active_sample_workflow_context_fingerprint_ = identity.context_fingerprint;
        InvalidateSampleFilterViewCache();
        InvalidateSampleSortingSourceCache();
        action.workflow_changed = true;
    }
    labeling_.ActivateSource(identity);
}

void SampleWorkflowCoordinator::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    filters_.Clear();
    active_sample_workflow_identity_.reset();
    active_sample_workflow_context_fingerprint_.reset();
    selected_labeling_filter_source_id_.reset();
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
    InvalidateSampleFilterViewCache();
    InvalidateSampleSortingSourceCache();
    action.workflow_changed = true;
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
        labeling_,
        sample_count,
        *selected_sample_sort_source_id_);
    if (!source) {
        selected_sample_sort_source_id_.reset();
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

bool SampleWorkflowCoordinator::active_labeling_filter_source_selected() const
{
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr || !selected_labeling_filter_source_id_) {
        return false;
    }
    return *selected_labeling_filter_source_id_ == BuildLabelingFilterSourceId(*task);
}

std::vector<SampleFilterSource> SampleWorkflowCoordinator::BuildSampleFilterSources() const
{
    std::vector<SampleFilterSource> filter_sources;
    const SourceCollectionManifest* context = navigation_.active_context();
    if (context != nullptr) {
        filter_sources.reserve(context->annotations.size() + 1);
        for (const SampleAnnotationResult& annotation : context->annotations) {
            filter_sources.push_back(BuildAnnotationFilterSource(annotation));
        }
    }

    if (const SampleLabelingTask* task = labeling_.active_task()) {
        if (selected_labeling_filter_source_id_ &&
            *selected_labeling_filter_source_id_ == BuildLabelingFilterSourceId(*task)) {
            filter_sources.push_back(BuildLabelingFilterSource(*task));
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
        view.sources.reserve(filter_sources.size());
        for (const SampleFilterSource& source : filter_sources) {
            view.sources.push_back(BuildFilterSourceView(source, filters_));
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
        sample_sorting_source_cache_ = BuildSampleSortingSourceViews(context, labeling_, sample_count);
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

    const std::optional<std::size_t> filter_target = ApplySampleFilters(snapshot);
    command_result.action.navigation_inputs_changed = true;

    if (result.advance_requested && task != nullptr) {
        const SampleWorkflowCommandResult navigation_result =
            RequestSampleNavigation(BuildAutoAdvanceRequest(*task), snapshot);
        MergeSourceCollectionSessionAction(command_result.action, navigation_result.action);
        command_result.navigation = navigation_result.navigation;
        command_result.snapshot_index_to_load = navigation_result.snapshot_index_to_load;
    } else if (filter_target) {
        command_result.snapshot_index_to_load = filter_target;
    }
    return command_result;
}

}  // namespace specforge
