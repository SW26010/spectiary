#include "ui/sample_labeling_controller.h"

#include "domain/source_collection_manifest.h"
#include "ui/sample_annotation_labeling_rules.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kStateSaveDebounce = 500ms;
constexpr auto kStateSaveRetry = 2s;

bool HasPendingOutputSave(const SampleLabelingTask& task)
{
    return !task.pending_sample_indices.empty() || task.metadata_save_pending;
}

bool ShouldRetryOutputSave(const SampleLabelingTask& task)
{
    return task.output_path && HasPendingOutputSave(task) &&
           (task.save_state.kind == SampleLabelSaveStateKind::Pending ||
            task.save_state.kind == SampleLabelSaveStateKind::Failed);
}

bool OutputPathMatches(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code left_exists_error;
    std::error_code right_exists_error;
    const bool left_exists = std::filesystem::exists(left, left_exists_error) && !left_exists_error;
    const bool right_exists = std::filesystem::exists(right, right_exists_error) && !right_exists_error;
    std::error_code equivalent_error;
    if (left_exists && right_exists &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

SampleLabelResultMetadataSource SourceMetadataFromState(const SampleLabelingController::SourceState& state)
{
    SampleLabelResultMetadataSource source;
    source.source_name = state.source_name;
    source.source_fingerprint = state.source_fingerprint;
    source.context_fingerprint = state.context_fingerprint;
    source.spectrum_count = state.sample_count;
    return source;
}

bool CanDeleteTask(const SampleLabelingTask& task)
{
    if (!task.output_path) {
        return true;
    }
    return task.save_state.kind != SampleLabelSaveStateKind::Pending &&
           task.save_state.kind != SampleLabelSaveStateKind::Failed;
}

}  // namespace

SampleLabelingController::SampleLabelingController()
    : SampleLabelingController(DefaultSampleLabelingStateCachePath())
{
}

SampleLabelingController::SampleLabelingController(std::filesystem::path state_cache_path)
    : SampleLabelingController(
          std::move(state_cache_path),
          [](const std::filesystem::path& path) { return LoadSampleLabelingStateCache(path); })
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader)
    : SampleLabelingController(
          std::move(state_cache_path),
          std::move(state_cache_loader),
          [](SampleLabelingTask& task, const SampleLabelResultMetadataSource* source) {
              return PersistSampleLabelingTaskResult(task, source);
          })
{
}

SampleLabelingController::SampleLabelingController(
    std::filesystem::path state_cache_path,
    StateCacheLoader state_cache_loader,
    TaskPersister task_persister)
    : state_cache_path_(std::move(state_cache_path)),
      state_cache_loader_(std::move(state_cache_loader)),
      task_persister_(std::move(task_persister)),
      state_cache_save_scheduler_(kStateSaveDebounce, kStateSaveRetry),
      output_retry_scheduler_(kStateSaveRetry, kStateSaveRetry)
{
}

void SampleLabelingController::ActivateSource(std::string source_identity, std::size_t sample_count)
{
    EnsureStateCacheLoaded();
    if (source_identity.empty() || sample_count == 0) {
        ClearActiveSource();
        return;
    }

    const bool active_source_changed =
        !active_source_identity_ ||
        *active_source_identity_ != source_identity;
    SourceState* materialized = MaterializeSource(source_identity);
    SourceState& state = materialized == nullptr ? sources_[source_identity] : *materialized;
    bool tasks_replaced = false;
    if (state.sample_count != 0 && state.sample_count != sample_count) {
        state.tasks.clear();
        state.active_task_id.reset();
        tasks_replaced = true;
    }
    state.sample_count = sample_count;
    active_source_identity_ = std::move(source_identity);
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
    if (active_source_changed || tasks_replaced) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
}

void SampleLabelingController::ActivateSource(const SourceCollectionIdentity& identity)
{
    ActivateSource(identity.id, identity.spectrum_count);
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return;
    }
    state->source_name = identity.source_name;
    state->source_fingerprint = identity.source_fingerprint;
    state->context_fingerprint = identity.context_fingerprint;
    QueueStateSave();
    Touch();
}

