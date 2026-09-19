#pragma once

#include "plot/series_color_model.h"
#include "ui/theme.h"

#include <cstddef>
#include <string_view>

namespace spectiary {

[[nodiscard]] ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    std::size_t automatic_slot) noexcept;

[[nodiscard]] ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    StablePlotSeriesColorAssignments& assignments,
    std::string_view stable_series_id);

}  // namespace spectiary
