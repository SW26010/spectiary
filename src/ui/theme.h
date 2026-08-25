#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace specforge {

inline constexpr std::string_view kBuiltInDarkThemeStableId =
    "specforge.theme.dark";
inline constexpr std::string_view kBuiltInLightThemeStableId =
    "specforge.theme.light";

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

struct ThemeDescriptor {
    ThemeId id;
    ThemeColorScheme color_scheme = ThemeColorScheme::Dark;
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