BackgroundRetirementHandle SampleLabelingController::ActivatePreparedSource(
    const SourceCollectionIdentity& identity,
    std::optional<SourceState> prepared_state)
{
    if (identity.id.empty() || identity.spectrum_count == 0) {
        BackgroundRetirementHandle retired;
        if (prepared_state) {
            retired = MakeBackgroundRetirementHandle(std::move(*prepared_state));
        }
        ClearActiveSource();
        return retired;
    }
    const bool active_source_changed =
        !active_source_identity_ ||
        *active_source_identity_ != identity.id;
    auto existing = sources_.find(identity.id);
    BackgroundRetirementHandle retired;
    bool tasks_replaced = false;
    if (existing == sources_.end()) {
        SourceState state = prepared_state ? std::move(*prepared_state) : SourceState{};
        existing = sources_.emplace(identity.id, std::move(state)).first;
        tasks_replaced = true;
    } else if (existing->second.sample_count != 0 &&
               existing->second.sample_count != identity.spectrum_count) {
        auto retired_state = std::make_shared<SourceState>();
        *retired_state = std::move(existing->second);
        retired = std::move(retired_state);
        existing->second = prepared_state ? std::move(*prepared_state) : SourceState{};
        tasks_replaced = true;
    } else if (prepared_state) {
        retired = MakeBackgroundRetirementHandle(std::move(*prepared_state));
    }
    SourceState& state = existing->second;
    state.sample_count = identity.spectrum_count;
    state.source_name = identity.source_name;
    state.source_fingerprint = identity.source_fingerprint;
    state.context_fingerprint = identity.context_fingerprint;
    active_source_identity_ = identity.id;
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
    if (active_source_changed || tasks_replaced) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    return retired;
}

BackgroundRetirementHandle SampleLabelingController::AdoptPreparedStateCache(
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> cache_snapshot)
{
    if (!cache_snapshot || cache_snapshot == state_cache_snapshot_) {
        return {};
    }
    const bool first_load = !state_cache_loaded_;
    std::shared_ptr<const SampleLabelingStateCacheLoadResult> retired =
        std::exchange(state_cache_snapshot_, std::move(cache_snapshot));
    state_cache_loaded_ = true;
    if (first_load) {
        state_cache_load_warning_ = state_cache_snapshot_->warning;
    }
    Touch();
    return retired;
}

std::vector<BackgroundRetirementHandle> SampleLabelingController::ReleaseBackgroundResourcesForShutdown()
{
    const bool changed =
        state_cache_snapshot_ != nullptr || !sources_.empty() || active_source_identity_.has_value();
    std::vector<BackgroundRetirementHandle> resources;
    if (state_cache_snapshot_) {
        resources.push_back(std::move(state_cache_snapshot_));
    }
    if (!sources_.empty()) {
        resources.push_back(MakeBackgroundRetirementHandle(std::exchange(sources_, {})));
    }
    active_source_identity_.reset();
    if (changed) {
        BumpActiveSourceTasksGeneration();
        Touch();
    }
    return resources;
}

void SampleLabelingController::ClearActiveSource()
{
    if (!active_source_identity_) {
        return;
    }
    active_source_identity_.reset();
    BumpActiveSourceTasksGeneration();
    Touch();
}

void SampleLabelingController::RemoveSource(std::string_view source_identity)
{
    if (active_source_identity_ && *active_source_identity_ == source_identity) {
        active_source_identity_.reset();
        BumpActiveSourceTasksGeneration();
        Touch();
    }
}

SampleLabelingControllerView SampleLabelingController::View() const
{
    const SourceState* state = ActiveSource();
    return SampleLabelingControllerView{
        .active_task = ActiveTask(),
        .temporary_task = TemporaryTask(),
        .active_source_tasks = state == nullptr ? nullptr : &state->tasks,
        .revision = revision_};
}

