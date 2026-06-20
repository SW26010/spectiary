#include "ui/source_collection_session.h"

#include "domain/sample_annotation_io.h"
#include "domain/spectrum_fixture.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string SourceKey(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, error);
    return LowerAscii(PathToUtf8(error ? path : absolute_path));
}

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        if (metadata[index].key == key) {
            return metadata[index].value;
        }
    }
    return {};
}

bool HasDiagnosticAtLeast(const SpectrumSnapshotHandle& snapshot, SpectrumDiagnosticSeverity minimum)
{
    if (!snapshot) {
        return false;
    }
    const auto rank = [](SpectrumDiagnosticSeverity severity) {
        switch (severity) {
        case SpectrumDiagnosticSeverity::Error:
            return 2;
        case SpectrumDiagnosticSeverity::Warning:
            return 1;
        case SpectrumDiagnosticSeverity::Info:
        default:
            return 0;
        }
    };
    const int minimum_rank = rank(minimum);
    return std::any_of(snapshot->diagnostics.begin(), snapshot->diagnostics.end(), [rank, minimum_rank](const auto& d) {
        return rank(d.severity) >= minimum_rank;
    });
}

std::string_view SourceStateLabel(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "none";
    }
    if (HasDiagnosticAtLeast(snapshot, SpectrumDiagnosticSeverity::Error)) {
        return "error";
    }
    if (snapshot->capabilities.can_plot_current_spectrum) {
        return snapshot->diagnostics.empty() ? "loaded" : "loaded with diagnostics";
    }
    return "not plottable";
}

std::string SnapshotDisplayNameText(const SpectrumSnapshotHandle& snapshot, const std::filesystem::path& path)
{
    if (snapshot && !snapshot->source.display_name.empty()) {
        return snapshot->source.display_name;
    }
    return FileNameToUtf8(path);
}

std::string SnapshotTypeLabelText(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "unknown";
    }

    const std::string_view format = MetadataValue(snapshot->source.metadata, "format");
    if (!format.empty()) {
        return std::string{format};
    }

    const std::string_view source_type = MetadataValue(snapshot->source.metadata, "source_type");
    return source_type.empty() ? std::string{"unknown"} : std::string{source_type};
}

std::string SnapshotStateLabelText(const SpectrumSnapshotHandle& snapshot)
{
    return std::string{SourceStateLabel(snapshot)};
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

}  // namespace

SourceCollectionSessionCommand SourceCollectionSessionCommand::OpenSource(
    std::filesystem::path path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::OpenSource;
    command.path = std::move(path);
    command.spectrum_index = spectrum_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ActivateSource(std::size_t source_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ActivateSource;
    command.source_index = source_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::RemoveSource(std::size_t source_index)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::RemoveSource;
    command.source_index = source_index;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::NavigateSample(SampleNavigationRequest request)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::NavigateSample;
    command.navigation_request = std::move(request);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::AddReadOnlyAnnotation(std::filesystem::path path)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::AddReadOnlyAnnotation;
    command.path = std::move(path);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetSampleNameQuery(std::string query)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetSampleNameQuery;
    command.query = std::move(query);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::CommitSampleNameSelection;
    command.target_row = target_row;
    command.matched_name = std::move(matched_name);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::CreateDefaultLabelingTask()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::CreateDefaultLabelingTask;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::UpsertActiveLabel(SampleLabelDefinition label)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::UpsertActiveLabel;
    command.label = std::move(label);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingAutoAdvance(bool enabled)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingAutoAdvance;
    command.enabled = enabled;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingSkipLabeledOnAdvance;
    command.enabled = enabled;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingOutputPath(
    std::filesystem::path output_path)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingOutputPath;
    command.path = std::move(output_path);
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::AssignActiveLabelToCurrentSample(int code)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::AssignActiveLabelToCurrentSample;
    command.label_code = code;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ClearActiveLabelForCurrentSample()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ClearActiveLabelForCurrentSample;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::ClearFilters()
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::ClearFilters;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetFilterValueSelected(
    std::string source_id,
    std::string value_key,
    bool selected)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetFilterValueSelected;
    command.filter_source_id = std::move(source_id);
    command.filter_value_key = std::move(value_key);
    command.selected = selected;
    return command;
}

SourceCollectionSessionCommand SourceCollectionSessionCommand::SetActiveLabelingFilterSourceSelected(bool selected)
{
    SourceCollectionSessionCommand command;
    command.kind = SourceCollectionSessionCommandKind::SetActiveLabelingFilterSourceSelected;
    command.selected = selected;
    return command;
}

SourceCollectionSession::SourceCollectionSession(SnapshotLoader snapshot_loader)
    : snapshot_loader_(std::move(snapshot_loader)),
      snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      snapshot_loader_(std::move(snapshot_loader)),
      snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
}

