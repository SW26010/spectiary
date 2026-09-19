#pragma once

#include "app/runtime_paths.h"
#include "app/local_user_state_json.h"

#include "plot/spectrum_plot.h"

#include <filesystem>
#include <string>

namespace spectiary {

struct SpectrumViewportState {
    bool locked = false;
    std::string source_collection_identity;
    PlotViewLimits limits;

    [[nodiscard]] bool operator==(const SpectrumViewportState& other) const {
        return locked == other.locked &&
            source_collection_identity == other.source_collection_identity &&
            limits.x_min == other.limits.x_min && limits.x_max == other.limits.x_max &&
            limits.y_min == other.limits.y_min && limits.y_max == other.limits.y_max;
    }
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

}  // namespace spectiary
