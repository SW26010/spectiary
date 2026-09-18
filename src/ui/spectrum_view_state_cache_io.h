#pragma once

#include "app/runtime_paths.h"

#include "plot/spectrum_plot.h"

#include <filesystem>
#include <string>

namespace specforge {

struct SpectrumViewStateCache {
    bool locked = false;
    std::string source_collection_identity;
    PlotViewLimits limits;
    SpectrumPlotColors plot_colors;
};

struct SpectrumViewStateCacheLoadResult {
    SpectrumViewStateCache state;
    std::string warning;
    std::string diagnostic_detail;
};

[[nodiscard]] std::filesystem::path
DefaultSpectrumViewStateCachePath(const RuntimePaths& runtime_paths);

[[nodiscard]] SpectrumViewStateCacheLoadResult
LoadSpectrumViewStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSpectrumViewStateCache(
    const std::filesystem::path& path,
    const SpectrumViewStateCache& state,
    std::string* error_message = nullptr);

}  // namespace specforge
