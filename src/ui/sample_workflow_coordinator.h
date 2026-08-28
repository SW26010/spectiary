#pragma once

#include "domain/spectrum_snapshot.h"
#include "app/local_user_state.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_source_policy.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
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
struct SourceCollectionIntent;
struct SampleNavigationIntent;
struct ActiveSampleWorkflowIntent;
struct SampleFilteringIntent;
struct SampleSortingIntent;

struct SampleWorkflowTransitionOutcome {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    // When true, snapshot_index_to_load replaces an earlier composed target;
    // nullopt explicitly clears that target.
    bool snapshot_target_updated = false;
    std::optional<std::size_t> snapshot_index_to_load;
    bool changed = false;
    bool loaded = false;
    bool invalidate_view = false;
    std::optional<SampleLabelingWriteOperationResult>
        label_write;
    SampleLabelingOperationResult::Issue labeling_issue =
        SampleLabelingOperationResult::Issue::None;
    std::string message;
};

void MergeSampleWorkflowTransitionOutcome(
    SampleWorkflowTransitionOutcome& target,
    SampleWorkflowTransitionOutcome source);

struct PreparedSampleWorkflowActivationResult {
    SourceCollectionSessionAction action;
    std::vector<BackgroundRetirementHandle> background_retirement;
};

struct PendingSampleNavigation {
    SourceCollectionIdentity source_identity;
    std::size_t spectrum_index = 0;
    bool remember_labeling_position = false;
};

struct SampleWorkflowPersistenceStatus {
    LocalUserStatePersistenceStatus navigation;
    LocalUserStatePersistenceStatus labeling;
    LocalUserStatePersistenceStatus workflow;
};

struct SampleWorkflowStateFlushResult {
    bool navigation_saved = true;
    bool labeling_saved = true;
    bool workflow_saved = true;

    [[nodiscard]] bool all_saved() const
    {
        return navigation_saved && labeling_saved && workflow_saved;
    }
};

