#include "ui/sample_workflow_panel.h"
#include "ui/sample_workflow_shortcut.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class ScopedImGuiContext {
public:
    ScopedImGuiContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(font_pixels != nullptr && font_width > 0 && font_height > 0, "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }
};

void BeginFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();
}

struct WorkflowFrameObservation {
    bool first_focused = false;
    bool second_focused = false;
    specforge::SampleWorkflowShortcut shortcut;
};

WorkflowFrameObservation RenderWorkflowFrame(
    bool request_initial_focus,
    const specforge::SampleWorkflowShortcutContext& capabilities,
    const specforge::SampleLabelSet& label_set = {},
    bool focus_second = false)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Sample workflow shortcut integration");
    if (request_initial_focus && !focus_second) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("First");
    WorkflowFrameObservation observation;
    observation.first_focused = ImGui::IsItemFocused();
    ImGui::SameLine();
    if (request_initial_focus && focus_second) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("Second");
    observation.second_focused = ImGui::IsItemFocused();

    specforge::SampleWorkflowShortcutContext context = capabilities;
    context.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    context.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    observation.shortcut = specforge::RouteSampleWorkflowShortcut(context, label_set);
    ImGui::End();
    ImGui::EndFrame();
    return observation;
}

specforge::SampleLabelShortcutCapture RenderCaptureFrame()
{
    BeginFrame();
    ImGui::Begin("Label shortcut capture integration");
    const specforge::SampleLabelShortcutCapture capture = specforge::CaptureSampleLabelShortcut();
    ImGui::End();
    ImGui::EndFrame();
    return capture;
}

specforge::SampleWorkflowShortcut RenderTextInputFrame(
    bool request_initial_focus,
    char* text,
    std::size_t text_size)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Workflow text input integration");
    if (request_initial_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("Label name", text, text_size);
    const specforge::SampleWorkflowShortcut shortcut = specforge::RouteSampleWorkflowShortcut({
        .focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
        .hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows),
        .navigation_enabled = true,
        .labeling_enabled = true});
    ImGui::End();
    ImGui::EndFrame();
    return shortcut;
}

specforge::SampleWorkflowShortcut RenderForcedTextBlockFrame(bool text_input_blocked)
{
    BeginFrame();
    ImGui::Begin("Workflow text block integration");
    ImGuiIO& io = ImGui::GetIO();
    const bool previous_want_text_input = io.WantTextInput;
    io.WantTextInput = text_input_blocked;
    const specforge::SampleWorkflowShortcut shortcut = specforge::RouteSampleWorkflowShortcut({
        .focused = true,
        .navigation_enabled = true,
        .labeling_enabled = true});
    io.WantTextInput = previous_want_text_input;
    ImGui::End();
    ImGui::EndFrame();
    return shortcut;
}

struct PopupFrameObservation {
    bool popup_open = false;
    specforge::SampleWorkflowShortcut shortcut;
};

PopupFrameObservation RenderPopupFrame(bool open_popup, bool close_before_routing = false)
{
    BeginFrame();
    ImGui::Begin("Workflow popup integration");
    if (open_popup) {
        ImGui::OpenPopup("Sample action");
    }

    PopupFrameObservation observation;
    if (ImGui::BeginPopup("Sample action")) {
        observation.popup_open = true;
        ImGui::Button("Popup action");
        if (close_before_routing) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    observation.shortcut = specforge::RouteSampleWorkflowShortcut({
        .focused = true,
        .navigation_enabled = true,
        .labeling_enabled = true});
    ImGui::End();
    ImGui::EndFrame();
    return observation;
}

struct CrossWindowObservation {
    bool other_panel_focused = false;
    bool plot_hovered = false;
    specforge::SampleWorkflowShortcut shortcut;
};

CrossWindowObservation RenderPlotHoverWithOtherPanelFocus(bool request_initial_focus)
{
    BeginFrame();
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260.0f, 180.0f), ImGuiCond_Always);
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Other panel");
    if (request_initial_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("Other action");
    CrossWindowObservation observation;
    observation.other_panel_focused = ImGui::IsItemFocused();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(360.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_Always);
    ImGui::Begin("Plot", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("Plot");
    observation.plot_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    observation.shortcut = specforge::RouteSampleWorkflowShortcut({
        .focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
        .hovered = observation.plot_hovered,
        .allow_hover_fallback = true,
        .navigation_enabled = true,
        .labeling_enabled = true});
    ImGui::End();
    ImGui::EndFrame();
    return observation;
}

specforge::SampleWorkflowShortcut RenderHoverOnlyFrame(bool allow_hover_fallback)
{
    BeginFrame();
    ImGui::SetWindowFocus(nullptr);
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_Always);
    ImGui::Begin("Hover-only workflow context", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("Context");
    const specforge::SampleWorkflowShortcut shortcut = specforge::RouteSampleWorkflowShortcut({
        .focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
        .hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows),
        .allow_hover_fallback = allow_hover_fallback,
        .navigation_enabled = true});
    ImGui::End();
    ImGui::EndFrame();
    return shortcut;
}