std::uint64_t
SampleLabelingController::active_source_tasks_generation() const
{
    return active_source_tasks_generation_;
}

SampleLabelingTask* SampleLabelingController::ActiveTask()
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

const SampleLabelingTask* SampleLabelingController::ActiveTask() const
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

SampleLabelingTask* SampleLabelingController::TemporaryTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return nullptr;
    }
    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [](const auto& task) {
        return !task.output_path;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

const SampleLabelingTask* SampleLabelingController::TemporaryTask() const
{
    const SourceState* state = ActiveSource();
    if (state == nullptr) {
        return nullptr;
    }
    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [](const auto& task) {
        return !task.output_path;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

std::optional<SampleLabelingController::SourceState> SampleLabelingController::SourceStateForIdentity(
    std::string_view source_identity)
{
    EnsureStateCacheLoaded();
    const SourceState* state = MaterializeSource(source_identity);
    return state == nullptr ? std::nullopt : std::optional<SourceState>{*state};
}

SampleLabelingOperationResult SampleLabelingController::CreateTask(
    std::string task_id,
    std::string task_name)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty()) {
        return RejectOperation();
    }

    if (SampleLabelingTask* existing_temporary_task = TemporaryTask()) {
        if (state->active_task_id && *state->active_task_id == existing_temporary_task->task_id) {
            SampleLabelingOperationResult result = RejectOperation();
            result.accepted = true;
            return result;
        }
        state->active_task_id = existing_temporary_task->task_id;
        return CompleteMutation(
            nullptr,
            PersistencePolicy::ScheduleStateSave,
            TaskProjectionEffect::Unchanged);
    }

    auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (match == state->tasks.end()) {
        state->tasks.push_back(CreateSampleLabelingTask(std::move(task_id), std::move(task_name), state->sample_count));
        match = state->tasks.end() - 1;
    }
    if (state->active_task_id && *state->active_task_id == match->task_id) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = true;
        return result;
    }
    state->active_task_id = match->task_id;
    return CompleteMutation(
        &*match,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult
SampleLabelingController::StartOrResumeTemporaryTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr) {
        return RejectOperation();
    }

    const SampleLabelingTask* active_task = ActiveTask();
    const SampleLabelingTask* temporary_task =
        TemporaryTask();
    if (active_task != nullptr &&
        temporary_task != nullptr &&
        active_task->task_id ==
            temporary_task->task_id) {
        SampleLabelingOperationResult result =
            RejectOperation();
        result.accepted = true;
        return result;
    }
    if (active_task != nullptr &&
        !CanDeleteTask(*active_task)) {
        return RejectOperation();
    }

    std::string temporary_task_id;
    if (temporary_task != nullptr) {
        temporary_task_id = temporary_task->task_id;
    } else {
        temporary_task_id =
            TaskIdForNewSampleLabelingTask(
                kTemporarySampleLabelingTaskName,
                &state->tasks);
        state->tasks.push_back(
            CreateSampleLabelingTask(
                temporary_task_id,
                std::string{
                    kTemporarySampleLabelingTaskName},
                state->sample_count));
    }
    state->active_task_id =
        std::move(temporary_task_id);
    return CompleteMutation(
        nullptr,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::CreateTaskFromAnnotation(
    std::string task_id,
    std::string task_name,
    SampleLabelSet label_set,
    std::vector<int> values,
    std::filesystem::path output_path,
    bool metadata_clean)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty() || output_path.empty() || values.size() != state->sample_count) {
        return RejectOperation();
    }

    const auto output_match = std::find_if(state->tasks.begin(), state->tasks.end(), [&](const auto& task) {
        return task.output_path && task.values.size() == state->sample_count &&
               OutputPathMatches(*task.output_path, output_path);
    });
    if (output_match != state->tasks.end()) {
        if (output_match->task_id != task_id) {
            return RejectOperation();
        }
        if (state->active_task_id && *state->active_task_id == output_match->task_id) {
            SampleLabelingOperationResult result = RejectOperation();
            result.accepted = true;
            return result;
        }
        state->active_task_id = output_match->task_id;
        return CompleteMutation(
            nullptr,
            PersistencePolicy::ScheduleStateSave,
            TaskProjectionEffect::Unchanged);
    }

    const auto id_match = std::find_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (id_match != state->tasks.end()) {
        return RejectOperation();
    }

    SampleLabelingTask task = CreateSampleLabelingTask(std::move(task_id), std::move(task_name), state->sample_count);
    task.label_set = std::move(label_set);
    task.values = std::move(values);
    RebuildSampleLabelingTaskStatistics(task);
    task.output_path = std::move(output_path);
    if (metadata_clean) {
        MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
    } else {
        MarkSampleLabelTaskMetadataPending(task);
    }

    state->tasks.push_back(std::move(task));
    state->active_task_id = state->tasks.back().task_id;
    return CompleteMutation(
        &state->tasks.back(),
        metadata_clean
            ? PersistencePolicy::ScheduleStateSave
            : PersistencePolicy::PersistOutputIfSelected,
        TaskProjectionEffect::Changed);
}

