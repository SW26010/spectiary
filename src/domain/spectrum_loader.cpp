#include "domain/spectrum_loader.h"

#include "domain/spectrum_format_adapters.h"

namespace spectiary {

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPath(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    return detail::LoadSpectrumSnapshotFromPathImpl(path, spectrum_index);
}

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    return detail::LoadSpectrumSnapshotFromPathImplCancelable(path, spectrum_index, cancellation_requested);
}

SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListingCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    return detail::LoadFolderSpectrumSnapshotFromListingCancelable(
        path,
        spectrum_index,
        listing,
        cancellation_requested);
}

}  // namespace spectiary
