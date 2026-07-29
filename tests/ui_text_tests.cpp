#include "domain/sample_annotation_io.h"
#include "ui/source_collection_session_types.h"
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
    Require(
        UiText(
            UiLanguage::English,
            UiTextId::CaptureNextMainFrame) ==
            "Capture Next Main Frame",
        "English frame capture action should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::FrameCaptureOutputDirectory) ==
            "输出目录",
        "Chinese frame capture output label should be exact");
}

void TestAppearanceMappingsAreExact()
{
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    struct ExpectedText {
        UiTextId text_id;
        std::string_view english;
        std::string_view simplified_chinese;
    };
    constexpr std::array kExpectedTexts = {
        ExpectedText{
            UiTextId::Appearance,
            "Appearance",
            "外观"},
        ExpectedText{
            UiTextId::AppearancePageDescription,
            "Adjust the application theme without changing scientific plot semantics.",
            "调整应用主题，不改变科学绘图语义。"},
        ExpectedText{
            UiTextId::Theme,
            "Theme",
            "主题"},
        ExpectedText{
            UiTextId::FollowSystemTheme,
            "Follow system",
            "跟随系统"},
        ExpectedText{
            UiTextId::LightTheme,
            "Light",
            "浅色"},
        ExpectedText{
            UiTextId::DarkTheme,
            "Dark",
            "深色"},
        ExpectedText{
            UiTextId::AccentColor,
            "Accent color",
            "强调色"},
        ExpectedText{
            UiTextId::AppearanceThemeUnavailable,
            "Not available yet. The current UI uses the built-in dark style.",
            "暂不可用。当前界面使用内置深色样式。"},
        ExpectedText{
            UiTextId::UiScale,
            "UI scale",
            "界面缩放"},
        ExpectedText{
            UiTextId::Reset,
            "Reset",
            "重置"},
        ExpectedText{
            UiTextId::UiScaleDescription,
            "100% follows Windows display scaling. This setting adds an application-specific multiplier.",
            "100% 跟随 Windows 显示缩放；此设置用于调整应用自身的缩放倍率。"},
        ExpectedText{
            UiTextId::UiScaleLoadWarning,
            "The saved UI scale could not be loaded; using 100%.",
            "无法加载已保存的界面缩放比例，当前使用 100%。"},
        ExpectedText{
            UiTextId::UiScaleRejected,
            "The requested UI scale is not supported.",
            "请求的界面缩放比例不受支持。"},
        ExpectedText{
            UiTextId::UiScaleSaveError,
            "The UI scale could not be saved.",
            "无法保存界面缩放比例。"},
    };

    for (const ExpectedText& expected : kExpectedTexts) {
        Require(
            UiText(UiLanguage::English, expected.text_id) ==
                expected.english,
            "English Appearance text should be exact");
        Require(
            UiText(
                UiLanguage::SimplifiedChinese,
                expected.text_id) ==
                expected.simplified_chinese,
            "Chinese Appearance text should be exact");
    }
}

void TestSessionSemanticsAreLocalizedAtTheUiBoundary()
{
    using specforge::SampleAnnotationWorkflowRelationship;
    using specforge::SourceCollectionSourceState;
    using specforge::UiLanguage;
    using specforge::UiText;

    Require(
        UiText(
            UiLanguage::English,
            SourceCollectionSourceState::LoadedWithDiagnostics) ==
            "loaded with diagnostics",
        "English source state should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SourceCollectionSourceState::LoadedWithDiagnostics) ==
            "已加载（含诊断）",
        "Chinese source state should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SampleAnnotationWorkflowRelationship::
                ExternalLabelResult) == "外部",
        "Chinese annotation relationship should be exact");
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
        TestAppearanceMappingsAreExact();
        TestSessionSemanticsAreLocalizedAtTheUiBoundary();
        TestInvalidLanguageFallsBackToEnglish();
        TestCountSentinelIsNotDisplayable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
