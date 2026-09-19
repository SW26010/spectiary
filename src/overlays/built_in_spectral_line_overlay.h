#pragma once

#include "overlays/spectral_line_list_json.h"

namespace spectiary {
struct BuiltInSpectralLineOverlay {
    std::vector<line_list::GroupingView> grouping_views;
    // Absent inherits; an explicitly empty collection stays empty.
    std::optional<std::vector<line_list::ColorScheme>> color_schemes;
    bool operator==(const BuiltInSpectralLineOverlay&) const = default;
};

[[nodiscard]] SpectralLineListParseResult ComposeBuiltInSpectralLineList(
    const SpectralLineList& base, const BuiltInSpectralLineOverlay& overlay);

// These mutations validate before publication and never change the base.
[[nodiscard]] bool SetBuiltInMarkerColor(const SpectralLineList& base,
    BuiltInSpectralLineOverlay& overlay, std::string_view scheme_id,
    std::string_view marker_id, std::optional<std::string> rgba, std::string& error);
[[nodiscard]] bool ReplaceBuiltInGroupingView(const SpectralLineList& base,
    BuiltInSpectralLineOverlay& overlay, line_list::GroupingView view, std::string& error);
[[nodiscard]] bool RemoveBuiltInGroupingView(const SpectralLineList& base,
    BuiltInSpectralLineOverlay& overlay, std::string_view view_id, std::string& error);
[[nodiscard]] bool ReplaceBuiltInColorSchemes(const SpectralLineList& base,
    BuiltInSpectralLineOverlay& overlay,
    std::optional<std::vector<line_list::ColorScheme>> schemes, std::string& error);

// Internal record only; the enclosing state codec supplies version and identity.
[[nodiscard]] BuiltInSpectralLineOverlay DecodeBuiltInSpectralLineOverlay(const nlohmann::json& value);
[[nodiscard]] nlohmann::json EncodeBuiltInSpectralLineOverlay(const BuiltInSpectralLineOverlay& value);
} // namespace spectiary
