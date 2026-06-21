#include "ui/sample_labeling_controller.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace specforge {
namespace {

constexpr std::uint64_t kStateSaveDebounceFrames = 30;
constexpr std::uint64_t kStateSaveRetryFrames = 120;

bool ShouldRetryOutputSave(const SampleLabelingTask& task)
{
    return task.output_path && !task.pending_sample_indices.empty() &&
           (task.save_state.kind == SampleLabelSaveStateKind::Pending ||
            task.save_state.kind == SampleLabelSaveStateKind::Failed);
}

}  // namespace

SampleLabelingController::SampleLabelingController()
    : SampleLabelingController(DefaultSampleLabelingStateCachePath())
{
}

SampleLabelingController::SampleLabelingController(std::filesystem::path state_cache_path)
    : state_cache_path_(std::move(state_cache_path)),
      state_cache_save_scheduler_(kStateSaveDebounceFrames, kStateSaveRetryFrames)
{
}

void SampleLabelingController::ActivateSource(std::string source_identity, std::size_t sample_count)
{
    EnsureStateCacheLoaded();
    if (source_identity.empty() || sample_count == 0) {
        ClearActiveSource();
        return;
    }

    SourceState& state = sources_[source_identity];
    if (state.sample_count != 0 && state.sample_count != sample_count) {
        state.tasks.clear();
        state.active_task_id.reset();
    }
    state.sample_count = sample_count;
    active_source_identity_ = std::move(source_identity);
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
}

void SampleLabelingController::ClearActiveSource()
{
    active_source_identity_.reset();
}

void SampleLabelingController::RemoveSource(std::string_view source_identity)
{
    if (active_source_identity_ && *active_source_identity_ == source_identity) {
        active_source_identity_.reset();
    }
}

bool SampleLabelingController::has_active_source() const
{
    return ActiveSource() != nullptr;
}

SampleLabelingTask* SampleLabelingController::active_task()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

const SampleLabelingTask* SampleLabelingController::active_task() const
{
    const SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

SampleLabelingTask* SampleLabelingController::CreateTask(std::string task_id, std::string task_name)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty()) {
        return nullptr;
    }

    auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (match == state->tasks.end()) {
        state->tasks.push_back(CreateSampleLabelingTask(std::move(task_id), std::move(task_name), state->sample_count));
        match = state->tasks.end() - 1;
    }
    state->active_task_id = match->task_id;
    QueueStateSave();
    return active_task();
}

bool SampleLabelingController::UpsertActiveLabel(SampleLabelDefinition label)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || !UpsertSampleLabel(task->label_set, std::move(label))) {
        return false;
    }
    QueueStateSave();
    return true;
}

bool SampleLabelingController::SetActiveTaskOutputPath(std::filesystem::path output_path)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || output_path.empty()) {
        return false;
    }
    SelectSampleLabelTaskOutputPath(*task, std::move(output_path));
    if (ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    return true;
}

bool SampleLabelingController::RememberActivePosition(std::size_t sample_index)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || sample_index >= task->values.size()) {
        return false;
    }
    task->remembered_position = sample_index;
    QueueStateSave();
    return true;
}

bool SampleLabelingController::PersistActiveTask()
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }

    if (!task->output_path) {
        return PersistActiveTaskRecord();
    }

    const SampleLabelTaskPersistResult result = PersistSampleLabelingTaskResult(*task);
    if (!result.output_saved && ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    const bool record_saved = FlushStateCache();
    return result.output_saved && record_saved;
}

bool SampleLabelingController::PersistActiveTaskRecord()
{
    QueueStateSave();
    return FlushStateCache();
}

bool SampleLabelingController::MarkActiveOutputPersisted()
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }
    MarkSampleLabelTaskPersisted(*task, SampleLabelSaveStateKind::AutosavedToOutput);
    QueueStateSave();
    return true;
}

bool SampleLabelingController::MarkActiveOutputSaveFailed(std::string message)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }
    MarkSampleLabelTaskSaveFailed(*task, std::move(message));
    if (ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    return true;
}

SampleLabelWriteResult SampleLabelingController::AssignLabel(std::size_t sample_index, int code)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return {};
    }
    SampleLabelWriteResult result = AssignSampleLabel(*task, sample_index, code);
    if (result.changed) {
        if (ShouldRetryOutputSave(*task)) {
            QueueOutputRetry();
        }
        QueueStateSave();
    }
    return result;
}

SampleLabelWriteResult SampleLabelingController::ClearLabel(std::size_t sample_index)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return {};
    }
    SampleLabelWriteResult result = ClearSampleLabel(*task, sample_index);
    if (result.changed) {
        if (ShouldRetryOutputSave(*task)) {
            QueueOutputRetry();
        }
        QueueStateSave();
    }
    return result;
}

