#pragma once

#include "app/local_user_state.h"
#include "platform/exclusive_file_lease.h"
#include "ui/background_retirement.h"
#include "ui/sample_labeling_state_cache_io.h"

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

struct SourceCollectionIdentity;

// Borrowed read-only projection. The pointers remain valid only until the
// controller's next mutation or destruction; callers must not retain them
// across command submission or maintenance.
struct SampleLabelingControllerView {
    const SampleLabelingTask* active_task = nullptr;
    const SampleLabelingTask* temporary_task = nullptr;
    const std::vector<SampleLabelingTask>* active_source_tasks = nullptr;
    std::uint64_t revision = 0;
};

// Recovery classification is conservative and does not probe leases. Stale
// takes precedence over every other status when the source state came from an
// untrusted loaded/prepared cache snapshot or the task shape is invalid.
// Conflicting takes precedence for duplicate task identities or a known
// unavailable target; a lease conflict is an observed, expiring observation,
// not a live lease probe, and expires on the next trusted source refresh. With
// multiple temporary drafts, only a uniquely selected current draft remains
// Current. Current is the uniquely selected temporary draft in the current
// in-memory source. Recoverable means that the draft is structurally valid,
// trusted, not the current selection, and has no current conflict observation.
// It is an optimistic retryable state; it does not mean that a lease has been
// positively acquired.
enum class SampleLabelingRecoveryDraftStatus {
    Current,
    Conflicting,
    Stale,
    Recoverable,
};

// Borrowed read-only recovery projection. The task pointers remain valid only
// until the controller's next mutation or destruction; callers must not retain
// them across command submission or maintenance. Building this projection does
// not acquire, refresh, or release any lease.
struct SampleLabelingRecoveryDraftView {
    const SampleLabelingTask* task = nullptr;
    SampleLabelingRecoveryDraftStatus status =
        SampleLabelingRecoveryDraftStatus::Stale;
};

struct SampleLabelingRecoveryView {
    std::string source_identity;
    std::vector<SampleLabelingRecoveryDraftView> temporary_drafts;
    std::uint64_t revision = 0;
};

struct SampleLabelingOperationResult {
    enum class Issue {
        None,
        EditLeaseUnavailable,
        EditLeaseFailed,
        EditTargetChanged,
    };

    bool accepted = false;
    bool changed = false;
    bool task_projection_changed = false;
    bool output_save_attempted = false;
    bool output_saved = false;
    bool output_retry_scheduled = false;
    bool state_save_scheduled = false;
    bool state_save_attempted = false;
    bool state_saved = false;
    std::uint64_t revision = 0;
    Issue issue = Issue::None;
};

struct SampleLabelingWriteOperationResult {
    SampleLabelWriteResult write;
    SampleLabelingOperationResult operation;
};

struct SampleLabelingPreparedSourceActivationResult {
    BackgroundRetirementHandle background_retirement;
    bool prepared_task_projection_changed = false;
};

class SampleLabelingController {
public:
    using SourceState = SampleLabelingSourceState;
    using StateCacheLoader =
        std::function<SampleLabelingStateCacheLoadResult(const std::filesystem::path&)>;
    using TaskPersister = std::function<SampleLabelTaskPersistResult(
        SampleLabelingTask&,
        const SampleLabelResultMetadataSource*)>;

    SampleLabelingController();
    explicit SampleLabelingController(std::filesystem::path state_cache_path);
    SampleLabelingController(
        std::filesystem::path state_cache_path,
        StateCacheLoader state_cache_loader);
    SampleLabelingController(
        std::filesystem::path state_cache_path,
        StateCacheLoader state_cache_loader,
        TaskPersister task_persister);

    void ActivateSource(std::string source_identity, std::size_t sample_count);
    void ActivateSource(const SourceCollectionIdentity& identity);
    [[nodiscard]] SampleLabelingPreparedSourceActivationResult
        ActivatePreparedSource(
        const SourceCollectionIdentity& identity,
        std::optional<SourceState> prepared_state);
    [[nodiscard]] BackgroundRetirementHandle AdoptPreparedStateCache(
        std::shared_ptr<const SampleLabelingStateCacheLoadResult> cache_snapshot);
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();
    void ClearActiveSource();
    void RemoveSource(std::string_view source_identity);

