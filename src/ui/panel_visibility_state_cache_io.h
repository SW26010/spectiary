#pragma once

#include "app/runtime_paths.h"

#include "app/panel_visibility_state.h"

#include <filesystem>
#include <string>

namespace specforge {

struct PanelVisibilityStateCacheLoadResult {
    PanelVisibilityState state;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultPanelVisibilityStateCachePath(const RuntimePaths& runtime_paths);

[[nodiscard]] PanelVisibilityStateCacheLoadResult
LoadPanelVisibilityStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SavePanelVisibilityStateCache(
    const std::filesystem::path& path,
    const PanelVisibilityState& state);

}  // namespace specforge
