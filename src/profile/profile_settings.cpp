#include "profile/profile_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"

#include <cstdlib>
#include <memory>
#include <ostream>
#include <string>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind = "specforge.profile_settings";
constexpr int kSettingsSchemaVersion = 1;

}  // namespace

std::filesystem::path DefaultProfileSettingsPath()
{
    return DefaultLocalUserStatePath("profile-settings.json");
}

ProfileSettings LoadProfileSettings(const std::filesystem::path& path)
{
    ProfileSettings settings;
    const VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kSettingsFormatKind,
            {kSettingsSchemaVersion},
            "performance profile settings");
    if (!result.document) {
        return settings;
    }

    const JsonValue* output_directory =
        JsonObjectMember(result.document->root, "output_directory");
    if (output_directory != nullptr && output_directory->kind != JsonValue::Kind::Null) {
        settings.output_directory = ReadPersistedPathReference(*output_directory);
    }
    return settings;
}

bool SaveProfileSettings(
    const std::filesystem::path& path,
    const ProfileSettings& settings,
    std::string* error_message)
{
    return WriteVersionedJsonCacheFile(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "performance profile settings",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"output_directory\": ";
            if (settings.output_directory) {
                WritePersistedPathReference(stream, *settings.output_directory);
            } else {
                stream << "null";
            }
            stream << "\n";
            return true;
        },
        error_message);
}

std::optional<std::filesystem::path> ProfileOutputDirectoryEnvironmentOverride()
{
    wchar_t* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_wdupenv_s(&raw_value, &value_size, L"SPECFORGE_PROFILE_DIR") != 0 ||
        raw_value == nullptr) {
        return std::nullopt;
    }

    std::unique_ptr<wchar_t, decltype(&std::free)> value(raw_value, std::free);
    if (value.get()[0] == L'\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value.get());
}

ProfileOutputDirectoryResolution ResolveProfileOutputDirectory(
    const ProfileSettings& settings,
    const std::filesystem::path& default_directory,
    const std::optional<std::filesystem::path>& environment_override)
{
    if (environment_override && !environment_override->empty()) {
        return {*environment_override, ProfileOutputDirectorySource::Environment};
    }
    if (settings.output_directory && !settings.output_directory->empty()) {
        return {*settings.output_directory, ProfileOutputDirectorySource::UserSetting};
    }
    return {default_directory, ProfileOutputDirectorySource::Default};
}

}  // namespace specforge
