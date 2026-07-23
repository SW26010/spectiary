#pragma once

#include "domain/source_collection_manifest.h"
#include "platform/directory_change_generation.h"

#include <memory>

namespace specforge {

// Immutable folder listing plus the observation boundary under which it was
// enumerated. The listing is a cache; the generation decides whether it is
// still safe to reuse without another full scan.
struct SourceCollectionFolderListingGeneration {
    SourceCollectionFolderListing listing;
    DirectoryChangeGenerationHandle change_generation;

    [[nodiscard]] bool IsCurrent() const noexcept
    {
        return change_generation && change_generation->IsCurrent();
    }
};

using SourceCollectionFolderListingGenerationHandle =
    std::shared_ptr<const SourceCollectionFolderListingGeneration>;

}  // namespace specforge
