#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "plot/scientific_label.h"
#include "plot/spectral_line_label_layout.h"
#include "plot/spectrum_plot.h"
#include "ui/ui_text.h"

#include <optional>

namespace specforge {

struct SpectrumPlotState {
    bool fit_next_frame = true;
    SpectrumViewportRangeMode viewport_range_mode =
        SpectrumViewportRangeMode::Automatic;
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

struct SpectrumPlotRenderResult {
    bool plot_submitted = false;
    bool fit_applied = false;
    bool stored_limits_reused = false;
    bool pan_active = false;
    std::optional<PlotViewLimits> visible_limits;
};

[[nodiscard]] SpectrumPlotRenderResult RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    UiLanguage language,
    const SpectrumPlotProfileContext& profile,
    const SpectrumPlotStyle& style,
    const SpectrumPlotOverlays& overlays,
    const SpectrumPlotDisplayOptions& display,
    PlotTouchpadGestureSource* touchpad_gestures);

}  // namespace specforge