SourceCollectionSessionResult SourceCollectionSession::Submit(SourceCollectionSessionCommand command)
{
    SourceCollectionSessionResult result;
    switch (command.kind) {
    case SourceCollectionSessionCommandKind::OpenSource:
        result.action = OpenSource(command.path, command.spectrum_index);
        break;
    case SourceCollectionSessionCommandKind::ActivateSource:
        result.action = ActivateSource(command.source_index);
        break;
    case SourceCollectionSessionCommandKind::RemoveSource:
        result.action = RemoveSource(command.source_index);
        break;
    case SourceCollectionSessionCommandKind::NavigateSample:
        result.action = RequestSampleNavigation(command.navigation_request, &result.navigation);
        break;
    case SourceCollectionSessionCommandKind::AddReadOnlyAnnotation:
        result.action = AddReadOnlyAnnotationToActiveSource(command.path, &result.loaded, &result.message);
        break;
    case SourceCollectionSessionCommandKind::SetSampleNameQuery:
        result.action = SetSampleNameQuery(std::move(command.query));
        break;
    case SourceCollectionSessionCommandKind::CommitSampleNameSelection:
        result.action =
            CommitSampleNameSelection(command.target_row, std::move(command.matched_name), &result.navigation);
        break;
    case SourceCollectionSessionCommandKind::CreateDefaultLabelingTask:
        result.action = CreateDefaultLabelingTask();
        break;
    case SourceCollectionSessionCommandKind::UpsertActiveLabel:
        result.action = UpsertActiveLabel(std::move(command.label), &result.changed);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingAutoAdvance:
        result.action = SetActiveLabelingAutoAdvance(command.enabled);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingSkipLabeledOnAdvance:
        result.action = SetActiveLabelingSkipLabeledOnAdvance(command.enabled);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingOutputPath:
        result.action = SetActiveLabelingOutputPath(std::move(command.path));
        break;
    case SourceCollectionSessionCommandKind::AssignActiveLabelToCurrentSample:
        result.action = AssignActiveLabelToCurrentSample(command.label_code);
        break;
    case SourceCollectionSessionCommandKind::ClearActiveLabelForCurrentSample:
        result.action = ClearActiveLabelForCurrentSample();
        break;
    case SourceCollectionSessionCommandKind::ClearFilters:
        result.action = ClearFilters();
        break;
    case SourceCollectionSessionCommandKind::SetFilterValueSelected:
        result.action = SetFilterValueSelected(
            std::move(command.filter_source_id),
            std::move(command.filter_value_key),
            command.selected);
        break;
    case SourceCollectionSessionCommandKind::SetActiveLabelingFilterSourceSelected:
        result.action = SetActiveLabelingFilterSourceSelected(command.selected);
        break;
    }
    result.view = View();
    return result;
}