SampleLabelingOperationResult SampleLabelingController::ActivateTask(std::string_view task_id)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty()) {
        return RejectOperation();
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (match == state->tasks.end()) {
        return RejectOperation();
    }
    if (state->active_task_id && *state->active_task_id == match->task_id) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = true;
        return result;
    }

    state->active_task_id = match->task_id;
    return CompleteMutation(
        nullptr,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::UpsertActiveLabel(SampleLabelDefinition label)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || !UpsertSampleLabel(task->label_set, std::move(label))) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkSampleLabelTaskMetadataPending(*task);
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::UpdateActiveLabel(
    int original_code,
    SampleLabelDefinition label,
    bool allow_used_code_change)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr ||
        !UpdateSampleLabel(*task, original_code, std::move(label), allow_used_code_change)) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkSampleLabelTaskMetadataPending(*task);
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RemoveActiveLabel(int code)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || !RemoveSampleLabel(*task, code)) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    MarkSampleLabelTaskMetadataPending(*task);
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RenameActiveTask(std::string task_name)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task_name.empty() || task->task_name == task_name) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr && !task_name.empty();
        return result;
    }

    task->task_name = std::move(task_name);
    MarkSampleLabelTaskMetadataPending(*task);
    return CompleteMutation(
        task,
        PersistencePolicy::PersistOutputIfSelected,
        task->output_path
            ? TaskProjectionEffect::Changed
            : TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SetActiveAutoAdvance(bool enabled)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->auto_advance == enabled) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    task->auto_advance = enabled;
    return CompleteMutation(
        task,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SetActiveSkipLabeledOnAdvance(bool enabled)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || task->skip_labeled_on_advance == enabled) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = task != nullptr;
        return result;
    }
    task->skip_labeled_on_advance = enabled;
    return CompleteMutation(
        task,
        PersistencePolicy::FlushStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::SaveActiveTemporaryTaskToOutput(
    std::filesystem::path output_path,
    std::string task_name)
{
    SampleLabelingTask* task = ActiveTask();
    SourceState* state = ActiveSource();
    if (task == nullptr || state == nullptr || task->output_path || output_path.empty() || task_name.empty()) {
        return RejectOperation();
    }

    const auto conflict = std::find_if(state->tasks.begin(), state->tasks.end(), [&](const auto& existing) {
        return existing.task_id != task->task_id && existing.output_path &&
               OutputPathMatches(*existing.output_path, output_path);
    });
    if (conflict != state->tasks.end()) {
        task->save_state.message_kind =
            SampleLabelSaveMessageKind::OutputPathAlreadyUsed;
        task->save_state.message.clear();
        SampleLabelingOperationResult result =
            CompleteMutation(
                task,
                PersistencePolicy::ScheduleStateSave,
                TaskProjectionEffect::Unchanged);
        result.accepted = false;
        return result;
    }

    SampleLabelingTask candidate = *task;
    candidate.task_name = std::move(task_name);
    SelectSampleLabelTaskOutputPath(candidate, std::move(output_path));
    const SampleLabelResultMetadataSource source = SourceMetadataFromState(*state);
    const SampleLabelTaskPersistResult persist_result =
        task_persister_(candidate, &source);
    if (!persist_result.output_saved) {
        task->save_state.kind = SampleLabelSaveStateKind::Failed;
        task->save_state.pending_count = candidate.save_state.pending_count;
        if (candidate.save_state.message_kind !=
            SampleLabelSaveMessageKind::None) {
            task->save_state.message_kind =
                candidate.save_state.message_kind;
        } else {
            task->save_state.message_kind =
                persist_result.message.empty()
                ? SampleLabelSaveMessageKind::OutputSaveFailed
                : SampleLabelSaveMessageKind::SystemDetail;
        }
        if (task->save_state.message_kind ==
            SampleLabelSaveMessageKind::SystemDetail) {
            task->save_state.message =
                !candidate.save_state.message.empty()
                ? candidate.save_state.message
                : persist_result.message;
        } else {
            task->save_state.message.clear();
        }
    } else {
        *task = std::move(candidate);
    }

    if (persist_result.output_saved) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    QueueStateSave();
    SampleLabelingOperationResult result;
    result.accepted = true;
    result.changed = true;
    result.output_save_attempted = true;
    result.output_saved = persist_result.output_saved;
    result.state_save_scheduled = true;
    result.state_save_attempted = true;
    result.state_saved = FlushStateCache();
    result.revision = revision_;
    return result;
}

bool SampleLabelingController::CanDeactivateActiveTask() const
{
    const SampleLabelingTask* task = ActiveTask();
    if (task == nullptr) {
        return false;
    }
    return CanDeleteTask(*task);
}

bool SampleLabelingController::CanDeleteActiveTask() const
{
    return CanDeactivateActiveTask();
}

SampleLabelingOperationResult SampleLabelingController::DeactivateActiveTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id || !CanDeactivateActiveTask()) {
        return RejectOperation();
    }
    state->active_task_id.reset();
    return CompleteMutation(
        nullptr,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::DeleteActiveTask()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id || !CanDeleteActiveTask()) {
        return RejectOperation();
    }

    const SampleLabelingTask* active_task = ActiveTask();
    const TaskProjectionEffect projection_effect =
        active_task != nullptr && active_task->output_path
        ? TaskProjectionEffect::Changed
        : TaskProjectionEffect::Unchanged;
    const std::string task_id = *state->active_task_id;
    state->tasks.erase(
        std::remove_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
            return task.task_id == task_id;
        }),
        state->tasks.end());
    state->active_task_id.reset();
    return CompleteMutation(
        nullptr,
        PersistencePolicy::ScheduleStateSave,
        projection_effect);
}

