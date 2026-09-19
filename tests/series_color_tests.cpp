#include "plot/series_color.h"
#include "plot/spectrum_plot.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool NearlyEqual(float left, float right)
{
    return std::abs(left - right) < 0.00001f;
}

bool SameColor(const ImVec4& left, const ImVec4& right)
{
    return NearlyEqual(left.x, right.x) &&
           NearlyEqual(left.y, right.y) &&
           NearlyEqual(left.z, right.z) &&
           NearlyEqual(left.w, right.w);
}

double LinearColorComponent(double component)
{
    return component <= 0.04045
        ? component / 12.92
        : std::pow(
              (component + 0.055) / 1.055,
              2.4);
}

double RelativeLuminance(const ImVec4& color)
{
    return
        0.2126 * LinearColorComponent(color.x) +
        0.7152 * LinearColorComponent(color.y) +
        0.0722 * LinearColorComponent(color.z);
}

double ContrastRatio(
    const ImVec4& left,
    const ImVec4& right)
{
    const double left_luminance =
        RelativeLuminance(left);
    const double right_luminance =
        RelativeLuminance(right);
    return
        (std::max(left_luminance, right_luminance) +
         0.05) /
        (std::min(left_luminance, right_luminance) +
         0.05);
}

void TestColorModelExpressesAutoAndExplicitColor()
{
    const spectiary::PlotSeriesColor automatic;
    const spectiary::RgbaColor custom{
        .red = 0.12f,
        .green = 0.34f,
        .blue = 0.56f,
        .alpha = 0.78f,
    };
    const spectiary::PlotSeriesColor explicit_color =
        spectiary::PlotSeriesColor::ExplicitColor(
            custom);

    Require(
        automatic ==
                spectiary::PlotSeriesColor::Auto() &&
            automatic.mode() ==
                spectiary::PlotSeriesColorMode::Auto &&
            !automatic.explicit_color(),
        "plot series colors should default to Auto without storing a resolved theme color");
    Require(
        explicit_color.mode() ==
                spectiary::PlotSeriesColorMode::
                    ExplicitColor &&
            explicit_color.explicit_color() &&
            *explicit_color.explicit_color() == custom,
        "ExplicitColor should preserve the exact RGBA user value");
    Require(
        spectiary::PlotSeriesColor::Auto() ==
            automatic,
        "restoring a series color should return to the canonical Auto state");
}

void TestSpectrumPlotStyleDefaultsToAuto()
{
    const spectiary::SpectrumPlotStyle style;
    Require(
        style.colors.raw_spectrum ==
                spectiary::PlotSeriesColor::Auto() &&
            style.colors.gaussian_smoothing ==
                spectiary::PlotSeriesColor::Auto() &&
            style.colors.median_smoothing ==
                spectiary::PlotSeriesColor::Auto(),
        "every built-in spectrum curve should enter the shared series-color path in Auto mode by default");
    Require(
        spectiary::SpectrumSeriesStableId(
            spectiary::SpectrumPlotSeries::RawSpectrum) ==
                spectiary::kRawSpectrumPlotSeriesId &&
            spectiary::SpectrumSeriesStableId(
                spectiary::SpectrumPlotSeries::GaussianSmoothing) ==
                spectiary::kGaussianSmoothingPlotSeriesId &&
            spectiary::SpectrumSeriesStableId(
                spectiary::SpectrumPlotSeries::MedianSmoothing) ==
                spectiary::kMedianSmoothingPlotSeriesId,
        "the spectrum series mapping should preserve stable identities across UI, persistence, and rendering");
}

void TestAssignmentsRemainStableAcrossFrames()
{
    spectiary::StablePlotSeriesColorAssignments
        assignments;
    const std::size_t raw_slot =
        assignments.SlotFor(
            spectiary::kRawSpectrumPlotSeriesId);
    const std::size_t gaussian_slot =
        assignments.SlotFor(
            spectiary::kGaussianSmoothingPlotSeriesId);

    Require(
        raw_slot == 0 &&
            gaussian_slot == 1 &&
            assignments.SlotFor(
                spectiary::kRawSpectrumPlotSeriesId) ==
                raw_slot &&
            assignments.SlotFor(
                spectiary::kGaussianSmoothingPlotSeriesId) ==
                gaussian_slot &&
            assignments.size() == 2,
        "stable series identities should retain one cached slot instead of being reassigned per frame");

    assignments.Clear();
    Require(
        assignments.size() == 0 &&
            assignments.SlotFor(
                spectiary::kRawSpectrumPlotSeriesId) ==
                0,
        "clearing an owner scope should restart its stable palette assignment sequence");
}

