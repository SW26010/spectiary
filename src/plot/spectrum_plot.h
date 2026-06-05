#pragma once

#include "domain/spectrum_fixture.h"

namespace specforge {

struct SpectrumPlotState {
    bool fit_next_frame = true;
    bool show_points = false;
};

void RenderSpectrumPlot(const SpectrumSeries& series, SpectrumPlotState& state);

}  // namespace specforge
