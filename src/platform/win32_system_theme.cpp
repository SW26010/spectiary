#include "platform/win32_system_theme.h"

#include <Windows.h>

#include <cstdint>

namespace spectiary {
namespace {

constexpr const wchar_t* kPersonalizeRegistryKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kAppsUseLightThemeRegistryValue =
    L"AppsUseLightTheme";

}  // namespace

std::optional<ThemeId> ReadWindowsSystemTheme()
{
    DWORD apps_use_light_theme = 0;
    DWORD value_size = sizeof(apps_use_light_theme);
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER,
        kPersonalizeRegistryKey,
        kAppsUseLightThemeRegistryValue,
        RRF_RT_REG_DWORD,
        nullptr,
        &apps_use_light_theme,
        &value_size);
    if (result != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return WindowsSystemThemeIdFromAppsUseLightTheme(
        static_cast<std::uint32_t>(apps_use_light_theme));
}

}  // namespace spectiary
