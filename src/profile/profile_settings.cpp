#include "profile/profile_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <cstdlib>
#include <memory>
#include <string>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind = "specforge.profile_settings";
constexpr int kSettingsSchemaVersion = 1;

}  // namespace

std::filesystem::path DefaultProfileSettingsPath(const RuntimePaths& runtime_paths)
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kProfileSettings, runtime_paths);
}

ProfileSettingsLoadResult LoadProfileSettings(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path)
{
    ProfileSettingsLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kSettingsFormatKind,
            {kSettingsSchemaVersion},
            "performance profile settings");
    loaded.warning = std::move(result.warning);
    if (!result.document) {
        return loaded;
    }

    const nlohmann::json* output_directory =
        JsonObjectMember(result.document->root, "output_directory");
    if (output_directory != nullptr && output_directory->type() != nlohmann::json::value_t::null) {
        std::optional<std::filesystem::path> parsed =
            ReadPersistedPathReference(*output_directory, runtime_paths);
        if (!parsed) {
            loaded.warning =
                "Performance profile settings member "
                "'output_directory' is invalid and was "
                "ignored.";
            return loaded;
        }
        loaded.settings.output_directory = std::move(parsed);
    }
    return loaded;
}

bool SaveProfileSettings(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const ProfileSettings& settings,
    std::string* error_message)
{
    const nlohmann::json output_directory =
        settings.output_directory
        ? PersistedPathReferenceJson(
              *settings.output_directory, runtime_paths)
        : nlohmann::json();
    return WriteVersionedJsonCacheDocument(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "performance profile settings",
        nlohmann::json::object({
            {"output_directory", output_directory},
        }),
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
