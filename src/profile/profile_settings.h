#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace specforge {

struct ProfileSettings {
    std::optional<std::filesystem::path> output_directory;

    [[nodiscard]] bool operator==(const ProfileSettings&) const = default;
};

enum class ProfileOutputDirectorySource {
    Default,
    UserSetting,
    Environment,
};

struct ProfileOutputDirectoryResolution {
    std::filesystem::path directory;
    ProfileOutputDirectorySource source = ProfileOutputDirectorySource::Default;

    [[nodiscard]] bool operator==(
        const ProfileOutputDirectoryResolution&) const = default;
};

[[nodiscard]] std::filesystem::path DefaultProfileSettingsPath();
[[nodiscard]] ProfileSettings LoadProfileSettings(const std::filesystem::path& path);
[[nodiscard]] bool SaveProfileSettings(
    const std::filesystem::path& path,
    const ProfileSettings& settings,
    std::string* error_message = nullptr);
[[nodiscard]] std::optional<std::filesystem::path> ProfileOutputDirectoryEnvironmentOverride();
[[nodiscard]] ProfileOutputDirectoryResolution ResolveProfileOutputDirectory(
    const ProfileSettings& settings,
    const std::filesystem::path& default_directory,
    const std::optional<std::filesystem::path>& environment_override);

}  // namespace specforge