specforge::SourceCollectionSessionView MakeLabelingPanelView(int label_code, char shortcut)
{
    specforge::SourceCollectionSessionView view;
    view.labeling.has_active_source = true;
    view.labeling.current_index = 0;
    view.labeling.has_active_task = true;
    view.labeling.active_task_is_temporary = false;
    view.labeling.task_id = "quality";
    view.labeling.task_name = "Quality";
    view.labeling.sample_count = 1;
    view.labeling.can_deactivate_task = true;
    view.labeling.can_delete_task = true;
    view.labeling.label_set.labels.push_back(
        specforge::SampleLabelDefinition{label_code, "Quality", shortcut});
    return view;
}

specforge::SampleWorkflowShortcut RenderLabelingPanelFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& frame_view,
    const specforge::SourceCollectionSessionView& latest_view,
    bool request_initial_focus)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 500.0f), ImGuiCond_Always);
    bool open = true;
    specforge::SampleWorkflowShortcut shortcut;
    (void)panel.RenderLabeling(
        frame_view,
        [](specforge::SourceCollectionSessionIntent) {
            return specforge::SourceCollectionSessionResult{};
        },
        [&latest_view]() -> const specforge::SourceCollectionSessionView& {
            return latest_view;
        },
        &open,
        []() -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        shortcut);
    ImGui::EndFrame();
    return shortcut;
}

struct LabelingTaskSwitchFrameObservation {
    specforge::SampleWorkflowShortcut shortcut;
    int submission_count = 0;
    bool popup_open = false;
    bool selector_hovered = false;
    bool temporary_action_hovered = false;
    ImVec2 content_start;
    ImVec2 popup_content_start;
};

LabelingTaskSwitchFrameObservation RenderLabelingTaskSwitchFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& frame_view,
    specforge::SourceCollectionSessionView& latest_view,
    const specforge::SourceCollectionSessionView& activated_view,
    bool request_initial_focus)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 500.0f), ImGuiCond_Always);
    bool open = true;
    LabelingTaskSwitchFrameObservation observation;
    (void)panel.RenderLabeling(
        frame_view,
        [&observation, &latest_view, &activated_view](specforge::SourceCollectionSessionIntent) {
            ++observation.submission_count;
            latest_view = activated_view;
            specforge::SourceCollectionSessionResult result;
            result.action.workflow_changed = true;
            return result;
        },
        [&latest_view]() -> const specforge::SourceCollectionSessionView& {
            return latest_view;
        },
        &open,
        []() -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        observation.shortcut);
    observation.popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGuiWindow* window = ImGui::FindWindowByName(specforge::SampleWorkflowPanelUi::LabelingWindowName())) {
        observation.content_start = window->DC.CursorStartPos;
        observation.selector_hovered = GImGui->HoveredId == window->GetID("##labeling_task_selector");
    }
    if (!GImGui->OpenPopupStack.empty()) {
        if (ImGuiWindow* popup_window = GImGui->OpenPopupStack.back().Window) {
            observation.popup_content_start = popup_window->DC.CursorStartPos;
            observation.temporary_action_hovered =
                GImGui->HoveredId == popup_window->GetID("New labeling task");
        }
    }
    ImGui::EndFrame();
    return observation;
}

void TestShortcutDisplayUsesKeyboardLegends()
{
    Require(specforge::FormatSampleLabelShortcut('q') == "Q", "lowercase shortcut should display as Q");
    Require(specforge::FormatSampleLabelShortcut('Q') == "Q", "uppercase input should display canonically");
    Require(specforge::FormatSampleLabelShortcut('3') == "3", "digit shortcut should retain its legend");
    Require(specforge::FormatSampleLabelShortcut('\0') == "None", "unbound shortcut should display as None");
}

