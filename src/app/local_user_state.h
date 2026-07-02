#pragma once

#include <cstdint>
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
    LocalUserStateSaveScheduler() = default;
    LocalUserStateSaveScheduler(std::uint64_t debounce_frames, std::uint64_t retry_frames);

    void MarkDirty();
    void MarkDirty(std::uint64_t frame_index);
    [[nodiscard]] bool ShouldAttemptSave(std::uint64_t frame_index);
    void MarkSaveSucceeded();
    void MarkSaveSucceeded(LocalUserStateSaveStatus& status);
    void MarkSaveFailed(std::uint64_t frame_index);
    void MarkSaveFailed(std::uint64_t frame_index, LocalUserStateSaveStatus& status, std::string message);

    [[nodiscard]] bool dirty() const;

private:
    std::uint64_t debounce_frames_ = 0;
    std::uint64_t retry_frames_ = 0;
    std::uint64_t next_save_frame_ = 0;
    bool dirty_ = false;
};

}  // namespace specforge
