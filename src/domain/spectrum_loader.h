#pragma once

#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>

namespace specforge {

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPath(
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0);

}  // namespace specforge
