#pragma once

#include "app/runtime_paths.h"

#include "domain/sample_filter.h"
#include "ui/sample_navigation_sequence.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spectiary {

struct SampleAnnotationDisplayNameOverride {
    std::string source_id;
    std::string display_name;
};

struct SampleWorkflowSourceState {
    std::vector<SampleFilterCondition> filter_conditions;
    std::vector<std::string> selected_filter_source_ids;
    std::vector<std::string> selected_sample_sort_source_ids;
    std::vector<SampleAnnotationDisplayNameOverride> annotation_display_names;
    std::optional<std::string> selected_sample_sort_source_id;
    SampleNavigationSortDirection selected_sample_sort_direction =
        SampleNavigationSortDirection::Ascending;
};

struct SampleWorkflowStateCache {
    std::unordered_map<std::string, SampleWorkflowSourceState> sources_by_identity;
};

struct SampleWorkflowStateCacheLoadResult {
    SampleWorkflowStateCache cache;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultSampleWorkflowStateCachePath(const RuntimePaths& runtime_paths);

[[nodiscard]] SampleWorkflowStateCacheLoadResult
LoadSampleWorkflowStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint = {});

[[nodiscard]] bool SaveSampleWorkflowStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SampleWorkflowStateCache& cache);

}  // namespace spectiary
