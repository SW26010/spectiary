#include "ui/theme.h"

#include <implot.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool NearlyEqual(float left, float right)
{
    return std::fabs(left - right) < 0.00001f;
}

bool SameColor(const ImVec4& left, const ImVec4& right)
{
    return NearlyEqual(left.x, right.x) &&
           NearlyEqual(left.y, right.y) &&
           NearlyEqual(left.z, right.z) &&
           NearlyEqual(left.w, right.w);
}

float Luminance(const ImVec4& color)
{
    return 0.2126f * color.x +
           0.7152f * color.y +
           0.0722f * color.z;
}

void TestBuiltInThemesHaveStableIdentities()
{
    const std::span<const specforge::ThemeDescriptor> descriptors =
        specforge::BuiltInThemeDescriptors();
    Require(
        descriptors.size() == 2,
        "the initial registry should expose the two built-in themes");
    Require(
        specforge::BuiltInDarkThemeId().value() ==
                "specforge.theme.dark" &&
            specforge::BuiltInLightThemeId().value() ==
                "specforge.theme.light",
        "built-in themes should expose stable non-boolean identities");
    Require(
        specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInDarkThemeId())
                ->color_scheme ==
            specforge::ThemeColorScheme::Dark &&
            specforge::FindBuiltInThemeDescriptor(
                specforge::BuiltInLightThemeId())
                    ->color_scheme ==
                specforge::ThemeColorScheme::Light,
        "theme descriptors should carry color-scheme metadata separately from identity");
}

void TestWindowsPreferenceMapsToConcreteThemeIds()
{
    Require(
        specforge::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::uint32_t{0}) ==
            specforge::BuiltInDarkThemeId(),
        "Windows AppsUseLightTheme=0 should resolve to the dark theme ID");
    Require(
        specforge::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::uint32_t{1}) ==
            specforge::BuiltInLightThemeId(),
        "Windows AppsUseLightTheme=1 should resolve to the light theme ID");
    Require(
        !specforge::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::nullopt),
        "an unreadable Windows preference should remain unresolved at the platform boundary");
}

void TestFollowSystemResolutionFallsBackToDark()
{
    const specforge::ThemeSelection follow_system =
        specforge::ThemeSelection::FollowSystem();
    Require(
        specforge::ResolveThemeId(
            follow_system,
            specforge::BuiltInLightThemeId()) ==
            specforge::BuiltInLightThemeId(),
        "follow-system should use the concrete system theme identity");
    Require(
        specforge::ResolveThemeId(
            follow_system,
            specforge::BuiltInDarkThemeId()) ==
            specforge::BuiltInDarkThemeId(),
        "follow-system should resolve the dark Windows state without changing selection policy");
    Require(
        specforge::ResolveThemeId(
            follow_system,
            std::nullopt) ==
            specforge::BuiltInDarkThemeId(),
        "an unavailable system theme should safely resolve to dark");
}

void TestExplicitSelectionHasPriority()
{
    const specforge::ThemeSelection explicit_light =
        specforge::ThemeSelection::Explicit(
            specforge::BuiltInLightThemeId());
    Require(
        specforge::ResolveThemeId(
            explicit_light,
            specforge::BuiltInDarkThemeId()) ==
            specforge::BuiltInLightThemeId(),
        "an explicit theme identity should take priority over the system theme");
    Require(
        specforge::ResolveThemeId(
            explicit_light,
            std::nullopt) ==
            specforge::BuiltInLightThemeId(),
        "an explicit theme should remain unaffected when the system theme cannot be read");
}

void TestSelectionStableValuesRoundTrip()
{
    const std::array selections = {
        specforge::ThemeSelection::FollowSystem(),
        specforge::ThemeSelection::Explicit(
            specforge::BuiltInDarkThemeId()),
        specforge::ThemeSelection::Explicit(
            specforge::BuiltInLightThemeId()),
    };
    for (const specforge::ThemeSelection& selection :
         selections) {
        Require(
            specforge::ParseThemeSelectionStableValue(
                specforge::ThemeSelectionStableValue(
                    selection)) == selection,
            "theme selection automation values should round-trip through stable non-boolean identities");
    }
    Require(
        !specforge::ParseThemeSelectionStableValue(
            "specforge.theme.unknown"),
        "unknown theme automation values should be rejected at the registry boundary");
}

