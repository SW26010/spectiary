#pragma once

#include "app/runtime_paths.h"
#include "app/local_user_state_json.h"

#include "plot/spectrum_plot.h"

#include <filesystem>
#include <string>

namespace specforge {

struct SpectrumPlotPreferences {
    SpectrumPlotColors plot_colors;
};

struct SpectrumPlotPreferencesLoadResult {
    SpectrumPlotPreferences state;
    bool document_present = false;
    VersionedJsonCacheLoadIssueKind issue_kind =
        VersionedJsonCacheLoadIssueKind::None;
    std::string warning;
    std::string diagnostic_detail;
};

[[nodiscard]] std::filesystem::path
DefaultSpectrumPlotPreferencesPath(const RuntimePaths& runtime_paths);

[[nodiscard]] SpectrumPlotPreferencesLoadResult
LoadSpectrumPlotPreferences(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSpectrumPlotPreferences(
    const std::filesystem::path& path,
    const SpectrumPlotPreferences& state,
    std::string* error_message = nullptr);

}  // namespace specforge
