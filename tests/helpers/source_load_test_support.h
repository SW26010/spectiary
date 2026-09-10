#pragma once

#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_load_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>

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

// Single-request driver for integration tests; all preparation runs in the queue.
inline SourceCollectionLoadCompletion WaitForSourceCompletion(
    SourceCollectionLoadQueue& queue,
    std::uint64_t id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        auto results = queue.TakeCompleted();
        if (!results.empty()) {
            if (results.size() != 1 || results.front().task_id != id) {
                throw std::runtime_error("unexpected source completion");
            }
            return std::move(results.front());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    queue.Cancel(id);
    throw std::runtime_error("source queue did not complete before test deadline");
}

}  // namespace specforge::test_support
