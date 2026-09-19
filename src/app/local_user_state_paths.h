#pragma once

namespace spectiary::local_user_state_paths {

inline constexpr char kProfileLogDirectory[] = "logs";
inline constexpr char kFrameCaptureDirectory[] = "captures";
inline constexpr char kImGuiIni[] = "imgui-layout-v2.ini";
inline constexpr char kUiLanguageSettings[] = "ui-language.json";
inline constexpr char kAppearanceSettings[] =
    "appearance-settings.json";
inline constexpr char kUiScaleSettings[] = "ui-scale.json";
inline constexpr char kInputSettings[] = "input-settings.json";
inline constexpr char kExternalSourceSettings[] =
    "external-source-settings.json";
inline constexpr char kProfileSettings[] = "profile-settings.json";
inline constexpr char kPanelVisibilityState[] = "panel-visibility.json";
inline constexpr char kSpectrumPlotPreferences[] = "spectrum-plot-preferences.json";
inline constexpr char kSpectrumViewportState[] = "spectrum-viewport-state.json";
inline constexpr char kLegacySpectrumViewState[] = "spectrum-view-state.json";
inline constexpr char kSourceSessionState[] = "source-session.json";
inline constexpr char kSampleNavigationState[] =
    "sample-navigation-state.json";
inline constexpr char kSampleLabelingState[] =
    "sample-labeling-state.json";
inline constexpr char kSampleLabelingDrafts[] =
    "sample-labeling-drafts.json";
inline constexpr char kLegacySampleLabelingState[] =
    "sample-labeling-tasks.json";
inline constexpr char kSampleWorkflowState[] =
    "sample-workflow-state.json";
inline constexpr char kSpectralLineUserState[] =
    "spectral-line-grouping-views.json";

}  // namespace spectiary::local_user_state_paths
