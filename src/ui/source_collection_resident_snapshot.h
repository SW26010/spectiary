#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_folder_listing_generation.h"

#include <cstddef>

namespace specforge {

// A snapshot may skip decode only while this complete verification boundary
// remains current. The raw row index is intentionally independent of any
// filtered or sorted navigation sequence.
struct SourceCollectionResidentSnapshot {
    std::size_t spectrum_index = 0;
    SpectrumSnapshotHandle snapshot;
    SourceCollectionContextReuseProof context_reuse_proof;
    SourceCollectionFolderListingGenerationHandle folder_listing_generation;
};

}  // namespace specforge
