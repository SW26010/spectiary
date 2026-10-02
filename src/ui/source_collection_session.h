#pragma once
#include "app/initial_source.h"

#include "app/runtime_paths.h"

#include "app/local_user_state.h"
#include "domain/sample_label_export.h"
#include "domain/spectrum_snapshot.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_preparation.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectiary {

class SampleWorkflowCoordinator;
struct SourceCollectionPanelUiTestAccess;
struct SampleWorkflowTransitionOutcome;
class SourceCollectionRoster;
struct SourceCollectionRosterOpenResult;
class SourceCollectionSessionStatePersistence;

struct SourceCollectionLoadHint {
    SourceCollectionReuseCandidate reuse;
};

struct SourceOpenPlan {
    SourceCollectionLoadRequest load;
    bool session_changed = false;
};

struct SourceCollectionSnapshotPrefetchPlan {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
    SourceCollectionLoadHint load_hint;
};

struct SourceCollectionSnapshotPrefetchStoreResult {
    bool stored = false;
    std::vector<BackgroundRetirementHandle> background_retirement;
};

enum class SourceCollectionSessionIntentKind {
    SourceCollection,
    SampleNavigation,
    ActiveSampleWorkflow,
    SampleFiltering,
    SampleSorting,
};

enum class SourceCollectionIntentKind {
    SwitchActive,
    Remove,
    AddReadOnlyAnnotationResult,
    RemoveReadOnlyAnnotationResult,
    RenameAnnotationResultDisplayName,
};

enum class SampleNavigationIntentKind {
    Move,
    SetSampleNameQuery,
    CommitSampleNameSelection,
};

enum class ActiveSampleWorkflowIntentKind {
    StartOrResumeTemporaryLabelingTask,
    RecoverTemporaryLabelingTask,
    DeleteTemporaryLabelingTask,
    ActivateLabelingTaskFromAnnotation,
    DeleteActiveLabelingTask,
    RenameActiveLabelingTask,
    UpsertActiveLabel,
    UpdateActiveLabel,
    RemoveActiveLabel,
    SetActiveLabelingAutoAdvance,
    SetActiveLabelingSkipLabeledOnAdvance,
    SetActiveLabelingOutputPath,
    ExportActiveLabels,
    DeactivateActiveLabelingTask,
    AssignActiveLabelToCurrentSample,
    ClearActiveLabelForCurrentSample,
    UndoLastLabelWrite,
};

enum class SampleFilteringIntentKind {
    ClearFilters,
    AddFilterSource,
    RemoveFilterSource,
    SetFilterValueSelected,
};

enum class SampleSortingIntentKind {
    ClearSorting,
    AddSortSource,
    RemoveSortSource,
    SetSortSource,
    SetSortDirection,
};

struct SourceCollectionIntent {
    [[nodiscard]] static SourceCollectionIntent SwitchActive(std::size_t source_index);
    [[nodiscard]] static SourceCollectionIntent Remove(std::size_t source_index);
    [[nodiscard]] static SourceCollectionIntent AddReadOnlyAnnotationResult(std::filesystem::path path);
    [[nodiscard]] static SourceCollectionIntent RemoveReadOnlyAnnotationResult(std::filesystem::path path);
    [[nodiscard]] static SourceCollectionIntent RenameAnnotationResultDisplayName(
        std::filesystem::path path,
        std::string display_name);

private:
    friend class SourceCollectionSession;
    friend class SampleWorkflowCoordinator;
    friend struct SourceCollectionSessionIntent;

    SourceCollectionIntent() = default;

    SourceCollectionIntentKind kind = SourceCollectionIntentKind::SwitchActive;
    std::filesystem::path path;
    std::string display_name;
    std::size_t source_index = 0;
};

struct SampleNavigationIntent {
    [[nodiscard]] static SampleNavigationIntent Move(SampleNavigationRequest request);
    [[nodiscard]] static SampleNavigationIntent SetSampleNameQuery(std::string query);
    [[nodiscard]] static SampleNavigationIntent CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name);

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionPanelUiTestAccess;
    friend class SampleWorkflowCoordinator;
    friend struct SourceCollectionSessionIntent;

    SampleNavigationIntent() = default;

    SampleNavigationIntentKind kind = SampleNavigationIntentKind::Move;
    SampleNavigationRequest request;
    std::string query;
    std::size_t target_row = 0;
    std::string matched_name;
};

