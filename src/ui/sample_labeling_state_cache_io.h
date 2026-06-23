#pragma once

#include "domain/sample_labeling.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace specforge {

struct SampleLabelingSourceState {
    std::size_t sample_count = 0;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::vector<SampleLabelingTask> tasks;
    std::optional<std::string> active_task_id;
};

struct SampleLabelingStateCache {
    std::unordered_map<std::string, SampleLabelingSourceState> sources;
};

struct SampleLabelingStateCacheLoadResult {
    SampleLabelingStateCache cache;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultSampleLabelingStateCachePath();

[[nodiscard]] SampleLabelingStateCacheLoadResult LoadSampleLabelingStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSampleLabelingStateCache(
    const std::filesystem::path& path,
    const SampleLabelingStateCache& cache);

}  // namespace specforge