    [[nodiscard]] SampleLabelingControllerView View() const;
    [[nodiscard]] SampleLabelingRecoveryView RecoveryView() const;
    [[nodiscard]] std::uint64_t
        active_source_tasks_generation() const;
    [[nodiscard]] std::optional<SourceState> SourceStateForIdentity(
        std::string_view source_identity);
    [[nodiscard]] SampleLabelingOperationResult CreateTask(
        std::string task_id,
        std::string task_name);
    [[nodiscard]] SampleLabelingOperationResult
        StartOrResumeTemporaryTask();
    [[nodiscard]] SampleLabelingOperationResult CreateTaskFromAnnotation(
        std::string task_id,
        std::string task_name,
        SampleLabelSet label_set,
        std::vector<int> values,
        std::filesystem::path output_path,
        bool metadata_clean);
    [[nodiscard]] SampleLabelingOperationResult ActivateTask(std::string_view task_id);
    [[nodiscard]] SampleLabelingOperationResult UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] SampleLabelingOperationResult UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change);
    [[nodiscard]] SampleLabelingOperationResult RemoveActiveLabel(int code);
    [[nodiscard]] SampleLabelingOperationResult RenameActiveTask(std::string task_name);
    [[nodiscard]] SampleLabelingOperationResult SetActiveAutoAdvance(bool enabled);
    [[nodiscard]] SampleLabelingOperationResult SetActiveSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SampleLabelingOperationResult SaveActiveTemporaryTaskToOutput(
        std::filesystem::path output_path,
        std::string task_name);
    [[nodiscard]] bool CanDeactivateActiveTask() const;
    [[nodiscard]] bool CanDeleteActiveTask() const;
    [[nodiscard]] SampleLabelingOperationResult DeactivateActiveTask();
    [[nodiscard]] SampleLabelingOperationResult DeleteActiveTask();
    [[nodiscard]] SampleLabelingOperationResult RememberActivePosition(std::size_t sample_index);
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCache();
    [[nodiscard]] bool state_save_pending() const;
    [[nodiscard]] bool state_save_failed() const;
    [[nodiscard]] std::string_view state_save_error() const;
    [[nodiscard]] std::string_view state_load_warning() const;
    [[nodiscard]] LocalUserStatePersistenceStatus PersistenceStatus() const;

    [[nodiscard]] SampleLabelingWriteOperationResult AssignLabel(std::size_t sample_index, int code);
    [[nodiscard]] SampleLabelingWriteOperationResult ClearLabel(std::size_t sample_index);

