#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ImPlotStyle;

namespace specforge {

struct ThemeSelection;

inline constexpr std::string_view kBuiltInDarkThemeStableId =
    "specforge.theme.dark";
inline constexpr std::string_view kBuiltInLightThemeStableId =
    "specforge.theme.light";
inline constexpr std::string_view kFollowSystemThemeStableValue =
    "follow-system";

class ThemeId {
public:
    ThemeId() = default;
    explicit ThemeId(const char* stable_id);
    explicit ThemeId(std::string stable_id);
    explicit ThemeId(std::string_view stable_id);

    [[nodiscard]] std::string_view value() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] bool operator==(const ThemeId&) const = default;

private:
    std::string stable_id_;
};

enum class ThemeColorScheme {
    Dark,
    Light,
};

// Application-owned colors are named by meaning so feature code never needs
// to infer whether the active theme is light or dark. ImGui and ImPlot keep
// ownership of their standard widget colors; this palette covers colors used
// by SpecForge rendering and status affordances.
struct SemanticPalette {
    ImVec4 background{};
    ImVec4 surface{};
    ImVec4 text{};
    ImVec4 muted{};
    ImVec4 accent{};
    ImVec4 selection{};
    ImVec4 warning{};
    ImVec4 error{};
    ImVec4 success{};
    ImVec4 plot_line{};
    ImVec4 plot_grid{};
    ImVec4 plot_axis{};
    ImVec4 annotation{};
    ImVec4 overlay_background{};
    ImVec4 overlay_text{};
    ImVec4 plot_crosshair{};
    ImVec4 spectral_balmer{};
    ImVec4 spectral_molecule{};
    ImVec4 spectral_heavy_element{};
    ImVec4 spectral_default{};
    ImVec4 smoothing_gaussian{};
    ImVec4 smoothing_median{};
};

struct ThemeDescriptor {
    ThemeId id;
    ThemeColorScheme color_scheme = ThemeColorScheme::Dark;
    SemanticPalette palette;
    std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 1.0f};
};

[[nodiscard]] ThemeId BuiltInDarkThemeId();
[[nodiscard]] ThemeId BuiltInLightThemeId();
[[nodiscard]] std::span<const ThemeDescriptor>
BuiltInThemeDescriptors() noexcept;
[[nodiscard]] const ThemeDescriptor* FindThemeDescriptor(
    std::span<const ThemeDescriptor> descriptors,
    const ThemeId& id) noexcept;
[[nodiscard]] const ThemeDescriptor* FindBuiltInThemeDescriptor(
    const ThemeId& id) noexcept;
[[nodiscard]] const ThemeDescriptor& ResolveBuiltInThemeDescriptor(
    const ThemeSelection& selection,
    std::optional<ThemeId> system_theme);

// These functions intentionally update colors only. Runtime theme switches
// must preserve docking, sizing, rounding, padding, DPI scaling, and every
// other non-color style setting.
void ApplyImGuiThemeColors(
    const ThemeDescriptor& theme,
    ImGuiStyle& style);
void ApplyImPlotThemeColors(
    const ThemeDescriptor& theme,
    ImPlotStyle& style);

// Rendering happens on the UI thread. Activating a descriptor makes its
// semantic palette available to feature renderers without spreading theme
// identity checks throughout the codebase.
void ActivateTheme(const ThemeDescriptor& theme);
[[nodiscard]] const SemanticPalette& ActiveSemanticPalette() noexcept;

enum class ThemeSelectionPolicy {
    FollowSystem,
    Explicit,
};

struct ThemeSelection {
    ThemeSelectionPolicy policy =
        ThemeSelectionPolicy::FollowSystem;
    ThemeId explicit_theme_id;

    [[nodiscard]] static ThemeSelection FollowSystem();
    [[nodiscard]] static ThemeSelection Explicit(ThemeId theme_id);

    [[nodiscard]] bool operator==(const ThemeSelection&) const = default;
};

[[nodiscard]] bool IsSupportedThemeSelection(
    const ThemeSelection& selection) noexcept;
[[nodiscard]] std::string_view ThemeSelectionStableValue(
    const ThemeSelection& selection) noexcept;
[[nodiscard]] std::optional<ThemeSelection>
ParseThemeSelectionStableValue(std::string_view value);

// Resolving a selection deliberately does not require a built-in descriptor.
// Theme consumers can therefore use the same path for future or test-only
// descriptors instead of growing light/dark branches.
[[nodiscard]] ThemeId ResolveThemeId(
    const ThemeSelection& selection,
    std::optional<ThemeId> system_theme);

// Windows represents its application color preference as a DWORD. A missing
// value means the preference could not be read and remains unresolved here.
[[nodiscard]] std::optional<ThemeId>
WindowsSystemThemeIdFromAppsUseLightTheme(
    std::optional<std::uint32_t> apps_use_light_theme);

}  // namespace specforge