SampleLabelingOperationResult SampleLabelingController::RememberActivePosition(std::size_t sample_index)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr || sample_index >= task->values.size()) {
        return RejectOperation();
    }
    if (task->remembered_position && *task->remembered_position == sample_index) {
        SampleLabelingOperationResult result = RejectOperation();
        result.accepted = true;
        return result;
    }
    task->remembered_position = sample_index;
    return CompleteMutation(
        task,
        PersistencePolicy::ScheduleStateSave,
        TaskProjectionEffect::Unchanged);
}

SampleLabelingOperationResult SampleLabelingController::RejectOperation() const
{
    return SampleLabelingOperationResult{.revision = revision_};
}

SampleLabelingOperationResult SampleLabelingController::CompleteMutation(
    SampleLabelingTask* task,
    PersistencePolicy persistence,
    TaskProjectionEffect projection_effect)
{
    if (projection_effect == TaskProjectionEffect::Changed) {
        BumpActiveSourceTasksGeneration();
    }
    Touch();
    QueueStateSave();

    SampleLabelingOperationResult result;
    result.accepted = true;
    result.changed = true;
    result.state_save_scheduled = true;
    if (persistence == PersistencePolicy::PersistOutputIfSelected &&
        task != nullptr && task->output_path) {
        result.output_save_attempted = true;
        result.output_saved = PersistTaskOutput(*task);
        result.output_retry_scheduled = ShouldRetryOutputSave(*task);
        result.state_save_attempted = true;
        result.state_saved = FlushStateCache();
    } else if (persistence == PersistencePolicy::FlushStateSave) {
        result.state_save_attempted = true;
        result.state_saved = FlushStateCache();
    }
    result.revision = revision_;
    return result;
}

