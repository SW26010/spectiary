#pragma once

#include "app/local_user_state.h"
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

struct SampleLabelingOperationResult {
    bool accepted = false;
    bool changed = false;
    bool output_save_attempted = false;
    bool output_saved = false;
    bool output_retry_scheduled = false;
    bool state_save_scheduled = false;
    bool state_save_attempted = false;
    bool state_saved = false;
    std::uint64_t revision = 0;
};

struct SampleLabelingWriteOperationResult {
    SampleLabelWriteResult write;
    SampleLabelingOperationResult operation;
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
    [[nodiscard]] BackgroundRetirementHandle ActivatePreparedSource(
        const SourceCollectionIdentity& identity,
        std::optional<SourceState> prepared_state);
    [[nodiscard]] BackgroundRetirementHandle AdoptPreparedStateCache(
        std::shared_ptr<const SampleLabelingStateCacheLoadResult> cache_snapshot);
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();
    void ClearActiveSource();
    void RemoveSource(std::string_view source_identity);

    [[nodiscard]] SampleLabelingControllerView View() const;
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
    };

    enum class TaskProjectionEffect {
        Unchanged,
        Changed,
    };

    [[nodiscard]] SampleLabelingTask* ActiveTask();
    [[nodiscard]] const SampleLabelingTask* ActiveTask() const;
    [[nodiscard]] SampleLabelingTask* TemporaryTask();
    [[nodiscard]] const SampleLabelingTask* TemporaryTask() const;
    [[nodiscard]] SourceState* ActiveSource();
    [[nodiscard]] const SourceState* ActiveSource() const;
    [[nodiscard]] SourceState* MaterializeSource(std::string_view source_identity);
    [[nodiscard]] SampleLabelingOperationResult RejectOperation() const;
    [[nodiscard]] SampleLabelingOperationResult CompleteMutation(
        SampleLabelingTask* task,
        PersistencePolicy persistence,
        TaskProjectionEffect projection_effect);
    [[nodiscard]] bool PersistTaskOutput(
        SampleLabelingTask& task,
        const SourceState* source_state = nullptr);
    void BumpActiveSourceTasksGeneration();
    void Touch();
    void EnsureStateCacheLoaded();
    void QueueStateSave();
    void QueueOutputRetry();
    [[nodiscard]] bool TryRetryOutputSaves();
    [[nodiscard]] bool MaybeRetryOutputSaves(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] bool TrySaveStateCache();

    std::unordered_map<std::string, SourceState> sources_;
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> state_cache_snapshot_;
    std::filesystem::path state_cache_path_;
    StateCacheLoader state_cache_loader_;
    TaskPersister task_persister_;
    LocalUserStateSaveScheduler state_cache_save_scheduler_;
    LocalUserStateSaveScheduler output_retry_scheduler_;
    LocalUserStateSaveStatus state_cache_save_status_;
    std::optional<std::string> active_source_identity_;
    std::uint64_t revision_ = 0;
    std::uint64_t active_source_tasks_generation_ = 0;
    bool state_cache_loaded_ = false;
    std::string state_cache_load_warning_;
};

}  // namespace specforge
