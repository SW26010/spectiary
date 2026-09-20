#include "overlays/spectral_line_projection.h"
#include <utility>

namespace spectiary {
SpectralLineProjection ProjectSpectralLineList(
    SpectralLinePlotSource source, const SpectrumSnapshotHandle& snapshot)
{
    SpectralLineProjection result;
    result.marker_labels_visible = source.marker_labels_visible;
    result.layout_scope_id = source.list.id;
    // This guard must never migrate into canonical loading or activation.
    const auto& coordinate = source.list.coordinate;
    if (coordinate.unit != line_list::Unit::Angstrom ||
        coordinate.medium != line_list::Medium::Vacuum || !coordinate.laboratory_rest) {
        result.status = SpectralLineProjectionStatus::UnsupportedCoordinates;
        return result;
    }
    if (!snapshot || !snapshot->capabilities.can_show_spectral_lines) return result;
    // Existing snapshot rest-frame warnings remain a presentation responsibility.
    result.status = SpectralLineProjectionStatus::Available;
    result.visible_markers = std::move(source.visible_markers);
    return result;
}
} // namespace spectiary
