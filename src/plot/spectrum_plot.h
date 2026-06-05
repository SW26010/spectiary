#pragma once

#include "domain/spectrum_snapshot.h"

#include <imgui.h>

#include <cstdint>

namespace specforge {

class ProfileSink;

struct SpectrumPlotState {
    bool fit_next_frame = true;
    bool show_points = false;
    bool pan_drag_active = false;
    bool has_profile_limits = false;
    double profiled_x_min = 0.0;
    double profiled_x_max = 0.0;
    double profiled_y_min = 0.0;
    double profiled_y_max = 0.0;
};

struct SpectrumPlotProfileContext {
    ProfileSink* sink = nullptr;
    std::uint64_t frame_index = 0;
};

struct SpectrumPlotStyle {
    ImVec4 line_color = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);
    float line_weight = 1.4f;
};

void RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile = {},
    const SpectrumPlotStyle& style = {});

}  // namespace specforge
