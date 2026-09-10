#pragma once

#include "ui/sample_workflow_preparation.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace specforge::test_support {

// Read real, absent cache files under an isolated path instead of replacing
// cache loading. No directories are created and no user state is consulted.
inline SampleWorkflowPreparationPaths EmptyWorkflowCachePaths()
{
    static std::atomic_uint64_t next_id = 0;
    const auto root = std::filesystem::temp_directory_path() /
        ("specforge_empty_workflow_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(next_id.fetch_add(1)));
    return {root / "labeling.json", root / "workflow.json", root / "navigation.json"};
}

}  // namespace specforge::test_support