void TestShortcutCaptureAcceptsLettersAndKeypadDigits()
{
    ScopedImGuiContext context;
    (void)RenderCaptureFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
    specforge::SampleLabelShortcutCapture capture = RenderCaptureFrame();
    Require(
        capture.kind == specforge::SampleLabelShortcutCaptureKind::Captured && capture.shortcut == 'q',
        "Q should capture as canonical lowercase q");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, false);
    (void)RenderCaptureFrame();

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Keypad3, true);
    capture = RenderCaptureFrame();
    Require(
        capture.kind == specforge::SampleLabelShortcutCaptureKind::Captured && capture.shortcut == '3',
        "keypad 3 should capture as the portable digit binding");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Keypad3, false);
    (void)RenderCaptureFrame();
}

void TestShortcutCaptureRejectsModifiedAndReservedKeys()
{
    ScopedImGuiContext context;
    (void)RenderCaptureFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
    specforge::SampleLabelShortcutCapture capture = RenderCaptureFrame();
    Require(capture.kind == specforge::SampleLabelShortcutCaptureKind::Unsupported, "Ctrl+Q should be rejected");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    (void)RenderCaptureFrame();

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    capture = RenderCaptureFrame();
    Require(capture.kind == specforge::SampleLabelShortcutCaptureKind::Unsupported, "Right Arrow should be reserved");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderCaptureFrame();
}

void TestShortcutCaptureSupportsClearAndCancel()
{
    ScopedImGuiContext context;
    (void)RenderCaptureFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    specforge::SampleLabelShortcutCapture capture = RenderCaptureFrame();
    Require(capture.kind == specforge::SampleLabelShortcutCaptureKind::Cleared, "Backspace should clear capture");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
    (void)RenderCaptureFrame();

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    capture = RenderCaptureFrame();
    Require(capture.kind == specforge::SampleLabelShortcutCaptureKind::Cancelled, "Escape should cancel capture");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    (void)RenderCaptureFrame();
}

void TestShortcutConflictRequiresTheSameKeyTwice()
{
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{1, "bad", 'q'});
    labels.labels.push_back(specforge::SampleLabelDefinition{2, "good", 'g'});

    specforge::SampleLabelShortcutSelection selection = specforge::ResolveSampleLabelShortcutSelection(
        'q',
        2,
        '\0',
        labels);
    Require(
        selection.kind == specforge::SampleLabelShortcutSelectionKind::ConflictRequiresRepeat &&
            selection.conflicting_label_code == 1,
        "first Q should identify the existing owner without accepting the transfer");

    selection = specforge::ResolveSampleLabelShortcutSelection('q', 2, 'q', labels);
    Require(
        selection.kind == specforge::SampleLabelShortcutSelectionKind::Accepted &&
            selection.conflicting_label_code == 1,
        "second Q should accept the transfer from the existing owner");

    selection = specforge::ResolveSampleLabelShortcutSelection('g', 2, 'q', labels);
    Require(
        selection.kind == specforge::SampleLabelShortcutSelectionKind::Accepted &&
            selection.conflicting_label_code == specforge::kUnlabeledSampleLabelCode,
        "pressing the edited label's existing key should not be treated as a conflict");
}

void TestNavigationAndLabelCommandsShareOneRouter()
{
    ScopedImGuiContext context;
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{7, "quality", 'q'});
    const specforge::SampleWorkflowShortcutContext capabilities{
        .navigation_enabled = true,
        .labeling_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities, labels);
    (void)RenderWorkflowFrame(false, capabilities, labels);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
    specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 7,
        "Q should route to its active task label");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, false);
    (void)RenderWorkflowFrame(false, capabilities, labels);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(shortcut.kind == specforge::SampleWorkflowShortcutKind::NextSample, "Right Arrow should route next");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderWorkflowFrame(false, capabilities, labels);
}

void TestLabelingPanelRoutesTheLatestSessionProjection()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    const specforge::SourceCollectionSessionView stale_view = MakeLabelingPanelView(7, 'q');
    const specforge::SourceCollectionSessionView latest_view = MakeLabelingPanelView(8, 'g');
    (void)RenderLabelingPanelFrame(panel, stale_view, latest_view, true);
    (void)RenderLabelingPanelFrame(panel, stale_view, latest_view, false);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_G, true);
    const specforge::SampleWorkflowShortcut shortcut =
        RenderLabelingPanelFrame(panel, stale_view, latest_view, false);
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 8,
        "Labeling Panel should register shortcuts from the latest session projection after a mutation");
}

