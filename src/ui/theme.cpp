#include "ui/theme.h"

#include <implot.h>

#include <array>
#include <utility>

namespace specforge {
namespace {

SemanticPalette DarkSemanticPalette()
{
    return {
        .background = ImVec4(0.08f, 0.09f, 0.10f, 1.0f),
        .surface = ImVec4(0.12f, 0.13f, 0.15f, 1.0f),
        .text = ImVec4(0.95f, 0.96f, 0.97f, 1.0f),
        .muted = ImVec4(0.62f, 0.70f, 0.78f, 1.0f),
        .accent = ImVec4(0.34f, 0.63f, 0.86f, 1.0f),
        .selection = ImVec4(0.30f, 0.62f, 0.92f, 1.0f),
        .warning = ImVec4(0.95f, 0.74f, 0.30f, 1.0f),
        .error = ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
        .success = ImVec4(0.30f, 0.69f, 0.31f, 1.0f),
        .plot_auto_series = {
            ImVec4(0.34f, 0.63f, 0.86f, 1.0f),
            ImVec4(1.00f, 0.58f, 0.25f, 1.0f),
            ImVec4(0.35f, 0.75f, 0.47f, 1.0f),
            ImVec4(0.85f, 0.45f, 0.78f, 1.0f),
            ImVec4(0.28f, 0.76f, 0.78f, 1.0f),
            ImVec4(0.90f, 0.75f, 0.30f, 1.0f),
            ImVec4(0.58f, 0.52f, 0.92f, 1.0f),
            ImVec4(0.95f, 0.42f, 0.35f, 1.0f),
        },
        .plot_grid = ImVec4(0.62f, 0.70f, 0.78f, 0.25f),
        .plot_axis = ImVec4(0.78f, 0.84f, 0.88f, 0.74f),
        .annotation = ImVec4(0.66f, 0.72f, 0.82f, 0.72f),
        .overlay_background = ImVec4(0.086f, 0.086f, 0.094f, 0.86f),
        .overlay_text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
        .plot_crosshair = ImVec4(0.72f, 0.78f, 0.82f, 0.48f),
        .spectral_balmer = ImVec4(0.95f, 0.42f, 0.35f, 0.78f),
        .spectral_molecule = ImVec4(0.43f, 0.78f, 0.64f, 0.76f),
        .spectral_heavy_element = ImVec4(0.95f, 0.72f, 0.32f, 0.78f),
        .spectral_default = ImVec4(0.66f, 0.72f, 0.82f, 0.72f),
    };
}

SemanticPalette LightSemanticPalette()
{
    return {
        .background = ImVec4(0.94f, 0.94f, 0.94f, 1.0f),
        .surface = ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
        .text = ImVec4(0.10f, 0.11f, 0.12f, 1.0f),
        .muted = ImVec4(0.34f, 0.39f, 0.45f, 1.0f),
        .accent = ImVec4(0.05f, 0.38f, 0.68f, 1.0f),
        .selection = ImVec4(0.08f, 0.43f, 0.78f, 1.0f),
        .warning = ImVec4(0.70f, 0.42f, 0.02f, 1.0f),
        .error = ImVec4(0.78f, 0.16f, 0.13f, 1.0f),
        .success = ImVec4(0.10f, 0.50f, 0.25f, 1.0f),
        .plot_auto_series = {
            ImVec4(0.05f, 0.38f, 0.68f, 1.0f),
            ImVec4(0.78f, 0.31f, 0.02f, 1.0f),
            ImVec4(0.08f, 0.50f, 0.24f, 1.0f),
            ImVec4(0.66f, 0.18f, 0.55f, 1.0f),
            ImVec4(0.00f, 0.48f, 0.53f, 1.0f),
            ImVec4(0.62f, 0.43f, 0.00f, 1.0f),
            ImVec4(0.38f, 0.28f, 0.72f, 1.0f),
            ImVec4(0.72f, 0.12f, 0.10f, 1.0f),
        },
        .plot_grid = ImVec4(0.22f, 0.28f, 0.35f, 0.25f),
        .plot_axis = ImVec4(0.18f, 0.22f, 0.27f, 0.78f),
        .annotation = ImVec4(0.30f, 0.36f, 0.44f, 0.78f),
        .overlay_background = ImVec4(0.97f, 0.97f, 0.98f, 0.92f),
        .overlay_text = ImVec4(0.08f, 0.09f, 0.10f, 1.0f),
        .plot_crosshair = ImVec4(0.20f, 0.27f, 0.34f, 0.55f),
        .spectral_balmer = ImVec4(0.72f, 0.12f, 0.10f, 0.82f),
        .spectral_molecule = ImVec4(0.08f, 0.48f, 0.30f, 0.82f),
        .spectral_heavy_element = ImVec4(0.68f, 0.38f, 0.02f, 0.82f),
        .spectral_default = ImVec4(0.28f, 0.34f, 0.42f, 0.78f),
    };
}

SemanticPalette& ActivePaletteStorage()
{
    static SemanticPalette palette = DarkSemanticPalette();
    return palette;
}

}  // namespace

ThemeId::ThemeId(const char* stable_id)
    : stable_id_(stable_id != nullptr ? stable_id : "")
{
}

ThemeId::ThemeId(std::string stable_id)
    : stable_id_(std::move(stable_id))
{
}

ThemeId::ThemeId(std::string_view stable_id)
    : stable_id_(stable_id)
{
}

std::string_view ThemeId::value() const noexcept
{
    return stable_id_;
}

bool ThemeId::empty() const noexcept
{
    return stable_id_.empty();
}

ThemeId BuiltInDarkThemeId()
{
    return ThemeId(kBuiltInDarkThemeStableId);
}

ThemeId BuiltInLightThemeId()
{
    return ThemeId(kBuiltInLightThemeStableId);
}

std::span<const ThemeDescriptor>
BuiltInThemeDescriptors() noexcept
{
    static const std::array descriptors = {
        ThemeDescriptor{
            .id = BuiltInDarkThemeId(),
            .color_scheme = ThemeColorScheme::Dark,
            .palette = DarkSemanticPalette(),
            .clear_color = {0.08f, 0.09f, 0.10f, 1.0f},
        },
        ThemeDescriptor{
            .id = BuiltInLightThemeId(),
            .color_scheme = ThemeColorScheme::Light,
            .palette = LightSemanticPalette(),
            .clear_color = {0.94f, 0.94f, 0.94f, 1.0f},
        },
    };
    return descriptors;
}

const ThemeDescriptor* FindThemeDescriptor(
    std::span<const ThemeDescriptor> descriptors,
    const ThemeId& id) noexcept
{
    for (const ThemeDescriptor& descriptor : descriptors) {
        if (descriptor.id == id) {
            return &descriptor;
        }
    }
    return nullptr;
}

const ThemeDescriptor* FindBuiltInThemeDescriptor(
    const ThemeId& id) noexcept
{
    return FindThemeDescriptor(
        BuiltInThemeDescriptors(),
        id);
}

const ThemeDescriptor& ResolveBuiltInThemeDescriptor(
    const ThemeSelection& selection,
    std::optional<ThemeId> system_theme)
{
    const ThemeId resolved = ResolveThemeId(
        selection,
        std::move(system_theme));
    if (const ThemeDescriptor* descriptor =
            FindBuiltInThemeDescriptor(resolved)) {
        return *descriptor;
    }
    return *FindBuiltInThemeDescriptor(BuiltInDarkThemeId());
}

void ApplyImGuiThemeColors(
    const ThemeDescriptor& theme,
    ImGuiStyle& style)
{
    if (theme.color_scheme == ThemeColorScheme::Light) {
        ImGui::StyleColorsLight(&style);
    } else {
        ImGui::StyleColorsDark(&style);
    }

    // SpecForge always renders with platform viewports enabled. Keep their
    // background opaque after every official color-table reset so startup
    // and runtime theme switches produce the same compositor result.
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
}

void ApplyImPlotThemeColors(
    const ThemeDescriptor& theme,
    ImPlotStyle& style)
{
    ImPlot::StyleColorsAuto(&style);
    style.Colors[ImPlotCol_AxisText] =
        theme.palette.plot_axis;
    style.Colors[ImPlotCol_AxisGrid] =
        theme.palette.plot_grid;
    style.Colors[ImPlotCol_AxisTick] =
        theme.palette.plot_grid;
    style.Colors[ImPlotCol_Selection] =
        theme.palette.selection;
    style.Colors[ImPlotCol_Crosshairs] =
        theme.palette.plot_crosshair;
    style.Colors[ImPlotCol_InlayText] =
        theme.palette.annotation;
}

void ActivateTheme(const ThemeDescriptor& theme)
{
    ActivePaletteStorage() = theme.palette;
}

const SemanticPalette& ActiveSemanticPalette() noexcept
{
    return ActivePaletteStorage();
}

ThemeSelection ThemeSelection::FollowSystem()
{
    return {};
}

ThemeSelection ThemeSelection::Explicit(ThemeId theme_id)
{
    return {
        .policy = ThemeSelectionPolicy::Explicit,
        .explicit_theme_id = std::move(theme_id),
    };
}

bool IsSupportedThemeSelection(
    const ThemeSelection& selection) noexcept
{
    switch (selection.policy) {
    case ThemeSelectionPolicy::FollowSystem:
        return selection.explicit_theme_id.empty();
    case ThemeSelectionPolicy::Explicit:
        return !selection.explicit_theme_id.empty() &&
               FindBuiltInThemeDescriptor(
                   selection.explicit_theme_id) != nullptr;
    }
    return false;
}

std::string_view ThemeSelectionStableValue(
    const ThemeSelection& selection) noexcept
{
    if (selection.policy ==
        ThemeSelectionPolicy::FollowSystem) {
        return kFollowSystemThemeStableValue;
    }
    return selection.explicit_theme_id.value();
}

std::optional<ThemeSelection>
ParseThemeSelectionStableValue(std::string_view value)
{
    if (value == kFollowSystemThemeStableValue) {
        return ThemeSelection::FollowSystem();
    }
    const ThemeId id(value);
    if (FindBuiltInThemeDescriptor(id) == nullptr) {
        return std::nullopt;
    }
    return ThemeSelection::Explicit(id);
}

ThemeId ResolveThemeId(
    const ThemeSelection& selection,
    std::optional<ThemeId> system_theme)
{
    if (selection.policy == ThemeSelectionPolicy::Explicit &&
        !selection.explicit_theme_id.empty()) {
        return selection.explicit_theme_id;
    }
    if (system_theme && !system_theme->empty()) {
        return std::move(*system_theme);
    }
    return BuiltInDarkThemeId();
}

std::optional<ThemeId>
WindowsSystemThemeIdFromAppsUseLightTheme(
    std::optional<std::uint32_t> apps_use_light_theme)
{
    if (!apps_use_light_theme) {
        return std::nullopt;
    }
    return *apps_use_light_theme == 0
        ? BuiltInDarkThemeId()
        : BuiltInLightThemeId();
}

}  // namespace specforge
