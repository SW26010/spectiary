#pragma once

#include "app/runtime_paths.h"

#include "domain/spectrum_snapshot.h"
#include "app/local_user_state.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_source_policy.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/sample_workflow_preparation.h"
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

namespace spectiary {

struct PreparedSampleWorkflowState;
struct SampleWorkflowPreparationCacheBundle;
struct SourceCollectionIntent;
struct SampleNavigationIntent;
struct ActiveSampleWorkflowIntent;
struct SampleFilteringIntent;
struct SampleSortingIntent;

struct SampleWorkflowLoadTarget {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
};

struct SampleWorkflowFollowUp {
    // Empty means retain existing work. Replacement supplies both fields.
    std::optional<SampleWorkflowLoadTarget> load;
    std::optional<std::filesystem::path> cancel_source_path;
};

struct SampleWorkflowView {
    SourceCollectionNavigationView navigation;
    SourceCollectionLabelingView labeling;
    SourceCollectionFilterView filter;
    SourceCollectionSampleSortingView sorting;
    bool can_add_read_only_annotation = false;
};

struct SampleWorkflowTransitionOutcome {
    SampleWorkflowFollowUp follow_up;
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
    std::optional<std::size_t> snapshot_index_to_load;
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

struct PreparedWorkflowReconciliation {
    SourceCollectionLoadError error;
    std::vector<BackgroundRetirementHandle> background_retirement;
};

enum class PreparedSourceDisposition {
    Rejected,
    // No roster adoption; follow_up_spectrum_index is the only load target.
    FollowUp,
    // No roster adoption; cancel this source's follow-up and clear provenance.
    NavigationCanceled,
    // Workflow is committed. The supplied snapshot may now enter the roster.
    Adopt,
};

struct PreparedSourceTransition {
    PreparedSourceDisposition disposition = PreparedSourceDisposition::Rejected;
    SourceCollectionLoadError error;
    SourceCollectionSessionAction action;
    std::vector<BackgroundRetirementHandle> background_retirement;
    // Complete presentation/navigation outcome; callers must not query current
    // or pending navigation to interpret this transition.
    std::optional<std::size_t> current_index;
    std::optional<std::size_t> follow_up_spectrum_index;
    bool completes_pending_navigation = false;
    bool invalidate_view = false;
};

class SampleWorkflowCoordinator {
public:
    using WorkflowStateCacheLoader =
        std::function<SampleWorkflowStateCacheLoadResult(const std::filesystem::path&)>;
    // Detached construction never resolves process storage implicitly.
    SampleWorkflowCoordinator();
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        const RuntimePaths& runtime_paths = {});
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
        WorkflowStateCacheLoader workflow_state_cache_loader,
        const RuntimePaths& runtime_paths = {});
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
        WorkflowStateCacheLoader workflow_state_cache_loader,
        SampleLabelingController::CanonicalDocumentPublisher
            canonical_document_publisher,
        const RuntimePaths& runtime_paths = {});
    SampleWorkflowCoordinator(
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingController::StateCacheLoader labeling_state_cache_loader,
        WorkflowStateCacheLoader workflow_state_cache_loader,
        SampleLabelingController::CanonicalDocumentPublisher
            canonical_document_publisher,
        SampleLabelingController::CanonicalValuesPublisher
            canonical_values_publisher,
        const RuntimePaths& runtime_paths = {});

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
    // Runs before roster adoption. Only Adopt permits installing the supplied
    // snapshot; all other dispositions retain the resident roster snapshot.
    [[nodiscard]] PreparedSourceTransition CommitPreparedSource(
        std::string_view source_key, const SpectrumSnapshotHandle& snapshot,
        std::size_t spectrum_index, std::uint64_t live_revision,
        PreparedSourceCollectionPayload payload, bool activate = true);
    [[nodiscard]] std::optional<SourceCollectionIdentity> ActiveSourceIdentity() const;
    [[nodiscard]] std::optional<std::filesystem::path> ActiveSourcePath() const;
    [[nodiscard]] std::optional<SourceCollectionIdentity> KnownSourceIdentity(
        std::string_view source_key) const;
    [[nodiscard]] SampleWorkflowTransitionOutcome SyncKnownActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SampleWorkflowTransitionOutcome ClearActiveWorkflow();
    void BeginRestoringSourceSession();
    void EndRestoringSourceSession();
    [[nodiscard]] BackgroundRetirementHandle RemoveSource(std::string_view source_key);
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();
    void SetDeferredSampleNavigation(bool enabled);
    SampleWorkflowTransitionOutcome CancelDeferredSampleNavigation();
    [[nodiscard]] std::optional<std::size_t> pending_sample_index() const;

    [[nodiscard]] bool RestoreReadOnlyAnnotationsForActiveSource(
        const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSourceKey(
        std::string_view source_key) const;
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::filesystem::path>>
        AnnotationPathsBySourceKey() const;
    [[nodiscard]] ExactSampleNameResolution
    ResolveExactSampleName(std::string_view name) const;
    [[nodiscard]] std::optional<std::size_t> current_index() const;
    [[nodiscard]] std::vector<std::size_t> AdjacentNavigationRows(
        SampleNavigationDirection direction,
        SampleNavigationPrefetchPolicy policy = {}) const;

    [[nodiscard]] SampleWorkflowTransitionOutcome RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now,
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCaches();
    [[nodiscard]] bool PrepareLabelingForInteractiveClose();
    [[nodiscard]] SampleWorkflowStateFlushResult
        FlushStateCachesWithStatus();
    [[nodiscard]] SampleWorkflowPersistenceStatus PersistenceStatus() const;

    // Transfers prepared projection storage into the caller's stable view.
    // Presentation policy (including failed samples) remains with the session.
    [[nodiscard]] SampleWorkflowView BuildView(
        const SpectrumSnapshotHandle& source_snapshot,
        const SpectrumSnapshotHandle& current_sample_snapshot);

private:
    void DiscardPreparedViewCaches();
    [[nodiscard]] SourceCollectionNavigationView NavigationView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionLabelingView LabelingView(const SpectrumSnapshotHandle& snapshot) const;
    [[nodiscard]] SourceCollectionFilterView BuildFilterView(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] SourceCollectionSampleSortingView BuildSortingView(
        const SpectrumSnapshotHandle& snapshot);
    [[nodiscard]] bool can_add_read_only_annotation() const;
    struct TransitionState {
        std::uint64_t presentation_revision = 0;
        std::optional<SampleWorkflowLoadTarget> pending_load;
        std::uint64_t context_generation = 0;
    };
    [[nodiscard]] std::optional<SampleWorkflowLoadTarget> PendingLoadTarget() const;
    [[nodiscard]] TransitionState CaptureTransition(
        NavigationTargetResolutionReport* target_resolution = nullptr) const;
    [[nodiscard]] PreparedSampleWorkflowActivationResult SyncPreparedActiveSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        SourceCollectionContext context,
        PreparedSampleWorkflowState prepared_workflow,
        bool present_explicit_member = false,
        bool activate = true);
    [[nodiscard]] SampleWorkflowTransitionOutcome SyncReusedPreparedKnownSource(
        std::optional<std::string> source_key,
        const SpectrumSnapshotHandle& snapshot,
        const SourceCollectionIdentity& identity,
        bool present_explicit_member = false);
    [[nodiscard]] bool RetargetDeferredSampleNavigation(std::size_t spectrum_index);
    [[nodiscard]] bool CommitDeferredSampleNavigation(std::size_t spectrum_index);
    void CompletePreparedDeferredSampleNavigation(const PendingSampleNavigation& pending);
    [[nodiscard]] std::optional<PendingSampleNavigation> pending_sample_navigation() const;
    [[nodiscard]] PreparedWorkflowReconciliation ReconcilePreparedSource(
        std::string_view source_key, const SpectrumSnapshotHandle& snapshot,
        std::size_t spectrum_index, std::uint64_t live_revision,
        PreparedSourceCollectionPayload& payload);
    [[nodiscard]] bool CanReusePreparedKnownSource(
        std::optional<std::string> source_key,
        const SourceCollectionIdentity& identity) const;
    [[nodiscard]] std::optional<SampleWorkflowSourceState> WorkflowStateForSourceIdentity(
        std::string_view source_identity);
    [[nodiscard]] std::optional<SampleLabelingSourceState> LabelingStateForSourceIdentity(
        std::string_view source_identity);
    [[nodiscard]] SampleWorkflowTransitionOutcome CompleteTransition(
        SampleWorkflowTransitionOutcome outcome,
        const SpectrumSnapshotHandle& snapshot,
        const TransitionState& before,
        bool align_snapshot_target = false);
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
    [[nodiscard]] SampleWorkflowTransitionOutcome RenameActiveLabelingTask(
        std::string expected_task_id,
        std::string requested_name);
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
    [[nodiscard]] SampleWorkflowTransitionOutcome ExportActiveLabels(
        std::filesystem::path output_path,
        SampleLabelExportFormat format);
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
        SynchronizeActiveCanonicalAsdfAttachment(
            SourceCollectionSessionAction* action = nullptr);
    [[nodiscard]] bool
        SynchronizeCanonicalAsdfOwnerAttachments(
            const SpectrumSnapshotHandle& snapshot,
            SourceCollectionSessionAction* action = nullptr);
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
    [[nodiscard]] bool SnapshotMatchesActiveSource(const SpectrumSnapshotHandle& snapshot) const;
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
    RuntimePaths runtime_paths_;
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

}  // namespace spectiary
