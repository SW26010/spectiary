#include "ui/sample_workflow_source_policy.h"

#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_sorting_sources.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace specforge {
namespace {

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

bool PathExists(const std::filesystem::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool IsAnnotationSourceId(std::string_view source_id)
{
    constexpr std::string_view kPrefix = "annotation:";
    return source_id.size() > kPrefix.size() && source_id.substr(0, kPrefix.size()) == kPrefix;
}

bool IsLabelingSourceId(std::string_view source_id)
{
    constexpr std::string_view kPrefix = "labeling:";
    return source_id.size() > kPrefix.size() && source_id.substr(0, kPrefix.size()) == kPrefix;
}

bool IsDefaultSampleSortSourceId(std::string_view source_id)
{
    return source_id == "source-order" || source_id == "sample-name";
}

bool HasSelectedSourceId(const std::vector<std::string>& source_ids, std::string_view source_id)
{
    return std::any_of(source_ids.begin(), source_ids.end(), [source_id](const std::string& selected_source_id) {
        return selected_source_id == source_id;
    });
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
    if (context == nullptr || !IsAnnotationSourceId(source_id)) {
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
    const SampleWorkflowSourceContext& context,
    std::string_view source_id)
{
    return HasAnnotationFilterSourceById(
               context.collection,
               context.labeling_tasks,
               context.sample_count,
               source_id) ||
           FindLabelingFilterSourceById(
               context.labeling_tasks,
               context.sample_count,
               source_id) != nullptr;
}

bool IsSampleSortSourceAvailable(
    const SampleWorkflowSourceContext& context,
    std::string_view source_id)
{
    return BuildSampleSortingSource(
               context.collection,
               context.labeling_tasks,
               context.sample_count,
               source_id)
        .has_value();
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

const SampleLabelingTask* FindLocalTaskByOutputPath(
    const std::vector<SampleLabelingTask>* tasks,
    const std::filesystem::path& path)
{
    if (tasks == nullptr || path.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(tasks->begin(), tasks->end(), [&path](const SampleLabelingTask& task) {
        return task.output_path && SampleWorkflowPathsReferToSameFile(*task.output_path, path);
    });
    return match == tasks->end() ? nullptr : &*match;
}

bool HasStoredState(const SampleWorkflowSourceState& state)
{
    return !state.filter_conditions.empty() ||
           !state.selected_filter_source_ids.empty() ||
           !state.selected_sample_sort_source_ids.empty() ||
           !state.annotation_display_names.empty() ||
           (state.selected_sample_sort_source_id && !state.selected_sample_sort_source_id->empty()) ||
           state.selected_sample_sort_direction != SampleNavigationSortDirection::Ascending;
}

}  // namespace

bool SampleWorkflowPathsReferToSameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
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

const SampleAnnotationResult* FindSampleWorkflowAnnotationByPath(
    const SourceCollectionManifest& context,
    const std::filesystem::path& path)
{
    const auto match = std::find_if(context.annotations.begin(), context.annotations.end(), [&path](const auto& annotation) {
        return SampleWorkflowPathsReferToSameFile(annotation.path, path);
    });
    return match == context.annotations.end() ? nullptr : &*match;
}

void SampleWorkflowSourcePolicy::Clear()
{
    filters_.Clear();
    selected_filter_source_ids_.clear();
    selected_sample_sort_source_ids_.clear();
    annotation_display_names_.clear();
    sample_sort_source_directions_.clear();
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
    InvalidateFilterViewCache();
    InvalidateSortingSourceCache();
}

void SampleWorkflowSourcePolicy::InvalidateFilterViewCache()
{
    filter_view_cache_valid_ = false;
}

void SampleWorkflowSourcePolicy::InvalidateSortingSourceCache()
{
    sorting_source_cache_valid_ = false;
}

bool SampleWorkflowSourcePolicy::RenameAnnotationDisplayName(
    const SampleWorkflowSourceContext& context,
    const std::filesystem::path& path,
    std::string display_name)
{
    display_name = TrimAscii(display_name);
    if (path.empty()) {
        return false;
    }

    const SampleAnnotationResult* annotation =
        context.collection == nullptr ? nullptr : FindSampleWorkflowAnnotationByPath(*context.collection, path);
    const SampleLabelingTask* local_task = annotation == nullptr
        ? FindLocalTaskByOutputPath(context.labeling_tasks, path)
        : FindLocalTaskForLoadedAnnotation(context.labeling_tasks, *annotation);
    if (annotation == nullptr && local_task == nullptr) {
        return false;
    }

    const std::string key = AnnotationDisplayNameKeyFromPath(
        local_task != nullptr && local_task->output_path ? *local_task->output_path : path);
    if (key.empty()) {
        return false;
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

    if (changed) {
        InvalidateFilterViewCache();
        InvalidateSortingSourceCache();
    }
    return changed;
}

std::string SampleWorkflowSourcePolicy::AnnotationDisplayName(
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

std::string SampleWorkflowSourcePolicy::LocalTaskAnnotationDisplayName(
    const SampleLabelingTask& task) const
{
    const std::string key = task.output_path ? AnnotationDisplayNameKeyFromPath(*task.output_path) : std::string{};
    const auto override = annotation_display_names_.find(key);
    if (override != annotation_display_names_.end()) {
        return override->second;
    }
    return task.task_name;
}

bool SampleWorkflowSourcePolicy::AddFilterSource(
    const SampleWorkflowSourceContext& context,
    std::string source_id)
{
    if (!IsSampleFilterSourceAvailable(context, source_id)) {
        return false;
    }
    if (!AddSelectedSourceId(selected_filter_source_ids_, std::move(source_id))) {
        return false;
    }
    InvalidateFilterViewCache();
    return true;
}

bool SampleWorkflowSourcePolicy::RemoveFilterSource(std::string_view source_id)
{
    const bool removed_source = RemoveSelectedSourceId(selected_filter_source_ids_, source_id);
    const std::string source_id_string{source_id};
    const bool had_condition = filters_.FindCondition(source_id_string) != nullptr;
    filters_.ClearCondition(source_id_string);
    if (!removed_source && !had_condition) {
        return false;
    }
    InvalidateFilterViewCache();
    return true;
}

void SampleWorkflowSourcePolicy::ClearFilters()
{
    filters_.Clear();
    InvalidateFilterViewCache();
}

bool SampleWorkflowSourcePolicy::SetFilterValueSelected(
    const SampleWorkflowSourceContext& context,
    std::string source_id,
    std::string value_key,
    bool selected)
{
    if (!IsSelectedFilterSource(source_id) || !IsSampleFilterSourceAvailable(context, source_id)) {
        return false;
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
    InvalidateFilterViewCache();
    return true;
}

bool SampleWorkflowSourcePolicy::has_filter_conditions() const
{
    return !filters_.conditions().empty();
}

SampleFilterEvaluation SampleWorkflowSourcePolicy::EvaluateFilters(
    const SampleWorkflowSourceContext& context) const
{
    return EvaluateFilterSources(filters_, BuildSelectedFilterSources(context), context.sample_count);
}

SourceCollectionFilterView SampleWorkflowSourcePolicy::BuildFilterView(
    const SampleWorkflowSourceContext& context) const
{
    SourceCollectionFilterView view = CachedFilterView(context);
    view.sample_count = context.sample_count;
    return view;
}

void SampleWorkflowSourcePolicy::ClearSampleSorting()
{
    selected_sample_sort_source_id_.reset();
    selected_sample_sort_direction_ = SampleNavigationSortDirection::Ascending;
    SetSampleSortSourceDirection("source-order", SampleNavigationSortDirection::Ascending);
}

bool SampleWorkflowSourcePolicy::AddSampleSortSource(
    const SampleWorkflowSourceContext& context,
    std::string source_id)
{
    if (IsDefaultSampleSortSourceId(source_id) || !IsSampleSortSourceAvailable(context, source_id)) {
        return false;
    }
    if (!AddSelectedSourceId(selected_sample_sort_source_ids_, std::move(source_id))) {
        return false;
    }
    SetSampleSortSourceDirection(selected_sample_sort_source_ids_.back(), SampleNavigationSortDirection::Ascending);
    return true;
}

bool SampleWorkflowSourcePolicy::RemoveSampleSortSource(std::string_view source_id)
{
    if (IsDefaultSampleSortSourceId(source_id) || !RemoveSelectedSampleSortSource(source_id)) {
        return false;
    }
    return true;
}

bool SampleWorkflowSourcePolicy::SetSampleSortSource(
    const SampleWorkflowSourceContext& context,
    std::string source_id)
{
    if (source_id.empty() || !IsSampleSortSourceAvailable(context, source_id)) {
        return false;
    }
    if (!IsDefaultSampleSortSourceId(source_id)) {
        (void)AddSelectedSourceId(selected_sample_sort_source_ids_, source_id);
    }
    selected_sample_sort_direction_ = SampleSortSourceDirection(source_id);
    selected_sample_sort_source_id_ = std::move(source_id);
    return true;
}

void SampleWorkflowSourcePolicy::SetSampleSortDirection(SampleNavigationSortDirection direction)
{
    selected_sample_sort_direction_ = direction;
    if (selected_sample_sort_source_id_) {
        SetSampleSortSourceDirection(*selected_sample_sort_source_id_, direction);
    } else {
        SetSampleSortSourceDirection("source-order", direction);
    }
}

SampleWorkflowSortChoiceResult SampleWorkflowSourcePolicy::BuildSortChoice(
    const SampleWorkflowSourceContext& context,
    bool remove_unavailable_active_source)
{
    SampleWorkflowSortChoiceResult result;
    if (context.sample_count == 0 || !selected_sample_sort_source_id_) {
        return result;
    }

    std::optional<SampleSortingSource> source = BuildSampleSortingSource(
        context.collection,
        context.labeling_tasks,
        context.sample_count,
        *selected_sample_sort_source_id_);
    if (!source) {
        if (remove_unavailable_active_source) {
            const std::string removed_source_id = *selected_sample_sort_source_id_;
            result.state_changed = RemoveSelectedSampleSortSource(removed_source_id);
        }
        return result;
    }

    SampleNavigationSortChoice sort_choice;
    sort_choice.active = true;
    sort_choice.direction = selected_sample_sort_direction_;
    sort_choice.values = std::move(source->values);
    result.choice = std::move(sort_choice);
    return result;
}

SourceCollectionSampleSortingView SampleWorkflowSourcePolicy::BuildSortingView(
    const SampleWorkflowSourceContext& context) const
{
    SourceCollectionSampleSortingView view;
    view.direction = selected_sample_sort_direction_;
    view.source_order_direction = SampleSortSourceDirection("source-order");
    const std::vector<SourceCollectionSampleSortSourceView>& all_sources =
        CachedSortingSourceViews(context);
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

void SampleWorkflowSourcePolicy::RestoreState(const SampleWorkflowSourceState& state)
{
    Clear();

    for (const std::string& source_id : state.selected_filter_source_ids) {
        if (IsAnnotationSourceId(source_id) || IsLabelingSourceId(source_id)) {
            (void)AddSelectedSourceId(selected_filter_source_ids_, source_id);
        }
    }
    for (const SampleFilterCondition& condition : state.filter_conditions) {
        if (IsAnnotationSourceId(condition.source_id) || IsLabelingSourceId(condition.source_id)) {
            filters_.SetCondition(condition.source_id, condition.allowed_value_keys);
            (void)AddSelectedSourceId(selected_filter_source_ids_, condition.source_id);
        }
    }
    for (const std::string& source_id : state.selected_sample_sort_source_ids) {
        if (!IsDefaultSampleSortSourceId(source_id)) {
            (void)AddSelectedSourceId(selected_sample_sort_source_ids_, source_id);
        }
    }
    for (const SampleAnnotationDisplayNameOverride& display_name : state.annotation_display_names) {
        if (IsAnnotationSourceId(display_name.source_id) && !display_name.display_name.empty()) {
            annotation_display_names_[display_name.source_id] = display_name.display_name;
        }
    }
    selected_sample_sort_source_id_ = state.selected_sample_sort_source_id;
    if (selected_sample_sort_source_id_ && !IsDefaultSampleSortSourceId(*selected_sample_sort_source_id_)) {
        (void)AddSelectedSourceId(selected_sample_sort_source_ids_, *selected_sample_sort_source_id_);
    }
    selected_sample_sort_direction_ = state.selected_sample_sort_direction;
    if (selected_sample_sort_source_id_) {
        SetSampleSortSourceDirection(*selected_sample_sort_source_id_, selected_sample_sort_direction_);
    }
    InvalidateFilterViewCache();
    InvalidateSortingSourceCache();
}

SampleWorkflowSourceState SampleWorkflowSourcePolicy::StoreState() const
{
    SampleWorkflowSourceState state;
    state.filter_conditions = filters_.conditions();
    state.selected_filter_source_ids = selected_filter_source_ids_;
    state.selected_sample_sort_source_ids = selected_sample_sort_source_ids_;
    state.annotation_display_names.reserve(annotation_display_names_.size());
    for (const auto& [source_id, display_name] : annotation_display_names_) {
        if (IsAnnotationSourceId(source_id) && !display_name.empty()) {
            state.annotation_display_names.push_back(
                SampleAnnotationDisplayNameOverride{source_id, display_name});
        }
    }
    state.selected_sample_sort_source_id = selected_sample_sort_source_id_;
    state.selected_sample_sort_direction = selected_sample_sort_direction_;
    return state;
}

bool SampleWorkflowSourcePolicy::HasState() const
{
    return HasStoredState(StoreState());
}

bool SampleWorkflowSourcePolicy::IsSelectedFilterSource(std::string_view source_id) const
{
    return HasSelectedSourceId(selected_filter_source_ids_, source_id);
}

bool SampleWorkflowSourcePolicy::IsSelectedSampleSortSource(std::string_view source_id) const
{
    return HasSelectedSourceId(selected_sample_sort_source_ids_, source_id);
}

SampleNavigationSortDirection SampleWorkflowSourcePolicy::SampleSortSourceDirection(
    std::string_view source_id) const
{
    const auto match = sample_sort_source_directions_.find(std::string(source_id));
    if (match != sample_sort_source_directions_.end()) {
        return match->second;
    }
    return SampleNavigationSortDirection::Ascending;
}

void SampleWorkflowSourcePolicy::SetSampleSortSourceDirection(
    std::string_view source_id,
    SampleNavigationSortDirection direction)
{
    if (source_id.empty()) {
        return;
    }
    sample_sort_source_directions_[std::string(source_id)] = direction;
}

bool SampleWorkflowSourcePolicy::RemoveSelectedSampleSortSource(std::string_view source_id)
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

std::vector<SampleFilterSource> SampleWorkflowSourcePolicy::BuildSelectedFilterSources(
    const SampleWorkflowSourceContext& context) const
{
    std::vector<SampleFilterSource> filter_sources;
    if (context.collection == nullptr || context.sample_count == 0) {
        return filter_sources;
    }

    filter_sources.reserve(selected_filter_source_ids_.size());
    std::unordered_set<std::string> emitted_labeling_source_ids;
    for (const SampleAnnotationResult& annotation : context.collection->annotations) {
        if (const SampleLabelingTask* local_task =
                FindLocalTaskForLoadedAnnotation(context.labeling_tasks, annotation)) {
            if (!IsLabelingSampleFilterCandidate(*local_task, context.sample_count)) {
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
        if (!IsAnnotationSampleFilterCandidate(context.labeling_tasks, annotation, context.sample_count)) {
            continue;
        }
        SampleFilterSource source = BuildAnnotationFilterSource(annotation);
        source.name = AnnotationDisplayName(annotation);
        if (IsSelectedFilterSource(source.id)) {
            filter_sources.push_back(std::move(source));
        }
    }
    if (context.labeling_tasks != nullptr) {
        for (const SampleLabelingTask& task : *context.labeling_tasks) {
            if (!task.output_path || !IsLabelingSampleFilterCandidate(task, context.sample_count)) {
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

const SourceCollectionFilterView& SampleWorkflowSourcePolicy::CachedFilterView(
    const SampleWorkflowSourceContext& context) const
{
    if (!filter_view_cache_valid_ ||
        filter_view_cache_context_ != context.collection ||
        filter_view_cache_sample_count_ != context.sample_count) {
        SourceCollectionFilterView view;
        view.sample_count = context.sample_count;
        const std::vector<SampleFilterSource> filter_sources = BuildSelectedFilterSources(context);
        view.evaluation = EvaluateFilterSources(filters_, filter_sources, context.sample_count);
        view.evaluation.included_samples.clear();
        if (context.collection != nullptr && context.sample_count > 0) {
            std::unordered_set<std::string> emitted_labeling_source_ids;
            for (const SampleAnnotationResult& annotation : context.collection->annotations) {
                if (const SampleLabelingTask* local_task =
                        FindLocalTaskForLoadedAnnotation(context.labeling_tasks, annotation)) {
                    if (!IsLabelingSampleFilterCandidate(*local_task, context.sample_count)) {
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
                        context.labeling_tasks,
                        annotation,
                        context.sample_count)) {
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
            if (context.labeling_tasks != nullptr) {
                for (const SampleLabelingTask& task : *context.labeling_tasks) {
                    if (!task.output_path || !IsLabelingSampleFilterCandidate(task, context.sample_count)) {
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
        filter_view_cache_ = std::move(view);
        filter_view_cache_context_ = context.collection;
        filter_view_cache_sample_count_ = context.sample_count;
        filter_view_cache_valid_ = true;
    }
    return filter_view_cache_;
}

const std::vector<SourceCollectionSampleSortSourceView>&
SampleWorkflowSourcePolicy::CachedSortingSourceViews(
    const SampleWorkflowSourceContext& context) const
{
    if (!sorting_source_cache_valid_ ||
        sorting_source_cache_context_ != context.collection ||
        sorting_source_cache_sample_count_ != context.sample_count) {
        sorting_source_cache_ = BuildSampleSortingSourceViews(
            context.collection,
            context.labeling_tasks,
            context.sample_count);
        if (context.collection != nullptr) {
            for (SourceCollectionSampleSortSourceView& source_view : sorting_source_cache_) {
                if (source_view.annotation_path.empty()) {
                    continue;
                }
                if (const SampleAnnotationResult* annotation =
                        FindSampleWorkflowAnnotationByPath(*context.collection, source_view.annotation_path)) {
                    source_view.name = AnnotationDisplayName(*annotation);
                }
            }
        }
        sorting_source_cache_context_ = context.collection;
        sorting_source_cache_sample_count_ = context.sample_count;
        sorting_source_cache_valid_ = true;
    }
    return sorting_source_cache_;
}

}  // namespace specforge