private:
    enum class PersistencePolicy {
        ScheduleStateSave,
        FlushStateSave,
        PersistOutputIfSelected,
        // Shortcut writes accept the in-memory overlay and defer lock contention.
        PersistOutputIfSelectedInteractive,
    };

    enum class TaskProjectionEffect {
        Unchanged,
        Changed,
    };

    struct TaskEditLeaseSet {
        struct Component {
            ExclusiveFileLease baseline;
            std::vector<ExclusiveFileLease> aliases;

            [[nodiscard]] bool Held() const noexcept
            {
                if (!baseline) {
                    return false;
                }
                for (const ExclusiveFileLease& alias : aliases) {
                    if (!alias) {
                        return false;
                    }
                }
                return true;
            }

            void Reset() noexcept
            {
                baseline.Reset();
                aliases.clear();
            }
        };

        Component task_identity;
        Component temporary_slot;
        std::vector<Component> output_artifacts;
        std::string task_identity_key;
        std::string temporary_slot_key;
        std::vector<std::string> output_artifact_keys;
    };

    struct TaskEditLeaseAcquireResult {
        TaskEditLeaseSet::Component component;
        ExclusiveFileLeaseAcquireStatus status =
            ExclusiveFileLeaseAcquireStatus::Failed;
        std::string error;
    };

    enum class TaskRefreshStatus {
        Ready,
        Missing,
        Failed,
    };

    enum class TaskActivationExpectation {
        AnyTask,
        TemporaryTask,
    };

    struct TaskActivationPreparation {
        TaskEditLeaseSet leases;
        SampleLabelingStateCache latest_cache;
        std::optional<SampleLabelingTask> task;
        ExclusiveFileLeaseAcquireStatus lease_status =
            ExclusiveFileLeaseAcquireStatus::Failed;
        TaskRefreshStatus refresh_status =
            TaskRefreshStatus::Failed;
        std::string error;
    };

    struct TaskCreationPreparation {
        TaskEditLeaseSet leases;
        std::string task_id;
        ExclusiveFileLeaseAcquireStatus lease_status =
            ExclusiveFileLeaseAcquireStatus::Failed;
        bool ready = false;
        std::string error;
    };

    struct TaskOutputPersistenceAttempt {
        SampleLabelTaskPersistResult persist_result;
        bool output_saved = false;
        bool artifacts_replaced = false;
        ExclusiveFileLeaseAcquireStatus lease_status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        std::string lease_error;
    };

    [[nodiscard]] SampleLabelingTask* ActiveTask();
    [[nodiscard]] const SampleLabelingTask* ActiveTask() const;
    [[nodiscard]] SampleLabelingTask* TemporaryTask();
    [[nodiscard]] const SampleLabelingTask* TemporaryTask() const;
    [[nodiscard]] SourceState* ActiveSource();
    [[nodiscard]] const SourceState* ActiveSource() const;
    [[nodiscard]] SourceState* MaterializeSource(std::string_view source_identity);
    [[nodiscard]] SourceState MergeRefreshedSourceState(
        std::string_view source_identity,
        const SourceState& local,
        SourceState refreshed) const;
    [[nodiscard]] SampleLabelingOperationResult RejectOperation() const;
    [[nodiscard]] SampleLabelingOperationResult
        RejectEditLeaseUnavailable() const;
    [[nodiscard]] SampleLabelingOperationResult
        RejectEditLeaseFailed() const;
    [[nodiscard]] SampleLabelingOperationResult
        RejectEditTargetChanged() const;
    [[nodiscard]] SampleLabelingOperationResult
        RejectLeaseAcquireStatus(
            ExclusiveFileLeaseAcquireStatus status) const;
    [[nodiscard]] SampleLabelingOperationResult CompleteMutation(
        SampleLabelingTask* task,
        PersistencePolicy persistence,
        TaskProjectionEffect projection_effect);
    [[nodiscard]] TaskOutputPersistenceAttempt PersistTaskOutput(
        SampleLabelingTask& task,
        const SourceState* source_state,
        TaskEditLeaseSet& leases);
    [[nodiscard]] bool CommitTaskRecoveryCheckpoint(
        std::string_view source_identity,
        const SourceState& state,
        const SampleLabelingTask& task,
        bool expected_absent,
        std::string* error_message);
    void BumpActiveSourceTasksGeneration();
    void Touch();
    void UpdateRecoveryTaskTrustFromSnapshot(
        const SampleLabelingStateCacheLoadResult& snapshot);
    void ReconcileRecoveryTaskTrust(
        std::string_view source_identity,
        const SourceState& state,
        const std::unordered_set<std::string>& prepared_task_ids,
        const std::unordered_set<std::string>&
            preserved_local_task_ids,
        std::optional<bool> prepared_snapshot_trusted);
    void ClearRecoveryTaskTrust(
        std::string_view source_identity,
        std::string_view task_id);
    void ExpireTaskLeaseConflictsOnTrustedRefresh(
        std::string_view source_identity);
    void EnsureStateCacheLoaded();
    void QueueStateSave();
    void QueueOutputRetry();
    [[nodiscard]] bool TryRetryOutputSaves();
    [[nodiscard]] bool MaybeRetryOutputSaves(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] bool TrySaveStateCache(
        bool wait_for_commit_lock = false);
    [[nodiscard]] TaskActivationPreparation
        PrepareTaskActivation(
            std::string_view source_identity,
            const SampleLabelingTask& known_task,
            std::size_t sample_count,
            bool reuse_deferred_lease);
    [[nodiscard]] SampleLabelingOperationResult
        ActivateTaskWithExpectation(
            std::string_view task_id,
            TaskActivationExpectation expectation);
    [[nodiscard]] TaskCreationPreparation
        PrepareTaskCreation(
            std::string_view source_identity,
            std::string_view requested_task_id,
            std::size_t sample_count,
            const std::vector<SampleLabelingTask>&
                known_tasks) const;
    [[nodiscard]] ExclusiveFileLeaseAcquireResult
        TryAttachTemporarySlotLease(
            TaskEditLeaseSet& leases,
            std::string_view source_identity,
            const SampleLabelingTask& task) const;
    [[nodiscard]] TaskEditLeaseAcquireResult
        TryAcquireTaskEditLease(std::string_view lease_key) const;
    [[nodiscard]] ExclusiveFileLeaseAcquireResult
        TryAttachOutputLease(
            TaskEditLeaseSet& leases,
            const SampleLabelingTask& task,
            bool resolve_physical_paths = true) const;
    [[nodiscard]] bool ActiveTaskLeaseMatches(
        std::string_view source_identity,
        const SampleLabelingTask& task) const;
    [[nodiscard]] bool TaskIdentityLeaseHeld(
        const TaskEditLeaseSet& leases,
        std::string_view source_identity,
        std::string_view task_id) const;
    [[nodiscard]] bool LocalTaskProjectionProtected(
        std::string_view source_identity,
        std::string_view task_id) const;
    void AdoptActiveTaskLeases(
        TaskEditLeaseSet leases);
    void TransitionActiveTaskLeases(
        TaskEditLeaseSet leases,
        bool pending_patch_saved);
    void ReleaseUnneededActiveLeaseComponents();
    void DeferActiveTaskLeases();
    void ReleaseActiveTaskLeaseForTransition();
    [[nodiscard]] bool RestoreActiveTaskLease();
    void ReleaseActiveTaskLease() noexcept;
    [[nodiscard]] const SampleLabelingTask*
        PendingTaskUpsert(
            std::string_view source_identity,
            std::string_view task_id) const;
    void MarkSourceMetadataUpsert(
        std::string_view source_identity,
        const SourceState& state);
    void MarkTaskUpsert(
        std::string_view source_identity,
        const SourceState& state,
        const SampleLabelingTask& task,
        bool expected_absent = false);
    void MarkTaskTombstone(
        std::string_view source_identity,
        const SourceState& state,
        std::string task_id);
    void MarkActiveTaskSelection(
        std::string_view source_identity,
        const SourceState& state);
    [[nodiscard]] std::optional<bool>
        LatestCacheHasOutputConflict(
            const SampleLabelingTask& candidate) const;

    std::unordered_map<std::string, SourceState> sources_;
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> state_cache_snapshot_;
    std::filesystem::path state_cache_path_;
    StateCacheLoader state_cache_loader_;
    TaskPersister task_persister_;
    LocalUserStateSaveScheduler state_cache_save_scheduler_;
    LocalUserStateSaveScheduler output_retry_scheduler_;
    LocalUserStateSaveStatus state_cache_save_status_;
    std::optional<std::string> active_source_identity_;
    // Source-level entries are retained only as cache-refresh hints. Recovery
    // classification uses the task-target entries below.
    std::unordered_set<std::string>
        lease_unavailable_source_identities_;
    // Task-level entries are transient lease observations. A trusted source
    // refresh expires them so a later activation can re-probe the target.
    std::unordered_set<std::string>
        lease_unavailable_task_targets_;
    // Task IDs materialized from an untrusted cache snapshot remain stale in
    // the recovery projection until that task is adopted from a trusted
    // snapshot or successfully validated/persisted. Metadata-only patches do
    // not clear these task-level provenance markers.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        recovery_untrusted_task_ids_by_source_;
    TaskEditLeaseSet active_task_leases_;
    std::vector<TaskEditLeaseSet> deferred_task_leases_;
    SampleLabelingStateCachePatch pending_cache_patch_;
    std::uint64_t revision_ = 0;
    std::uint64_t active_source_tasks_generation_ = 0;
    bool state_cache_loaded_ = false;
    std::string state_cache_load_warning_;
};

}  // namespace specforge
