#include "overlays/spectral_line_session_state.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace spectiary {
std::string EncodeLineListColor(const RgbaColor& color)
{
    if (!IsValidRgbaColor(color)) throw std::runtime_error("invalid RGBA color");
    const auto byte = [](float value) { return static_cast<unsigned>(std::lround(value * 255.0f)); };
    std::array<char, 10> text{};
    std::snprintf(text.data(), text.size(), "#%02X%02X%02X%02X", byte(color.red), byte(color.green), byte(color.blue), byte(color.alpha));
    return text.data();
}
PlotSeriesColor DecodeLineListColor(std::string_view color)
{
    if (color.size() != 9 || color[0] != '#') return PlotSeriesColor::Auto();
    std::array<float, 4> channels{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        unsigned value = 0;
        const char* first = color.data() + 1 + i * 2;
        const auto parsed = std::from_chars(first, first + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != first + 2) return PlotSeriesColor::Auto();
        channels[i] = static_cast<float>(value) / 255.0f;
    }
    return PlotSeriesColor::ExplicitColor({channels[0], channels[1], channels[2], channels[3]});
}
void NormalizeSpectralLineSession(SpectralLineSessionState& session, const SpectralLineList& effective)
{
    const auto has_view = [&](std::string_view id) {
        return std::any_of(effective.grouping_views.begin(), effective.grouping_views.end(), [&](const auto& v) { return v.id == id; });
    };
    if (!has_view(session.active_view_id)) session.active_view_id = effective.grouping_views.empty() ? "" : effective.grouping_views.front().id;
    if (std::none_of(effective.color_schemes.begin(), effective.color_schemes.end(), [&](const auto& s) { return s.id == session.active_color_scheme_id; }))
        session.active_color_scheme_id = effective.color_schemes.empty() ? "" : effective.color_schemes.front().id;
    // Restoration fallback does not modify canonical/overlay contents. Metadata
    // and hidden marker choices may remain inert until those identities return.
}
} // namespace spectiary
