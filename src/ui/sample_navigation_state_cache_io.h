#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace specforge {

struct SampleNavigationStateCache {
    std::unordered_map<std::string, std::size_t> last_indices_by_source_identity;
};

[[nodiscard]] std::filesystem::path DefaultSampleNavigationStateCachePath();

[[nodiscard]] SampleNavigationStateCache LoadSampleNavigationStateCache(const std::filesystem::path& path);

[[nodiscard]] bool SaveSampleNavigationStateCache(
    const std::filesystem::path& path,
    const SampleNavigationStateCache& cache);

}  // namespace specforge
