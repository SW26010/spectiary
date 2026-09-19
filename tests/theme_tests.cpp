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
    const std::span<const spectiary::ThemeDescriptor> descriptors =
        spectiary::BuiltInThemeDescriptors();
    Require(
        descriptors.size() == 2,
        "the initial registry should expose the two built-in themes");
    Require(
        spectiary::BuiltInDarkThemeId().value() ==
                "builtin.theme.dark" &&
            spectiary::BuiltInLightThemeId().value() ==
                "builtin.theme.light",
        "built-in themes should expose stable non-boolean identities");
    Require(
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
                ->color_scheme ==
            spectiary::ThemeColorScheme::Dark &&
            spectiary::FindBuiltInThemeDescriptor(
                spectiary::BuiltInLightThemeId())
                    ->color_scheme ==
                spectiary::ThemeColorScheme::Light,
        "theme descriptors should carry color-scheme metadata separately from identity");
}

void TestWindowsPreferenceMapsToConcreteThemeIds()
{
    Require(
        spectiary::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::uint32_t{0}) ==
            spectiary::BuiltInDarkThemeId(),
        "Windows AppsUseLightTheme=0 should resolve to the dark theme ID");
    Require(
        spectiary::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::uint32_t{1}) ==
            spectiary::BuiltInLightThemeId(),
        "Windows AppsUseLightTheme=1 should resolve to the light theme ID");
    Require(
        !spectiary::WindowsSystemThemeIdFromAppsUseLightTheme(
            std::nullopt),
        "an unreadable Windows preference should remain unresolved at the platform boundary");
}

void TestFollowSystemResolutionFallsBackToDark()
{
    const spectiary::ThemeSelection follow_system =
        spectiary::ThemeSelection::FollowSystem();
    Require(
        spectiary::ResolveThemeId(
            follow_system,
            spectiary::BuiltInLightThemeId()) ==
            spectiary::BuiltInLightThemeId(),
        "follow-system should use the concrete system theme identity");
    Require(
        spectiary::ResolveThemeId(
            follow_system,
            spectiary::BuiltInDarkThemeId()) ==
            spectiary::BuiltInDarkThemeId(),
        "follow-system should resolve the dark Windows state without changing selection policy");
    Require(
        spectiary::ResolveThemeId(
            follow_system,
            std::nullopt) ==
            spectiary::BuiltInDarkThemeId(),
        "an unavailable system theme should safely resolve to dark");
}

void TestExplicitSelectionHasPriority()
{
    const spectiary::ThemeSelection explicit_light =
        spectiary::ThemeSelection::Explicit(
            spectiary::BuiltInLightThemeId());
    Require(
        spectiary::ResolveThemeId(
            explicit_light,
            spectiary::BuiltInDarkThemeId()) ==
            spectiary::BuiltInLightThemeId(),
        "an explicit theme identity should take priority over the system theme");
    Require(
        spectiary::ResolveThemeId(
            explicit_light,
            std::nullopt) ==
            spectiary::BuiltInLightThemeId(),
        "an explicit theme should remain unaffected when the system theme cannot be read");
}

void TestSelectionStableValuesRoundTrip()
{
    const std::array selections = {
        spectiary::ThemeSelection::FollowSystem(),
        spectiary::ThemeSelection::Explicit(
            spectiary::BuiltInDarkThemeId()),
        spectiary::ThemeSelection::Explicit(
            spectiary::BuiltInLightThemeId()),
    };
    for (const spectiary::ThemeSelection& selection :
         selections) {
        Require(
            spectiary::ParseThemeSelectionStableValue(
                spectiary::ThemeSelectionStableValue(
                    selection)) == selection,
            "theme selection automation values should round-trip through stable non-boolean identities");
    }
    Require(
        !spectiary::ParseThemeSelectionStableValue(
            "spectiary.theme.unknown"),
        "unknown theme automation values should be rejected at the registry boundary");
}

