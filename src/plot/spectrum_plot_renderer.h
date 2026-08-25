#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "plot/scientific_label.h"
#include "plot/spectral_line_label_layout.h"
#include "plot/spectrum_plot.h"
#include "ui/ui_text.h"

#include <optional>

namespace specforge {

struct SpectrumSmoothingCache {
    SpectrumValueVector source;
    SpectrumSmoothingSettings settings;
    SpectrumValueVector values;
};

struct SpectrumPlotState {
    bool fit_next_frame = true;
    SpectrumViewportRangeMode viewport_range_mode =
        SpectrumViewportRangeMode::Automatic;
    bool show_raw_curve = true;
    bool show_points = false;
    bool show_gaussian_smoothed = false;
    bool show_median_smoothed = false;
    SpectrumSmoothingParameters smoothing_parameters;
    SpectrumSmoothingCache gaussian_smoothing_cache;
    SpectrumSmoothingCache median_smoothing_cache;
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
    StablePlotSeriesColorAssignments
        series_color_assignments;
};

struct SpectrumPlotRenderResult {
    // True when a valid plot frame was presented, even if every data series is hidden.
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
