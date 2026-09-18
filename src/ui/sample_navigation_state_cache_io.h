#pragma once

#include "app/runtime_paths.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace specforge {

struct SampleNavigationStateCache {
    std::unordered_map<std::string, std::size_t> last_indices_by_source_identity;
};

struct SampleNavigationStateCacheLoadResult {
    SampleNavigationStateCache cache;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultSampleNavigationStateCachePath(const RuntimePaths& runtime_paths);

[[nodiscard]] SampleNavigationStateCacheLoadResult
LoadSampleNavigationStateCache(const std::filesystem::path& path);

[[nodiscard]] bool SaveSampleNavigationStateCache(
    const std::filesystem::path& path,
    const SampleNavigationStateCache& cache);

}  // namespace specforge
