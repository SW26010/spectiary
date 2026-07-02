#include "app/local_user_state.h"

#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr std::string_view kPathKindAbsolute = "absolute";
constexpr std::string_view kPathKindPackageRelative = "package_relative";

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path PathFromUtf8(std::string_view value)
{
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char character : value) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(utf8);
}

std::filesystem::path NormalizedAbsolutePath(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute_path = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    const std::filesystem::path candidate = error ? path : absolute_path;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
    return (error ? candidate : canonical).lexically_normal();
}

bool IsDotDot(const std::filesystem::path& path)
{
    return path == "..";
}

bool IsSafePackageRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& part : path) {
        if (IsDotDot(part)) {
            return false;
        }
    }
    return true;
}

bool TryMakePackageRelativePath(const std::filesystem::path& path, std::filesystem::path& relative_path)
{
    const RuntimePaths runtime_paths = DefaultRuntimePaths();
    if (runtime_paths.release_profile != ReleaseProfile::Portable || runtime_paths.package_root.empty()) {
        return false;
    }

    const std::filesystem::path package_root = NormalizedAbsolutePath(runtime_paths.package_root);
    const std::filesystem::path absolute_path = NormalizedAbsolutePath(path);
    std::error_code error;
    std::filesystem::path candidate = std::filesystem::relative(absolute_path, package_root, error);
    if (error || !IsSafePackageRelativePath(candidate)) {
        return false;
    }

    relative_path = candidate.lexically_normal();
    return true;
}

std::optional<std::filesystem::path> TryRebaseLegacyPackagePath(const std::filesystem::path& path)
{
    const RuntimePaths runtime_paths = DefaultRuntimePaths();
    if (runtime_paths.release_profile != ReleaseProfile::Portable || runtime_paths.package_root.empty()) {
        return std::nullopt;
    }

    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
        return path;
    }

    const std::filesystem::path package_name = runtime_paths.package_root.filename();
    if (package_name.empty()) {
        return std::nullopt;
    }

    std::filesystem::path suffix;
    bool found_package_root_name = false;
    for (const std::filesystem::path& part : path) {
        if (found_package_root_name) {
            suffix /= part;
            continue;
        }
        if (part == package_name) {
            found_package_root_name = true;
        }
    }

    if (!found_package_root_name || suffix.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path rebased = runtime_paths.package_root / suffix;
    if (std::filesystem::exists(rebased, error) && !error) {
        return rebased;
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path)
{
    return DefaultRuntimePaths().local_user_state_root / std::move(relative_path);
}

std::string UserPathDisplayText(const std::filesystem::path& path)
{
    std::filesystem::path relative_path;
    if (TryMakePackageRelativePath(path, relative_path)) {
        return PathToUtf8(relative_path);
    }
    return PathToUtf8(path);
}

std::optional<std::filesystem::path> ReadPersistedPathReference(const JsonValue& value)
{
    if (value.kind == JsonValue::Kind::String) {
        std::filesystem::path legacy_path = PathFromUtf8(value.string_value);
        if (std::optional<std::filesystem::path> rebased = TryRebaseLegacyPackagePath(legacy_path)) {
            return rebased;
        }
        return legacy_path;
    }

    if (value.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    const std::optional<std::string> path_kind = ReadJsonStringMember(value, "path_kind");
    const std::optional<std::string> path_text = ReadJsonStringMember(value, "path");
    if (!path_kind || !path_text || path_text->empty()) {
        return std::nullopt;
    }

    if (*path_kind == kPathKindPackageRelative) {
        const std::filesystem::path relative_path = PathFromUtf8(*path_text);
        if (!IsSafePackageRelativePath(relative_path)) {
            return std::nullopt;
        }
        return DefaultRuntimePaths().package_root / relative_path;
    }
    if (*path_kind == kPathKindAbsolute) {
        return PathFromUtf8(*path_text);
    }

    return std::nullopt;
}

void WritePersistedPathReference(std::ostream& stream, const std::filesystem::path& path)
{
    std::filesystem::path relative_path;
    if (TryMakePackageRelativePath(path, relative_path)) {
        stream << "{ \"path_kind\": ";
        WriteJsonString(stream, kPathKindPackageRelative);
        stream << ", \"path\": ";
        WriteJsonString(stream, PathToUtf8(relative_path));
        stream << " }";
        return;
    }

    stream << "{ \"path_kind\": ";
    WriteJsonString(stream, kPathKindAbsolute);
    stream << ", \"path\": ";
    WriteJsonString(stream, PathToUtf8(path));
    stream << " }";
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