void TestResolverPreservesSyntheticFutureIdentity()
{
    const spectiary::ThemeId future_theme(
        std::string_view("spectiary.theme.synthetic"));
    const spectiary::ThemeSelection selection =
        spectiary::ThemeSelection::Explicit(future_theme);
    Require(
        spectiary::ResolveThemeId(
            selection,
            spectiary::BuiltInDarkThemeId()) ==
            future_theme,
        "the unified resolver should preserve a non-light/dark theme identity without a new branch");
    const std::array future_registry = {
        spectiary::ThemeDescriptor{
            .id = future_theme,
            .color_scheme =
                spectiary::ThemeColorScheme::Dark,
        },
    };
    Require(
        spectiary::FindThemeDescriptor(
            future_registry,
            spectiary::ResolveThemeId(
                selection,
                spectiary::BuiltInLightThemeId())) ==
            &future_registry.front(),
        "a synthetic theme should pass through resolution into the shared descriptor lookup path");
    Require(
        !spectiary::IsSupportedThemeSelection(selection),
        "the current settings registry should still reject unshipped themes");
}

void TestResolvedDescriptorsOwnAllRenderingColors()
{
    const spectiary::ThemeDescriptor& dark =
        spectiary::ResolveBuiltInThemeDescriptor(
            spectiary::ThemeSelection::Explicit(
                spectiary::BuiltInDarkThemeId()),
            spectiary::BuiltInLightThemeId());
    const spectiary::ThemeDescriptor& light =
        spectiary::ResolveBuiltInThemeDescriptor(
            spectiary::ThemeSelection::FollowSystem(),
            spectiary::BuiltInLightThemeId());
    Require(
        dark.id == spectiary::BuiltInDarkThemeId() &&
            light.id == spectiary::BuiltInLightThemeId(),
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
    for (const spectiary::ThemeDescriptor& theme :
         spectiary::BuiltInThemeDescriptors()) {
        ImGuiStyle actual;
        actual.WindowRounding = 7.25f;
        actual.FramePadding = ImVec2(13.0f, 17.0f);
        actual.ItemSpacing = ImVec2(19.0f, 23.0f);
        actual.FontScaleMain = 1.37f;
        ImGuiStyle official = actual;
        if (theme.color_scheme ==
            spectiary::ThemeColorScheme::Light) {
            ImGui::StyleColorsLight(&official);
        } else {
            ImGui::StyleColorsDark(&official);
        }
        official.Colors[ImGuiCol_WindowBg].w = 1.0f;

        spectiary::ApplyImGuiThemeColors(theme, actual);
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
    for (const spectiary::ThemeDescriptor& theme :
         spectiary::BuiltInThemeDescriptors()) {
        ImPlotStyle actual;
        actual.PlotBorderSize = 4.75f;
        actual.PlotPadding = ImVec2(11.0f, 13.0f);
        actual.Use24HourClock = true;
        ImPlotStyle automatic = actual;
        ImPlot::StyleColorsAuto(&automatic);

        spectiary::ApplyImPlotThemeColors(theme, actual);
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
    spectiary::ThemeDescriptor synthetic{
        .id = spectiary::ThemeId(
            "spectiary.theme.synthetic.light"),
        .color_scheme =
            spectiary::ThemeColorScheme::Light,
        .palette =
            spectiary::FindBuiltInThemeDescriptor(
                spectiary::BuiltInLightThemeId())
                ->palette,
        .clear_color = {0.91f, 0.92f, 0.93f, 1.0f},
    };
    synthetic.palette.accent =
        ImVec4(0.21f, 0.31f, 0.71f, 1.0f);

    ImGuiStyle actual;
    ImGuiStyle official = actual;
    ImGui::StyleColorsLight(&official);
    official.Colors[ImGuiCol_WindowBg].w = 1.0f;
    spectiary::ApplyImGuiThemeColors(
        synthetic,
        actual);
    spectiary::ActivateTheme(synthetic);
    Require(
        SameColor(
            actual.Colors[ImGuiCol_WindowBg],
            official.Colors[ImGuiCol_WindowBg]) &&
            SameColor(
                spectiary::ActiveSemanticPalette().accent,
                synthetic.palette.accent),
        "a third theme should use the same descriptor-driven runtime path without an identity branch");

    spectiary::ActivateTheme(
        *spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId()));
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
