#pragma once

#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
#include <functional>

namespace spectiary::detail {

bool IsFitsSourcePath(const std::filesystem::path& path);

SpectrumSnapshotHandle LoadFitsSnapshot(const std::filesystem::path& path, std::size_t spectrum_index);
SpectrumSnapshotHandle LoadFitsSnapshotCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const std::function<bool()>& cancellation_requested);

}  // namespace spectiary::detail
