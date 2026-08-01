#pragma once

namespace specforge::local_user_state_paths {

inline constexpr char kProfileLogDirectory[] = "logs";
inline constexpr char kFrameCaptureDirectory[] = "captures";
inline constexpr char kImGuiIni[] = "specforge-imgui-v2.ini";
inline constexpr char kUiLanguageSettings[] = "ui-language.json";
inline constexpr char kUiScaleSettings[] = "ui-scale.json";
inline constexpr char kInputSettings[] = "input-settings.json";
inline constexpr char kProfileSettings[] = "profile-settings.json";
inline constexpr char kPanelVisibilityState[] = "panel-visibility.json";
inline constexpr char kSourceSessionState[] = "source-session.json";
inline constexpr char kSampleNavigationState[] =
    "sample-navigation-state.json";
inline constexpr char kSampleLabelingState[] =
    "sample-labeling-tasks.json";
inline constexpr char kSampleWorkflowState[] =
    "sample-workflow-state.json";
inline constexpr char kSpectralLineUserState[] =
    "spectral-line-grouping-views.json";

}  // namespace specforge::local_user_state_paths
