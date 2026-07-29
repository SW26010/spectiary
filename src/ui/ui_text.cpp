#include "ui/ui_text.h"

#include "domain/sample_annotation_io.h"
#include "ui/source_collection_session_types.h"

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
        std::string_view{"Choose the language used by the Settings language page."},
        std::string_view{"Application language"},
        std::string_view{"English"},
        std::string_view{"Simplified Chinese"},
        std::string_view{
            "Localization is still in progress. Other parts of SpecForge currently remain in English."},
        std::string_view{
            "The saved application language could not be loaded. English is being used."},
        std::string_view{
            "The application language could not be saved. The previous language is still in use."},
        std::string_view{"Experimental frame capture"},
        std::string_view{
            "Captures the requested frame from the main application viewport only. Detached viewport windows are excluded."},
        std::string_view{"Capture Next Main Frame"},
        std::string_view{"Waiting for the next frame..."},
        std::string_view{"PNG via synchronous GPU readback."},
        std::string_view{"Output directory"},
        std::string_view{"unknown"},
        std::string_view{"none"},
        std::string_view{"error"},
        std::string_view{"loaded"},
        std::string_view{"loaded with diagnostics"},
        std::string_view{"not plottable"},
        std::string_view{"plain"},
        std::string_view{"external"},
        std::string_view{"local"},
    },
    std::array{
        std::string_view{"设置"},
        std::string_view{"语言"},
        std::string_view{"选择“设置”中语言页面使用的语言。"},
        std::string_view{"应用语言"},
        std::string_view{"英语"},
        std::string_view{"简体中文"},
        std::string_view{"本地化仍在逐步进行；SpecForge 的其它界面目前仍保持英文。"},
        std::string_view{"无法加载已保存的应用语言，当前使用英语。"},
        std::string_view{"无法保存应用语言，仍继续使用此前的语言。"},
        std::string_view{"实验性画面捕获"},
        std::string_view{"仅从主应用视口捕获所请求的画面；不包含分离的视口窗口。"},
        std::string_view{"捕获下一主画面帧"},
        std::string_view{"正在等待下一帧……"},
        std::string_view{"通过同步 GPU 回读写入 PNG。"},
        std::string_view{"输出目录"},
        std::string_view{"未知"},
        std::string_view{"无"},
        std::string_view{"错误"},
        std::string_view{"已加载"},
        std::string_view{"已加载（含诊断）"},
        std::string_view{"无法绘图"},
        std::string_view{"普通"},
        std::string_view{"外部"},
        std::string_view{"本地"},
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

std::string_view UiText(
    UiLanguage language,
    SourceCollectionSourceState state) noexcept
{
    switch (state) {
    case SourceCollectionSourceState::Unavailable:
        return UiText(language, UiTextId::SourceStateUnavailable);
    case SourceCollectionSourceState::Error:
        return UiText(language, UiTextId::SourceStateError);
    case SourceCollectionSourceState::Loaded:
        return UiText(language, UiTextId::SourceStateLoaded);
    case SourceCollectionSourceState::LoadedWithDiagnostics:
        return UiText(
            language,
            UiTextId::SourceStateLoadedWithDiagnostics);
    case SourceCollectionSourceState::NotPlottable:
        return UiText(language, UiTextId::SourceStateNotPlottable);
    }
    return UiText(language, UiTextId::SourceStateUnavailable);
}

std::string_view UiText(
    UiLanguage language,
    SampleAnnotationWorkflowRelationship relationship) noexcept
{
    switch (relationship) {
    case SampleAnnotationWorkflowRelationship::PlainAnnotation:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipPlain);
    case SampleAnnotationWorkflowRelationship::ExternalLabelResult:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipExternal);
    case SampleAnnotationWorkflowRelationship::LocalLabelingTask:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipLocal);
    }
    return UiText(language, UiTextId::AnnotationRelationshipPlain);
}

}  // namespace specforge
