#pragma once

#include <string_view>

namespace specforge {

enum class UiLanguage {
    English,
    SimplifiedChinese,
    Count,  // Non-language sentinel.
};

enum class UiTextId {
    Settings,
    Language,
    LanguagePageDescription,
    ApplicationLanguage,
    EnglishLanguageName,
    SimplifiedChineseLanguageName,
    LocalizationInProgress,
    LanguageLoadWarning,
    LanguageSaveError,
    Count,  // Non-display sentinel.
};

[[nodiscard]] std::string_view UiText(UiLanguage language, UiTextId text_id) noexcept;

}  // namespace specforge
