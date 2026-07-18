#pragma once

#include "domain/sample_filter.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

struct SampleWorkflowSourceContext {
    const SourceCollectionManifest* collection = nullptr;
    const std::vector<SampleLabelingTask>* labeling_tasks = nullptr;
    std::size_t sample_count = 0;
};

struct SampleWorkflowSortChoiceResult {
    std::optional<SampleNavigationSortChoice> choice;
    bool state_changed = false;
};

[[nodiscard]] bool SampleWorkflowPathsReferToSameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right);
[[nodiscard]] const SampleAnnotationResult* FindSampleWorkflowAnnotationByPath(
    const SourceCollectionManifest& context,
    const std::filesystem::path& path);

class SampleWorkflowSourcePolicy {
public:
    void Clear();
    void InvalidateFilterViewCache();
    void InvalidateSortingSourceCache();

    [[nodiscard]] bool RenameAnnotationDisplayName(
        const SampleWorkflowSourceContext& context,
        const std::filesystem::path& path,
        std::string display_name);
    [[nodiscard]] std::string AnnotationDisplayName(
        const SampleAnnotationResult& annotation,
        const SampleLabelingTask* local_task = nullptr) const;
    [[nodiscard]] std::string LocalTaskAnnotationDisplayName(const SampleLabelingTask& task) const;

    [[nodiscard]] bool AddFilterSource(
        const SampleWorkflowSourceContext& context,
        std::string source_id);
    [[nodiscard]] bool RemoveFilterSource(std::string_view source_id);
    void ClearFilters();
    [[nodiscard]] bool SetFilterValueSelected(
        const SampleWorkflowSourceContext& context,
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] bool RemoveSampleFilterValue(
        std::string_view source_id,
        std::string_view value_key);
    [[nodiscard]] bool ReplaceSampleFilterValue(
        std::string_view source_id,
        std::string_view old_value_key,
        std::string_view new_value_key);
    [[nodiscard]] bool has_filter_conditions() const;
    [[nodiscard]] SampleFilterEvaluation EvaluateFilters(
        const SampleWorkflowSourceContext& context) const;
    [[nodiscard]] SourceCollectionFilterView BuildFilterView(
        const SampleWorkflowSourceContext& context) const;

    void ClearSampleSorting();
    [[nodiscard]] bool AddSampleSortSource(
        const SampleWorkflowSourceContext& context,
        std::string source_id);
    [[nodiscard]] bool RemoveSampleSortSource(std::string_view source_id);
    [[nodiscard]] bool SetSampleSortSource(
        const SampleWorkflowSourceContext& context,
        std::string source_id);
    void SetSampleSortDirection(SampleNavigationSortDirection direction);
    [[nodiscard]] SampleWorkflowSortChoiceResult BuildSortChoice(
        const SampleWorkflowSourceContext& context,
        bool remove_unavailable_active_source);
    [[nodiscard]] SourceCollectionSampleSortingView BuildSortingView(
        const SampleWorkflowSourceContext& context) const;

    void RestoreState(const SampleWorkflowSourceState& state);
    [[nodiscard]] SampleWorkflowSourceState StoreState() const;
    [[nodiscard]] bool HasState() const;

private:
    [[nodiscard]] bool IsSelectedFilterSource(std::string_view source_id) const;
    [[nodiscard]] bool IsSelectedSampleSortSource(std::string_view source_id) const;
    [[nodiscard]] SampleNavigationSortDirection SampleSortSourceDirection(
        std::string_view source_id) const;
    void SetSampleSortSourceDirection(
        std::string_view source_id,
        SampleNavigationSortDirection direction);
    [[nodiscard]] bool RemoveSelectedSampleSortSource(std::string_view source_id);
    [[nodiscard]] std::vector<SampleFilterSource> BuildSelectedFilterSources(
        const SampleWorkflowSourceContext& context) const;
    [[nodiscard]] const SourceCollectionFilterView& CachedFilterView(
        const SampleWorkflowSourceContext& context) const;
    [[nodiscard]] const std::vector<SourceCollectionSampleSortSourceView>& CachedSortingSourceViews(
        const SampleWorkflowSourceContext& context) const;

    SampleFilterController filters_;
    std::vector<std::string> selected_filter_source_ids_;
    std::vector<std::string> selected_sample_sort_source_ids_;
    std::unordered_map<std::string, std::string> annotation_display_names_;
    std::unordered_map<std::string, SampleNavigationSortDirection> sample_sort_source_directions_;
    std::optional<std::string> selected_sample_sort_source_id_;
    SampleNavigationSortDirection selected_sample_sort_direction_ =
        SampleNavigationSortDirection::Ascending;

    mutable bool sorting_source_cache_valid_ = false;
    mutable std::size_t sorting_source_cache_sample_count_ = 0;
    mutable const SourceCollectionManifest* sorting_source_cache_context_ = nullptr;
    mutable std::vector<SourceCollectionSampleSortSourceView> sorting_source_cache_;
    mutable bool filter_view_cache_valid_ = false;
    mutable std::size_t filter_view_cache_sample_count_ = 0;
    mutable const SourceCollectionManifest* filter_view_cache_context_ = nullptr;
    mutable SourceCollectionFilterView filter_view_cache_;
};

}  // namespace specforge
