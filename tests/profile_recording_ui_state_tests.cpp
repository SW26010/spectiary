#include "ui/profile_recording_ui_state.h"

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

void TestInactivePresentationStartsRecording()
{
    const specforge::ProfileRecordingUiPresentation presentation =
        specforge::ResolveProfileRecordingUiPresentation(false, false);
    Require(presentation.status_text == "Not recording", "inactive status should be explicit");
    Require(presentation.menu_action == "Start Recording", "inactive menu should start recording");
    Require(!presentation.show_recording_indicator, "inactive status should not show the red recording indicator");
    Require(
        specforge::ResolveProfileRecordingToggleAction(false, false) ==
            specforge::ProfileRecordingToggleAction::Start,
        "inactive toggle should route to Start");
}

void TestActivePresentationStopsRecording()
{
    const specforge::ProfileRecordingUiPresentation presentation =
        specforge::ResolveProfileRecordingUiPresentation(true, false);
    Require(presentation.status_text == "Performance recording", "active status should identify recording");
    Require(presentation.menu_action == "Stop Recording", "active menu should stop recording");
    Require(presentation.show_recording_indicator, "active status should show the red recording indicator");
    Require(
        specforge::ResolveProfileRecordingToggleAction(true, false) ==
            specforge::ProfileRecordingToggleAction::Stop,
        "active toggle should route to Stop");
}

void TestStoppingPresentationDisablesToggle()
{
    const specforge::ProfileRecordingUiPresentation presentation =
        specforge::ResolveProfileRecordingUiPresentation(false, true);
    Require(presentation.status_text == "Finishing recording...", "draining status should be visible");
    Require(!presentation.menu_action_enabled, "the menu should be disabled while the writer drains");
    Require(!presentation.show_recording_indicator, "draining should not look like active recording");
    Require(
        specforge::ResolveProfileRecordingToggleAction(false, true) ==
            specforge::ProfileRecordingToggleAction::None,
        "a second toggle should be ignored while the writer drains");
}

}  // namespace

int main()
{
    try {
        TestInactivePresentationStartsRecording();
        TestActivePresentationStopsRecording();
        TestStoppingPresentationDisablesToggle();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
