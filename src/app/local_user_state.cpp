#include "app/local_user_state.h"

#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"

#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace specforge {

std::string LocalUserStatePathToUtf8(
    const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

namespace {

constexpr std::string_view kPathKindAbsolute = "absolute";
constexpr std::string_view kPathKindPackageRelative = "package_relative";

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
    return (error ? path : absolute_path).lexically_normal();
}

bool WindowsPathComponentEquals(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const std::wstring& left_text = left.native();
    const std::wstring& right_text = right.native();
    if (left_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return left_text == right_text;
    }
    return CompareStringOrdinal(
               left_text.data(),
               static_cast<int>(left_text.size()),
               right_text.data(),
               static_cast<int>(right_text.size()),
               TRUE) == CSTR_EQUAL;
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
    if (runtime_paths.storage_profile != StorageProfile::Portable || runtime_paths.package_root.empty()) {
        return false;
    }

    const std::filesystem::path package_root = NormalizedAbsolutePath(runtime_paths.package_root);
    const std::filesystem::path absolute_path = NormalizedAbsolutePath(path);

    auto path_part = absolute_path.begin();
    for (auto package_part = package_root.begin(); package_part != package_root.end(); ++package_part, ++path_part) {
        if (path_part == absolute_path.end() || !WindowsPathComponentEquals(*path_part, *package_part)) {
            return false;
        }
    }

    std::filesystem::path candidate;
    for (; path_part != absolute_path.end(); ++path_part) {
        candidate /= *path_part;
    }
    if (candidate.empty()) {
        candidate = ".";
    }
    if (!IsSafePackageRelativePath(candidate)) {
        return false;
    }

    relative_path = std::move(candidate);
    return true;
}

std::optional<std::filesystem::path> TryRebaseLegacyPackagePath(const std::filesystem::path& path)
{
    const RuntimePaths runtime_paths = DefaultRuntimePaths();
    if (runtime_paths.storage_profile != StorageProfile::Portable || runtime_paths.package_root.empty()) {
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
        return LocalUserStatePathToUtf8(relative_path);
    }
    return LocalUserStatePathToUtf8(path);
}

std::optional<std::filesystem::path> ReadPersistedPathReference(const nlohmann::json& value)
{
    if (value.type() == nlohmann::json::value_t::string) {
        std::filesystem::path legacy_path = PathFromUtf8(value.get_ref<const std::string&>());
        if (std::optional<std::filesystem::path> rebased = TryRebaseLegacyPackagePath(legacy_path)) {
            return rebased;
        }
        return legacy_path;
    }

    if (value.type() != nlohmann::json::value_t::object) {
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
        if (relative_path == ".") {
            return DefaultRuntimePaths().package_root;
        }
        return DefaultRuntimePaths().package_root / relative_path;
    }
    if (*path_kind == kPathKindAbsolute) {
        return PathFromUtf8(*path_text);
    }

    return std::nullopt;
}

nlohmann::json PersistedPathReferenceJson(
    const std::filesystem::path& path)
{
    std::filesystem::path relative_path;
    if (TryMakePackageRelativePath(path, relative_path)) {
        return nlohmann::json::object({
            {"path_kind",
             nlohmann::json(kPathKindPackageRelative)},
            {"path",
             nlohmann::json(
                 LocalUserStatePathToUtf8(relative_path))},
        });
    }
    return nlohmann::json::object({
        {"path_kind", nlohmann::json(kPathKindAbsolute)},
        {"path",
         nlohmann::json(
             LocalUserStatePathToUtf8(path))},
    });
}

void WritePersistedPathReference(std::ostream& stream, const std::filesystem::path& path)
{
    std::filesystem::path relative_path;
    if (TryMakePackageRelativePath(path, relative_path)) {
        stream << "{ \"path_kind\": ";
        WriteJsonString(stream, kPathKindPackageRelative);
        stream << ", \"path\": ";
        WriteJsonString(
            stream,
            LocalUserStatePathToUtf8(relative_path));
        stream << " }";
        return;
    }

    stream << "{ \"path_kind\": ";
    WriteJsonString(stream, kPathKindAbsolute);
    stream << ", \"path\": ";
    WriteJsonString(
        stream,
        LocalUserStatePathToUtf8(path));
    stream << " }";
}

void LocalUserStateSaveStatus::Clear()
{
    failed_ = false;
    recovered_ = false;
    message_.clear();
}

void AppendLocalUserStateHealth(
    LocalUserStateHealthView& health,
    LocalUserStateArea area,
    const LocalUserStatePersistenceStatus& status)
{
    const auto promote = [&](LocalUserStateHealthKind kind) {
        const auto priority = [](LocalUserStateHealthKind value) {
            switch (value) {
            case LocalUserStateHealthKind::Healthy:
                return 0;
            case LocalUserStateHealthKind::Recovered:
                return 1;
            case LocalUserStateHealthKind::Warning:
                return 2;
            case LocalUserStateHealthKind::Retrying:
                return 3;
            }
            return 0;
        };
        if (priority(kind) > priority(health.kind)) {
            health.kind = kind;
        }
    };
    const auto append_message = [&](
                                    LocalUserStateHealthMessageKind kind,
                                    std::string_view diagnostic_detail) {
        health.messages.push_back({
            .area = area,
            .kind = kind,
            .diagnostic_detail =
                std::string(diagnostic_detail),
        });
    };
    const auto diagnostic_detail = [](
                                       std::string_view preferred,
                                       std::string_view legacy) {
        return preferred.empty() ? legacy : preferred;
    };

    if (!status.load_warning.empty()) {
        promote(LocalUserStateHealthKind::Warning);
        append_message(
            LocalUserStateHealthMessageKind::LoadWarning,
            diagnostic_detail(
                status.load_diagnostic_detail,
                status.load_warning));
    }
    if (status.retrying) {
        promote(LocalUserStateHealthKind::Retrying);
        append_message(
            LocalUserStateHealthMessageKind::SaveRetrying,
            diagnostic_detail(
                status.save_diagnostic_detail,
                status.save_message));
    } else if (status.recovered) {
        promote(LocalUserStateHealthKind::Recovered);
        append_message(
            LocalUserStateHealthMessageKind::Recovered,
            diagnostic_detail(
                status.save_diagnostic_detail,
                status.save_message));
    } else if (!status.save_message.empty()) {
        promote(LocalUserStateHealthKind::Warning);
        append_message(
            LocalUserStateHealthMessageKind::SaveWarning,
            diagnostic_detail(
                status.save_diagnostic_detail,
                status.save_message));
    }
}

void LocalUserStateSaveStatus::ClearRecovered()
{
    recovered_ = false;
    if (!failed_) {
        message_.clear();
    }
}

void LocalUserStateSaveStatus::MarkFailed(std::string message)
{
    failed_ = true;
    recovered_ = false;
    message_ = std::move(message);
}

void LocalUserStateSaveStatus::MarkSaveSucceeded()
{
    recovered_ = failed_;
    failed_ = false;
    if (!recovered_) {
        message_.clear();
    }
}

bool LocalUserStateSaveStatus::failed() const
{
    return failed_;
}

bool LocalUserStateSaveStatus::recovered() const
{
    return recovered_;
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
    Duration debounce,
    Duration retry)
    : debounce_(debounce),
      retry_(retry)
{
}

void LocalUserStateSaveScheduler::MarkDirty()
{
    MarkDirtyAt(Clock::now());
}

void LocalUserStateSaveScheduler::MarkDirtyAt(TimePoint now)
{
    dirty_ = true;
    const TimePoint debounce_deadline = now + debounce_;
    if (!next_attempt_time_ || debounce_deadline > *next_attempt_time_) {
        next_attempt_time_ = debounce_deadline;
    }
}

bool LocalUserStateSaveScheduler::ShouldAttemptSave(TimePoint now) const
{
    return dirty_ && next_attempt_time_ && now >= *next_attempt_time_;
}

void LocalUserStateSaveScheduler::MarkSaveSucceeded()
{
    dirty_ = false;
    next_attempt_time_.reset();
}

void LocalUserStateSaveScheduler::MarkSaveSucceeded(LocalUserStateSaveStatus& status)
{
    MarkSaveSucceeded();
    status.MarkSaveSucceeded();
}

void LocalUserStateSaveScheduler::CancelPendingSave()
{
    dirty_ = false;
    next_attempt_time_.reset();
}

void LocalUserStateSaveScheduler::MarkSaveFailed()
{
    MarkSaveFailedAt(Clock::now());
}

void LocalUserStateSaveScheduler::MarkSaveFailedAt(TimePoint now)
{
    dirty_ = true;
    next_attempt_time_ = now + retry_;
}

void LocalUserStateSaveScheduler::MarkSaveFailed(
    LocalUserStateSaveStatus& status,
    std::string message)
{
    MarkSaveFailedAt(Clock::now(), status, std::move(message));
}

void LocalUserStateSaveScheduler::MarkSaveFailedAt(
    TimePoint now,
    LocalUserStateSaveStatus& status,
    std::string message)
{
    MarkSaveFailedAt(now);
    status.MarkFailed(std::move(message));
}

bool LocalUserStateSaveScheduler::dirty() const
{
    return dirty_;
}

std::optional<LocalUserStateSaveScheduler::TimePoint> LocalUserStateSaveScheduler::next_attempt_time() const
{
    return dirty_ ? next_attempt_time_ : std::nullopt;
}

LocalUserStatePersistenceLifecycle::LocalUserStatePersistenceLifecycle(
    Duration debounce,
    Duration retry)
    : save_scheduler_(debounce, retry)
{
}

void LocalUserStatePersistenceLifecycle::SetLoadWarning(
    std::string warning,
    std::string diagnostic_detail)
{
    load_warning_ = std::move(warning);
    load_diagnostic_detail_ = std::move(diagnostic_detail);
}

void LocalUserStatePersistenceLifecycle::ClearLoadWarning()
{
    load_warning_.clear();
    load_diagnostic_detail_.clear();
}

void LocalUserStatePersistenceLifecycle::MarkDirty()
{
    MarkDirtyAt(Clock::now());
}

void LocalUserStatePersistenceLifecycle::MarkDirtyAt(TimePoint now)
{
    save_status_.ClearRecovered();
    save_scheduler_.MarkDirtyAt(now);
}

void LocalUserStatePersistenceLifecycle::CancelPendingSave()
{
    save_scheduler_.CancelPendingSave();
}

bool LocalUserStatePersistenceLifecycle::ShouldAttemptSave(TimePoint now) const
{
    return save_scheduler_.ShouldAttemptSave(now);
}

std::optional<LocalUserStatePersistenceLifecycle::TimePoint>
LocalUserStatePersistenceLifecycle::NextMaintenanceDeadline() const
{
    return save_scheduler_.next_attempt_time();
}

LocalUserStatePersistenceLifecycle::FlushOutcome
LocalUserStatePersistenceLifecycle::RunMaintenance(
    TimePoint now,
    const SaveOperation& save)
{
    if (!ShouldAttemptSave(now)) {
        return FlushOutcome::NotNeeded;
    }
    return CompleteSaveAfterOperation(save);
}

LocalUserStatePersistenceLifecycle::FlushOutcome
LocalUserStatePersistenceLifecycle::Flush(const SaveOperation& save)
{
    if (!dirty()) {
        return FlushOutcome::NotNeeded;
    }
    return CompleteSaveAfterOperation(save);
}

bool LocalUserStatePersistenceLifecycle::dirty() const
{
    return save_scheduler_.dirty();
}

LocalUserStatePersistenceStatus
LocalUserStatePersistenceLifecycle::PersistenceStatus() const
{
    return {
        .retrying = save_scheduler_.dirty() && save_status_.failed(),
        .recovered = save_status_.recovered(),
        .load_warning = load_warning_,
        .save_message = save_status_.message(),
        .load_diagnostic_detail = load_diagnostic_detail_,
        .save_diagnostic_detail = save_status_.message(),
    };
}

LocalUserStatePersistenceLifecycle::FlushOutcome
LocalUserStatePersistenceLifecycle::CompleteSaveAfterOperation(
    const SaveOperation& save)
{
    SaveResult result = save ? save() : SaveResult{};
    return CompleteSave(Clock::now(), std::move(result));
}

LocalUserStatePersistenceLifecycle::FlushOutcome
LocalUserStatePersistenceLifecycle::CompleteSave(
    TimePoint now,
    SaveResult result)
{
    if (result.saved) {
        ClearLoadWarning();
        save_scheduler_.MarkSaveSucceeded(save_status_);
        return FlushOutcome::Saved;
    }

    if (result.error.empty()) {
        result.error = "Could not save local user state.";
    }
    save_scheduler_.MarkSaveFailedAt(
        now,
        save_status_,
        std::move(result.error));
    return FlushOutcome::Failed;
}

}  // namespace specforge
