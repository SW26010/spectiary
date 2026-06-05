#include "plot/spectrum_plot.h"

#include <implot.h>

#include <algorithm>

namespace specforge {
namespace {

struct Bounds {
    double x_min = 0.0;
    double x_max = 1.0;
    double y_min = 0.0;
    double y_max = 1.0;
};

Bounds ComputeBounds(const SpectrumSeries& series)
{
    Bounds bounds;
    if (series.wavelength.empty() || series.flux.empty()) {
        return bounds;
    }

    const auto [x_min, x_max] = std::minmax_element(series.wavelength.begin(), series.wavelength.end());
    const auto [y_min, y_max] = std::minmax_element(series.flux.begin(), series.flux.end());
    bounds.x_min = *x_min;
    bounds.x_max = *x_max;
    bounds.y_min = *y_min;
    bounds.y_max = *y_max;

    const double y_padding = (bounds.y_max - bounds.y_min) * 0.08;
    bounds.y_min -= y_padding;
    bounds.y_max += y_padding;
    return bounds;
}

}  // namespace

void RenderSpectrumPlot(const SpectrumSeries& series, SpectrumPlotState& state)
{
    if (series.wavelength.empty() || series.flux.empty()) {
        return;
    }

    if (state.fit_next_frame) {
        const Bounds bounds = ComputeBounds(series);
        ImPlot::SetNextAxesLimits(bounds.x_min, bounds.x_max, bounds.y_min, bounds.y_max, ImPlotCond_Always);
        state.fit_next_frame = false;
    }

    if (ImPlot::BeginPlot("Spectrum##main_spectrum", ImVec2(-1.0f, -1.0f), ImPlotFlags_Crosshairs)) {
        ImPlot::SetupAxis(ImAxis_X1, "wavelength");
        ImPlot::SetupAxis(ImAxis_Y1, "flux");

        ImPlotSpec spec;
        if (state.show_points) {
            spec.Marker = ImPlotMarker_Circle;
            spec.MarkerSize = 2.0f;
        }
        ImPlot::PlotLine(
            series.name.c_str(),
            series.wavelength.data(),
            series.flux.data(),
            static_cast<int>(std::min(series.wavelength.size(), series.flux.size())),
            spec);
        ImPlot::EndPlot();
    }
}

}  // namespace specforge
