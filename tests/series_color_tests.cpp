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
    const specforge::PlotSeriesColor automatic;
    const specforge::RgbaColor custom{
        .red = 0.12f,
        .green = 0.34f,
        .blue = 0.56f,
        .alpha = 0.78f,
    };
    const specforge::PlotSeriesColor explicit_color =
        specforge::PlotSeriesColor::ExplicitColor(
            custom);

    Require(
        automatic ==
                specforge::PlotSeriesColor::Auto() &&
            automatic.mode() ==
                specforge::PlotSeriesColorMode::Auto &&
            !automatic.explicit_color(),
        "plot series colors should default to Auto without storing a resolved theme color");
    Require(
        explicit_color.mode() ==
                specforge::PlotSeriesColorMode::
                    ExplicitColor &&
            explicit_color.explicit_color() &&
            *explicit_color.explicit_color() == custom,
        "ExplicitColor should preserve the exact RGBA user value");
    Require(
        specforge::PlotSeriesColor::Auto() ==
            automatic,
        "restoring a series color should return to the canonical Auto state");
}

void TestSpectrumPlotStyleDefaultsToAuto()
{
    const specforge::SpectrumPlotStyle style;
    Require(
        style.colors.raw_spectrum ==
                specforge::PlotSeriesColor::Auto() &&
            style.colors.gaussian_smoothing ==
                specforge::PlotSeriesColor::Auto() &&
            style.colors.median_smoothing ==
                specforge::PlotSeriesColor::Auto(),
        "every built-in spectrum curve should enter the shared series-color path in Auto mode by default");
    Require(
        specforge::SpectrumSeriesStableId(
            specforge::SpectrumPlotSeries::RawSpectrum) ==
                specforge::kRawSpectrumPlotSeriesId &&
            specforge::SpectrumSeriesStableId(
                specforge::SpectrumPlotSeries::GaussianSmoothing) ==
                specforge::kGaussianSmoothingPlotSeriesId &&
            specforge::SpectrumSeriesStableId(
                specforge::SpectrumPlotSeries::MedianSmoothing) ==
                specforge::kMedianSmoothingPlotSeriesId,
        "the spectrum series mapping should preserve stable identities across UI, persistence, and rendering");
}

void TestAssignmentsRemainStableAcrossFrames()
{
    specforge::StablePlotSeriesColorAssignments
        assignments;
    const std::size_t raw_slot =
        assignments.SlotFor(
            specforge::kRawSpectrumPlotSeriesId);
    const std::size_t gaussian_slot =
        assignments.SlotFor(
            specforge::kGaussianSmoothingPlotSeriesId);

    Require(
        raw_slot == 0 &&
            gaussian_slot == 1 &&
            assignments.SlotFor(
                specforge::kRawSpectrumPlotSeriesId) ==
                raw_slot &&
            assignments.SlotFor(
                specforge::kGaussianSmoothingPlotSeriesId) ==
                gaussian_slot &&
            assignments.size() == 2,
        "stable series identities should retain one cached slot instead of being reassigned per frame");

    assignments.Clear();
    Require(
        assignments.size() == 0 &&
            assignments.SlotFor(
                specforge::kRawSpectrumPlotSeriesId) ==
                0,
        "clearing an owner scope should restart its stable palette assignment sequence");
}

void TestAutoPaletteUsesDistinctSlotsBeforeWrapping()
{
    const specforge::SemanticPalette& palette =
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInDarkThemeId())
            ->palette;
    specforge::StablePlotSeriesColorAssignments
        assignments;
    std::vector<ImVec4> resolved;
    resolved.reserve(
        specforge::kPlotAutoSeriesColorCount + 1);

    for (std::size_t index = 0;
         index < specforge::kPlotAutoSeriesColorCount + 1;
         ++index) {
        resolved.push_back(
            specforge::ResolvePlotSeriesColor(
                specforge::PlotSeriesColor::Auto(),
                palette,
                assignments,
                "visible-series-" +
                    std::to_string(index)));
    }

    for (std::size_t left = 0;
         left < specforge::kPlotAutoSeriesColorCount;
         ++left) {
        for (std::size_t right = left + 1;
             right <
                specforge::kPlotAutoSeriesColorCount;
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
    const specforge::SemanticPalette& dark =
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInDarkThemeId())
            ->palette;
    const specforge::SemanticPalette& light =
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInLightThemeId())
            ->palette;
    specforge::StablePlotSeriesColorAssignments
        assignments;

    const ImVec4 dark_color =
        specforge::ResolvePlotSeriesColor(
            specforge::PlotSeriesColor::Auto(),
            dark,
            assignments,
            specforge::kMedianSmoothingPlotSeriesId);
    const ImVec4 light_color =
        specforge::ResolvePlotSeriesColor(
            specforge::PlotSeriesColor::Auto(),
            light,
            assignments,
            specforge::kMedianSmoothingPlotSeriesId);
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
    const specforge::SemanticPalette& dark =
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInDarkThemeId())
            ->palette;
    const specforge::SemanticPalette& light =
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInLightThemeId())
            ->palette;
    const specforge::RgbaColor custom{
        .red = 0.91f,
        .green = 0.13f,
        .blue = 0.47f,
        .alpha = 0.63f,
    };
    specforge::StablePlotSeriesColorAssignments
        assignments;

    const ImVec4 explicit_dark =
        specforge::ResolvePlotSeriesColor(
            specforge::PlotSeriesColor::ExplicitColor(
                custom),
            dark,
            assignments,
            specforge::kRawSpectrumPlotSeriesId);
    const ImVec4 explicit_light =
        specforge::ResolvePlotSeriesColor(
            specforge::PlotSeriesColor::ExplicitColor(
                custom),
            light,
            assignments,
            specforge::kRawSpectrumPlotSeriesId);
    const ImVec4 restored_auto =
        specforge::ResolvePlotSeriesColor(
            specforge::PlotSeriesColor::Auto(),
            light,
            assignments,
            specforge::kRawSpectrumPlotSeriesId);

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
        specforge::ApplyRawSpectrumSmoothingEmphasis(
            specforge::PlotSeriesColor::Auto(),
            resolved);
    const ImVec4 explicit_color =
        specforge::ApplyRawSpectrumSmoothingEmphasis(
            specforge::PlotSeriesColor::ExplicitColor(
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
    return 0;
}
