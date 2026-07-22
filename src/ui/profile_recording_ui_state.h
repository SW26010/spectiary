#pragma once

#include <string_view>

namespace specforge {

enum class ProfileRecordingToggleAction {
    None,
    Start,
    Stop,
};

struct ProfileRecordingUiPresentation {
    std::string_view status_text;
    std::string_view menu_action;
    bool show_recording_indicator = false;
    bool menu_action_enabled = true;
};

[[nodiscard]] constexpr ProfileRecordingUiPresentation ResolveProfileRecordingUiPresentation(
    bool recording,
    bool stopping) noexcept
{
    if (stopping) {
        return {"Finishing recording...", "Finishing Recording...", false, false};
    }
    if (recording) {
        return {"Performance recording", "Stop Recording", true, true};
    }
    return {"Not recording", "Start Recording", false, true};
}

[[nodiscard]] constexpr ProfileRecordingToggleAction ResolveProfileRecordingToggleAction(
    bool recording,
    bool stopping) noexcept
{
    if (stopping) {
        return ProfileRecordingToggleAction::None;
    }
    return recording ? ProfileRecordingToggleAction::Stop : ProfileRecordingToggleAction::Start;
}

}  // namespace specforge
