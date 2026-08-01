#pragma once

#include <filesystem>
#include <string>

namespace specforge {

inline constexpr bool kDefaultLiveNumericNavigation = true;

struct InputSettings {
    bool live_numeric_navigation =
        kDefaultLiveNumericNavigation;
};

struct InputSettingsLoadResult {
    InputSettings settings;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultInputSettingsPath();
[[nodiscard]] InputSettingsLoadResult LoadInputSettings(
    const std::filesystem::path& path);
[[nodiscard]] bool SaveInputSettings(
    const std::filesystem::path& path,
    const InputSettings& settings,
    std::string* error_message = nullptr);

}  // namespace specforge
