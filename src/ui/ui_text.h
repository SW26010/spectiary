#pragma once

#include <string_view>

namespace specforge {

enum class SampleAnnotationWorkflowRelationship;
enum class SourceCollectionSourceState;

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
    ExperimentalFrameCapture,
    ExperimentalFrameCaptureDescription,
    CaptureNextMainFrame,
    FrameCaptureWaiting,
    FrameCaptureReadbackNote,
    FrameCaptureOutputDirectory,
    UnknownSourceType,
    SourceStateUnavailable,
    SourceStateError,
    SourceStateLoaded,
    SourceStateLoadedWithDiagnostics,
    SourceStateNotPlottable,
    AnnotationRelationshipPlain,
    AnnotationRelationshipExternal,
    AnnotationRelationshipLocal,
    Count,  // Non-display sentinel.
};

[[nodiscard]] std::string_view UiText(UiLanguage language, UiTextId text_id) noexcept;
[[nodiscard]] std::string_view UiText(
    UiLanguage language,
    SourceCollectionSourceState state) noexcept;
[[nodiscard]] std::string_view UiText(
    UiLanguage language,
    SampleAnnotationWorkflowRelationship relationship) noexcept;

}  // namespace specforge
