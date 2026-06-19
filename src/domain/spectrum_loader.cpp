#include "domain/spectrum_loader.h"

#include "domain/spectrum_format_adapters.h"

namespace specforge {

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPath(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    return detail::LoadSpectrumSnapshotFromPathImpl(path, spectrum_index);
}

}  // namespace specforge
