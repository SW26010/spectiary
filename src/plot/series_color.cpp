#include "plot/series_color.h"

namespace specforge {

PlotSeriesColor PlotSeriesColor::Auto() noexcept
{
    return {};
}

PlotSeriesColor PlotSeriesColor::ExplicitColor(
    RgbaColor color) noexcept
{
    PlotSeriesColor selection;
    selection.explicit_color_ = color;
    return selection;
}

PlotSeriesColorMode PlotSeriesColor::mode() const noexcept
{
    return explicit_color_
        ? PlotSeriesColorMode::ExplicitColor
        : PlotSeriesColorMode::Auto;
}

const std::optional<RgbaColor>&
PlotSeriesColor::explicit_color() const noexcept
{
    return explicit_color_;
}

std::size_t StablePlotSeriesColorAssignments::SlotFor(
    std::string_view stable_series_id)
{
    const auto [entry, inserted] =
        assignments_.try_emplace(
            std::string(stable_series_id),
            next_slot_);
    if (inserted) {
        ++next_slot_;
    }
    return entry->second;
}

void StablePlotSeriesColorAssignments::Clear() noexcept
{
    assignments_.clear();
    next_slot_ = 0;
}

std::size_t
StablePlotSeriesColorAssignments::size() const noexcept
{
    return assignments_.size();
}

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

}  // namespace specforge