void TestLabelingPanelTaskSwitchRegistersTheNewShortcutInTheSelectionFrame()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView inactive_view;
    inactive_view.labeling.has_active_source = true;
    inactive_view.labeling.current_index = 0;
    inactive_view.labeling.sample_count = 1;
    specforge::SourceCollectionSessionView latest_view = inactive_view;
    const specforge::SourceCollectionSessionView activated_view = MakeLabelingPanelView(8, 'g');

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    LabelingTaskSwitchFrameObservation observation =
        RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, true);

    ImVec2 selector_position;
    for (float y = 25.0f; y <= 150.0f && !observation.selector_hovered; y += 2.0f) {
        selector_position = ImVec2(observation.content_start.x + 50.0f, y);
        ImGui::GetIO().AddMousePosEvent(selector_position.x, selector_position.y);
        observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    }
    Require(observation.selector_hovered, "integration fixture should locate the labeling task selector");
    ImGui::GetIO().AddMousePosEvent(selector_position.x, selector_position.y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    Require(observation.popup_open, "labeling task selector should open for the integration fixture");

    ImVec2 temporary_action_position;
    const float popup_content_x = observation.popup_content_start.x + 50.0f;
    const float popup_search_start_y = observation.popup_content_start.y - 20.0f;
    const float popup_search_end_y = observation.popup_content_start.y + 100.0f;
    for (float y = popup_search_start_y;
         y <= popup_search_end_y && !observation.temporary_action_hovered;
         y += 2.0f) {
        temporary_action_position = ImVec2(popup_content_x, y);
        ImGui::GetIO().AddMousePosEvent(temporary_action_position.x, temporary_action_position.y);
        observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    }
    Require(observation.temporary_action_hovered, "integration fixture should locate the temporary task action");
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    Require(observation.submission_count == 1, "selecting the temporary sample labeling task should submit once");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_G, true);
    observation = RenderLabelingTaskSwitchFrame(panel, activated_view, latest_view, activated_view, false);
    Require(
        observation.shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel &&
            observation.shortcut.label_code == 8,
        "the first new shortcut after a task switch should be routed without a settling frame");
}

void TestConsecutiveLabelCommandsDoNotNeedASettlingFrame()
{
    ScopedImGuiContext context;
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{7, "quality", 'q'});
    labels.labels.push_back(specforge::SampleLabelDefinition{8, "good", 'g'});
    const specforge::SampleWorkflowShortcutContext capabilities{.labeling_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities, labels);
    (void)RenderWorkflowFrame(false, capabilities, labels);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
    specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 7,
        "Q should route to the first label");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_G, true);
    shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 8,
        "G on the immediately following frame should not be swallowed");
}

void TestUndoThenNavigationDoesNotNeedASettlingFrame()
{
    ScopedImGuiContext context;
    const specforge::SampleWorkflowShortcutContext capabilities{
        .navigation_enabled = true,
        .labeling_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities);
    (void)RenderWorkflowFrame(false, capabilities);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, true);
    specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, capabilities).shortcut;
    Require(shortcut.kind == specforge::SampleWorkflowShortcutKind::UndoLabelWrite, "Ctrl+Z should route undo");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    shortcut = RenderWorkflowFrame(false, capabilities).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::NextSample,
        "Right Arrow immediately after undo should not be swallowed");
}

void TestTopRowThenKeypadDigitDoesNotNeedASettlingFrame()
{
    ScopedImGuiContext context;
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{3, "three", '3'});
    const specforge::SampleWorkflowShortcutContext capabilities{.labeling_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities, labels);
    (void)RenderWorkflowFrame(false, capabilities, labels);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_3, true);
    specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 3,
        "top-row 3 should route to the digit label");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_3, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Keypad3, true);
    shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 3,
        "keypad 3 immediately after top-row 3 should not be swallowed");
}

void TestCtrlZRequiresLabelingContextAndYieldsToTextEditing()
{
    ScopedImGuiContext context;
    const specforge::SampleWorkflowShortcutContext labeling_capability{.labeling_enabled = true};
    (void)RenderWorkflowFrame(true, labeling_capability);
    (void)RenderWorkflowFrame(false, labeling_capability);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, true);
    specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, labeling_capability).shortcut;
    Require(shortcut.kind == specforge::SampleWorkflowShortcutKind::UndoLabelWrite, "Ctrl+Z should undo labeling");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    (void)RenderWorkflowFrame(false, labeling_capability);

    char text[32] = "label";
    (void)RenderTextInputFrame(true, text, sizeof(text));
    (void)RenderTextInputFrame(false, text, sizeof(text));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, true);
    shortcut = RenderTextInputFrame(false, text, sizeof(text));
    Require(shortcut.kind == specforge::SampleWorkflowShortcutKind::None, "text editing should retain Ctrl+Z");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    (void)RenderTextInputFrame(false, text, sizeof(text));
}

