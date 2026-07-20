#pragma once

#include "domain/spectrum_loader.h"

namespace specforge {
struct SourceCollectionFolderListing;
}

#include <cstddef>
#include <filesystem>

namespace specforge::detail {

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathImpl(
    const std::filesystem::path& path,
    std::size_t spectrum_index);
SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathImplCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SpectrumLoadCancellationCheck& cancellation_requested);

SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListing(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing);
SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListingCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing,
    const SpectrumLoadCancellationCheck& cancellation_requested);

}  // namespace specforge::detail
