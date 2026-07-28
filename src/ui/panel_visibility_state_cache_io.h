#pragma once

#include "app/local_user_state.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace specforge {

struct PanelVisibilityState {
    bool files = true;
    bool navigation = true;
    bool annotations = true;
    bool labeling = true;
    bool filters = true;
    bool sorting = true;
    bool smoothing = true;
    bool information = true;
    bool spectral_lines = true;

    [[nodiscard]] bool operator==(const PanelVisibilityState&) const = default;
};

struct PanelVisibilityStateCacheLoadResult {
    PanelVisibilityState state;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultPanelVisibilityStateCachePath();

[[nodiscard]] PanelVisibilityStateCacheLoadResult
LoadPanelVisibilityStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SavePanelVisibilityStateCache(
    const std::filesystem::path& path,
    const PanelVisibilityState& state);

class PanelVisibilityStatePersistence {
public:
    explicit PanelVisibilityStatePersistence(
        std::filesystem::path cache_path = DefaultPanelVisibilityStateCachePath(),
        LocalUserStateSaveScheduler::Duration debounce = std::chrono::milliseconds(500),
        LocalUserStateSaveScheduler::Duration retry = std::chrono::seconds(2));

    [[nodiscard]] PanelVisibilityState Load();
    void MarkDirtyIfChanged(
        const PanelVisibilityState& previous,
        const PanelVisibilityState& current);
    [[nodiscard]] std::optional<bool> RunMaintenance(
        const PanelVisibilityState& state,
        LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool Flush(const PanelVisibilityState& state);
    [[nodiscard]] LocalUserStatePersistenceStatus
        PersistenceStatus() const;

private:
    std::filesystem::path cache_path_;
    LocalUserStateSaveScheduler save_scheduler_;
    LocalUserStateSaveStatus save_status_;
    std::string load_warning_;
};

}  // namespace specforge