void TestOpenPopupSuppressesWorkflowCommands()
{
    ScopedImGuiContext context;
    (void)RenderPopupFrame(false);
    (void)RenderPopupFrame(false);
    const PopupFrameObservation opened = RenderPopupFrame(true);
    Require(opened.popup_open, "the workflow popup should open");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const PopupFrameObservation pressed = RenderPopupFrame(false);
    Require(
        pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::None,
        "an open popup should suppress workflow navigation");
    Require(pressed.popup_open, "the popup should remain open after Right Arrow");
}

void TestModifiedArrowDoesNotNavigateSamples()
{
    ScopedImGuiContext context;
    const specforge::SampleWorkflowShortcutContext capabilities{.navigation_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities);
    (void)RenderWorkflowFrame(false, capabilities);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const WorkflowFrameObservation pressed = RenderWorkflowFrame(false, capabilities);
    Require(
        pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::None,
        "Ctrl+Right Arrow should not request sample navigation");
}

void TestArrowCommandsRetainControlFocus()
{
    const specforge::SampleWorkflowShortcutContext capabilities{.navigation_enabled = true};
    {
        ScopedImGuiContext context;
        (void)RenderWorkflowFrame(true, capabilities);
        const WorkflowFrameObservation initial = RenderWorkflowFrame(false, capabilities);
        Require(initial.first_focused, "right-arrow fixture should begin with the first button focused");

        ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
        const WorkflowFrameObservation pressed = RenderWorkflowFrame(false, capabilities);
        Require(
            pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::NextSample,
            "Right Arrow should request the next sample");
        Require(pressed.first_focused, "Right Arrow should retain the first button focus");
        Require(!pressed.second_focused, "Right Arrow should not move focus to the second button");
    }
    {
        ScopedImGuiContext context;
        (void)RenderWorkflowFrame(true, capabilities, {}, true);
        const WorkflowFrameObservation initial = RenderWorkflowFrame(false, capabilities);
        Require(initial.second_focused, "left-arrow fixture should begin with the second button focused");

        ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
        const WorkflowFrameObservation pressed = RenderWorkflowFrame(false, capabilities);
        Require(
            pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::PreviousSample,
            "Left Arrow should request the previous sample");
        Require(pressed.second_focused, "Left Arrow should retain the second button focus");
        Require(!pressed.first_focused, "Left Arrow should not move focus to the first button");
    }
}

void TestCaptureBlocksWorkflowCommands()
{
    ScopedImGuiContext context;
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{7, "quality", 'q'});
    const specforge::SampleWorkflowShortcutContext capabilities{
        .labeling_enabled = true,
        .blocked = true};
    (void)RenderWorkflowFrame(true, capabilities, labels);
    (void)RenderWorkflowFrame(false, capabilities, labels);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
    const specforge::SampleWorkflowShortcut shortcut = RenderWorkflowFrame(false, capabilities, labels).shortcut;
    Require(shortcut.kind == specforge::SampleWorkflowShortcutKind::None, "shortcut capture should block assignment");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, false);
    (void)RenderWorkflowFrame(false, capabilities, labels);
}