void TestAutoPaletteUsesDistinctSlotsBeforeWrapping()
{
    const spectiary::SemanticPalette& palette =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
            ->palette;
    spectiary::StablePlotSeriesColorAssignments
        assignments;
    std::vector<ImVec4> resolved;
    resolved.reserve(
        spectiary::kPlotAutoSeriesColorCount + 1);

    for (std::size_t index = 0;
         index < spectiary::kPlotAutoSeriesColorCount + 1;
         ++index) {
        resolved.push_back(
            spectiary::ResolvePlotSeriesColor(
                spectiary::PlotSeriesColor::Auto(),
                palette,
                assignments,
                "visible-series-" +
                    std::to_string(index)));
    }

    for (std::size_t left = 0;
         left < spectiary::kPlotAutoSeriesColorCount;
         ++left) {
        for (std::size_t right = left + 1;
             right <
                spectiary::kPlotAutoSeriesColorCount;
             ++right) {
            Require(
                !SameColor(
                    resolved[left],
                    resolved[right]),
                "simultaneously introduced Auto series should receive distinct palette entries before wrapping");
        }
    }
    Require(
        SameColor(
            resolved.front(),
            resolved.back()),
        "automatic palette assignment should wrap deterministically after exhausting the theme palette");
}

void TestAutoResolutionTracksResolvedTheme()
{
    const spectiary::SemanticPalette& dark =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
            ->palette;
    const spectiary::SemanticPalette& light =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInLightThemeId())
            ->palette;
    spectiary::StablePlotSeriesColorAssignments
        assignments;

    const ImVec4 dark_color =
        spectiary::ResolvePlotSeriesColor(
            spectiary::PlotSeriesColor::Auto(),
            dark,
            assignments,
            spectiary::kMedianSmoothingPlotSeriesId);
    const ImVec4 light_color =
        spectiary::ResolvePlotSeriesColor(
            spectiary::PlotSeriesColor::Auto(),
            light,
            assignments,
            spectiary::kMedianSmoothingPlotSeriesId);
    Require(
        !SameColor(dark_color, light_color) &&
            SameColor(
                dark_color,
                dark.plot_auto_series.front()) &&
            SameColor(
                light_color,
                light.plot_auto_series.front()),
        "one stable Auto slot should resolve through the currently resolved theme instead of freezing an RGB value");

    for (const ImVec4& color :
         dark.plot_auto_series) {
        Require(
            ContrastRatio(color, dark.background) >=
                3.5,
            "every dark-theme Auto series color should retain readable contrast against the plot background");
    }
    for (const ImVec4& color :
         light.plot_auto_series) {
        Require(
            ContrastRatio(color, light.background) >=
                3.5,
            "every light-theme Auto series color should retain readable contrast against the plot background");
    }
}

void TestExplicitColorWinsAndReturningToAutoReusesSlot()
{
    const spectiary::SemanticPalette& dark =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
            ->palette;
    const spectiary::SemanticPalette& light =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInLightThemeId())
            ->palette;
    const spectiary::RgbaColor custom{
        .red = 0.91f,
        .green = 0.13f,
        .blue = 0.47f,
        .alpha = 0.63f,
    };
    spectiary::StablePlotSeriesColorAssignments
        assignments;

    const ImVec4 explicit_dark =
        spectiary::ResolvePlotSeriesColor(
            spectiary::PlotSeriesColor::ExplicitColor(
                custom),
            dark,
            assignments,
            spectiary::kRawSpectrumPlotSeriesId);
    const ImVec4 explicit_light =
        spectiary::ResolvePlotSeriesColor(
            spectiary::PlotSeriesColor::ExplicitColor(
                custom),
            light,
            assignments,
            spectiary::kRawSpectrumPlotSeriesId);
    const ImVec4 restored_auto =
        spectiary::ResolvePlotSeriesColor(
            spectiary::PlotSeriesColor::Auto(),
            light,
            assignments,
            spectiary::kRawSpectrumPlotSeriesId);

    const ImVec4 expected_custom(
        custom.red,
        custom.green,
        custom.blue,
        custom.alpha);
    Require(
        SameColor(explicit_dark, expected_custom) &&
            SameColor(explicit_light, expected_custom),
        "ExplicitColor should take priority and remain unchanged across theme resolution");
    Require(
        assignments.size() == 1 &&
            SameColor(
                restored_auto,
                light.plot_auto_series.front()),
        "returning to Auto should reuse the series' reserved stable slot and current theme palette");
}