struct ActiveSampleWorkflowIntent {
    [[nodiscard]] static ActiveSampleWorkflowIntent StartOrResumeTemporaryLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent RecoverTemporaryLabelingTask(
        std::string source_identity,
        std::string task_id);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeleteTemporaryLabelingTask(
        std::string source_identity,
        std::string task_id);
    [[nodiscard]] static ActiveSampleWorkflowIntent ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeleteActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent RenameActiveLabelingTask(
        std::string expected_task_id,
        std::string requested_name);
    [[nodiscard]] static ActiveSampleWorkflowIntent UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] static ActiveSampleWorkflowIntent UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change = false);
    [[nodiscard]] static ActiveSampleWorkflowIntent RemoveActiveLabel(int code);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent ExportActiveLabels(
        std::filesystem::path output_path,
        SampleLabelExportFormat format);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeactivateActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] static ActiveSampleWorkflowIntent ClearActiveLabelForCurrentSample();
    [[nodiscard]] static ActiveSampleWorkflowIntent UndoLastLabelWrite();

private:
    friend class SourceCollectionSession;
    friend class SampleWorkflowCoordinator;
    friend struct SourceCollectionSessionIntent;
    friend struct SourceCollectionPanelUiTestAccess;

    ActiveSampleWorkflowIntent() = default;

    ActiveSampleWorkflowIntentKind kind = ActiveSampleWorkflowIntentKind::StartOrResumeTemporaryLabelingTask;
    std::filesystem::path path;
    std::string source_identity;
    std::string task_id;
    std::string requested_name;
    SampleLabelDefinition label;
    bool enabled = false;
    int label_code = kUnlabeledSampleLabelCode;
    bool allow_used_label_code_change = false;
    SampleLabelExportFormat export_format =
        SampleLabelExportFormat::Npy;
};

struct SampleFilteringIntent {
    [[nodiscard]] static SampleFilteringIntent Clear();
    [[nodiscard]] static SampleFilteringIntent AddSource(std::string source_id);
    [[nodiscard]] static SampleFilteringIntent RemoveSource(std::string source_id);
    [[nodiscard]] static SampleFilteringIntent SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);

private:
    friend class SourceCollectionSession;
    friend class SampleWorkflowCoordinator;
    friend struct SourceCollectionSessionIntent;

    SampleFilteringIntent() = default;

    SampleFilteringIntentKind kind = SampleFilteringIntentKind::ClearFilters;
    std::string source_id;
    std::string value_key;
    bool selected = false;
};

struct SampleSortingIntent {
    [[nodiscard]] static SampleSortingIntent Clear();
    [[nodiscard]] static SampleSortingIntent AddSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent RemoveSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent SetSortSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent SetSortDirection(SampleNavigationSortDirection direction);

private:
    friend class SourceCollectionSession;
    friend class SampleWorkflowCoordinator;
    friend struct SourceCollectionSessionIntent;

    friend struct SourceCollectionPanelUiTestAccess;

    SampleSortingIntent() = default;

    SampleSortingIntentKind kind = SampleSortingIntentKind::ClearSorting;
    std::string source_id;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
};

