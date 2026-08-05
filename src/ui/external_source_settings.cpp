#include "ui/external_source_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <utility>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind =
    "specforge.external_source.settings";
constexpr int kSettingsSchemaVersion = 1;
constexpr const char* kOpenExternalSourceAsFolderMember =
    "open_external_source_as_folder";
constexpr const char* kLegacyOpenExternalFitsAsFolderMember =
    "open_external_fits_as_folder";

}  // namespace

std::filesystem::path DefaultExternalSourceSettingsPath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kExternalSourceSettings);
}

ExternalSourceSettingsLoadResult LoadExternalSourceSettings(
    const std::filesystem::path& path)
{
    ExternalSourceSettingsLoadResult loaded;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kSettingsFormatKind,
            {kSettingsSchemaVersion},
            "external source settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const JsonValue* open_external_source_as_folder =
        JsonObjectMember(
            cache.document->root,
            kOpenExternalSourceAsFolderMember);
    const char* setting_member =
        kOpenExternalSourceAsFolderMember;
    if (open_external_source_as_folder == nullptr) {
        open_external_source_as_folder = JsonObjectMember(
            cache.document->root,
            kLegacyOpenExternalFitsAsFolderMember);
        setting_member = kLegacyOpenExternalFitsAsFolderMember;
    }
    if (open_external_source_as_folder == nullptr ||
        open_external_source_as_folder->kind !=
            JsonValue::Kind::Bool) {
        loaded.warning =
            "Ignored external source settings: "
            + std::string(setting_member) +
            " must be boolean.";
        return loaded;
    }

    loaded.settings.open_external_source_as_folder =
        open_external_source_as_folder->bool_value;
    return loaded;
}

bool SaveExternalSourceSettings(
    const std::filesystem::path& path,
    const ExternalSourceSettings& settings,
    std::string* error_message)
{
    return WriteVersionedJsonCacheDocument(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "external source settings",
        JsonObjectValue({
            {kOpenExternalSourceAsFolderMember,
             JsonBoolValue(
                 settings.open_external_source_as_folder)},
        }),
        error_message);
}

}  // namespace specforge
