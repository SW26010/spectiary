#pragma once

#include "domain/spectrum_fixture.h"

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

void RenderSpectrumPlot(
    const SpectrumSeries& series,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile = {});

}  // namespace specforge
