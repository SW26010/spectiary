#include "overlays/built_in_spectral_line_overlay.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace spectiary {
SpectralLineListParseResult ComposeBuiltInSpectralLineList(
    const SpectralLineList& base, const BuiltInSpectralLineOverlay& overlay)
{
    SpectralLineListParseResult result;
    if (!ValidateSpectralLineList(base, result.error)) return result;
    auto effective = base;
    effective.grouping_views.insert(effective.grouping_views.end(), overlay.grouping_views.begin(), overlay.grouping_views.end());
    if (overlay.color_schemes) effective.color_schemes = *overlay.color_schemes;
    if (ValidateSpectralLineList(effective, result.error)) result.list = std::move(effective);
    return result;
}

namespace {
bool Publish(const SpectralLineList& base, BuiltInSpectralLineOverlay& target,
             BuiltInSpectralLineOverlay candidate, std::string& error)
{
    auto result = ComposeBuiltInSpectralLineList(base, candidate);
    error = std::move(result.error);
    if (!result.list) return false;
    target = std::move(candidate);
    return true;
}
bool BaseView(const SpectralLineList& base, std::string_view id)
{
    return std::any_of(base.grouping_views.begin(), base.grouping_views.end(),
                       [&](const auto& view) { return view.id == id; });
}
}

bool SetBuiltInMarkerColor(const SpectralLineList& base, BuiltInSpectralLineOverlay& overlay,
    std::string_view scheme_id, std::string_view marker_id, std::optional<std::string> rgba, std::string& error)
{
    const auto effective = ComposeBuiltInSpectralLineList(base, overlay);
    if (!effective.list) { error = effective.error; return false; }
    if (std::none_of(base.markers.begin(), base.markers.end(), [&](const auto& marker) { return marker.id == marker_id; })) {
        error = "marker identity does not exist"; return false;
    }
    auto schemes = effective.list->color_schemes;
    auto scheme = std::find_if(schemes.begin(), schemes.end(), [&](const auto& value) { return value.id == scheme_id; });
    if (scheme == schemes.end()) { error = "color scheme identity does not exist"; return false; }
    const auto old = scheme->colors.find(std::string(marker_id));
    if ((!rgba && old == scheme->colors.end()) || (rgba && old != scheme->colors.end() && old->second == *rgba)) {
        error.clear(); return true;
    }
    if (rgba) scheme->colors[std::string(marker_id)] = *rgba;
    else scheme->colors.erase(std::string(marker_id));
    auto candidate = overlay;
    candidate.color_schemes = std::move(schemes);
    return Publish(base, overlay, std::move(candidate), error);
}

bool ReplaceBuiltInGroupingView(const SpectralLineList& base, BuiltInSpectralLineOverlay& overlay,
    line_list::GroupingView view, std::string& error)
{
    if (BaseView(base, view.id)) { error = "base grouping view is read-only"; return false; }
    auto candidate = overlay;
    const auto found = std::find_if(candidate.grouping_views.begin(), candidate.grouping_views.end(),
        [&](const auto& value) { return value.id == view.id; });
    if (found == candidate.grouping_views.end()) candidate.grouping_views.push_back(std::move(view));
    else *found = std::move(view);
    return Publish(base, overlay, std::move(candidate), error);
}

bool RemoveBuiltInGroupingView(const SpectralLineList& base, BuiltInSpectralLineOverlay& overlay,
    std::string_view view_id, std::string& error)
{
    if (BaseView(base, view_id)) { error = "base grouping view is read-only"; return false; }
    auto candidate = overlay;
    if (std::erase_if(candidate.grouping_views, [&](const auto& view) { return view.id == view_id; }) == 0) {
        error = "grouping view identity does not exist"; return false;
    }
    return Publish(base, overlay, std::move(candidate), error);
}

bool ReplaceBuiltInColorSchemes(const SpectralLineList& base, BuiltInSpectralLineOverlay& overlay,
    std::optional<std::vector<line_list::ColorScheme>> schemes, std::string& error)
{
    auto candidate = overlay;
    candidate.color_schemes = std::move(schemes);
    return Publish(base, overlay, std::move(candidate), error);
}

BuiltInSpectralLineOverlay DecodeBuiltInSpectralLineOverlay(const nlohmann::json& value)
{
    if (!value.is_object() || !value.contains("grouping_views") || !value.at("grouping_views").is_array())
        throw std::runtime_error("overlay requires a grouping_views array");
    for (const auto& [key, child] : value.items()) {
        if ((key != "grouping_views" && key != "color_schemes") || !child.is_array())
            throw std::runtime_error("unknown or invalid overlay field: " + key);
    }
    BuiltInSpectralLineOverlay overlay;
    for (const auto& view : value.at("grouping_views")) overlay.grouping_views.push_back(DecodeLineListGroupingView(view));
    if (value.contains("color_schemes")) {
        overlay.color_schemes.emplace();
        for (const auto& scheme : value.at("color_schemes")) overlay.color_schemes->push_back(DecodeLineListColorScheme(scheme));
    }
    return overlay;
}

nlohmann::json EncodeBuiltInSpectralLineOverlay(const BuiltInSpectralLineOverlay& value)
{
    nlohmann::json result = {{"grouping_views", nlohmann::json::array()}};
    for (const auto& view : value.grouping_views) result["grouping_views"].push_back(EncodeLineListGroupingView(view));
    if (value.color_schemes) {
        result["color_schemes"] = nlohmann::json::array();
        for (const auto& scheme : *value.color_schemes) result["color_schemes"].push_back(EncodeLineListColorScheme(scheme));
    }
    return result;
}
} // namespace spectiary