struct SourceCollectionSessionIntent {
    [[nodiscard]] static SourceCollectionSessionIntent EditSourceCollection(SourceCollectionIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent UpdateSampleNavigation(SampleNavigationIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ApplySampleFiltering(SampleFilteringIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ApplySampleSorting(SampleSortingIntent intent);
    [[nodiscard]] SourceCollectionSessionIntentKind intent_kind() const noexcept
    {
        return kind;
    }

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionPanelUiTestAccess;

    SourceCollectionSessionIntent() = default;

    SourceCollectionSessionIntentKind kind = SourceCollectionSessionIntentKind::SourceCollection;
    SourceCollectionIntent source_collection;
    SampleNavigationIntent sample_navigation;
    ActiveSampleWorkflowIntent active_sample_workflow;
    SampleFilteringIntent sample_filtering;
    SampleSortingIntent sample_sorting;
};

struct SourceCollectionSessionResult {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    std::optional<std::size_t> follow_up_spectrum_index;
    // Bound to the workflow that produced the row, which may precede roster adoption.
    std::optional<std::filesystem::path> follow_up_source_path;
    std::optional<std::filesystem::path> canceled_source_follow_up_path;
    std::vector<BackgroundRetirementHandle> background_retirement;
    bool changed = false;
    bool loaded = false;
    bool view_invalidated = false;
    std::optional<SampleLabelingWriteOperationResult>
        label_write;
    SampleLabelingOperationResult::Issue labeling_issue =
        SampleLabelingOperationResult::Issue::None;
    SourceCollectionLoadError load_error;
    std::string message;
};

class SourceCollectionSession {
public:
    // Detached construction never resolves process storage implicitly.
    SourceCollectionSession();
    SourceCollectionSession(
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingStateCacheLoadPolicy
            labeling_state_cache_load_policy =
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputs,
        const RuntimePaths& runtime_paths = {},
        SourceSessionStartupPolicy startup_source_policy = SourceSessionStartupPolicy::RestoreSavedActive);
    SourceCollectionSession(
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingStateCacheLoadPolicy
            labeling_state_cache_load_policy,
        SampleLabelingController::CanonicalDocumentPublisher
            canonical_document_publisher,
        const RuntimePaths& runtime_paths = {},
        SourceSessionStartupPolicy startup_source_policy = SourceSessionStartupPolicy::RestoreSavedActive);
    SourceCollectionSession(
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SampleLabelingStateCacheLoadPolicy
            labeling_state_cache_load_policy,
        SampleLabelingController::CanonicalDocumentPublisher
            canonical_document_publisher,
        SampleLabelingController::CanonicalValuesPublisher
            canonical_values_publisher,
        const RuntimePaths& runtime_paths = {},
        SourceSessionStartupPolicy startup_source_policy = SourceSessionStartupPolicy::RestoreSavedActive);
    ~SourceCollectionSession();

    SourceCollectionSession(SourceCollectionSession&&) noexcept;
    SourceCollectionSession& operator=(SourceCollectionSession&&) noexcept;
    SourceCollectionSession(const SourceCollectionSession&) = delete;
    SourceCollectionSession& operator=(const SourceCollectionSession&) = delete;

    [[nodiscard]] SourceCollectionSessionResult Submit(
        SourceCollectionSessionIntent intent,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] bool SupersedesPendingSourceActivation(
        const SourceCollectionSessionIntent& intent) const;
    [[nodiscard]] const SourceCollectionSessionView& View();
    // Supersedes pending navigation and captures one coherent in-memory source
    // candidate. Filesystem resolution and validation remain worker operations.
    [[nodiscard]] SourceOpenPlan PlanSourceOpen(
        const SourceOpenRequest& request, std::size_t spectrum_index = 0);
    // The caller must admit the completion against its activation intent first.
    [[nodiscard]] SourceCollectionSessionResult CommitPreparedOpen(
        PreparedSourceCollection prepared, bool activate = true,
        bool recover_failed_presentation = true);
    [[nodiscard]] ExactSampleNameResolution
    ResolveExactSampleName(std::string_view name) const;
    // A deferred pending target is the origin for a subsequent navigation command.
    [[nodiscard]] std::optional<std::size_t> EffectiveSampleNavigationIndex() const;
    [[nodiscard]] SpectrumSnapshotHandle CurrentSampleSnapshot() const;
    [[nodiscard]] const std::optional<SourceCollectionLoadFailure>& CurrentSampleFailure() const
    { return current_sample_failure_; }
    [[nodiscard]] SourceCollectionSessionAction FailCurrentSample(SourceCollectionLoadFailure failure);
    [[nodiscard]] SpectrumSnapshotHandle CurrentSourceSnapshot() const;
    [[nodiscard]] std::optional<std::string>
    CurrentSourceCollectionIdentity() const;
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSource(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<SourceCollectionLoadHint> LoadHintForSource(
        const std::filesystem::path& path,
        std::optional<std::size_t> spectrum_index = std::nullopt);
    [[nodiscard]] std::optional<SourceCollectionSnapshotPrefetchPlan>
        PlanSnapshotPrefetch(
            SampleNavigationDirection direction,
            SampleNavigationPrefetchPolicy policy = {});
    [[nodiscard]] SourceCollectionSnapshotPrefetchStoreResult
        StorePrefetchedSnapshot(
            const std::filesystem::path& path,
            SourceCollectionResidentSnapshot resident);
    [[nodiscard]] std::optional<SourceCollectionDeferredRestorePlan> TakeDeferredRestorePlan();
    void FinishDeferredRestore();
    void RecordExplicitSourceActivation();
    [[nodiscard]] SourceCollectionSessionResult RecordRestoreFailure(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        SourceCollectionLoadError error);
    // Startup keeps the display empty for an unavailable active row, or while
    // the saved active row is pending (null index). User selection cannot
    // activate unavailable rows.
    [[nodiscard]] SourceCollectionSessionResult RestoreEmptyActiveSource(
        std::optional<std::size_t> source_index);
    [[nodiscard]] bool HasUnresolvedSourceIntent(const std::filesystem::path& path) const;
    [[nodiscard]] bool ForgetUnresolvedSourceIntent(const std::filesystem::path& path);
    [[nodiscard]] bool CancelPendingSampleNavigation(
        const std::filesystem::path& path,
        std::size_t spectrum_index);

    [[nodiscard]] SourceCollectionSessionResult RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCaches();
    [[nodiscard]] bool PrepareLabelingForInteractiveClose();
    [[nodiscard]] SourceCollectionStateFlushResult
        FlushStateCachesWithStatus();
    [[nodiscard]] std::vector<BackgroundRetirementHandle>
        TakeViewRetirement();
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();

private:
    [[nodiscard]] bool CancelActivePendingSampleNavigation();
    [[nodiscard]] std::optional<std::pair<std::filesystem::path, std::size_t>>
    ExistingSpectrumMember(const std::filesystem::path& member);
    [[nodiscard]] SampleWorkflowTransitionOutcome ActivateSource(
        std::size_t source_index);
    [[nodiscard]] SampleWorkflowTransitionOutcome RemoveSource(
        std::size_t source_index,
        std::vector<BackgroundRetirementHandle>& background_retirement,
        std::optional<std::filesystem::path>* canceled_source_follow_up_path);
    [[nodiscard]] std::vector<SourceCollectionSavedSource> SavedSourcesWithAnnotations() const;
    [[nodiscard]] LocalUserStateHealthView PersistenceHealth() const;
    void PrepareDeferredSourceSessionRestore(SourceSessionStartupPolicy startup_source_policy);
    void MarkSourceSessionCacheDirty();
    void InvalidateView();
    void AppendPendingBackgroundRetirement(
        std::vector<BackgroundRetirementHandle>& retirement);
    void ApplyWorkflowTransitionOutcome(
        SourceCollectionSessionResult& result,
        SampleWorkflowTransitionOutcome outcome);
    [[nodiscard]] std::optional<std::size_t>
    PresentedSampleIndex() const;
    void RecordSampleTransition(
        SourceCollectionSampleTransitionReason reason,
        std::optional<std::size_t> from_sample_index,
        std::optional<std::size_t> current_sample_index,
        std::optional<int> accepted_label_value =
            std::nullopt);
    [[nodiscard]] SourceCollectionSessionAction AdoptRosterOpenResult(
        SourceCollectionRosterOpenResult result);

    std::unique_ptr<SourceCollectionRoster> roster_;
    std::optional<SourceCollectionLoadFailure> current_sample_failure_;
    bool sample_recovery_pending_ = false;
    std::unique_ptr<SampleWorkflowCoordinator> workflow_;
    std::unique_ptr<SourceCollectionSessionStatePersistence> source_session_state_;
    std::optional<SourceCollectionDeferredRestorePlan> deferred_restore_plan_;
    std::vector<SourceCollectionSavedSource> unresolved_deferred_restore_sources_;
    bool deferred_restore_active_ = false;
    std::vector<BackgroundRetirementHandle> pending_background_retirement_;
    std::unordered_map<std::string, std::uint64_t> live_workflow_revisions_;
    std::optional<SourceCollectionSampleTransitionView>
        sample_transition_;
    std::shared_ptr<SourceCollectionSessionView> session_view_cache_;
    std::vector<BackgroundRetirementHandle>
        pending_session_view_retirement_;
    std::uint64_t session_view_revision_ = 0;
    std::uint64_t cached_session_view_revision_ = 0;
};

}  // namespace spectiary