void TestFirstCommandAfterBlockingIsNotSwallowed()
{
    specforge::SampleLabelSet labels;
    labels.labels.push_back(specforge::SampleLabelDefinition{7, "quality", 'q'});

    {
        ScopedImGuiContext context;
        const specforge::SampleWorkflowShortcutContext blocked{
            .labeling_enabled = true,
            .blocked = true};
        (void)RenderWorkflowFrame(true, blocked, labels);
        (void)RenderWorkflowFrame(false, blocked, labels);

        ImGui::GetIO().AddKeyEvent(ImGuiKey_Q, true);
        const specforge::SampleWorkflowShortcutContext unblocked{.labeling_enabled = true};
        const specforge::SampleWorkflowShortcut shortcut =
            RenderWorkflowFrame(false, unblocked, labels).shortcut;
        Require(
            shortcut.kind == specforge::SampleWorkflowShortcutKind::AssignLabel && shortcut.label_code == 7,
            "the first label key after shortcut capture should not be swallowed");
    }
    {
        ScopedImGuiContext context;
        (void)RenderForcedTextBlockFrame(true);
        (void)RenderForcedTextBlockFrame(true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
        const specforge::SampleWorkflowShortcut shortcut = RenderForcedTextBlockFrame(false);
        Require(
            shortcut.kind == specforge::SampleWorkflowShortcutKind::NextSample,
            "the first navigation key after text input releases ownership should not be swallowed");
    }
    {
        ScopedImGuiContext context;
        (void)RenderPopupFrame(true);
        const PopupFrameObservation closing = RenderPopupFrame(false, true);
        Require(closing.popup_open, "the popup close frame should begin with the popup open");

        ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
        const PopupFrameObservation pressed = RenderPopupFrame(false);
        Require(
            pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::PreviousSample,
            "the first navigation key after a popup closes should not be swallowed");
    }
}

void TestPlotHoverYieldsToAnotherPanelsFocus()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(500.0f, 100.0f);
    (void)RenderPlotHoverWithOtherPanelFocus(true);
    const CrossWindowObservation initial = RenderPlotHoverWithOtherPanelFocus(false);
    Require(initial.other_panel_focused, "other panel should own keyboard focus");
    Require(initial.plot_hovered, "pointer should hover Plot");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const CrossWindowObservation pressed = RenderPlotHoverWithOtherPanelFocus(false);
    Require(
        pressed.shortcut.kind == specforge::SampleWorkflowShortcutKind::None,
        "Plot hover should not override another panel's keyboard focus");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderPlotHoverWithOtherPanelFocus(false);
}

void TestHoverFallbackIsExplicitlyOptedIn()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(100.0f, 100.0f);
    (void)RenderHoverOnlyFrame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    specforge::SampleWorkflowShortcut shortcut = RenderHoverOnlyFrame(false);
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::None,
        "hover-only Labeling-style contexts should not own workflow keys");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderHoverOnlyFrame(false);

    (void)RenderHoverOnlyFrame(true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    shortcut = RenderHoverOnlyFrame(true);
    Require(
        shortcut.kind == specforge::SampleWorkflowShortcutKind::NextSample,
        "Plot-style contexts should support the explicit hover fallback");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderHoverOnlyFrame(true);
}

void TestTabStillMovesControlFocus()
{
    ScopedImGuiContext context;
    const specforge::SampleWorkflowShortcutContext capabilities{.navigation_enabled = true};
    (void)RenderWorkflowFrame(true, capabilities);
    const WorkflowFrameObservation initial = RenderWorkflowFrame(false, capabilities);
    Require(initial.first_focused, "first action should begin focused");
    ImGui::SetNavCursorVisible(true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
    (void)RenderWorkflowFrame(false, capabilities);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);
    (void)RenderWorkflowFrame(false, capabilities);
    const WorkflowFrameObservation settled = RenderWorkflowFrame(false, capabilities);
    Require(settled.second_focused, "Tab should retain standard control navigation");
}

}  // namespace

int main()
{
    TestShortcutDisplayUsesKeyboardLegends();
    TestShortcutCaptureAcceptsLettersAndKeypadDigits();
    TestShortcutCaptureRejectsModifiedAndReservedKeys();
    TestShortcutCaptureSupportsClearAndCancel();
    TestShortcutConflictRequiresTheSameKeyTwice();
    TestNavigationAndLabelCommandsShareOneRouter();
    TestLabelingPanelRoutesTheLatestSessionProjection();
    TestLabelingPanelTaskSwitchRegistersTheNewShortcutInTheSelectionFrame();
    TestConsecutiveLabelCommandsDoNotNeedASettlingFrame();
    TestUndoThenNavigationDoesNotNeedASettlingFrame();
    TestTopRowThenKeypadDigitDoesNotNeedASettlingFrame();
    TestCtrlZRequiresLabelingContextAndYieldsToTextEditing();
    TestOpenPopupSuppressesWorkflowCommands();
    TestModifiedArrowDoesNotNavigateSamples();
    TestArrowCommandsRetainControlFocus();
    TestCaptureBlocksWorkflowCommands();
    TestFirstCommandAfterBlockingIsNotSwallowed();
    TestPlotHoverYieldsToAnotherPanelsFocus();
    TestHoverFallbackIsExplicitlyOptedIn();
    TestTabStillMovesControlFocus();
    return 0;
}
