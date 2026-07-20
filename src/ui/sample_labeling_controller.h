#pragma once

#include "app/local_user_state.h"
#include "ui/background_retirement.h"
#include "ui/sample_labeling_state_cache_io.h"

#include <cstddef>
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

class SampleLabelingController {
public:
    using SourceState = SampleLabelingSourceState;
    using StateCacheLoader =
        std::function<SampleLabelingStateCacheLoadResult(const std::filesystem::path&)>;

    SampleLabelingController();
    explicit SampleLabelingController(std::filesystem::path state_cache_path);
    SampleLabelingController(
        std::filesystem::path state_cache_path,
        StateCacheLoader state_cache_loader);

    void ActivateSource(std::string source_identity, std::size_t sample_count);
    void ActivateSource(const SourceCollectionIdentity& identity);
    [[nodiscard]] BackgroundRetirementHandle ActivatePreparedSource(
        const SourceCollectionIdentity& identity,
        std::optional<SourceState> prepared_state,
        std::string state_load_warning = {});
    [[nodiscard]] BackgroundRetirementHandle AdoptPreparedStateCache(
        std::shared_ptr<const SampleLabelingStateCacheLoadResult> cache_snapshot);
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();
    void ClearActiveSource();
    void RemoveSource(std::string_view source_identity);

    [[nodiscard]] bool has_active_source() const;
    [[nodiscard]] SampleLabelingTask* active_task();
    [[nodiscard]] const SampleLabelingTask* active_task() const;
    [[nodiscard]] SampleLabelingTask* temporary_task();
    [[nodiscard]] const SampleLabelingTask* temporary_task() const;
    [[nodiscard]] const std::vector<SampleLabelingTask>* active_source_tasks() const;
    [[nodiscard]] std::optional<SourceState> SourceStateForIdentity(
        std::string_view source_identity);
    [[nodiscard]] const SampleLabelingTask* FindActiveSourceTaskByOutputPath(
        const std::filesystem::path& output_path,
        std::string_view task_id,
        std::size_t sample_count) const;
    [[nodiscard]] SampleLabelingTask* CreateTask(std::string task_id, std::string task_name);
    [[nodiscard]] SampleLabelingTask* CreateTaskFromAnnotation(
        std::string task_id,
        std::string task_name,
        SampleLabelSet label_set,
        std::vector<int> values,
        std::filesystem::path output_path,
        bool metadata_clean);
    [[nodiscard]] bool ActivateTask(std::string_view task_id);
    [[nodiscard]] bool UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] bool UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change);
    [[nodiscard]] bool RemoveActiveLabel(int code);
    [[nodiscard]] bool RenameActiveTask(std::string task_name);
    [[nodiscard]] bool SetActiveTaskOutputPath(std::filesystem::path output_path);
    [[nodiscard]] bool SaveActiveTemporaryTaskToOutput(
        std::filesystem::path output_path,
        std::string task_name);
    [[nodiscard]] bool CanDeactivateActiveTask() const;
    [[nodiscard]] bool CanDeleteActiveTask() const;
    [[nodiscard]] bool DeactivateActiveTask();
    [[nodiscard]] bool DeleteActiveTask();
    [[nodiscard]] bool RememberActivePosition(std::size_t sample_index);
    [[nodiscard]] bool PersistActiveTask();
    [[nodiscard]] bool PersistActiveTaskRecord();
    [[nodiscard]] bool MarkActiveOutputPersisted();
    [[nodiscard]] bool MarkActiveOutputSaveFailed(std::string message);
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCache();
    [[nodiscard]] bool state_save_pending() const;
    [[nodiscard]] bool state_save_failed() const;
    [[nodiscard]] std::string_view state_save_error() const;
    [[nodiscard]] std::string_view state_load_warning() const;

    [[nodiscard]] SampleLabelWriteResult AssignLabel(std::size_t sample_index, int code);
    [[nodiscard]] SampleLabelWriteResult ClearLabel(std::size_t sample_index);

private:
    [[nodiscard]] SourceState* ActiveSource();
    [[nodiscard]] const SourceState* ActiveSource() const;
    [[nodiscard]] SourceState* MaterializeSource(std::string_view source_identity);
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
    LocalUserStateSaveScheduler state_cache_save_scheduler_;
    LocalUserStateSaveScheduler output_retry_scheduler_;
    LocalUserStateSaveStatus state_cache_save_status_;
    std::optional<std::string> active_source_identity_;
    bool state_cache_loaded_ = false;
    std::string state_cache_load_warning_;
};

}  // namespace specforge
