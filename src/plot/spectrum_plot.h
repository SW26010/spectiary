#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "overlays/spectral_line_catalog.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace specforge {

class ProfileSink;

struct SpectrumPlotState {
    bool fit_next_frame = true;
    bool show_points = false;
    bool show_smoothed = false;
    bool show_raw_when_smoothed = true;
    SpectrumSmoothingSettings smoothing;
    SpectrumValueVector smoothing_cache_source;
    SpectrumSmoothingSettings smoothing_cache_settings;
    SpectrumValueVector smoothed_y_values;
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

struct SpectrumPlotOverlays {
    const SpectralLineMarker* const* spectral_lines = nullptr;
    std::size_t spectral_line_count = 0;
    bool show_spectral_line_labels = true;
};

void RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile = {},
    const SpectrumPlotStyle& style = {},
    const SpectrumPlotOverlays& overlays = {});

}  // namespace specforge
