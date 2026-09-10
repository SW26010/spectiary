#pragma once

#include "ui/source_collection_load_queue.h"
#include "ui/source_collection_load_dependencies.h"

#include <utility>

namespace specforge {

struct SourceCollectionLoadQueueExecutionOptions {
    // Zero selects the production policy: hardware concurrency, capped at four.
    std::size_t foreground_limit = 0;
    std::function<void()> before_worker_start;
};

struct SourceCollectionLoadQueueTestAccess {
    [[nodiscard]] static SourceCollectionLoadQueue Create(
        SourceCollectionLoadDependencies adapters,
        SourceCollectionLoadQueueExecutionOptions options)
    {
        return SourceCollectionLoadQueue(std::move(adapters), std::move(options));
    }
};

[[nodiscard]] inline SourceCollectionLoadQueue
MakeSourceCollectionLoadQueueForTesting(
    SourceCollectionLoadDependencies adapters = {},
    SourceCollectionLoadQueueExecutionOptions options = {})
{
    return SourceCollectionLoadQueueTestAccess::Create(
        std::move(adapters), std::move(options));
}

}  // namespace specforge
