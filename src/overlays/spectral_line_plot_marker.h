#pragma once

#include "overlays/spectral_line_catalog.h"
#include "plot/series_color_model.h"

#include <cstddef>

namespace specforge {

// Immutable frame data shared by the panel controller and plot renderer.
// automatic_color_slot is assigned from catalog + marker identity, not view
// order, so filtering, regrouping, and visibility changes cannot reshuffle it.
struct SpectralLinePlotMarker {
    const SpectralLineMarker* marker = nullptr;
    PlotSeriesColor color = PlotSeriesColor::Auto();
    std::size_t automatic_color_slot = 0;
};

}  // namespace specforge
