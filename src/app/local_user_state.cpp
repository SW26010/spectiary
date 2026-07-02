#include "app/local_user_state.h"

#include "app/runtime_paths.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {

std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path)
{
    return DefaultRuntimePaths().local_user_state_root / std::move(relative_path);
}

void LocalUserStateSaveStatus::Clear()
{
    failed_ = false;
    message_.clear();
}

void LocalUserStateSaveStatus::MarkFailed(std::string message)
{
    failed_ = true;
    message_ = std::move(message);
}

bool LocalUserStateSaveStatus::failed() const
{
    return failed_;
}

const std::string& LocalUserStateSaveStatus::message() const
{
    return message_;
}

std::string_view LocalUserStateSaveStatus::message_view() const
{
    return message_;
}

LocalUserStateSaveScheduler::LocalUserStateSaveScheduler(
    std::uint64_t debounce_frames,
    std::uint64_t retry_frames)
    : debounce_frames_(debounce_frames),
      retry_frames_(retry_frames)
{
}

void LocalUserStateSaveScheduler::MarkDirty()
{
    dirty_ = true;
}

void LocalUserStateSaveScheduler::MarkDirty(std::uint64_t frame_index)
{
    dirty_ = true;
    next_save_frame_ = std::max(next_save_frame_, frame_index + debounce_frames_);
}

bool LocalUserStateSaveScheduler::ShouldAttemptSave(std::uint64_t frame_index)
{
    if (!dirty_) {
        return false;
    }
    if (next_save_frame_ == 0) {
        next_save_frame_ = frame_index + debounce_frames_;
        return false;
    }
    return frame_index >= next_save_frame_;
}

void LocalUserStateSaveScheduler::MarkSaveSucceeded()
{
    dirty_ = false;
    next_save_frame_ = 0;
}

void LocalUserStateSaveScheduler::MarkSaveSucceeded(LocalUserStateSaveStatus& status)
{
    MarkSaveSucceeded();
    status.Clear();
}

void LocalUserStateSaveScheduler::MarkSaveFailed(std::uint64_t frame_index)
{
    dirty_ = true;
    next_save_frame_ = frame_index + retry_frames_;
}

void LocalUserStateSaveScheduler::MarkSaveFailed(
    std::uint64_t frame_index,
    LocalUserStateSaveStatus& status,
    std::string message)
{
    MarkSaveFailed(frame_index);
    status.MarkFailed(std::move(message));
}

bool LocalUserStateSaveScheduler::dirty() const
{
    return dirty_;
}

}  // namespace specforge
