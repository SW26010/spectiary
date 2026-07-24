#pragma once

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

struct JsonValue;

[[nodiscard]] std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path);
[[nodiscard]] std::string UserPathDisplayText(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> ReadPersistedPathReference(const JsonValue& value);
[[nodiscard]] JsonValue PersistedPathReferenceJson(
    const std::filesystem::path& path);
void WritePersistedPathReference(std::ostream& stream, const std::filesystem::path& path);

class LocalUserStateSaveStatus {
public:
    void Clear();
    void MarkFailed(std::string message);

    [[nodiscard]] bool failed() const;
    [[nodiscard]] const std::string& message() const;
    [[nodiscard]] std::string_view message_view() const;

private:
    bool failed_ = false;
    std::string message_;
};

class LocalUserStateSaveScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    LocalUserStateSaveScheduler() = default;
    LocalUserStateSaveScheduler(Duration debounce, Duration retry);

    void MarkDirty();
    void MarkDirtyAt(TimePoint now);
    [[nodiscard]] bool ShouldAttemptSave(TimePoint now) const;
    void MarkSaveSucceeded();
    void MarkSaveSucceeded(LocalUserStateSaveStatus& status);
    void MarkSaveFailed();
    void MarkSaveFailedAt(TimePoint now);
    void MarkSaveFailed(LocalUserStateSaveStatus& status, std::string message);
    void MarkSaveFailedAt(TimePoint now, LocalUserStateSaveStatus& status, std::string message);

    [[nodiscard]] bool dirty() const;
    [[nodiscard]] std::optional<TimePoint> next_attempt_time() const;

private:
    Duration debounce_ = Duration::zero();
    Duration retry_ = Duration::zero();
    std::optional<TimePoint> next_attempt_time_;
    bool dirty_ = false;
};

}  // namespace specforge
