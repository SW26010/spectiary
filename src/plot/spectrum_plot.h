#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "overlays/spectral_line_catalog.h"
#include "plot/plot_touchpad_gesture.h"
#include "plot/scientific_label.h"
#include "plot/spectral_line_label_layout.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

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
    bool has_last_limits = false;
    double last_x_min = 0.0;
    double last_x_max = 0.0;
    double last_y_min = 0.0;
    double last_y_max = 0.0;
    bool sync_last_limits_next_frame = false;
    SpectralLineLabelLayoutWorkspace spectral_line_name_layout;
    SpectralLineLabelLayoutWorkspace spectral_line_wavelength_layout;
    ScientificLabelCache scientific_label_cache;
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
    std::string_view layout_scope_id;
    ImFont* spectral_line_label_font = nullptr;
};

struct SpectrumPlotDisplayOptions {
    bool edge_axis_overlay = false;
    bool include_edge_pixels = false;
    bool native_transparent_axes = false;
};

[[nodiscard]] bool IsPlotPanDragActive(
    bool was_active,
    bool plot_hovered,
    bool left_button_down,
    bool left_button_dragging) noexcept;

[[nodiscard]] bool RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile = {},
    const SpectrumPlotStyle& style = {},
    const SpectrumPlotOverlays& overlays = {},
    const SpectrumPlotDisplayOptions& display = {},
    PlotTouchpadGestureSource* touchpad_gestures = nullptr);

}  // namespace specforge
