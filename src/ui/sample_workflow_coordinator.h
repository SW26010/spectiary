#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "app/local_user_state.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

struct SampleWorkflowCommandResult {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    std::optional<std::size_t> snapshot_index_to_load;
};

class SampleWorkflowCoordinator {
public:
    SampleWorkflowCoordinator();
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path);

    [[nodiscard]] SourceCollectionSessionAction SyncActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveWorkflow();
    void BeginRestoringSourceSession();
    void EndRestoringSourceSession();
    void RemoveSource(std::string_view source_key);

    [[nodiscard]] SampleWorkflowCommandResult RequestSampleNavigation(
        const SampleNavigationRequest& request,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        bool* loaded = nullptr,
        std::string* message = nullptr);
    [[nodiscard]] SourceCollectionSessionAction RemoveReadOnlyAnnotationFromActiveSource(
        const std::filesystem::path& path);
    [[nodiscard]] SourceCollectionSessionAction RenameAnnotationDisplayNameForActiveSource(
        std::filesystem::path path,
        std::string display_name);
    [[nodiscard]] bool RestoreReadOnlyAnnotationsForActiveSource(
        const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::filesystem::path>>
        AnnotationPathsBySourceKey() const;
    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SampleWorkflowCommandResult CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction CreateDefaultLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction CreateLabelingTask(std::string task_name);
    [[nodiscard]] SourceCollectionSessionAction ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] SourceCollectionSessionAction RenameActiveLabelingTask(std::string task_name);
    [[nodiscard]] SourceCollectionSessionAction DeleteActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction DeactivateActiveLabelingTask();
    [[nodiscard]] SampleWorkflowCommandResult AssignActiveLabelToCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        int code);
    [[nodiscard]] SampleWorkflowCommandResult ClearActiveLabelForCurrentSample(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction ClearFilters(const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction AddFilterSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction RemoveFilterSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetFilterValueSelected(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SourceCollectionSessionAction ClearSampleSorting(const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction AddSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction RemoveSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetSampleSortDirection(
        const SpectrumSnapshotHandle& snapshot,
        SampleNavigationSortDirection direction);

    [[nodiscard]] SourceCollectionNavigationView NavigationView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionLabelingView LabelingView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionFilterView FilterView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionSampleSortingView SortingView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] bool can_add_read_only_annotation() const;
    [[nodiscard]] std::optional<std::size_t> current_index() const;

    void MaybeSaveStateCaches(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCaches();

private:
    struct NavigationInputReconcileRequest {
        bool workflow_changed = false;
        bool filters_changed = false;
        bool sorting_changed = false;
    };

    struct NavigationInputReconcileEffects {
        bool workflow_changed = false;
        bool navigation_inputs_changed = false;
        std::optional<std::size_t> snapshot_index_to_load;
    };

    void SyncSampleWorkflowSession(const SpectrumSnapshotHandle& snapshot, SourceCollectionSessionAction& action);
    void ClearSampleWorkflow(SourceCollectionSessionAction& action);
    [[nodiscard]] NavigationInputReconcileEffects ReconcileNavigationInputs(
        const SpectrumSnapshotHandle& snapshot,
        NavigationInputReconcileRequest request);
    static void ApplyNavigationInputEffects(
        SourceCollectionSessionAction& action,
        const NavigationInputReconcileEffects& effects);
    static void ApplyNavigationInputEffects(
        SampleWorkflowCommandResult& result,
        const NavigationInputReconcileEffects& effects);
    std::optional<std::size_t> ApplySampleFilters(const SpectrumSnapshotHandle& snapshot);
    std::optional<std::size_t> ApplySampleSorting(const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] std::size_t ActiveSampleCount(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] std::optional<std::size_t> ActiveSampleIndex(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SampleFilterEvaluation EvaluateSampleFilters(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] bool IsSelectedFilterSource(std::string_view source_id) const;
    [[nodiscard]] bool IsSelectedSampleSortSource(std::string_view source_id) const;
    [[nodiscard]] std::string AnnotationDisplayName(
        const SampleAnnotationResult& annotation,
        const SampleLabelingTask* local_task = nullptr) const;
    [[nodiscard]] std::string LocalTaskAnnotationDisplayName(const SampleLabelingTask& task) const;
    [[nodiscard]] SampleNavigationSortDirection SampleSortSourceDirection(std::string_view source_id) const;
    void SetSampleSortSourceDirection(std::string_view source_id, SampleNavigationSortDirection direction);
    [[nodiscard]] std::vector<SampleFilterSource> BuildSampleFilterSources() const;
    [[nodiscard]] bool RemoveSelectedSampleSortSource(std::string_view source_id);
    [[nodiscard]] const SourceCollectionFilterView& CachedSampleFilterView(std::size_t sample_count) const;
    void InvalidateSampleFilterViewCache();
    [[nodiscard]] const std::vector<SourceCollectionSampleSortSourceView>& CachedSampleSortingSourceViews(
        std::size_t sample_count) const;
    void InvalidateSampleSortingSourceCache();
    void EnsureWorkflowStateCacheLoaded();
    void RestoreActiveWorkflowState(std::string_view source_identity);
    void StoreActiveWorkflowState();
    void MarkActiveWorkflowStateDirty();
    [[nodiscard]] bool SaveWorkflowStateCache();
    [[nodiscard]] bool FlushWorkflowStateCache();
    [[nodiscard]] SampleWorkflowCommandResult ApplyLabelWriteResult(
        const SpectrumSnapshotHandle& snapshot,
        const SampleLabelWriteResult& result);

    SampleNavigationController navigation_;
    SampleLabelingController labeling_;
    SampleFilterController filters_;
    std::optional<std::string> active_sample_workflow_identity_;
    std::optional<std::string> active_sample_workflow_context_fingerprint_;
    std::vector<std::string> selected_filter_source_ids_;
    std::vector<std::string> selected_sample_sort_source_ids_;
    std::unordered_map<std::string, std::string> annotation_display_names_;
    std::unordered_map<std::string, SampleNavigationSortDirection> sample_sort_source_directions_;
    std::optional<std::string> selected_sample_sort_source_id_;
    SampleNavigationSortDirection selected_sample_sort_direction_ =
        SampleNavigationSortDirection::Ascending;
    std::filesystem::path workflow_state_cache_path_;
    SampleWorkflowStateCache workflow_state_cache_;
    LocalUserStateSaveScheduler workflow_state_save_scheduler_;
    bool workflow_state_cache_loaded_ = false;
    bool restoring_source_session_ = false;
    mutable bool sample_sorting_source_cache_valid_ = false;
    mutable std::size_t sample_sorting_source_cache_sample_count_ = 0;
    mutable const SourceCollectionManifest* sample_sorting_source_cache_context_ = nullptr;
    mutable std::vector<SourceCollectionSampleSortSourceView> sample_sorting_source_cache_;
    mutable bool sample_filter_view_cache_valid_ = false;
    mutable std::size_t sample_filter_view_cache_sample_count_ = 0;
    mutable const SourceCollectionManifest* sample_filter_view_cache_context_ = nullptr;
    mutable SourceCollectionFilterView sample_filter_view_cache_;
};

}  // namespace specforge
