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
    ApplicationLanguage,
    EnglishLanguageName,
    SimplifiedChineseLanguageName,
    Count,  // Non-display sentinel.
};

[[nodiscard]] std::string_view UiText(UiLanguage language, UiTextId text_id) noexcept;

}  // namespace specforge