class SampleWorkflowCoordinator {
public:
    using WorkflowStateCacheLoader =
        std::function<SampleWorkflowStateCacheLoadResult(const std::filesystem::path&)>;
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
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
        WorkflowStateCacheLoader workflow_state_cache_loader,
        SampleLabelingController::CanonicalValuesPublisher
            canonical_values_publisher);

    [[nodiscard]] SampleWorkflowTransitionOutcome Apply(
        SourceCollectionIntent intent,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome Apply(
        SampleNavigationIntent intent,
        const SpectrumSnapshotHandle& snapshot,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowTransitionOutcome Apply(
        ActiveSampleWorkflowIntent intent,
        const SpectrumSnapshotHandle& snapshot,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowTransitionOutcome Apply(
        SampleFilteringIntent intent,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome Apply(
        SampleSortingIntent intent,
        const SpectrumSnapshotHandle& snapshot);

    [[nodiscard]] SampleWorkflowTransitionOutcome SyncActiveSource(
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
    [[nodiscard]] SampleWorkflowTransitionOutcome SyncReusedPreparedKnownSource(
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
    [[nodiscard]] SampleWorkflowTransitionOutcome SyncKnownActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome ClearActiveWorkflow();
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

    [[nodiscard]] bool RestoreReadOnlyAnnotationsForActiveSource(
        const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSourceKey(
        std::string_view source_key) const;
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::filesystem::path>>
        AnnotationPathsBySourceKey() const;
    [[nodiscard]] SourceCollectionNavigationView NavigationView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionLabelingView LabelingView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] ExactSampleNameResolution
    ResolveExactSampleName(std::string_view name) const;
    [[nodiscard]] SourceCollectionFilterView BuildFilterView(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSampleSortingView BuildSortingView(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] bool can_add_read_only_annotation() const;
    [[nodiscard]] std::optional<std::size_t> current_index() const;
    [[nodiscard]] std::vector<std::size_t> AdjacentNavigationRows(
        SampleNavigationDirection direction,
        SampleNavigationPrefetchPolicy policy = {}) const;

    [[nodiscard]] SampleWorkflowTransitionOutcome RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCaches();
    [[nodiscard]] SampleWorkflowStateFlushResult
        FlushStateCachesWithStatus();
    [[nodiscard]] SampleWorkflowPersistenceStatus PersistenceStatus() const;

private:
    [[nodiscard]] SampleWorkflowTransitionOutcome CompleteTransition(
        SampleWorkflowTransitionOutcome outcome,
        const SpectrumSnapshotHandle& snapshot,
        std::uint64_t presentation_revision_before,
        bool align_snapshot_target = false) const;
    [[nodiscard]] SampleWorkflowTransitionOutcome SyncActiveSourceWithContext(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        SourceCollectionContext context,
        std::optional<std::size_t> prepared_index);
    [[nodiscard]] SampleWorkflowTransitionOutcome RequestSampleNavigation(
        const SampleNavigationRequest& request,
        const SpectrumSnapshotHandle& snapshot,
        std::optional<std::size_t> deferred_base_index = std::nullopt,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowTransitionOutcome AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path);
    [[nodiscard]] SampleWorkflowTransitionOutcome RemoveReadOnlyAnnotationFromActiveSource(
        const std::filesystem::path& path);
    [[nodiscard]] SampleWorkflowTransitionOutcome RenameAnnotationDisplayNameForActiveSource(
        std::filesystem::path path,
        std::string display_name);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetSampleNameQuery(std::string query);
    [[nodiscard]] SampleWorkflowTransitionOutcome CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome StartOrResumeTemporaryLabelingTask();
    [[nodiscard]] SampleWorkflowTransitionOutcome RecoverTemporaryLabelingTask(
        std::string source_identity,
        std::string task_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome DeleteTemporaryLabelingTask(
        std::string source_identity,
        std::string task_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] SampleWorkflowTransitionOutcome DeleteActiveLabelingTask();
    [[nodiscard]] SampleWorkflowTransitionOutcome UpsertActiveLabel(
        SampleLabelDefinition label);
    [[nodiscard]] SampleWorkflowTransitionOutcome UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change);
    [[nodiscard]] SampleWorkflowTransitionOutcome RemoveActiveLabel(int code);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetActiveLabelingOutputPath(
        std::filesystem::path output_path);
    [[nodiscard]] SampleWorkflowTransitionOutcome DeactivateActiveLabelingTask();
    [[nodiscard]] SampleWorkflowTransitionOutcome AssignActiveLabelToCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        int code,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowTransitionOutcome ClearActiveLabelForCurrentSample(
        const SpectrumSnapshotHandle& snapshot,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SampleWorkflowTransitionOutcome UndoLastLabelWrite(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome ClearFilters(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome AddFilterSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome RemoveFilterSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetFilterValueSelected(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SampleWorkflowTransitionOutcome ClearSampleSorting(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome AddSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome RemoveSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetSampleSortSource(
        const SpectrumSnapshotHandle& snapshot,
        std::string source_id);
    [[nodiscard]] SampleWorkflowTransitionOutcome SetSampleSortDirection(
        const SpectrumSnapshotHandle& snapshot,
        SampleNavigationSortDirection direction);

    struct NavigationInputReconcileRequest {
        bool workflow_changed = false;
        bool filters_changed = false;
        bool sorting_changed = false;
    };

    struct NavigationInputReconcileEffects {
        bool workflow_changed = false;
        bool navigation_inputs_changed = false;
        bool snapshot_target_updated = false;
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
        SampleLabelingCanonicalSourceDescriptor
            source_descriptor,
        SourceCollectionSessionAction& action);
    [[nodiscard]] bool
        SynchronizeActiveCanonicalAsdfAttachment();
    void ClearSampleWorkflow(SourceCollectionSessionAction& action);
    [[nodiscard]] NavigationInputReconcileEffects ReconcileNavigationInputs(
        const SpectrumSnapshotHandle& snapshot,
        NavigationInputReconcileRequest request);
    static void ApplyNavigationInputEffects(
        SampleWorkflowTransitionOutcome& outcome,
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
    [[nodiscard]] LocalUserStatePersistenceLifecycle::SaveResult
        SaveWorkflowStateCache();
    [[nodiscard]] bool FlushWorkflowStateCache();
    [[nodiscard]] SampleWorkflowTransitionOutcome ApplyLabelWriteResult(
        const SpectrumSnapshotHandle& snapshot,
        SampleLabelingWriteOperationResult result,
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
    LocalUserStatePersistenceLifecycle workflow_state_persistence_;
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
