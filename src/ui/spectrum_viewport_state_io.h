#pragma once

#include "app/runtime_paths.h"
#include "app/local_user_state_json.h"

#include "plot/spectrum_plot.h"

#include <filesystem>
#include <string>

namespace specforge {

struct SpectrumViewportState {
    bool locked = false;
    std::string source_collection_identity;
    PlotViewLimits limits;
};

struct SpectrumViewportStateLoadResult {
    SpectrumViewportState state;
    bool document_present = false;
    VersionedJsonCacheLoadIssueKind issue_kind =
        VersionedJsonCacheLoadIssueKind::None;
    std::string warning;
    std::string diagnostic_detail;
};

[[nodiscard]] std::filesystem::path
DefaultSpectrumViewportStatePath(const RuntimePaths& runtime_paths);

[[nodiscard]] SpectrumViewportStateLoadResult
LoadSpectrumViewportState(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSpectrumViewportState(
    const std::filesystem::path& path,
    const SpectrumViewportState& state,
    std::string* error_message = nullptr);

}  // namespace specforge
