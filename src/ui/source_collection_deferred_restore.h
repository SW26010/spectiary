#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {

class SourceCollectionDeferredRestoreTracker {
public:
    void Begin(std::optional<std::filesystem::path> historical_active_path)
    {
        active_ = true;
        historical_active_path_ = std::move(historical_active_path);
        task_ids_.clear();
    }

    void Track(std::uint64_t task_id)
    {
        if (active_) {
            task_ids_.insert(task_id);
        }
    }

    [[nodiscard]] bool IsTracked(std::uint64_t task_id) const
    {
        return active_ && task_ids_.contains(task_id);
    }

    void Complete(std::uint64_t task_id)
    {
        task_ids_.erase(task_id);
    }

    [[nodiscard]] bool ReadyToFinish() const
    {
        return active_ && task_ids_.empty();
    }

    [[nodiscard]] std::optional<std::filesystem::path> Finish()
    {
        active_ = false;
        task_ids_.clear();
        return std::exchange(historical_active_path_, std::nullopt);
    }

    [[nodiscard]] std::vector<std::uint64_t> Supersede()
    {
        std::vector<std::uint64_t> task_ids(task_ids_.begin(), task_ids_.end());
        active_ = false;
        task_ids_.clear();
        historical_active_path_.reset();
        return task_ids;
    }

    [[nodiscard]] bool active() const
    {
        return active_;
    }

private:
    std::unordered_set<std::uint64_t> task_ids_;
    std::optional<std::filesystem::path> historical_active_path_;
    bool active_ = false;
};

}  // namespace specforge
