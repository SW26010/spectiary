#include "app/local_user_state.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

std::filesystem::path DefaultLocalUserStateRoot()
{
#ifdef _WIN32
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return std::filesystem::path(value) / L"SpecForge";
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "SpecForge";
}

}  // namespace

std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path)
{
    return DefaultLocalUserStateRoot() / std::move(relative_path);
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