void TestResolverPreservesSyntheticFutureIdentity()
{
    const specforge::ThemeId future_theme(
        std::string_view("specforge.theme.synthetic"));
    const specforge::ThemeSelection selection =
        specforge::ThemeSelection::Explicit(future_theme);
    Require(
        specforge::ResolveThemeId(
            selection,
            specforge::BuiltInDarkThemeId()) ==
            future_theme,
        "the unified resolver should preserve a non-light/dark theme identity without a new branch");
    const std::array future_registry = {
        specforge::ThemeDescriptor{
            .id = future_theme,
            .color_scheme =
                specforge::ThemeColorScheme::Dark,
        },
    };
    Require(
        specforge::FindThemeDescriptor(
            future_registry,
            specforge::ResolveThemeId(
                selection,
                specforge::BuiltInLightThemeId())) ==
            &future_registry.front(),
        "a synthetic theme should pass through resolution into the shared descriptor lookup path");
    Require(
        !specforge::IsSupportedThemeSelection(selection),
        "the current settings registry should still reject unshipped themes");
}

void TestResolvedDescriptorsOwnAllRenderingColors()
{
    const specforge::ThemeDescriptor& dark =
        specforge::ResolveBuiltInThemeDescriptor(
            specforge::ThemeSelection::Explicit(
                specforge::BuiltInDarkThemeId()),
            specforge::BuiltInLightThemeId());
    const specforge::ThemeDescriptor& light =
        specforge::ResolveBuiltInThemeDescriptor(
            specforge::ThemeSelection::FollowSystem(),
            specforge::BuiltInLightThemeId());
    Require(
        dark.id == specforge::BuiltInDarkThemeId() &&
            light.id == specforge::BuiltInLightThemeId(),
        "descriptor resolution should preserve explicit priority and follow-system identity");
    Require(
        Luminance(dark.palette.background) <
                Luminance(dark.palette.text) &&
            Luminance(light.palette.background) >
                Luminance(light.palette.text),
        "each semantic palette should provide readable background/text polarity");
    Require(
        dark.clear_color[0] < light.clear_color[0] &&
            dark.clear_color[1] < light.clear_color[1] &&
            dark.clear_color[2] < light.clear_color[2],
        "the descriptor should own distinct D3D clear colors for dark and light themes");
    Require(
        SameColor(
            dark.palette.plot_auto_series.front(),
            dark.palette.accent) &&
            SameColor(
                light.palette.plot_auto_series.front(),
                light.palette.accent),
        "the first Auto series color should derive plot emphasis from the application accent without a duplicate legacy field");
}

void TestImGuiThemeApplicationUsesOfficialColorsAndOpaqueViewportBackground()
{
    for (const specforge::ThemeDescriptor& theme :
         specforge::BuiltInThemeDescriptors()) {
        ImGuiStyle actual;
        actual.WindowRounding = 7.25f;
        actual.FramePadding = ImVec2(13.0f, 17.0f);
        actual.ItemSpacing = ImVec2(19.0f, 23.0f);
        actual.FontScaleMain = 1.37f;
        ImGuiStyle official = actual;
        if (theme.color_scheme ==
            specforge::ThemeColorScheme::Light) {
            ImGui::StyleColorsLight(&official);
        } else {
            ImGui::StyleColorsDark(&official);
        }
        official.Colors[ImGuiCol_WindowBg].w = 1.0f;

        specforge::ApplyImGuiThemeColors(theme, actual);
        for (int color = 0; color < ImGuiCol_COUNT; ++color) {
            Require(
                SameColor(actual.Colors[color], official.Colors[color]),
                "runtime ImGui application should use the official theme color table with an opaque viewport background");
        }
        Require(
            NearlyEqual(actual.WindowRounding, 7.25f) &&
                NearlyEqual(actual.FramePadding.x, 13.0f) &&
                NearlyEqual(actual.FramePadding.y, 17.0f) &&
                NearlyEqual(actual.ItemSpacing.x, 19.0f) &&
                NearlyEqual(actual.ItemSpacing.y, 23.0f) &&
                NearlyEqual(actual.FontScaleMain, 1.37f),
            "a runtime theme switch must preserve every sampled non-color ImGui setting");
    }
}

