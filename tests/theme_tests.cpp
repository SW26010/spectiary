#include "ui/theme.h"

#include <array>
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

}  // namespace

int main()
{
    TestBuiltInThemesHaveStableIdentities();
    TestWindowsPreferenceMapsToConcreteThemeIds();
    TestFollowSystemResolutionFallsBackToDark();
    TestExplicitSelectionHasPriority();
    TestResolverPreservesSyntheticFutureIdentity();
    return 0;
}
