#pragma once

#include "overlays/spectral_line_plot_marker.h"
#include <vector>

namespace spectiary {

// Borrowed, immutable active generation plus session presentation. Consume within
// the current frame, before any controller mutation/maintenance or owner switch.
// All canonical coordinates (including hidden markers) are retained in list.
// visible_markers still uses canonical coordinates; it is NOT renderer input.
struct SpectralLinePlotSource {
    const SpectralLineList& list;
    std::vector<SpectralLinePlotMarker> visible_markers;
    bool marker_labels_visible = true;
};

} // namespace spectiary