bool SampleLabelingController::PersistTaskOutput(
    SampleLabelingTask& task,
    const SourceState* source_state)
{
    const SourceState* state = source_state == nullptr ? ActiveSource() : source_state;
    const SampleLabelResultMetadataSource source_metadata = state == nullptr
        ? SampleLabelResultMetadataSource{}
        : SourceMetadataFromState(*state);
    const SampleLabelResultMetadataSource* source = state == nullptr ? nullptr : &source_metadata;
    const SampleLabelTaskPersistResult result = task_persister_(task, source);
    if (!result.output_saved && ShouldRetryOutputSave(task)) {
        QueueOutputRetry();
    }
    return result.output_saved;
}

void SampleLabelingController::
    BumpActiveSourceTasksGeneration()
{
    ++active_source_tasks_generation_;
}

void SampleLabelingController::Touch()
{
    ++revision_;
}

SampleLabelingWriteOperationResult SampleLabelingController::AssignLabel(
    std::size_t sample_index,
    int code)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr) {
        SampleLabelingWriteOperationResult result;
        result.operation = RejectOperation();
        return result;
    }
    SampleLabelingWriteOperationResult result;
    result.write = AssignSampleLabel(*task, sample_index, code);
    if (result.write.changed) {
        result.operation =
            CompleteMutation(
                task,
                PersistencePolicy::PersistOutputIfSelected,
                task->output_path
                    ? TaskProjectionEffect::Changed
                    : TaskProjectionEffect::Unchanged);
    } else {
        result.operation = RejectOperation();
        result.operation.accepted = result.write.accepted;
    }
    return result;
}

SampleLabelingWriteOperationResult SampleLabelingController::ClearLabel(std::size_t sample_index)
{
    SampleLabelingTask* task = ActiveTask();
    if (task == nullptr) {
        SampleLabelingWriteOperationResult result;
        result.operation = RejectOperation();
        return result;
    }
    SampleLabelingWriteOperationResult result;
    result.write = ClearSampleLabel(*task, sample_index);
    if (result.write.changed) {
        result.operation =
            CompleteMutation(
                task,
                PersistencePolicy::PersistOutputIfSelected,
                task->output_path
                    ? TaskProjectionEffect::Changed
                    : TaskProjectionEffect::Unchanged);
    } else {
        result.operation = RejectOperation();
        result.operation.accepted = result.write.accepted;
    }
    return result;
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

SampleLabelingController::SourceState* SampleLabelingController::MaterializeSource(
    std::string_view source_identity)
{
    const std::string identity{source_identity};
    if (const auto state = sources_.find(identity); state != sources_.end()) {
        return &state->second;
    }
    if (!state_cache_snapshot_) {
        return nullptr;
    }
    const auto cached = state_cache_snapshot_->cache.sources.find(identity);
    if (cached == state_cache_snapshot_->cache.sources.end()) {
        return nullptr;
    }
    return &sources_.emplace(identity, cached->second).first->second;
}

void SampleLabelingController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    SampleLabelingStateCacheLoadResult result = state_cache_loader_(state_cache_path_);
    for (auto& [identity, state] : result.cache.sources) {
        sources_.try_emplace(std::move(identity), std::move(state));
    }
    state_cache_load_warning_ = std::move(result.warning);
    Touch();
}

