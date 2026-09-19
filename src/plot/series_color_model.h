#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spectiary {

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

[[nodiscard]] inline bool IsValidRgbaColor(
    const RgbaColor& color) noexcept
{
    const auto channel_is_valid = [](float value) {
        return std::isfinite(value) && value >= 0.0f &&
               value <= 1.0f;
    };
    return channel_is_valid(color.red) &&
           channel_is_valid(color.green) &&
           channel_is_valid(color.blue) &&
           channel_is_valid(color.alpha);
}

enum class PlotSeriesColorMode {
    Auto,
    ExplicitColor,
};

// Auto deliberately stores no resolved RGB value. The same selection can
// therefore resolve against a different theme without mutating user state.
class PlotSeriesColor {
public:
    PlotSeriesColor() noexcept = default;

    [[nodiscard]] static PlotSeriesColor Auto() noexcept
    {
        return {};
    }

    [[nodiscard]] static PlotSeriesColor ExplicitColor(
        RgbaColor color) noexcept
    {
        PlotSeriesColor selection;
        selection.explicit_color_ = color;
        return selection;
    }

    [[nodiscard]] PlotSeriesColorMode mode() const noexcept
    {
        return explicit_color_
            ? PlotSeriesColorMode::ExplicitColor
            : PlotSeriesColorMode::Auto;
    }

    [[nodiscard]] const std::optional<RgbaColor>&
    explicit_color() const noexcept
    {
        return explicit_color_;
    }

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

    void Clear() noexcept
    {
        assignments_.clear();
        next_slot_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return assignments_.size();
    }

private:
    std::unordered_map<std::string, std::size_t>
        assignments_;
    std::size_t next_slot_ = 0;
};

}  // namespace spectiary
