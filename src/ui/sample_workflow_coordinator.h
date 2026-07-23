#pragma once

#include "domain/spectrum_snapshot.h"
#include "app/local_user_state.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_source_policy.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

struct PreparedSampleWorkflowState;
struct SampleWorkflowPreparationCacheBundle;

struct SampleWorkflowCommandResult {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    std::optional<std::size_t> snapshot_index_to_load;
};

struct PreparedSampleWorkflowActivationResult {
    SourceCollectionSessionAction action;
    std::vector<BackgroundRetirementHandle> background_retirement;
};

struct PendingSampleNavigation {
    SourceCollectionIdentity source_identity;
    std::size_t spectrum_index = 0;
    bool remember_labeling_position = false;
};

class SampleWorkflowCoordinator {
public:
    using WorkflowStateCacheLoader =
        std::function<SampleWorkflowStateCache(const std::filesystem::path&)>;
    SampleWorkflowCoordinator();
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path);
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
        WorkflowStateCacheLoader workflow_state_cache_loader);

    [[nodiscard]] SourceCollectionSessionAction SyncActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] PreparedSampleWorkflowActivationResult SyncPreparedActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        SourceCollectionContext context,
        PreparedSampleWorkflowState prepared_workflow);
    [[nodiscard]] bool CanReusePreparedKnownSource(
        std::optional<std::string> source_key,
        const SourceCollectionIdentity& identity) const;
    [[nodiscard]] SourceCollectionSessionAction SyncReusedPreparedKnownSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        const SourceCollectionIdentity& identity);
    [[nodiscard]] std::optional<SourceCollectionIdentity> ActiveSourceIdentity() const;
    [[nodiscard]] std::optional<SourceCollectionIdentity> KnownSourceIdentity(
        std::string_view source_key) const;
    [[nodiscard]] std::optional<std::size_t> KnownSourceCurrentIndex(
        std::string_view source_key) const;
    [[nodiscard]] std::optional<SampleWorkflowSourceState> WorkflowStateForSourceIdentity(
        std::string_view source_identity);
    [[nodiscard]] std::optional<SampleLabelingSourceState> LabelingStateForSourceIdentity(
        std::string_view source_identity);
    [[nodiscard]] SourceCollectionSessionAction SyncKnownActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveWorkflow();
    void BeginRestoringSourceSession();
    void EndRestoringSourceSession();
    [[nodiscard]] BackgroundRetirementHandle RemoveSource(std::string_view source_key);
    void DiscardPreparedViewCaches();
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();
    void SetDeferredSampleNavigation(bool enabled);
    [[nodiscard]] bool RetargetDeferredSampleNavigation(std::size_t spectrum_index);
    [[nodiscard]] bool CommitDeferredSampleNavigation(std::size_t spectrum_index);
    void CompletePreparedDeferredSampleNavigation(const PendingSampleNavigation& pending);
    void CancelDeferredSampleNavigation();
    [[nodiscard]] std::optional<std::size_t> pending_sample_index() const;
    [[nodiscard]] std::optional<PendingSampleNavigation> pending_sample_navigation() const;

    [[nodiscard]] SampleWorkflowCommandResult RequestSampleNavigation(
        const SampleNavigationRequest& request,
        const SpectrumSnapshotHandle& snapshot,
        std::optional<std::size_t> deferred_base_index = std::nullopt,
        NavigationTargetResolutionReport* target_resolution = nullptr);
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
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSourceKey(
        std::string_view source_key) const;
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::filesystem::path>>
        AnnotationPathsBySourceKey() const;
    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SampleWorkflowCommandResult CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSessionAction StartOrResumeTemporaryLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] SourceCollectionSessionAction DeleteActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction RemoveActiveLabel(
        int code,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction DeactivateActiveLabelingTask();
    [[nodiscard]] SampleWorkflowCommandResult AssignActiveLabelToCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        int code,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowCommandResult ClearActiveLabelForCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowCommandResult UndoLastLabelWrite(
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
    [[nodiscard]] std::vector<std::size_t> AdjacentNavigationRows(
        SampleNavigationDirection direction,
        SampleNavigationPrefetchPolicy policy = {}) const;

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCaches();

private:
    [[nodiscard]] SourceCollectionSessionAction SyncActiveSourceWithContext(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        SourceCollectionContext context,
        std::optional<std::size_t> prepared_index);

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

    struct LabelUndoEntry {
        std::size_t sample_index = 0;
        int previous_code = kUnlabeledSampleLabelCode;
        int current_code = kUnlabeledSampleLabelCode;
    };

    struct LabelUndoHistory {
        std::string workflow_identity;
        std::string task_id;
        std::vector<LabelUndoEntry> entries;
    };

    void SyncSampleWorkflowSession(
        const SourceCollectionIdentity& identity,
        SourceCollectionSessionAction& action);
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
    [[nodiscard]] SampleWorkflowSourceContext SourcePolicyContext(
        const SpectrumSnapshotHandle& snapshot) const;
    void EnsureWorkflowStateCacheLoaded();
    void AdoptPreparedCache(
        const std::shared_ptr<const SampleWorkflowPreparationCacheBundle>& cache,
        std::vector<BackgroundRetirementHandle>& background_retirement);
    [[nodiscard]] const SampleWorkflowSourceState* CachedWorkflowState(
        std::string_view source_identity) const;
    void RestoreActiveWorkflowState(std::string_view source_identity);
    void StoreActiveWorkflowState();
    void MarkActiveWorkflowStateDirty();
    [[nodiscard]] bool SaveWorkflowStateCache();
    [[nodiscard]] bool FlushWorkflowStateCache();
    [[nodiscard]] SampleWorkflowCommandResult ApplyLabelWriteResult(
        const SpectrumSnapshotHandle& snapshot,
        const SampleLabelWriteResult& result,
        NavigationTargetResolutionReport* target_resolution = nullptr,
        bool record_undo = true,
        std::optional<std::size_t> restore_sample_index = std::nullopt);
    void RecordLabelUndo(const SampleLabelWriteResult& result);
    void ClearLabelUndoHistory();
    [[nodiscard]] bool LabelUndoHistoryMatchesActiveTask() const;

    SampleNavigationController navigation_;
    SampleLabelingController labeling_;
    SampleWorkflowSourcePolicy workflow_sources_;
    std::optional<std::string> active_sample_workflow_identity_;
    std::optional<std::string> active_sample_workflow_context_fingerprint_;
    std::filesystem::path workflow_state_cache_path_;
    std::shared_ptr<const SampleWorkflowStateCache> workflow_state_cache_snapshot_;
    SampleWorkflowStateCache workflow_state_cache_;
    std::unordered_set<std::string> workflow_state_tombstones_;
    WorkflowStateCacheLoader workflow_state_cache_loader_;
    LocalUserStateSaveScheduler workflow_state_save_scheduler_;
    bool workflow_state_cache_loaded_ = false;
    bool restoring_source_session_ = false;
    bool deferred_sample_navigation_ = false;
    std::optional<LabelUndoHistory> label_undo_history_;
    // One-shot prepared projections are moved into the Shell session-view
    // cache on first presentation, avoiding an O(N) UI-thread copy.
    mutable std::optional<SourceCollectionFilterView> prepared_filter_view_;
    mutable std::optional<SourceCollectionSampleSortingView> prepared_sorting_view_;
};

}  // namespace specforge