SourceCollectionSessionView SourceCollectionSession::View() const
{
    SourceCollectionSessionView view;
    view.snapshot = snapshot_;
    view.current_source_index = current_source_index_;
    view.sources.reserve(sources_.size());
    for (const SourceListEntry& entry : sources_) {
        SourceCollectionSourceView source_view;
        source_view.path = entry.path;
        source_view.display_name = entry.display_name;
        source_view.type_label = entry.type_label;
        source_view.state_label = entry.state_label;
        view.sources.push_back(std::move(source_view));
    }
    view.can_add_read_only_annotation = can_add_read_only_annotation();
    view.navigation = NavigationView();
    view.labeling = LabelingView();
    view.filter = FilterView();
    return view;
}

SourceCollectionSessionAction SourceCollectionSession::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionAction action;
    SpectrumSnapshotHandle loaded_snapshot = snapshot_loader_(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot), action);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ActivateSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    if (source_index >= sources_.size()) {
        return action;
    }

    SourceListEntry& entry = sources_[source_index];
    current_source_index_ = source_index;
    SetSnapshot(entry.cached_snapshot, action);
    MergeSourceCollectionSessionAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    if (source_index >= sources_.size()) {
        return action;
    }

    const bool removed_current = current_source_index_ && *current_source_index_ == source_index;
    navigation_.RemoveSource(sources_[source_index].key);
    std::optional<std::size_t> next_current_index;
    if (removed_current && sources_.size() > 1) {
        next_current_index = source_index + 1 < sources_.size() ? source_index : source_index - 1;
    }

    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(source_index));

    if (removed_current) {
        current_source_index_.reset();
        ClearSampleWorkflow(action);
        if (next_current_index) {
            MergeSourceCollectionSessionAction(action, ActivateSource(*next_current_index));
        } else {
            navigation_.ClearActiveSource();
            SetSnapshot(MakeSmallSyntheticSpectrumSnapshot(), action);
            action.navigation_inputs_changed = true;
        }
        return action;
    }

    if (current_source_index_ && *current_source_index_ > source_index) {
        current_source_index_ = *current_source_index_ - 1;
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RequestSampleNavigation(
    const SampleNavigationRequest& request,
    SampleNavigationResult* navigation_result)
{
    SourceCollectionSessionAction action;
    const SampleNavigationResult navigation = navigation_.Navigate(request);
    if (navigation_result != nullptr) {
        *navigation_result = navigation;
    }
    if (navigation.has_active_source && navigation.target_found && ShouldRememberLabelingPosition(request.kind)) {
        (void)labeling_.RememberActivePosition(navigation.current_index);
    }
    if (navigation.has_active_source && navigation.target_found) {
        if (!snapshot_ || snapshot_->collection.current_index != navigation.current_index) {
            MergeSourceCollectionSessionAction(action, LoadActiveSourceAt(navigation.current_index));
        } else {
            action.navigation_inputs_changed = true;
        }
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AddReadOnlyAnnotationToActiveSource(
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
        ApplySampleFilters();
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetSampleNameQuery(std::string query)
{
    navigation_.SetSampleNameQuery(std::move(query));
    return {};
}

SourceCollectionSessionAction SourceCollectionSession::CommitSampleNameSelection(
    std::size_t target_row,
    std::string matched_name,
    SampleNavigationResult* navigation_result)
{
    navigation_.SetSampleNameQuery(std::move(matched_name));
    return RequestSampleNavigation(SampleNavigationRequest::LocateRow(target_row), navigation_result);
}

SourceCollectionSessionAction SourceCollectionSession::CreateDefaultLabelingTask()
{
    SourceCollectionSessionAction action;
    if (labeling_.CreateTask("manual-labeling", "Manual labeling") != nullptr) {
        ApplySampleFilters();
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::UpsertActiveLabel(SampleLabelDefinition label, bool* changed)
{
    SourceCollectionSessionAction action;
    const bool label_changed = labeling_.UpsertActiveLabel(std::move(label));
    if (changed != nullptr) {
        *changed = label_changed;
    }
    if (label_changed) {
        ApplySampleFilters();
        action.navigation_inputs_changed = true;
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingAutoAdvance(bool enabled)
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

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingSkipLabeledOnAdvance(bool enabled)
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

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingOutputPath(std::filesystem::path output_path)
{
    SourceCollectionSessionAction action;
    if (labeling_.SetActiveTaskOutputPath(std::move(output_path))) {
        (void)labeling_.PersistActiveTask();
    }
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::AssignActiveLabelToCurrentSample(int code)
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex();
    if (!sample_index) {
        return {};
    }
    return ApplyLabelWriteResult(labeling_.AssignLabel(*sample_index, code));
}

SourceCollectionSessionAction SourceCollectionSession::ClearActiveLabelForCurrentSample()
{
    const std::optional<std::size_t> sample_index = ActiveSampleIndex();
    if (!sample_index) {
        return {};
    }
    return ApplyLabelWriteResult(labeling_.ClearLabel(*sample_index));
}

void SourceCollectionSession::ApplySampleFilters()
{
    const std::size_t sample_count = ActiveSampleCount();
    if (sample_count == 0) {
        navigation_.ClearSampleFilter();
        return;
    }

    const SampleFilterEvaluation evaluation = filters_.Evaluate(BuildSampleFilterSources(), sample_count);
    if (evaluation.active) {
        navigation_.SetSampleFilter(evaluation.included_samples);
    } else {
        navigation_.ClearSampleFilter();
    }
}

SampleFilterEvaluation SourceCollectionSession::EvaluateSampleFilters() const
{
    const std::size_t sample_count = ActiveSampleCount();
    if (sample_count == 0) {
        return {};
    }
    return filters_.Evaluate(BuildSampleFilterSources(), sample_count);
}

SourceCollectionSessionAction SourceCollectionSession::ClearFilters()
{
    SourceCollectionSessionAction action;
    filters_.Clear();
    ApplySampleFilters();
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetFilterValueSelected(
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
    ApplySampleFilters();
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::SetActiveLabelingFilterSourceSelected(bool selected)
{
    SourceCollectionSessionAction action;
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr) {
        if (selected_labeling_filter_source_id_) {
            filters_.ClearCondition(*selected_labeling_filter_source_id_);
        }
        selected_labeling_filter_source_id_.reset();
        ApplySampleFilters();
        action.navigation_inputs_changed = true;
        return action;
    }

    const SampleFilterSource active_labeling_source = BuildLabelingFilterSource(*task);
    if (selected_labeling_filter_source_id_ && *selected_labeling_filter_source_id_ != active_labeling_source.id) {
        filters_.ClearCondition(*selected_labeling_filter_source_id_);
    }

    if (selected) {
        selected_labeling_filter_source_id_ = active_labeling_source.id;
    } else {
        filters_.ClearCondition(active_labeling_source.id);
        selected_labeling_filter_source_id_.reset();
    }
    ApplySampleFilters();
    action.navigation_inputs_changed = true;
    return action;
}

bool SourceCollectionSession::active_labeling_filter_source_selected() const
{
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr || !selected_labeling_filter_source_id_) {
        return false;
    }
    return *selected_labeling_filter_source_id_ == BuildLabelingFilterSource(*task).id;
}

SourceCollectionNavigationView SourceCollectionSession::NavigationView() const
{
    SourceCollectionNavigationView view;
    view.current_index = navigation_.current_index();
    view.sample_count = navigation_.spectrum_count().value_or(snapshot_ ? snapshot_->collection.spectrum_count : 0);
    view.has_active_source = snapshot_ && !snapshot_->source.path.empty() && view.sample_count > 0;
    view.can_move_previous = navigation_.can_move_previous();
    view.can_move_next = navigation_.can_move_next();
    view.filter_active = navigation_.filter_active();
    view.filtered_sample_count = navigation_.filtered_sample_count();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    if (const SampleCollectionContext* context = navigation_.active_context()) {
        view.has_sample_names = !context->sample_names.empty();
        view.annotation_messages = context->messages;

        const std::size_t current_index =
            view.current_index.value_or(snapshot_ ? snapshot_->collection.current_index : 0);
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

SourceCollectionLabelingView SourceCollectionSession::LabelingView() const
{
    SourceCollectionLabelingView view;
    view.has_active_source = snapshot_ && !snapshot_->source.path.empty() && ActiveSampleCount() > 0;
    view.current_index = ActiveSampleIndex();
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

SourceCollectionFilterView SourceCollectionSession::FilterView() const
{
    SourceCollectionFilterView view;
    view.sample_count = ActiveSampleCount();
    view.has_active_source = snapshot_ && !snapshot_->source.path.empty() && view.sample_count > 0;
    view.has_active_labeling_task = labeling_.active_task() != nullptr;
    view.active_labeling_filter_source_selected = active_labeling_filter_source_selected();
    view.navigation_filter_active = navigation_.filter_active();
    view.current_sample_in_filter = navigation_.current_sample_in_filter();
    view.evaluation = EvaluateSampleFilters();

    std::vector<SampleFilterSource> sources = BuildSampleFilterSources();
    view.sources.reserve(sources.size());
    for (SampleFilterSource& source : sources) {
        SourceCollectionFilterSourceView source_view;
        if (const SampleFilterCondition* condition = filters_.FindCondition(source.id)) {
            source_view.selected_value_keys = condition->allowed_value_keys;
        }
        source_view.id = std::move(source.id);
        source_view.name = std::move(source.name);
        source_view.filterable = source.filterable;
        source_view.options = std::move(source.options);
        view.sources.push_back(std::move(source_view));
    }
    return view;
}

std::vector<SampleFilterSource> SourceCollectionSession::BuildSampleFilterSources() const
{
    std::vector<SampleFilterSource> filter_sources;
    const SampleCollectionContext* context = navigation_.active_context();
    if (context != nullptr) {
        filter_sources.reserve(context->annotations.size() + 1);
        for (const SampleAnnotationResult& annotation : context->annotations) {
            filter_sources.push_back(BuildAnnotationFilterSource(annotation));
        }
    }

    if (const SampleLabelingTask* task = labeling_.active_task()) {
        SampleFilterSource labeling_source = BuildLabelingFilterSource(*task);
        if (selected_labeling_filter_source_id_ && *selected_labeling_filter_source_id_ == labeling_source.id) {
            filter_sources.push_back(std::move(labeling_source));
        }
    }
    return filter_sources;
}

bool SourceCollectionSession::can_add_read_only_annotation() const
{
    return navigation_.active_context() != nullptr && navigation_.spectrum_count().value_or(0) > 0;
}

void SourceCollectionSession::MaybeSaveStateCaches(std::uint64_t frame_index)
{
    labeling_.MaybeSaveStateCache(frame_index);
}

bool SourceCollectionSession::FlushStateCaches()
{
    return labeling_.FlushStateCache();
}

const SourceCollectionSession::SourceListEntry* SourceCollectionSession::current_source() const
{
    if (!current_source_index_ || *current_source_index_ >= sources_.size()) {
        return nullptr;
    }
    return &sources_[*current_source_index_];
}

std::size_t SourceCollectionSession::AddOrUpdateSource(
    const std::filesystem::path& path,
    SpectrumSnapshotHandle snapshot,
    std::size_t spectrum_index)
{
    const std::string key = SourceKey(path);
    const auto match = std::find_if(sources_.begin(), sources_.end(), [&key](const SourceListEntry& entry) {
        return entry.key == key;
    });
    if (match != sources_.end()) {
        match->path = path;
        match->display_name = SnapshotDisplayNameText(snapshot, path);
        match->type_label = SnapshotTypeLabelText(snapshot);
        match->state_label = SnapshotStateLabelText(snapshot);
        match->cached_snapshot = std::move(snapshot);
        match->last_spectrum_index = spectrum_index;
        return static_cast<std::size_t>(std::distance(sources_.begin(), match));
    }

    SourceListEntry entry;
    entry.path = path;
    entry.key = key;
    entry.display_name = SnapshotDisplayNameText(snapshot, path);
    entry.type_label = SnapshotTypeLabelText(snapshot);
    entry.state_label = SnapshotStateLabelText(snapshot);
    entry.cached_snapshot = std::move(snapshot);
    entry.last_spectrum_index = spectrum_index;
    sources_.push_back(std::move(entry));
    return sources_.size() - 1;
}

SourceCollectionSessionAction SourceCollectionSession::EnsureSnapshotMatchesNavigation()
{
    SourceCollectionSessionAction action;
    SyncSampleNavigationSession(action);
    const std::optional<std::size_t> navigation_index = navigation_.current_index();
    if (navigation_index && snapshot_ && snapshot_->collection.spectrum_count > 0 &&
        snapshot_->collection.current_index != *navigation_index) {
        MergeSourceCollectionSessionAction(action, LoadActiveSourceAt(*navigation_index));
        return action;
    }
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::LoadActiveSourceAt(std::size_t spectrum_index)
{
    SourceCollectionSessionAction action;
    const SourceListEntry* source = current_source();
    if (source == nullptr || source->path.empty()) {
        return action;
    }

    const std::filesystem::path path = source->path;
    SpectrumSnapshotHandle loaded_snapshot = snapshot_loader_(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot), action);
    SyncSampleNavigationSession(action);
    action.navigation_inputs_changed = true;
    return action;
}

void SourceCollectionSession::SyncSampleNavigationSession(SourceCollectionSessionAction& action)
{
    const bool has_active_source =
        current_source_index_ && *current_source_index_ < sources_.size() && snapshot_ && !snapshot_->source.path.empty();
    if (!has_active_source) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return;
    }

    navigation_.ActivateSource(sources_[*current_source_index_].key, snapshot_);
    SyncSampleWorkflowSession(action);
    ApplySampleFilters();
}

void SourceCollectionSession::SyncSampleWorkflowSession(SourceCollectionSessionAction& action)
{
    if (!snapshot_ || snapshot_->source.path.empty() || snapshot_->collection.spectrum_count == 0) {
        ClearSampleWorkflow(action);
        return;
    }

    const SampleCollectionIdentity identity = BuildSampleCollectionIdentity(*snapshot_);
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        filters_.Clear();
        selected_labeling_filter_source_id_.reset();
        active_sample_workflow_identity_ = identity.id;
        action.workflow_changed = true;
    }
    labeling_.ActivateSource(identity.id, identity.spectrum_count);
}

void SourceCollectionSession::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    filters_.Clear();
    active_sample_workflow_identity_.reset();
    selected_labeling_filter_source_id_.reset();
    action.workflow_changed = true;
}

void SourceCollectionSession::SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action)
{
    snapshot_ = std::move(snapshot);
    action.snapshot_changed = true;
}

std::size_t SourceCollectionSession::ActiveSampleCount() const
{
    return navigation_.spectrum_count().value_or(snapshot_ ? snapshot_->collection.spectrum_count : 0);
}

std::optional<std::size_t> SourceCollectionSession::ActiveSampleIndex() const
{
    if (std::optional<std::size_t> navigation_index = navigation_.current_index()) {
        return navigation_index;
    }
    if (snapshot_ && !snapshot_->source.path.empty() && snapshot_->collection.spectrum_count > 0) {
        return snapshot_->collection.current_index;
    }
    return std::nullopt;
}

SourceCollectionSessionAction SourceCollectionSession::ApplyLabelWriteResult(const SampleLabelWriteResult& result)
{
    SourceCollectionSessionAction action;
    if (!result.changed) {
        return action;
    }

    SampleLabelingTask* task = labeling_.active_task();
    if (task != nullptr && task->output_path) {
        (void)labeling_.PersistActiveTask();
    }

    ApplySampleFilters();
    action.navigation_inputs_changed = true;

    if (result.advance_requested && task != nullptr) {
        MergeSourceCollectionSessionAction(action, RequestSampleNavigation(BuildAutoAdvanceRequest(*task)));
    }
    return action;
}

}  // namespace specforge
