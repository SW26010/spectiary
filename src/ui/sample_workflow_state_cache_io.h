#pragma once

#include "domain/sample_filter.h"
#include "ui/sample_navigation_sequence.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace specforge {

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

[[nodiscard]] std::filesystem::path DefaultSampleWorkflowStateCachePath();

[[nodiscard]] SampleWorkflowStateCache LoadSampleWorkflowStateCache(
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint = {});

[[nodiscard]] bool SaveSampleWorkflowStateCache(
    const std::filesystem::path& path,
    const SampleWorkflowStateCache& cache);

}  // namespace specforge
