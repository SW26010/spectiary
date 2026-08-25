#include "ui/theme.h"

#include <array>
#include <utility>

namespace specforge {

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
        },
        ThemeDescriptor{
            .id = BuiltInLightThemeId(),
            .color_scheme = ThemeColorScheme::Light,
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