void TestSmoothingEmphasisPreservesExplicitRgba()
{
    const ImVec4 resolved(
        0.21f,
        0.43f,
        0.65f,
        0.72f);
    const ImVec4 automatic =
        spectiary::ApplyRawSpectrumSmoothingEmphasis(
            spectiary::PlotSeriesColor::Auto(),
            resolved);
    const ImVec4 explicit_color =
        spectiary::ApplyRawSpectrumSmoothingEmphasis(
            spectiary::PlotSeriesColor::ExplicitColor(
                {
                    .red = resolved.x,
                    .green = resolved.y,
                    .blue = resolved.z,
                    .alpha = resolved.w,
                }),
            resolved);

    Require(
        NearlyEqual(automatic.x, resolved.x) &&
            NearlyEqual(automatic.y, resolved.y) &&
            NearlyEqual(automatic.z, resolved.z) &&
            NearlyEqual(automatic.w, 0.30f),
        "smoothing should apply the intentional raw-curve opacity only to Auto color");
    Require(
        SameColor(explicit_color, resolved),
        "smoothing presentation must preserve a user's complete explicit RGBA value");
}

void TestSpectralLineVisualsShareOneResolvedColor()
{
    const spectiary::SemanticPalette& dark =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
            ->palette;
    const spectiary::SemanticPalette& light =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInLightThemeId())
            ->palette;
    spectiary::SpectralLinePlotMarker automatic;
    automatic.automatic_color_slot = 3;

    const auto dark_colors =
        spectiary::ResolveSpectralLineVisualColors(
            automatic,
            dark);
    const auto light_colors =
        spectiary::ResolveSpectralLineVisualColors(
            automatic,
            light);
    Require(
        SameColor(
            dark_colors.marker_and_label,
            dark.plot_auto_series[3]) &&
            SameColor(
                light_colors.marker_and_label,
                light.plot_auto_series[3]) &&
            !SameColor(
                dark_colors.marker_and_label,
                light_colors.marker_and_label),
        "an Auto spectral marker and its label should follow the assigned slot in the active theme palette");
    Require(
        NearlyEqual(
            dark_colors.band_fill.x,
            dark_colors.marker_and_label.x) &&
            NearlyEqual(
                dark_colors.band_fill.y,
                dark_colors.marker_and_label.y) &&
            NearlyEqual(
                dark_colors.band_fill.z,
                dark_colors.marker_and_label.z) &&
            NearlyEqual(
                dark_colors.band_fill.w,
                dark_colors.marker_and_label.w * 0.12f),
        "band fill should derive only opacity from the marker-and-label color");

    const ImVec4 custom(0.11f, 0.22f, 0.33f, 0.44f);
    automatic.color =
        spectiary::PlotSeriesColor::ExplicitColor({
            .red = custom.x,
            .green = custom.y,
            .blue = custom.z,
            .alpha = custom.w,
        });
    Require(
        SameColor(
            spectiary::ResolveSpectralLineVisualColors(
                automatic,
                dark)
                .marker_and_label,
            custom) &&
            SameColor(
                spectiary::ResolveSpectralLineVisualColors(
                    automatic,
                    light)
                    .marker_and_label,
                custom),
        "an explicit spectral marker and label RGBA should be exact and theme-independent");
}

}  // namespace

int main()
{
    TestColorModelExpressesAutoAndExplicitColor();
    TestSpectrumPlotStyleDefaultsToAuto();
    TestAssignmentsRemainStableAcrossFrames();
    TestAutoPaletteUsesDistinctSlotsBeforeWrapping();
    TestAutoResolutionTracksResolvedTheme();
    TestExplicitColorWinsAndReturningToAutoReusesSlot();
    TestSmoothingEmphasisPreservesExplicitRgba();
    TestSpectralLineVisualsShareOneResolvedColor();
    return 0;
}
