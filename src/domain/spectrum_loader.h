#pragma once

#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
#include <functional>

namespace specforge {

struct SourceCollectionFolderListing;

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPath(
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0);
using SpectrumLoadCancellationCheck = std::function<bool()>;
SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SpectrumLoadCancellationCheck& cancellation_requested);
SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListingCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing,
    const SpectrumLoadCancellationCheck& cancellation_requested);

}  // namespace specforge
