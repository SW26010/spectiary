#include "ui/ui_text.h"

#include <array>
#include <cstddef>

namespace specforge {
namespace {

constexpr std::size_t kUiLanguageCount = static_cast<std::size_t>(UiLanguage::Count);
constexpr std::size_t kUiTextCount = static_cast<std::size_t>(UiTextId::Count);

constexpr std::array kTextByLanguage = {
    std::array{
        std::string_view{"Settings"},
        std::string_view{"Language"},
        std::string_view{"Application language"},
        std::string_view{"English"},
        std::string_view{"Simplified Chinese"},
    },
    std::array{
        std::string_view{"设置"},
        std::string_view{"语言"},
        std::string_view{"应用语言"},
        std::string_view{"英语"},
        std::string_view{"简体中文"},
    },
};

static_assert(kTextByLanguage.size() == kUiLanguageCount);
static_assert(kTextByLanguage.front().size() == kUiTextCount);

}  // namespace

std::string_view UiText(UiLanguage language, UiTextId text_id) noexcept
{
    const std::size_t text_index = static_cast<std::size_t>(text_id);
    if (text_index >= kUiTextCount) {
        return {};
    }

    constexpr std::size_t kEnglishIndex = static_cast<std::size_t>(UiLanguage::English);
    const std::string_view english_text = kTextByLanguage[kEnglishIndex][text_index];

    const std::size_t language_index = static_cast<std::size_t>(language);
    if (language_index >= kUiLanguageCount) {
        return english_text;
    }

    const std::string_view translated = kTextByLanguage[language_index][text_index];
    return translated.empty() ? english_text : translated;
}

}  // namespace specforge
