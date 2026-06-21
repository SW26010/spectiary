#pragma once

#include "overlays/spectral_line_user_state.h"

#include <filesystem>
#include <string>

namespace specforge {

struct CatalogUserStateCacheLoadResult {
    CatalogUserStateCache cache;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultCatalogUserStateCachePath();
[[nodiscard]] CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path);
bool SaveCatalogUserStateCache(
    const std::filesystem::path& path,
    const CatalogUserStateCache& cache,
    std::string& error);

}  // namespace specforge

