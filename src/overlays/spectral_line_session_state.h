#pragma once
#include "overlays/built_in_spectral_line_overlay.h"
#include "plot/series_color_model.h"
#include <unordered_map>
#include <unordered_set>

namespace spectiary {
inline constexpr std::size_t kMaximumGeneratedNameCopyCount = 1024;
enum class GeneratedNameSource {
    None,
    CatalogGroupingView,
    DefaultGroupingView,
    DefaultGroup,
};

struct GeneratedNameMetadata {
    GeneratedNameSource source = GeneratedNameSource::None;
    std::size_t ordinal = 0;
    std::size_t copy_count = 0;
    std::string copy_base_name;

    [[nodiscard]] bool operator==(const GeneratedNameMetadata&) const = default;
};

struct SpectralLineSessionState {
    std::string active_view_id;
    std::string active_color_scheme_id;
    std::unordered_map<std::string, bool> marker_visibility;
    std::unordered_set<std::string> expanded_group_ids;
    // UI localization provenance, never canonical grouping content.
    std::unordered_map<std::string, GeneratedNameMetadata> view_names;
    std::unordered_map<std::string, GeneratedNameMetadata> group_names;
    bool operator==(const SpectralLineSessionState&) const = default;
};
struct BuiltInSpectralLineState {
    BuiltInSpectralLineOverlay overlay;
    SpectralLineSessionState session;
    bool operator==(const BuiltInSpectralLineState&) const = default;
};
[[nodiscard]] std::string EncodeLineListColor(const RgbaColor& color);
[[nodiscard]] PlotSeriesColor DecodeLineListColor(std::string_view color);
void NormalizeSpectralLineSession(SpectralLineSessionState& session, const SpectralLineList& effective);
} // namespace spectiary