void TestImPlotThemeApplicationUsesAutoBaseAndSemanticOverrides()
{
    for (const specforge::ThemeDescriptor& theme :
         specforge::BuiltInThemeDescriptors()) {
        ImPlotStyle actual;
        actual.PlotBorderSize = 4.75f;
        actual.PlotPadding = ImVec2(11.0f, 13.0f);
        actual.Use24HourClock = true;
        ImPlotStyle automatic = actual;
        ImPlot::StyleColorsAuto(&automatic);

        specforge::ApplyImPlotThemeColors(theme, actual);
        Require(
            SameColor(
                actual.Colors[ImPlotCol_AxisText],
                theme.palette.plot_axis) &&
                SameColor(
                    actual.Colors[ImPlotCol_AxisGrid],
                    theme.palette.plot_grid) &&
                SameColor(
                    actual.Colors[ImPlotCol_Selection],
                    theme.palette.selection) &&
                SameColor(
                    actual.Colors[ImPlotCol_Crosshairs],
                    theme.palette.plot_crosshair),
            "ImPlot should consume the resolved theme semantic plot colors");
        Require(
            SameColor(
                actual.Colors[ImPlotCol_FrameBg],
                automatic.Colors[ImPlotCol_FrameBg]) &&
                SameColor(
                    actual.Colors[ImPlotCol_PlotBg],
                    automatic.Colors[ImPlotCol_PlotBg]),
            "ImPlot colors not owned by the semantic palette should retain the official automatic base");
        Require(
            NearlyEqual(actual.PlotBorderSize, 4.75f) &&
                NearlyEqual(actual.PlotPadding.x, 11.0f) &&
                NearlyEqual(actual.PlotPadding.y, 13.0f) &&
                actual.Use24HourClock,
            "a runtime theme switch must preserve every sampled non-color ImPlot setting");
    }
}

void TestSyntheticThemeUsesSharedRuntimeApplicationPath()
{
    specforge::ThemeDescriptor synthetic{
        .id = specforge::ThemeId(
            "specforge.theme.synthetic.light"),
        .color_scheme =
            specforge::ThemeColorScheme::Light,
        .palette =
            specforge::FindBuiltInThemeDescriptor(
                specforge::BuiltInLightThemeId())
                ->palette,
        .clear_color = {0.91f, 0.92f, 0.93f, 1.0f},
    };
    synthetic.palette.accent =
        ImVec4(0.21f, 0.31f, 0.71f, 1.0f);

    ImGuiStyle actual;
    ImGuiStyle official = actual;
    ImGui::StyleColorsLight(&official);
    official.Colors[ImGuiCol_WindowBg].w = 1.0f;
    specforge::ApplyImGuiThemeColors(
        synthetic,
        actual);
    specforge::ActivateTheme(synthetic);
    Require(
        SameColor(
            actual.Colors[ImGuiCol_WindowBg],
            official.Colors[ImGuiCol_WindowBg]) &&
            SameColor(
                specforge::ActiveSemanticPalette().accent,
                synthetic.palette.accent),
        "a third theme should use the same descriptor-driven runtime path without an identity branch");

    specforge::ActivateTheme(
        *specforge::FindBuiltInThemeDescriptor(
            specforge::BuiltInDarkThemeId()));
}

}  // namespace

int main()
{
    TestBuiltInThemesHaveStableIdentities();
    TestWindowsPreferenceMapsToConcreteThemeIds();
    TestFollowSystemResolutionFallsBackToDark();
    TestExplicitSelectionHasPriority();
    TestSelectionStableValuesRoundTrip();
    TestResolverPreservesSyntheticFutureIdentity();
    TestResolvedDescriptorsOwnAllRenderingColors();
    TestImGuiThemeApplicationUsesOfficialColorsAndOpaqueViewportBackground();
    TestImPlotThemeApplicationUsesAutoBaseAndSemanticOverrides();
    TestSyntheticThemeUsesSharedRuntimeApplicationPath();
    return 0;
}
