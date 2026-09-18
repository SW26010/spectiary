#pragma once

#include "app/runtime_paths.h"

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

[[nodiscard]] std::filesystem::path DefaultSourceCollectionSessionStateCachePath(const RuntimePaths& runtime_paths);

[[nodiscard]] SourceCollectionSessionStateCacheLoadResult
LoadSourceCollectionSessionStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSourceCollectionSessionStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SourceCollectionSessionStateCache& cache);

}  // namespace specforge
