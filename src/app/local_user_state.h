#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct JsonValue;

struct LocalUserStatePersistenceStatus {
    bool retrying = false;
    bool recovered = false;
    std::string load_warning;
    std::string save_message;
    std::string load_diagnostic_detail;
    std::string save_diagnostic_detail;
};

enum class LocalUserStateHealthKind {
    Healthy,
    Warning,
    Retrying,
    Recovered,
};

enum class LocalUserStateArea {
    SourceSession,
    SampleNavigation,
    SampleLabeling,
    SampleWorkflow,
    Language,
    UiScale,
    Input,
    ProfileOutputDirectory,
    PanelVisibility,
    SpectralLines,
};

enum class LocalUserStateHealthMessageKind {
    LoadWarning,
    SaveWarning,
    SaveRetrying,
    Recovered,
};

struct LocalUserStateHealthMessage {
    LocalUserStateArea area =
        LocalUserStateArea::SourceSession;
    LocalUserStateHealthMessageKind kind =
        LocalUserStateHealthMessageKind::LoadWarning;
    std::string diagnostic_detail;

    [[nodiscard]] bool operator==(
        const LocalUserStateHealthMessage&) const =
        default;
};

struct LocalUserStateHealthView {
    LocalUserStateHealthKind kind =
        LocalUserStateHealthKind::Healthy;
    std::vector<LocalUserStateHealthMessage> messages;
};

void AppendLocalUserStateHealth(
    LocalUserStateHealthView& health,
    LocalUserStateArea area,
    const LocalUserStatePersistenceStatus& status);

[[nodiscard]] std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path);
[[nodiscard]] std::string LocalUserStatePathToUtf8(
    const std::filesystem::path& path);
[[nodiscard]] std::string UserPathDisplayText(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> ReadPersistedPathReference(const JsonValue& value);
[[nodiscard]] JsonValue PersistedPathReferenceJson(
    const std::filesystem::path& path);
void WritePersistedPathReference(std::ostream& stream, const std::filesystem::path& path);

class LocalUserStateSaveStatus {
public:
    void Clear();
    void ClearRecovered();
    void MarkFailed(std::string message);
    void MarkSaveSucceeded();

    [[nodiscard]] bool failed() const;
    [[nodiscard]] bool recovered() const;
    [[nodiscard]] const std::string& message() const;
    [[nodiscard]] std::string_view message_view() const;

private:
    bool failed_ = false;
    bool recovered_ = false;
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

// Storage owners keep their codecs, paths, and save functions. This concrete
// lifecycle only centralizes the state transitions shared by those owners.
class LocalUserStatePersistenceLifecycle {
public:
    using Clock = LocalUserStateSaveScheduler::Clock;
    using TimePoint = LocalUserStateSaveScheduler::TimePoint;
    using Duration = LocalUserStateSaveScheduler::Duration;

    struct SaveResult {
        bool saved = false;
        std::string error;
    };

    enum class FlushOutcome {
        NotNeeded,
        Saved,
        Failed,
    };

    using SaveOperation = std::function<SaveResult()>;

    LocalUserStatePersistenceLifecycle() = default;
    LocalUserStatePersistenceLifecycle(Duration debounce, Duration retry);

    void SetLoadWarning(
        std::string warning,
        std::string diagnostic_detail = {});
    void ClearLoadWarning();

    void MarkDirty();
    void MarkDirtyAt(TimePoint now);
    [[nodiscard]] bool ShouldAttemptSave(TimePoint now) const;
    [[nodiscard]] std::optional<TimePoint>
        NextMaintenanceDeadline() const;

    [[nodiscard]] FlushOutcome RunMaintenance(
        TimePoint now,
        const SaveOperation& save);
    [[nodiscard]] FlushOutcome Flush(const SaveOperation& save);

    [[nodiscard]] bool dirty() const;
    [[nodiscard]] LocalUserStatePersistenceStatus
        PersistenceStatus() const;

private:
    [[nodiscard]] FlushOutcome CompleteSaveAfterOperation(
        const SaveOperation& save);
    [[nodiscard]] FlushOutcome CompleteSave(
        TimePoint now,
        SaveResult result);

    LocalUserStateSaveScheduler save_scheduler_;
    LocalUserStateSaveStatus save_status_;
    std::string load_warning_;
    std::string load_diagnostic_detail_;
};

}  // namespace specforge