std::optional<int> SampleLabelingController::LabelCodeForShortcut(char shortcut) const
{
    const SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return std::nullopt;
    }
    return SampleLabelCodeForShortcut(task->label_set, shortcut);
}

SampleLabelingController::SourceState* SampleLabelingController::ActiveSource()
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

const SampleLabelingController::SourceState* SampleLabelingController::ActiveSource() const
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

void SampleLabelingController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    SampleLabelingStateCacheLoadResult result = LoadSampleLabelingStateCache(state_cache_path_);
    sources_ = std::move(result.cache.sources);
    state_cache_load_warning_ = std::move(result.warning);
}

void SampleLabelingController::QueueStateSave()
{
    state_cache_save_scheduler_.MarkDirty();
    state_cache_save_status_.Clear();
}

void SampleLabelingController::QueueOutputRetry()
{
    output_retry_pending_ = true;
}

bool SampleLabelingController::TryRetryOutputSaves()
{
    EnsureStateCacheLoaded();

    bool attempted = false;
    bool all_succeeded = true;
    for (auto& [identity, state] : sources_) {
        (void)identity;
        for (SampleLabelingTask& task : state.tasks) {
            if (!ShouldRetryOutputSave(task)) {
                continue;
            }
            attempted = true;
            const SampleLabelTaskPersistResult result = PersistSampleLabelingTaskResult(task);
            all_succeeded = all_succeeded && result.output_saved;
        }
    }

    if (!attempted) {
        output_retry_pending_ = false;
        next_output_retry_frame_ = 0;
        return true;
    }

    QueueStateSave();
    return all_succeeded;
}

bool SampleLabelingController::MaybeRetryOutputSaves(std::uint64_t frame_index)
{
    if (!output_retry_pending_) {
        return false;
    }
    if (next_output_retry_frame_ == 0) {
        next_output_retry_frame_ = frame_index + kStateSaveRetryFrames;
        return false;
    }
    if (frame_index < next_output_retry_frame_) {
        return false;
    }

    const bool all_succeeded = TryRetryOutputSaves();
    if (all_succeeded) {
        output_retry_pending_ = false;
        next_output_retry_frame_ = 0;
    } else {
        output_retry_pending_ = true;
        next_output_retry_frame_ = frame_index + kStateSaveRetryFrames;
    }
    return true;
}

bool SampleLabelingController::TrySaveStateCache()
{
    EnsureStateCacheLoaded();
    const auto normalize_saved_states = [](std::unordered_map<std::string, SourceState>& sources) {
        for (auto& [identity, state] : sources) {
            (void)identity;
            for (SampleLabelingTask& task : state.tasks) {
                if (!task.output_path) {
                    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::InternalDraftOnly);
                    continue;
                }
                task.save_state.pending_count = task.pending_sample_indices.size();
                if (task.pending_sample_indices.empty() &&
                    task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
                } else if (!task.pending_sample_indices.empty() &&
                           task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    task.save_state.kind = SampleLabelSaveStateKind::Pending;
                    task.save_state.message.clear();
                }
            }
        }
    };

    std::unordered_map<std::string, SourceState> sources_to_write = sources_;
    normalize_saved_states(sources_to_write);
    SampleLabelingStateCache cache;
    cache.sources = std::move(sources_to_write);
    const bool saved = SaveSampleLabelingStateCache(state_cache_path_, cache);
    if (saved) {
        normalize_saved_states(sources_);
        state_cache_save_scheduler_.MarkSaveSucceeded(state_cache_save_status_);
    } else {
        state_cache_save_scheduler_.MarkDirty();
        state_cache_save_status_.MarkFailed("could not write local sample-labeling task record");
    }
    return saved;
}

void SampleLabelingController::MaybeSaveStateCache(std::uint64_t frame_index)
{
    const bool output_retry_attempted = MaybeRetryOutputSaves(frame_index);
    if (output_retry_attempted && state_cache_save_scheduler_.dirty()) {
        if (!TrySaveStateCache()) {
            state_cache_save_scheduler_.MarkSaveFailed(frame_index);
        }
        return;
    }

    if (!state_cache_save_scheduler_.ShouldAttemptSave(frame_index)) {
        return;
    }

    if (!TrySaveStateCache()) {
        state_cache_save_scheduler_.MarkSaveFailed(frame_index);
    }
}

bool SampleLabelingController::FlushStateCache()
{
    if (!state_cache_save_scheduler_.dirty()) {
        return true;
    }
    return TrySaveStateCache();
}

bool SampleLabelingController::state_save_pending() const
{
    return state_cache_save_scheduler_.dirty();
}

bool SampleLabelingController::state_save_failed() const
{
    return state_cache_save_status_.failed();
}

std::string_view SampleLabelingController::state_save_error() const
{
    return state_cache_save_status_.message_view();
}

std::string_view SampleLabelingController::state_load_warning() const
{
    return state_cache_load_warning_;
}

}  // namespace specforge
