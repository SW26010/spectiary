#include "ui/ui_language_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind = "specforge.ui_language.settings";
constexpr int kSettingsSchemaVersion = 1;
constexpr std::string_view kEnglishStableValue = "en";
constexpr std::string_view kSimplifiedChineseStableValue = "zh-Hans";

std::string_view StableLanguageValue(UiLanguage language)
{
    switch (language) {
    case UiLanguage::English:
        return kEnglishStableValue;
    case UiLanguage::SimplifiedChinese:
        return kSimplifiedChineseStableValue;
    case UiLanguage::Count:
        return {};
    }
    return {};
}

}  // namespace

std::filesystem::path DefaultUiLanguageSettingsPath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kUiLanguageSettings);
}

UiLanguageSettingsLoadResult LoadUiLanguageSettings(
    const std::filesystem::path& path)
{
    UiLanguageSettingsLoadResult settings;
    VersionedJsonCacheLoadResult cache = LoadVersionedJsonCacheFile(
        path,
        kSettingsFormatKind,
        {kSettingsSchemaVersion},
        "UI language settings");
    if (!cache.document) {
        settings.warning = std::move(cache.warning);
        return settings;
    }

    const std::optional<std::string> language =
        ReadJsonStringMember(cache.document->root, "language");
    if (!language) {
        settings.warning =
            "Ignored UI language settings: the language value is missing or invalid.";
        return settings;
    }
    if (*language == kEnglishStableValue) {
        settings.language = UiLanguage::English;
        return settings;
    }
    if (*language == kSimplifiedChineseStableValue) {
        settings.language = UiLanguage::SimplifiedChinese;
        return settings;
    }

    settings.warning =
        "Ignored UI language settings: the language value is not supported.";
    return settings;
}

bool SaveUiLanguageSettings(
    const std::filesystem::path& path,
    UiLanguage language,
    std::string* error_message)
{
    const std::string_view stable_value = StableLanguageValue(language);
    if (stable_value.empty()) {
        if (error_message != nullptr) {
            *error_message = "The application language is not supported.";
        }
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "UI language settings",
        JsonObjectValue({
            {"language", JsonStringValue(stable_value)},
        }),
        error_message);
}

}  // namespace specforge
