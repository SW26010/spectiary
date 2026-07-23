#pragma once

#include "domain/sample_navigation_direction.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_folder_listing_generation.h"

#include <cstddef>
#include <cstdint>

namespace specforge {

enum class SourceCollectionResidentSnapshotOrigin {
    History,
    Prefetch,
};

// A snapshot may skip decode only while this complete verification boundary
// remains current. The raw row index is intentionally independent of any
// filtered or sorted navigation sequence.
struct SourceCollectionResidentSnapshot {
    std::size_t spectrum_index = 0;
    SpectrumSnapshotHandle snapshot;
    SourceCollectionContextReuseProof context_reuse_proof;
    SourceCollectionFolderListingGenerationHandle folder_listing_generation;
    SourceCollectionResidentSnapshotOrigin origin =
        SourceCollectionResidentSnapshotOrigin::History;
    std::uint64_t prefetch_id = 0;
    std::uint64_t prefetch_task_id = 0;
    std::int64_t prefetch_scheduled_ns = 0;
    SampleNavigationDirection prefetch_direction =
        SampleNavigationDirection::Next;
};

}  // namespace specforge
