#pragma once

#include "domain/spectrum_snapshot.h"
#include "overlays/spectral_line_plot_source.h"
#include <string_view>

namespace spectiary {

enum class SpectralLineProjectionStatus {
    Available,
    UnsupportedCoordinates,
    SpectrumUnavailable,
};

// Renderer-ready output. Borrowed marker pointers and scope have the same
// lifetime as the source generation; the renderer never interprets file units.
struct SpectralLineProjection {
    SpectralLineProjectionStatus status = SpectralLineProjectionStatus::SpectrumUnavailable;
    std::vector<SpectralLinePlotMarker> visible_markers;
    bool marker_labels_visible = true;
    std::string_view layout_scope_id;
};

// Temporary conservative projection boundary, not canonical validity policy.
// #105/#129 own future coordinate compatibility/conversion. For now only the
// existing laboratory/rest vacuum-Angstrom path is passed through unchanged.
[[nodiscard]] SpectralLineProjection ProjectSpectralLineList(
    SpectralLinePlotSource source, const SpectrumSnapshotHandle& snapshot);

} // namespace spectiary
