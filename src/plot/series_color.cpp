#include "plot/series_color.h"

namespace spectiary {

ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    std::size_t automatic_slot) noexcept
{
    if (selection.explicit_color()) {
        const RgbaColor& color =
            *selection.explicit_color();
        return {
            color.red,
            color.green,
            color.blue,
            color.alpha,
        };
    }

    const auto& colors =
        theme_palette.plot_auto_series;
    return colors[automatic_slot % colors.size()];
}

ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    StablePlotSeriesColorAssignments& assignments,
    std::string_view stable_series_id)
{
    return ResolvePlotSeriesColor(
        selection,
        theme_palette,
        assignments.SlotFor(stable_series_id));
}

}  // namespace spectiary
