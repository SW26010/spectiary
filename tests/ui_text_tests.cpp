#include "ui/ui_text.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestEveryDisplayTextIsPresent()
{
    constexpr std::size_t kLanguageCount =
        static_cast<std::size_t>(specforge::UiLanguage::Count);
    constexpr std::size_t kTextCount =
        static_cast<std::size_t>(specforge::UiTextId::Count);

    for (std::size_t language_index = 0; language_index < kLanguageCount; ++language_index) {
        const auto language = static_cast<specforge::UiLanguage>(language_index);
        for (std::size_t index = 0; index < kTextCount; ++index) {
            const auto text_id = static_cast<specforge::UiTextId>(index);
            Require(!specforge::UiText(language, text_id).empty(), "display text should not be empty");
        }
    }
}

void TestRepresentativeMappingsAreExact()
{
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    Require(UiText(UiLanguage::English, UiTextId::Language) == "Language", "English language label");
    Require(UiText(UiLanguage::SimplifiedChinese, UiTextId::Language) == "语言", "Chinese language label");
    Require(
        UiText(UiLanguage::English, UiTextId::EnglishLanguageName) == "English",
        "English name in English");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::EnglishLanguageName) == "英语",
        "English name in Chinese");
    Require(
        UiText(UiLanguage::English, UiTextId::SimplifiedChineseLanguageName) ==
            "Simplified Chinese",
        "Chinese name in English");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::SimplifiedChineseLanguageName) ==
            "简体中文",
        "Chinese name in Chinese");
    Require(
        UiText(UiLanguage::English, UiTextId::LocalizationInProgress) ==
            "Localization is still in progress. Other parts of SpecForge currently remain in English.",
        "English scope notice should be exact");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::LocalizationInProgress) ==
            "本地化仍在逐步进行；SpecForge 的其它界面目前仍保持英文。",
        "Chinese scope notice should be exact");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::LanguageSaveError) ==
            "无法保存应用语言，仍继续使用此前的语言。",
        "Chinese save failure should be exact");
}

void TestInvalidLanguageFallsBackToEnglish()
{
    constexpr std::array kInvalidLanguages = {
        specforge::UiLanguage::Count,
        static_cast<specforge::UiLanguage>(-1),
    };
    constexpr std::size_t kTextCount =
        static_cast<std::size_t>(specforge::UiTextId::Count);

    for (const specforge::UiLanguage invalid_language : kInvalidLanguages) {
        for (std::size_t index = 0; index < kTextCount; ++index) {
            const auto text_id = static_cast<specforge::UiTextId>(index);
            Require(
                specforge::UiText(invalid_language, text_id) ==
                    specforge::UiText(specforge::UiLanguage::English, text_id),
                "invalid language should fall back to English");
        }
    }
}

void TestCountSentinelIsNotDisplayable()
{
    Require(
        specforge::UiText(specforge::UiLanguage::English, specforge::UiTextId::Count).empty(),
        "count sentinel should not be displayable");
    Require(
        specforge::UiText(
            specforge::UiLanguage::SimplifiedChinese,
            specforge::UiTextId::Count)
            .empty(),
        "count sentinel should not be displayable in Chinese");
}

}  // namespace

int main()
{
    try {
        TestEveryDisplayTextIsPresent();
        TestRepresentativeMappingsAreExact();
        TestInvalidLanguageFallsBackToEnglish();
        TestCountSentinelIsNotDisplayable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
