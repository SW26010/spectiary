#pragma once

#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionSessionStateCache {
    std::vector<SourceCollectionSavedSource> sources;
    std::optional<std::size_t> active_source_index;
};

struct SourceCollectionSessionStateCacheLoadResult {
    SourceCollectionSessionStateCache cache;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultSourceCollectionSessionStateCachePath();

[[nodiscard]] SourceCollectionSessionStateCacheLoadResult
LoadSourceCollectionSessionStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSourceCollectionSessionStateCache(
    const std::filesystem::path& path,
    const SourceCollectionSessionStateCache& cache);

}  // namespace specforge
