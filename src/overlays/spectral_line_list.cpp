#include "overlays/spectral_line_list.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace spectiary {
namespace {
bool Positive(const std::optional<double>& value)
{
    return value && std::isfinite(*value) && *value > 0;
}
bool Identity(const std::string& id, std::unordered_set<std::string>& ids)
{
    return !id.empty() && ids.insert(id).second;
}
}

bool ValidateSpectralLineList(const SpectralLineList& list, std::string& error)
{
    error.clear();
    const auto fail = [&](std::string message) { error = std::move(message); return false; };
    if (list.id.empty() || list.name.empty()) return fail("line-list id and name must be non-empty");
    if (!list.coordinate.laboratory_rest ||
        (list.coordinate.unit != line_list::Unit::Angstrom &&
         list.coordinate.unit != line_list::Unit::Nanometer &&
         list.coordinate.unit != line_list::Unit::Micrometer) ||
        (list.coordinate.medium != line_list::Medium::Air &&
         list.coordinate.medium != line_list::Medium::Vacuum)) {
        return fail("invalid laboratory wavelength coordinate semantics");
    }
    std::unordered_set<std::string> markers, views, groups, schemes;
    for (const auto& marker : list.markers) {
        if (!Identity(marker.id, markers) || marker.name.empty())
            return fail("invalid or duplicate marker identity/name: " + marker.id);
        if (marker.kind == line_list::MarkerKind::Line) {
            if (!Positive(marker.coordinate) || marker.start || marker.end)
                return fail("invalid line coordinate shape: " + marker.id);
        } else if (marker.kind == line_list::MarkerKind::Band) {
            if (marker.coordinate || !Positive(marker.start) || !Positive(marker.end) ||
                *marker.start >= *marker.end) return fail("invalid band coordinate shape: " + marker.id);
        } else return fail("invalid marker kind: " + marker.id);
    }
    for (const auto& view : list.grouping_views) {
        if (!Identity(view.id, views) || view.name.empty())
            return fail("invalid or duplicate grouping view identity/name: " + view.id);
        for (const auto& group : view.groups) {
            if (!Identity(group.id, groups) || group.name.empty())
                return fail("invalid or duplicate document-wide group identity/name: " + group.id);
            std::unordered_set<std::string> references;
            for (const auto& id : group.marker_ids) {
                if (!markers.contains(id) || !references.insert(id).second)
                    return fail("dangling or repeated marker reference in group " + group.id + ": " + id);
            }
        }
    }
    for (const auto& scheme : list.color_schemes) {
        if (!Identity(scheme.id, schemes) || scheme.name.empty())
            return fail("invalid or duplicate color scheme identity/name: " + scheme.id);
        for (const auto& [id, color] : scheme.colors) {
            if (!markers.contains(id)) return fail("dangling color marker reference: " + id);
            if (color.size() != 9 || color[0] != '#' ||
                !std::all_of(color.begin() + 1, color.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                })) return fail("invalid RGBA8 color for marker: " + id);
        }
    }
    return true;
}

double SpectralLineMarkerPosition(const line_list::Marker& marker)
{
    return marker.kind == line_list::MarkerKind::Line ? marker.coordinate.value_or(0.0) :
        marker.start.value_or(0.0) / 2.0 + marker.end.value_or(0.0) / 2.0;
}
} // namespace spectiary