void SampleLabelingController::QueueStateSave()
{
    state_cache_save_scheduler_.MarkDirty();
    state_cache_save_status_.ClearRecovered();
}

void SampleLabelingController::QueueOutputRetry()
{
    output_retry_scheduler_.MarkDirty();
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
            all_succeeded = PersistTaskOutput(task, &state) && all_succeeded;
        }
    }

    if (!attempted) {
        return true;
    }

    QueueStateSave();
    Touch();
    return all_succeeded;
}

bool SampleLabelingController::MaybeRetryOutputSaves(LocalUserStateSaveScheduler::TimePoint now)
{
    if (!output_retry_scheduler_.ShouldAttemptSave(now)) {
        return false;
    }

    const bool all_succeeded = TryRetryOutputSaves();
    if (all_succeeded) {
        output_retry_scheduler_.MarkSaveSucceeded();
    } else {
        output_retry_scheduler_.MarkSaveFailed();
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
                    task.pending_sample_indices.clear();
                    task.metadata_save_pending = false;
                    task.save_state.pending_count = 0;
                    if (task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                        task.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
                        if (task.save_state.message_kind ==
                            SampleLabelSaveMessageKind::None) {
                            task.save_state.message.clear();
                        }
                    }
                    continue;
                }
                task.save_state.pending_count = task.pending_sample_indices.size();
                if (!HasPendingOutputSave(task) &&
                    task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
                } else if (HasPendingOutputSave(task) &&
                           task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    task.save_state.kind = SampleLabelSaveStateKind::Pending;
                    task.save_state.message_kind =
                        SampleLabelSaveMessageKind::None;
                    task.save_state.message.clear();
                }
            }
        }
    };

    std::unordered_map<std::string, SourceState> sources_to_write =
        state_cache_snapshot_ ? state_cache_snapshot_->cache.sources
                              : std::unordered_map<std::string, SourceState>{};
    for (const auto& [identity, state] : sources_) {
        sources_to_write[identity] = state;
    }
    normalize_saved_states(sources_to_write);
    SampleLabelingStateCache cache;
    cache.sources = std::move(sources_to_write);
    const bool saved = SaveSampleLabelingStateCache(state_cache_path_, cache);
    if (saved) {
        normalize_saved_states(sources_);
        state_cache_load_warning_.clear();
        state_cache_save_scheduler_.MarkSaveSucceeded(state_cache_save_status_);
    } else {
        state_cache_save_scheduler_.MarkDirty();
        state_cache_save_status_.MarkFailed("could not write local sample-labeling task record");
    }
    Touch();
    return saved;
}

void SampleLabelingController::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    const bool output_retry_attempted = MaybeRetryOutputSaves(now);
    if (output_retry_attempted && state_cache_save_scheduler_.dirty()) {
        if (!TrySaveStateCache()) {
            state_cache_save_scheduler_.MarkSaveFailed();
        }
        return;
    }

    if (!state_cache_save_scheduler_.ShouldAttemptSave(now)) {
        return;
    }

    if (!TrySaveStateCache()) {
        state_cache_save_scheduler_.MarkSaveFailed();
    }
}

std::optional<LocalUserStateSaveScheduler::TimePoint> SampleLabelingController::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        state_cache_save_scheduler_.next_attempt_time();
    const auto output_retry_deadline = output_retry_scheduler_.next_attempt_time();
    if (output_retry_deadline && (!deadline || *output_retry_deadline < *deadline)) {
        deadline = output_retry_deadline;
    }
    return deadline;
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

LocalUserStatePersistenceStatus
SampleLabelingController::PersistenceStatus() const
{
    return {
        .retrying = state_cache_save_status_.failed(),
        .recovered = state_cache_save_status_.recovered(),
        .load_warning = state_cache_load_warning_,
        .save_message = state_cache_save_status_.message(),
    };
}

}  // namespace specforge
