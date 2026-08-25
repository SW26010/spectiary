#pragma once

#include "ui/theme.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace specforge {

inline constexpr std::string_view kRawSpectrumPlotSeriesId =
    "spectrum.raw";
inline constexpr std::string_view kGaussianSmoothingPlotSeriesId =
    "spectrum.smoothing.gaussian";
inline constexpr std::string_view kMedianSmoothingPlotSeriesId =
    "spectrum.smoothing.median";

struct RgbaColor {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;

    [[nodiscard]] bool operator==(
        const RgbaColor&) const = default;
};

enum class PlotSeriesColorMode {
    Auto,
    ExplicitColor,
};

// Auto deliberately stores no resolved RGB value. The same selection can
// therefore resolve against a different theme without mutating user state.
class PlotSeriesColor {
public:
    PlotSeriesColor() noexcept = default;
    [[nodiscard]] static PlotSeriesColor Auto() noexcept;
    [[nodiscard]] static PlotSeriesColor ExplicitColor(
        RgbaColor color) noexcept;
    [[nodiscard]] PlotSeriesColorMode mode() const noexcept;
    [[nodiscard]] const std::optional<RgbaColor>&
    explicit_color() const noexcept;

    [[nodiscard]] bool operator==(
        const PlotSeriesColor&) const = default;

private:
    std::optional<RgbaColor> explicit_color_;
};

// The owner keeps one assignment table across frames. New identities receive
// successive slots so simultaneously introduced series use distinct palette
// entries before the palette wraps; existing identities never change slots.
class StablePlotSeriesColorAssignments {
public:
    [[nodiscard]] std::size_t SlotFor(
        std::string_view stable_series_id);
    void Clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, std::size_t>
        assignments_;
    std::size_t next_slot_ = 0;
};

[[nodiscard]] ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    std::size_t automatic_slot) noexcept;

[[nodiscard]] ImVec4 ResolvePlotSeriesColor(
    const PlotSeriesColor& selection,
    const SemanticPalette& theme_palette,
    StablePlotSeriesColorAssignments& assignments,
    std::string_view stable_series_id);

}  // namespace specforge
