#pragma once

#include "app/local_user_state.h"

#include <cstdint>
#include <filesystem>
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

[[nodiscard]] std::filesystem::path DefaultPanelVisibilityStateCachePath();

[[nodiscard]] PanelVisibilityState LoadPanelVisibilityStateCache(const std::filesystem::path& path);

[[nodiscard]] bool SavePanelVisibilityStateCache(
    const std::filesystem::path& path,
    const PanelVisibilityState& state);

class PanelVisibilityStatePersistence {
public:
    explicit PanelVisibilityStatePersistence(
        std::filesystem::path cache_path = DefaultPanelVisibilityStateCachePath(),
        std::uint64_t debounce_frames = 30,
        std::uint64_t retry_frames = 120);

    [[nodiscard]] PanelVisibilityState Load() const;
    void MarkDirtyIfChanged(
        const PanelVisibilityState& previous,
        const PanelVisibilityState& current,
        std::uint64_t frame_index);
    void MaybeSave(const PanelVisibilityState& state, std::uint64_t frame_index);
    [[nodiscard]] bool Flush(const PanelVisibilityState& state);

private:
    std::filesystem::path cache_path_;
    LocalUserStateSaveScheduler save_scheduler_;
};

}  // namespace specforge
