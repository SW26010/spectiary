#pragma once

#include "app/runtime_paths.h"
#include "app/local_user_state_json.h"

#include "plot/spectrum_plot.h"

#include <filesystem>
#include <string>

namespace spectiary {

// Read-only migration input. Never an active persistence owner.
struct LegacySpectrumViewState {
    bool locked = false;
    std::string source_collection_identity;
    PlotViewLimits limits;
    SpectrumPlotColors plot_colors;
};

struct LegacySpectrumViewStateLoadResult {
    LegacySpectrumViewState state;
    bool document_present = false;
    VersionedJsonCacheLoadIssueKind issue_kind =
        VersionedJsonCacheLoadIssueKind::None;
    std::string warning;
    std::string diagnostic_detail;
};

[[nodiscard]] LegacySpectrumViewStateLoadResult
LoadLegacySpectrumViewState(
    const std::filesystem::path& path);

}  // namespace spectiary
