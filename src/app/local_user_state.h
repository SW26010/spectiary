#pragma once

#include <cstdint>
#include <filesystem>

namespace specforge {

[[nodiscard]] std::filesystem::path DefaultLocalUserStatePath(std::filesystem::path relative_path);

class LocalUserStateSaveScheduler {
public:
    LocalUserStateSaveScheduler() = default;
    LocalUserStateSaveScheduler(std::uint64_t debounce_frames, std::uint64_t retry_frames);

    void MarkDirty();
    void MarkDirty(std::uint64_t frame_index);
    [[nodiscard]] bool ShouldAttemptSave(std::uint64_t frame_index);
    void MarkSaveSucceeded();
    void MarkSaveFailed(std::uint64_t frame_index);

    [[nodiscard]] bool dirty() const;

private:
    std::uint64_t debounce_frames_ = 0;
    std::uint64_t retry_frames_ = 0;
    std::uint64_t next_save_frame_ = 0;
    bool dirty_ = false;
};

}  // namespace specforge
