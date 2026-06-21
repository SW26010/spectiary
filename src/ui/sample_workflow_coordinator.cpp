#include "ui/sample_workflow_coordinator.h"

#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"

#include <algorithm>
#include <cctype>
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

    navigation_.ActivateSource(std::move(*source_key), snapshot);
    SyncSampleWorkflowSession(snapshot, action);
    ApplySampleFilters(snapshot);
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
        ApplySampleFilters(nullptr);
        action.navigation_inputs_changed = true;
    }
    return action;
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
    return RequestSampleNavigation(SampleNavigationRequest::LocateRow(target_row), snapshot);
}

SourceCollectionSessionAction SampleWorkflowCoordinator::CreateDefaultLabelingTask()
{
    SourceCollectionSessionAction action;
    if (labeling_.CreateTask("manual-labeling", "Manual labeling") != nullptr) {
        ApplySampleFilters(nullptr);
        action.navigation_inputs_changed = true;
    }
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
        ApplySampleFilters(nullptr);
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
    }
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

SourceCollectionNavigationView SampleWorkflowCoordinator::NavigationView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionNavigationView view;
    view.current_index = navigation_.current_index();
    view.sample_count = navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.can_move_previous = navigation_.can_move_previous();
    view.can_move_next = navigation_.can_move_next();
    view.filter_active = navigation_.filter_active();
    view.filtered_sample_count = navigation_.filtered_sample_count();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    if (snapshot) {
        view.current_sample_display_name = snapshot->current_spectrum.name;
    }
    const std::size_t current_index = view.current_index.value_or(snapshot ? snapshot->collection.current_index : 0);
    if (const SourceCollectionManifest* context = navigation_.active_context()) {
        view.has_sample_names = !context->sample_names.empty();
        view.annotation_messages = context->messages;
        if (current_index < context->sample_names.size()) {
            view.current_sample_name = context->sample_names[current_index];
        }

        view.current_annotations.reserve(context->annotations.size());
        for (const SampleAnnotationResult& annotation : context->annotations) {
            SourceCollectionAnnotationValueView annotation_view;
            annotation_view.name = annotation.name;
            annotation_view.path = annotation.path;
            if (current_index < annotation.values.size()) {
                annotation_view.display_text = annotation.values[current_index].display_text;
            } else {
                annotation_view.missing = true;
            }
            view.current_annotations.push_back(std::move(annotation_view));
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
        view.output_path = task->output_path;
        view.save_state = task->save_state;
    }
    view.state_save_failed = labeling_.state_save_failed();
    view.state_save_error = std::string{labeling_.state_save_error()};
    view.state_load_warning = std::string{labeling_.state_load_warning()};
    return view;
}

SourceCollectionFilterView SampleWorkflowCoordinator::FilterView(const SpectrumSnapshotHandle& snapshot) const
{
    SourceCollectionFilterView view;
    view.sample_count = ActiveSampleCount(snapshot);
    view.has_active_source = snapshot && !snapshot->source.path.empty() && view.sample_count > 0;
    view.has_active_labeling_task = labeling_.active_task() != nullptr;
    view.active_labeling_filter_source_selected = active_labeling_filter_source_selected();
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    const std::vector<SampleFilterSource> filter_sources = BuildSampleFilterSources();
    view.evaluation = EvaluateFilterSources(filters_, filter_sources, view.sample_count);
    view.sources.reserve(filter_sources.size());
    for (const SampleFilterSource& source : filter_sources) {
        view.sources.push_back(BuildFilterSourceView(source, filters_));
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
        active_sample_workflow_identity_ = identity.id;
        action.workflow_changed = true;
    }
    labeling_.ActivateSource(identity.id, identity.spectrum_count);
}

void SampleWorkflowCoordinator::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    filters_.Clear();
    active_sample_workflow_identity_.reset();
    selected_labeling_filter_source_id_.reset();
    action.workflow_changed = true;
}

void SampleWorkflowCoordinator::ApplySampleFilters(const SpectrumSnapshotHandle& snapshot)
{
    const std::size_t sample_count = ActiveSampleCount(snapshot);
    if (sample_count == 0) {
        navigation_.ClearSampleFilter();
        return;
    }
    if (filters_.conditions().empty()) {
        navigation_.ClearSampleFilter();
        return;
    }

    const std::vector<SampleFilterSource> filter_sources = BuildSampleFilterSources();
    const SampleFilterEvaluation evaluation = filters_.Evaluate(filter_sources, sample_count);
    if (evaluation.active) {
        navigation_.SetSampleFilter(evaluation.included_samples);
    } else {
        navigation_.ClearSampleFilter();
    }
}

std::size_t SampleWorkflowCoordinator::ActiveSampleCount(const SpectrumSnapshotHandle& snapshot) const
{
    return navigation_.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
}

std::optional<std::size_t> SampleWorkflowCoordinator::ActiveSampleIndex(const SpectrumSnapshotHandle& snapshot) const
{
    if (std::optional<std::size_t> navigation_index = navigation_.current_index()) {
        return navigation_index;
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

    ApplySampleFilters(snapshot);
    command_result.action.navigation_inputs_changed = true;

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
