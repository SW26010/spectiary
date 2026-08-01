#include "ui/input_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <utility>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind =
    "specforge.input.settings";
constexpr int kSettingsSchemaVersion = 1;
constexpr const char* kLiveNumericNavigationMember =
    "live_numeric_navigation";

}  // namespace

std::filesystem::path DefaultInputSettingsPath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kInputSettings);
}

InputSettingsLoadResult LoadInputSettings(
    const std::filesystem::path& path)
{
    InputSettingsLoadResult loaded;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kSettingsFormatKind,
            {kSettingsSchemaVersion},
            "input settings");
    if (!cache.document) {
        loaded.warning = std::move(cache.warning);
        return loaded;
    }

    const JsonValue* live_numeric_navigation =
        JsonObjectMember(
            cache.document->root,
            kLiveNumericNavigationMember);
    if (live_numeric_navigation == nullptr ||
        live_numeric_navigation->kind !=
            JsonValue::Kind::Bool) {
        loaded.warning =
            "Ignored input settings: "
            "live_numeric_navigation must be boolean.";
        return loaded;
    }

    loaded.settings.live_numeric_navigation =
        live_numeric_navigation->bool_value;
    return loaded;
}

bool SaveInputSettings(
    const std::filesystem::path& path,
    const InputSettings& settings,
    std::string* error_message)
{
    return WriteVersionedJsonCacheDocument(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "input settings",
        JsonObjectValue({
            {kLiveNumericNavigationMember,
             JsonBoolValue(
                 settings.live_numeric_navigation)},
        }),
        error_message);
}

}  // namespace specforge
