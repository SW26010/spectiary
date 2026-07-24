#pragma once

#include "ui/source_collection_load_queue.h"
#include "ui/source_collection_preparation_internal.h"

#include <utility>

namespace specforge {

struct SourceCollectionLoadQueueTestAccess {
    [[nodiscard]] static SourceCollectionLoadQueue Create(
        SourceCollectionPreparationAdapters adapters)
    {
        return SourceCollectionLoadQueue(std::move(adapters));
    }
};

[[nodiscard]] inline SourceCollectionLoadQueue
MakeSourceCollectionLoadQueueForTesting(
    SourceCollectionPreparationAdapters adapters = {})
{
    return SourceCollectionLoadQueueTestAccess::Create(
        std::move(adapters));
}

}  // namespace specforge
