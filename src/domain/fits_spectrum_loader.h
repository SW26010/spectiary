#pragma once

#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>

namespace specforge::detail {

bool IsFitsSourcePath(const std::filesystem::path& path);

SpectrumSnapshotHandle LoadFitsSnapshot(const std::filesystem::path& path, std::size_t spectrum_index);

}  // namespace specforge::detail
