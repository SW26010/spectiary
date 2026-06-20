#pragma once

#include "app/local_user_state.h"
#include "domain/sample_labeling.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

class SampleLabelingController {
public:
    struct SourceState {
        std::size_t sample_count = 0;
        std::vector<SampleLabelingTask> tasks;
        std::optional<std::string> active_task_id;
    };

    SampleLabelingController();
    explicit SampleLabelingController(std::filesystem::path state_cache_path);

    void ActivateSource(std::string source_identity, std::size_t sample_count);
    void ClearActiveSource();
    void RemoveSource(std::string_view source_identity);

    [[nodiscard]] bool has_active_source() const;
    [[nodiscard]] SampleLabelingTask* active_task();
    [[nodiscard]] const SampleLabelingTask* active_task() const;
    [[nodiscard]] SampleLabelingTask* CreateTask(std::string task_id, std::string task_name);
    [[nodiscard]] bool UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] bool SetActiveTaskOutputPath(std::filesystem::path output_path);
    [[nodiscard]] bool RememberActivePosition(std::size_t sample_index);
    [[nodiscard]] bool PersistActiveTask();
    [[nodiscard]] bool PersistActiveTaskRecord();
    [[nodiscard]] bool MarkActiveOutputPersisted();
    [[nodiscard]] bool MarkActiveOutputSaveFailed(std::string message);
    void MaybeSaveStateCache(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCache();
    [[nodiscard]] bool state_save_pending() const;
    [[nodiscard]] bool state_save_failed() const;
    [[nodiscard]] std::string_view state_save_error() const;
    [[nodiscard]] std::string_view state_load_warning() const;

    [[nodiscard]] SampleLabelWriteResult AssignLabel(std::size_t sample_index, int code);
    [[nodiscard]] SampleLabelWriteResult ClearLabel(std::size_t sample_index);
    [[nodiscard]] std::optional<int> LabelCodeForShortcut(char shortcut) const;

private:
    [[nodiscard]] SourceState* ActiveSource();
    [[nodiscard]] const SourceState* ActiveSource() const;
    void EnsureStateCacheLoaded();
    void QueueStateSave();
    void QueueOutputRetry();
    [[nodiscard]] bool TryRetryOutputSaves();
    [[nodiscard]] bool MaybeRetryOutputSaves(std::uint64_t frame_index);
    [[nodiscard]] bool TrySaveStateCache();

    std::unordered_map<std::string, SourceState> sources_;
    std::filesystem::path state_cache_path_;
    LocalUserStateSaveScheduler state_cache_save_scheduler_;
    LocalUserStateSaveStatus state_cache_save_status_;
    std::optional<std::string> active_source_identity_;
    bool state_cache_loaded_ = false;
    bool output_retry_pending_ = false;
    std::uint64_t next_output_retry_frame_ = 0;
    std::string state_cache_load_warning_;
};

}  // namespace specforge
