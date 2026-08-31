#include "ui/sample_workflow_panel.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_workflow_shortcut.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace specforge {

struct SourceCollectionPanelUiTestAccess {
    [[nodiscard]] static ActiveSampleWorkflowIntentKind
    ActiveWorkflowKind(const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.kind;
    }

    [[nodiscard]] static std::string_view
    ActiveWorkflowSourceIdentity(const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.source_identity;
    }

    [[nodiscard]] static std::string_view
    ActiveWorkflowTaskId(const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.task_id;
    }

    [[nodiscard]] static std::string_view
    ActiveWorkflowRequestedName(
        const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.requested_name;
    }

    [[nodiscard]] static SampleLabelExportFormat
    ActiveWorkflowExportFormat(
        const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.export_format;
    }

    [[nodiscard]] static const std::filesystem::path&
    ActiveWorkflowPath(
        const SourceCollectionSessionIntent& intent)
    {
        return intent.active_sample_workflow.path;
    }
};

struct SampleWorkflowPanelUiTestAccess {
    [[nodiscard]] static std::string_view
    LabelingOperationMessage(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_operation_message_;
    }

    static void CaptureLabelingOperationResult(
        SampleWorkflowPanelUi& panel,
        const SourceCollectionSessionResult& result,
        UiLanguage language)
    {
        panel.CaptureLabelingOperationResult(
            result,
            language);
    }

    [[nodiscard]] static bool IsRecoveryDraftRetained(
        const SampleWorkflowPanelUi& panel,
        std::string_view source_identity,
        std::string_view task_id)
    {
        std::string key;
        key.reserve(source_identity.size() + task_id.size() + 2);
        key.append(source_identity);
        key.push_back('\n');
        key.append(task_id);
        key.push_back('\n');
        return std::any_of(
            panel.retained_recovery_drafts_.begin(),
            panel.retained_recovery_drafts_.end(),
            [&key](const std::string& retained_key) {
                return retained_key.compare(
                           0,
                           key.size(),
                           key) == 0;
            });
    }

    [[nodiscard]] static std::string RecoveryDraftRowToken(
        const SourceCollectionSessionView& view,
        std::size_t draft_index)
    {
        return SampleWorkflowPanelUi::RecoveryDraftRowToken(
            view.labeling,
            draft_index);
    }

    [[nodiscard]] static bool IsRecoveryDraftRetainedAt(
        const SampleWorkflowPanelUi& panel,
        const SourceCollectionSessionView& view,
        std::size_t draft_index)
    {
        return panel.retained_recovery_drafts_.contains(
            RecoveryDraftRowToken(view, draft_index));
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingSelectorRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_selector_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingTaskNameRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_task_name_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingTaskIdCopyRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_task_id_copy_rect_;
    }

    [[nodiscard]] static std::string_view TaskNameEditTaskId(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.task_name_edit_task_id_;
    }

    [[nodiscard]] static std::string_view TaskNameEditBuffer(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.task_name_edit_buffer_;
    }

    [[nodiscard]] static std::string_view TaskNameEditBaseline(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.task_name_edit_baseline_;
    }

    [[nodiscard]] static std::string_view TaskNameValidationMessage(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.task_name_edit_validation_message_;
    }

    [[nodiscard]] static std::string ValidateTaskName(
        UiLanguage language,
        std::string_view task_name)
    {
        return SampleWorkflowPanelUi::ValidateTaskName(
            language,
            task_name);
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingPauseRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_pause_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingDeleteRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_delete_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingRecoveryRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_recovery_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    TemporaryLabelingActionRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.temporary_labeling_action_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingDeleteConfirmationRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_delete_confirmation_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    RecoveryActionRect(
        const SampleWorkflowPanelUi& panel,
        const SourceCollectionSessionView& view,
        std::size_t draft_index,
        std::string_view stable_id)
    {
        std::string key = RecoveryDraftRowToken(
            view,
            draft_index);
        key.push_back('\n');
        key.append(stable_id);
        const auto found = panel.recovery_action_rects_.find(key);
        if (found == panel.recovery_action_rects_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    RecoveryIdentityRect(
        const SampleWorkflowPanelUi& panel,
        const SourceCollectionSessionView& view,
        std::size_t draft_index)
    {
        const std::string key = RecoveryDraftRowToken(
            view,
            draft_index);
        const auto found = panel.recovery_identity_rects_.find(key);
        if (found == panel.recovery_identity_rects_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    static void SetEditingLabelCode(
        SampleWorkflowPanelUi& panel,
        std::optional<int> code)
    {
        panel.editing_label_code_ = code;
    }

    static void SetShortcutCaptureActive(
        SampleWorkflowPanelUi& panel,
        bool active)
    {
        panel.label_shortcut_capture_active_ = active;
    }

    static void SetActiveTaskId(
        SampleWorkflowPanelUi& panel,
        std::string task_id)
    {
        panel.active_task_id_ = std::move(task_id);
    }

    [[nodiscard]] static bool IsEditingLabelCode(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.editing_label_code_.has_value();
    }

    [[nodiscard]] static bool IsShortcutCaptureActive(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.label_shortcut_capture_active_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingExportRect(const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_export_rect_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingExportFormatRect(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_export_format_rect_;
    }

    [[nodiscard]] static SampleLabelExportFormat
    LabelingExportFormat(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_export_format_;
    }

    [[nodiscard]] static std::optional<std::array<float, 4>>
    LabelingOutputActionRect(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.labeling_output_action_rect_;
    }

    static void SetPendingAnnotationActivation(
        SampleWorkflowPanelUi& panel,
        const SourceCollectionAnnotationValueView& annotation)
    {
        panel.SetPendingAnnotationActivation(annotation);
    }

    [[nodiscard]] static SampleLabelingOutputArtifactFormat
    PendingAnnotationActivationOwnerFormat(
        const SampleWorkflowPanelUi& panel)
    {
        return panel.pending_annotation_activation_owner_format_;
    }
};

}  // namespace specforge

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
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        platform_io.Platform_ClipboardUserData = &clipboard_text_;
        platform_io.Platform_SetClipboardTextFn =
            [](ImGuiContext* context, const char* text) {
                auto* clipboard_text = static_cast<std::string*>(
                    context->PlatformIO.Platform_ClipboardUserData);
                *clipboard_text = text != nullptr ? text : "";
            };
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    [[nodiscard]] const std::string& clipboard_text() const
    {
        return clipboard_text_;
    }

private:
    std::string clipboard_text_;
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
    view.labeling.source_identity = "source";
    view.labeling.source_kind = "npy";
    view.labeling.current_index = 0;
    view.labeling.has_active_task = true;
    view.labeling.active_task_is_temporary = false;
    view.labeling.task_id = "quality";
    view.labeling.task_name = "Quality";
    view.labeling.sample_count = 1;
    view.labeling.can_export_label_values = true;
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
    int view_reads = 0;
    specforge::PanelSessionInteraction interaction(
        [](
            specforge::SourceCollectionSessionIntent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            return specforge::SourceCollectionSessionResult{};
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            return view_reads++ == 0
                ? frame_view
                : latest_view;
        }
    );
    panel.RenderLabeling(
        interaction,
        &open,
        [](std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        [](specforge::SampleLabelExportFormat, std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        shortcut);
    ImGui::EndFrame();
    return shortcut;
}

struct LabelingTaskNameFrameObservation {
    int submission_count = 0;
    std::optional<specforge::ActiveSampleWorkflowIntentKind>
        submitted_workflow_kind;
    std::string submitted_task_id;
    std::string submitted_name;
    std::string logged_text;
};

LabelingTaskNameFrameObservation RenderLabelingTaskNameFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view,
    bool capture_text = false,
    bool collapsed = false,
    ImVec2 window_size = ImVec2(620.0f, 700.0f))
{
    BeginFrame();
    ImGui::SetNextWindowPos(
        ImVec2(20.0f, 20.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        window_size,
        ImGuiCond_Always);
    ImGui::SetNextWindowCollapsed(
        collapsed,
        ImGuiCond_Always);
    bool open = true;
    LabelingTaskNameFrameObservation observation;
    if (capture_text) {
        ImGui::LogToBuffer();
    }
    specforge::PanelSessionInteraction interaction(
        [&observation](
            specforge::SourceCollectionSessionIntent intent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            ++observation.submission_count;
            if (intent.intent_kind() ==
                specforge::SourceCollectionSessionIntentKind::
                    ActiveSampleWorkflow) {
                observation.submitted_workflow_kind =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowKind(intent);
                observation.submitted_task_id =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowTaskId(intent);
                observation.submitted_name =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowRequestedName(intent);
            }
            specforge::SourceCollectionSessionResult result;
            result.changed = true;
            result.action.workflow_changed = true;
            return result;
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    specforge::SampleWorkflowShortcut shortcut;
    panel.RenderLabeling(
        interaction,
        &open,
        [](std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        [](specforge::SampleLabelExportFormat, std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        shortcut);
    if (capture_text) {
        observation.logged_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::EndFrame();
    return observation;
}

LabelingTaskNameFrameObservation FinalizeLabelingTaskNameEdit(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view)
{
    LabelingTaskNameFrameObservation observation;
    specforge::PanelSessionInteraction interaction(
        [&observation](
            specforge::SourceCollectionSessionIntent intent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            ++observation.submission_count;
            if (intent.intent_kind() ==
                specforge::SourceCollectionSessionIntentKind::
                    ActiveSampleWorkflow) {
                observation.submitted_workflow_kind =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowKind(intent);
                observation.submitted_task_id =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowTaskId(intent);
                observation.submitted_name =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowRequestedName(intent);
            }
            specforge::SourceCollectionSessionResult result;
            result.changed = true;
            result.action.workflow_changed = true;
            return result;
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    panel.FinalizeTaskNameEdit(
        interaction,
        specforge::UiLanguage::English);
    return observation;
}

ImVec2 RectCenter(const std::array<float, 4>& rect)
{
    return ImVec2(
        (rect[0] + rect[2]) * 0.5f,
        (rect[1] + rect[3]) * 0.5f);
}

void FocusLabelingTaskNameField(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view)
{
    (void)RenderLabelingTaskNameFrame(panel, view);
    const auto field_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingTaskNameRect(panel);
    Require(
        field_rect.has_value(),
        "an active task should expose its task-name field rectangle");
    const ImVec2 field_center = RectCenter(*field_rect);
    ImGui::GetIO().AddMousePosEvent(
        field_center.x,
        field_center.y);
    (void)RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderLabelingTaskNameFrame(panel, view);
}

void ReplaceFocusedText(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view,
    std::string_view replacement)
{
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
    (void)RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    (void)RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddInputCharactersUTF8(
        std::string(replacement).c_str());
}

struct LabelingExportFrameObservation {
    int output_path_request_count = 0;
    int export_path_request_count = 0;
    std::optional<specforge::ActiveSampleWorkflowIntentKind>
        submitted_workflow_kind;
    std::optional<specforge::SampleLabelExportFormat>
        requested_export_format;
    std::string requested_suggested_filename;
    std::string requested_output_suggested_filename;
    std::filesystem::path submitted_path;
    std::optional<specforge::SampleLabelExportFormat>
        submitted_export_format;
};

LabelingExportFrameObservation RenderLabelingExportFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view)
{
    BeginFrame();
    ImGui::SetNextWindowPos(
        ImVec2(20.0f, 20.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(520.0f, 500.0f),
        ImGuiCond_Always);
    bool open = true;
    LabelingExportFrameObservation observation;
    specforge::PanelSessionInteraction interaction(
        [&observation](
            specforge::SourceCollectionSessionIntent intent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            observation.submitted_workflow_kind =
                specforge::SourceCollectionPanelUiTestAccess::
                    ActiveWorkflowKind(intent);
            observation.submitted_export_format =
                specforge::SourceCollectionPanelUiTestAccess::
                    ActiveWorkflowExportFormat(intent);
            observation.submitted_path =
                specforge::SourceCollectionPanelUiTestAccess::
                    ActiveWorkflowPath(intent);
            return specforge::SourceCollectionSessionResult{};
        },
        [&view]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    specforge::SampleWorkflowShortcut shortcut;
    panel.RenderLabeling(
        interaction,
        &open,
        [&observation](std::string_view suggested_filename)
            -> std::optional<std::filesystem::path> {
            ++observation.output_path_request_count;
            observation.requested_output_suggested_filename =
                suggested_filename;
            return std::filesystem::path{"user-entered.final"};
        },
        [&observation](
            specforge::SampleLabelExportFormat format,
            std::string_view suggested_filename)
            -> std::optional<std::filesystem::path> {
            ++observation.export_path_request_count;
            observation.requested_export_format = format;
            observation.requested_suggested_filename =
                suggested_filename;
            return format == specforge::SampleLabelExportFormat::Csv
                ? std::filesystem::path{"export.csv"}
                : std::filesystem::path{"export.npy"};
        },
        shortcut);
    ImGui::EndFrame();
    return observation;
}

void SelectLabelExportFormatThroughUi(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view,
    specforge::SampleLabelExportFormat format)
{
    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingExportFormatRect(panel);
    Require(
        selector_rect.has_value(),
        "export format selector should expose a deterministic rectangle");
    const ImVec2 selector_position(
        ((*selector_rect)[0] + (*selector_rect)[2]) *
            0.5f,
        ((*selector_rect)[1] + (*selector_rect)[3]) *
            0.5f);
    ImGui::GetIO().AddMousePosEvent(
        selector_position.x,
        selector_position.y);
    (void)RenderLabelingExportFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingExportFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation opened =
        RenderLabelingExportFrame(panel, view);
    Require(
        !opened.requested_export_format &&
            !opened.submitted_workflow_kind &&
            !GImGui->OpenPopupStack.empty() &&
            GImGui->OpenPopupStack.back().Window != nullptr,
        "clicking the export format selector should open its popup without exporting");

    ImGuiWindow* popup_window =
        GImGui->OpenPopupStack.back().Window;
    ImGui::GetIO().AddMousePosEvent(
        popup_window->InnerRect.Min.x + 1.0f,
        popup_window->InnerRect.Min.y + 1.0f);
    const LabelingExportFrameObservation settled =
        RenderLabelingExportFrame(panel, view);
    Require(
        !settled.requested_export_format &&
            !settled.submitted_workflow_kind &&
            !GImGui->OpenPopupStack.empty() &&
            GImGui->OpenPopupStack.back().Window != nullptr,
        "settling the real export format popup must not execute an export");
    popup_window = GImGui->OpenPopupStack.back().Window;
    const int option_index =
        format == specforge::SampleLabelExportFormat::Npy
        ? 0
        : 1;
    const char* option_label =
        format == specforge::SampleLabelExportFormat::Csv
        ? "CSV##SpecForgeLabelExportFormatCsv"
        : "NPY##SpecForgeLabelExportFormatNpy";
    const ImGuiID option_id =
        popup_window->GetID(option_label);
    const float option_height = ImGui::GetTextLineHeight();
    const ImVec2 option_position(
        popup_window->DC.CursorStartPos.x +
            ImGui::GetStyle().FramePadding.x,
        popup_window->DC.CursorStartPos.y +
            static_cast<float>(option_index) *
                (option_height +
                 ImGui::GetStyle().ItemSpacing.y) +
            option_height * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        option_position.x,
        option_position.y);
    const LabelingExportFrameObservation hovered =
        RenderLabelingExportFrame(panel, view);
    Require(
        !hovered.requested_export_format &&
            !hovered.submitted_workflow_kind &&
            !GImGui->OpenPopupStack.empty() &&
            GImGui->HoveredId == option_id,
        "hovering a real export format option must hit that item without exporting");

    ImGui::GetIO().AddMousePosEvent(
        option_position.x,
        option_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    const LabelingExportFrameObservation pressed =
        RenderLabelingExportFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation selected =
        RenderLabelingExportFrame(panel, view);
    Require(
        !pressed.requested_export_format &&
            !pressed.submitted_workflow_kind &&
            !selected.requested_export_format &&
            !selected.submitted_workflow_kind &&
            specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingExportFormat(panel) == format,
        "clicking a real combo option should switch only the export format preference");
}

struct LabelingTaskSwitchFrameObservation {
    specforge::SampleWorkflowShortcut shortcut;
    int submission_count = 0;
    std::string operation_message;
    std::optional<specforge::ActiveSampleWorkflowIntentKind>
        submitted_workflow_kind;
    std::string submitted_source_identity;
    std::string submitted_task_id;
    bool popup_open = false;
    bool selector_hovered = false;
    bool temporary_action_hovered = false;
    ImVec2 content_start;
    ImVec2 popup_content_start;
};

struct RecoveryFrameObservation {
    int submission_count = 0;
    bool popup_open = false;
    ImGuiID hovered_id = 0;
    ImGuiID popup_confirm_id = 0;
    std::optional<specforge::ActiveSampleWorkflowIntentKind>
        submitted_workflow_kind;
    std::string submitted_source_identity;
    std::string submitted_task_id;
    std::string logged_text;
    float cursor_max_y = 0.0f;
};

specforge::SourceCollectionSessionView MakeRecoveryPanelView()
{
    specforge::SourceCollectionSessionView view;
    view.labeling.has_active_source = true;
    view.labeling.source_identity = "source/recovery";
    view.labeling.current_index = 0;
    view.labeling.sample_count = 5;
    view.labeling.has_temporary_task = true;
    view.labeling.recovery_drafts.push_back(
        {
            .task_id = "draft-1",
            .task_name = "Recovered draft",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
            .labeled_count = 2,
            .sample_count = 5,
        });
    return view;
}

specforge::SourceCollectionSessionView MakeActiveTemporaryRecoveryPanelView()
{
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    view.labeling.has_active_task = true;
    view.labeling.active_task_is_temporary = true;
    view.labeling.task_id =
        view.labeling.recovery_drafts.front().task_id;
    view.labeling.task_name =
        view.labeling.recovery_drafts.front().task_name;
    view.labeling.can_deactivate_task = true;
    view.labeling.can_delete_task = true;
    view.labeling.recovery_drafts.front().status =
        specforge::SampleLabelingRecoveryDraftStatus::Current;
    return view;
}

specforge::SourceCollectionSessionView MakeFormalTaskWithRecoveryDraftView()
{
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    view.labeling.has_active_task = true;
    view.labeling.active_task_is_temporary = false;
    view.labeling.task_id = "formal-task";
    view.labeling.task_name = "Formal task";
    view.labeling.can_deactivate_task = true;
    view.labeling.can_delete_task = true;
    return view;
}

specforge::SourceCollectionSessionView
MakeFormalTaskWithAmbiguousRecoveryDraftView()
{
    specforge::SourceCollectionSessionView view =
        MakeFormalTaskWithRecoveryDraftView();
    constexpr std::string_view shared_task_id = "shared-task";
    view.labeling.task_id = std::string(shared_task_id);
    view.labeling.task_ids = {
        std::string(shared_task_id),
        std::string(shared_task_id)};
    view.labeling.has_temporary_task = true;
    view.labeling.recovery_drafts.front().task_id =
        std::string(shared_task_id);
    return view;
}

specforge::SourceCollectionSessionView MakeDuplicateRecoveryPanelView()
{
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    view.labeling.recovery_drafts = {
        {
            .task_id = "duplicate-draft",
            .task_name = "Recovered draft A",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Stale,
            .labeled_count = 1,
            .sample_count = 5,
        },
        {
            .task_id = "duplicate-draft",
            .task_name = "Recovered draft B",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Stale,
            .labeled_count = 4,
            .sample_count = 5,
        },
    };
    return view;
}

specforge::SourceCollectionSessionView MakeFailedRecoveryPanelView()
{
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    auto& save_state =
        view.labeling.recovery_drafts.front().save_state;
    save_state.kind = specforge::SampleLabelSaveStateKind::Failed;
    save_state.pending_count = 1;
    save_state.message_kind =
        specforge::SampleLabelSaveMessageKind::SystemDetail;
    save_state.message = "Save to failed for the paused draft";
    return view;
}

RecoveryFrameObservation RenderRecoveryFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& frame_view,
    specforge::SourceCollectionSessionView& latest_view,
    bool request_initial_focus,
    specforge::UiLanguage language = specforge::UiLanguage::English,
    ImVec2 window_size = ImVec2(900.0f, 500.0f),
    specforge::SampleLabelingOperationResult::Issue submitted_issue =
        specforge::SampleLabelingOperationResult::Issue::None,
    bool capture_text = false,
    const specforge::SourceCollectionSessionView* submitted_view = nullptr);

RecoveryFrameObservation RenderRecoveryFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& frame_view,
    specforge::SourceCollectionSessionView& latest_view,
    bool request_initial_focus,
    specforge::UiLanguage language,
    ImVec2 window_size,
    specforge::SampleLabelingOperationResult::Issue submitted_issue,
    bool capture_text,
    const specforge::SourceCollectionSessionView* submitted_view)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
    ImGui::SetNextWindowSize(window_size);
    bool open = true;
    RecoveryFrameObservation observation;
    if (capture_text) {
        ImGui::LogToBuffer();
    }
    int view_reads = 0;
    specforge::PanelSessionInteraction interaction(
        [&](specforge::SourceCollectionSessionIntent intent,
            std::optional<specforge::NavigationLatencyInputKind>) {
            ++observation.submission_count;
            if (intent.intent_kind() ==
                specforge::SourceCollectionSessionIntentKind::
                    ActiveSampleWorkflow) {
                observation.submitted_workflow_kind =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowKind(intent);
                observation.submitted_source_identity =
                    std::string(
                        specforge::SourceCollectionPanelUiTestAccess::
                            ActiveWorkflowSourceIdentity(intent));
                observation.submitted_task_id =
                    std::string(
                        specforge::SourceCollectionPanelUiTestAccess::
                            ActiveWorkflowTaskId(intent));
            }
            latest_view = submitted_view != nullptr
                ? *submitted_view
                : frame_view;
            specforge::SourceCollectionSessionResult result;
            result.changed = submitted_issue ==
                specforge::SampleLabelingOperationResult::Issue::None;
            result.action.workflow_changed = result.changed;
            result.labeling_issue = submitted_issue;
            return result;
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            return view_reads++ == 0 ? frame_view : latest_view;
        });
    specforge::SampleWorkflowShortcut shortcut;
    panel.RenderLabeling(
        interaction,
        language,
        &open,
        [](std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        [](specforge::SampleLabelExportFormat, std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        shortcut);
    if (capture_text) {
        observation.logged_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    observation.popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    observation.hovered_id = GImGui->HoveredId;
    if (!GImGui->OpenPopupStack.empty()) {
        if (ImGuiWindow* popup_window =
                GImGui->OpenPopupStack.back().Window) {
            observation.popup_confirm_id = popup_window->GetID(
                "SpecForgeConfirmDeleteLabelingTask");
        }
    }
    if (ImGuiWindow* window = ImGui::FindWindowByName(
            specforge::SampleWorkflowPanelUi::LabelingWindowName())) {
        observation.cursor_max_y = window->DC.CursorMaxPos.y;
    }
    ImGui::EndFrame();
    return observation;
}

LabelingTaskSwitchFrameObservation RenderLabelingTaskSwitchFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& frame_view,
    specforge::SourceCollectionSessionView& latest_view,
    const specforge::SourceCollectionSessionView& activated_view,
    bool request_initial_focus,
    std::string result_message = {},
    specforge::SampleLabelingOperationResult::Issue submitted_issue =
        specforge::SampleLabelingOperationResult::Issue::None,
    const specforge::SourceCollectionSessionView* rejected_view = nullptr)
{
    BeginFrame();
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 500.0f), ImGuiCond_Always);
    bool open = true;
    LabelingTaskSwitchFrameObservation observation;
    int view_reads = 0;
    specforge::PanelSessionInteraction interaction(
        [&](
            specforge::SourceCollectionSessionIntent intent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            ++observation.submission_count;
            if (intent.intent_kind() ==
                specforge::SourceCollectionSessionIntentKind::
                    ActiveSampleWorkflow) {
                observation.submitted_workflow_kind =
                    specforge::SourceCollectionPanelUiTestAccess::
                        ActiveWorkflowKind(intent);
                observation.submitted_source_identity =
                    std::string(
                        specforge::SourceCollectionPanelUiTestAccess::
                            ActiveWorkflowSourceIdentity(intent));
                observation.submitted_task_id =
                    std::string(
                        specforge::SourceCollectionPanelUiTestAccess::
                            ActiveWorkflowTaskId(intent));
            }
            specforge::SourceCollectionSessionResult result;
            if (result_message.empty()) {
                result.labeling_issue = submitted_issue;
                result.changed = submitted_issue ==
                    specforge::SampleLabelingOperationResult::Issue::None;
                result.action.workflow_changed = result.changed;
                latest_view = result.changed
                    ? activated_view
                    : (rejected_view != nullptr ? *rejected_view : frame_view);
            } else {
                result.message = result_message;
            }
            return result;
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            return view_reads++ == 0
                ? frame_view
                : latest_view;
        }
    );
    panel.RenderLabeling(
        interaction,
        &open,
        [](std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        [](specforge::SampleLabelExportFormat, std::string_view)
            -> std::optional<std::filesystem::path> {
            return std::nullopt;
        },
        observation.shortcut);
    observation.operation_message =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingOperationMessage(panel);
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
                GImGui->HoveredId == popup_window->GetID("SpecForgeTemporaryLabelingTaskAction");
        }
    }
    ImGui::EndFrame();
    return observation;
}

struct SortingPopupFrameObservation {
    int submission_count = 0;
    bool popup_open = false;
    ImGuiID add_source_button_id = 0;
    ImGuiID localized_sample_name_item_id = 0;
};

SortingPopupFrameObservation RenderSortingPopupFrame(
    specforge::SampleWorkflowPanelUi& panel,
    const specforge::SourceCollectionSessionView& view)
{
    BeginFrame();
    ImGui::SetNextWindowPos(
        ImVec2(20.0f, 20.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(520.0f, 500.0f),
        ImGuiCond_Always);
    bool open = true;
    SortingPopupFrameObservation observation;
    specforge::PanelSessionInteraction interaction(
        [&](
            specforge::SourceCollectionSessionIntent,
            std::optional<
                specforge::NavigationLatencyInputKind>) {
            ++observation.submission_count;
            return specforge::SourceCollectionSessionResult{};
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });
    panel.RenderSorting(
        interaction,
        specforge::UiLanguage::SimplifiedChinese,
        &open);
    if (ImGuiWindow* window = ImGui::FindWindowByName(
            specforge::SampleWorkflowPanelUi::
                SortingWindowName())) {
        observation.add_source_button_id =
            window->GetID("+##AddSampleSortSource");
    }
    observation.popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId |
            ImGuiPopupFlags_AnyPopupLevel);
    if (!GImGui->OpenPopupStack.empty()) {
        if (ImGuiWindow* popup_window =
                GImGui->OpenPopupStack.back().Window) {
            const ImGuiID source_id =
                popup_window->GetID("sample-name");
            const std::string_view localized_name =
                specforge::UiText(
                    specforge::UiLanguage::
                        SimplifiedChinese,
                    specforge::UiTextId::
                        SampleNameSortSource);
            observation.localized_sample_name_item_id =
                ImHashStr(
                    localized_name.data(),
                    localized_name.size(),
                    source_id);
        }
    }
    ImGui::EndFrame();
    return observation;
}

void TestCanonicalAnnotationActivationUsesSingleFileConfirmation()
{
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionAnnotationValueView canonical;
    canonical.name = "Canonical quality";
    canonical.path = "quality.asdf";
    canonical.relationship =
        specforge::SampleAnnotationWorkflowRelationship::
            ExternalLabelResult;
    canonical.labeling_owner_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    specforge::SampleWorkflowPanelUiTestAccess::
        SetPendingAnnotationActivation(
            panel,
            canonical);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                PendingAnnotationActivationOwnerFormat(panel) ==
            specforge::SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
        "canonical annotation activation should retain its owner format in pending confirmation state");

    const specforge::SampleWorkflowAnnotationActivationTextIds
        canonical_text =
            specforge::SampleWorkflowAnnotationActivationText(
                canonical.relationship,
                canonical.labeling_owner_format);
    Require(
        canonical_text.editable_message ==
                specforge::UiTextId::
                    AdoptCanonicalAsdfEditableMessage &&
            !canonical_text.detail_message,
        "canonical adoption confirmation should describe one ASDF file without legacy sidecar detail");

    const specforge::SampleWorkflowAnnotationActivationTextIds
        legacy_external =
            specforge::SampleWorkflowAnnotationActivationText(
                specforge::
                    SampleAnnotationWorkflowRelationship::
                        ExternalLabelResult,
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        LegacyNpyWithSidecar);
    Require(
        legacy_external.editable_message ==
                specforge::UiTextId::
                    UseAnnotationEditableMessage &&
            legacy_external.detail_message ==
                specforge::UiTextId::
                    ExistingLabelMetadataReused,
        "legacy external annotation confirmation should retain its existing metadata-sidecar wording");

    const specforge::SampleWorkflowAnnotationActivationTextIds
        plain_annotation =
            specforge::SampleWorkflowAnnotationActivationText(
                specforge::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation,
                specforge::
                    SampleLabelingOutputArtifactFormat::
                        None);
    Require(
        plain_annotation.editable_message ==
                specforge::UiTextId::
                    UseAnnotationEditableMessage &&
            plain_annotation.detail_message ==
                specforge::UiTextId::
                    MetadataSidecarWillBeCreated,
        "ownerless plain annotation confirmation should retain its existing sidecar-creation wording");
}

void TestCanonicalOutputActionDistinguishesDraftMigrationAndCanonicalOwner()
{
    specforge::SourceCollectionLabelingView view;
    view.has_active_task = true;
    view.active_task_is_temporary = true;
    Require(
        specforge::SampleWorkflowCanonicalOutputActionTextId(
            view) == specforge::UiTextId::SaveTo,
        "temporary task should retain the existing Save to action");

    view.active_task_is_temporary = false;
    view.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            LegacyNpyWithSidecar;
    Require(
        specforge::SampleWorkflowCanonicalOutputActionTextId(
            view) == specforge::UiTextId::MigrateToAsdf,
        "legacy owner should expose an explicit ASDF migration action");

    view.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    Require(
        !specforge::SampleWorkflowCanonicalOutputActionTextId(
            view),
        "canonical owner should not expose another ownership migration action");
}

void TestLabelingPanelSuggestsCanonicalFilenameWithoutRewritingChosenPath()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(7, 'q');
    view.labeling.active_task_is_temporary = true;
    view.labeling.task_name = "a/b:c*";

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLabelingExportFrame(panel, view);
    const auto action_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingOutputActionRect(panel);
    Require(
        action_rect.has_value(),
        "temporary task should expose the canonical output chooser action");
    const ImVec2 action_position(
        ((*action_rect)[0] + (*action_rect)[2]) * 0.5f,
        ((*action_rect)[1] + (*action_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        action_position.x,
        action_position.y);
    (void)RenderLabelingExportFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingExportFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation submitted =
        RenderLabelingExportFrame(panel, view);
    Require(
        submitted.output_path_request_count == 1 &&
            submitted.requested_output_suggested_filename ==
                "a_b_c_.asdf" &&
            submitted.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    SetActiveLabelingOutputPath &&
            submitted.submitted_path == "user-entered.final",
        "Save to should suggest a safe ASDF name while forwarding the user's chosen path unchanged");
}

void TestLabelingPanelRoutesExportLabelsAsASeparateIntent()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    const specforge::SourceCollectionSessionView active_view =
        MakeLabelingPanelView(7, 'q');

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLabelingExportFrame(panel, active_view);
    const auto export_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingExportRect(panel);
    Require(
        export_rect.has_value() &&
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormatRect(panel) &&
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
                specforge::SampleLabelExportFormat::Npy,
        "an active NPY labeling source should expose Export Labels with the NPY selector default");
    const ImVec2 export_position(
        ((*export_rect)[0] + (*export_rect)[2]) * 0.5f,
        ((*export_rect)[1] + (*export_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        export_position.x,
        export_position.y);
    (void)RenderLabelingExportFrame(panel, active_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingExportFrame(panel, active_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation submitted =
        RenderLabelingExportFrame(panel, active_view);
    Require(
        submitted.export_path_request_count == 1 &&
            submitted.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    ExportActiveLabels &&
            submitted.requested_export_format ==
                specforge::SampleLabelExportFormat::Npy &&
            submitted.requested_suggested_filename ==
                "Quality.npy" &&
            submitted.submitted_export_format ==
                specforge::SampleLabelExportFormat::Npy,
        "Export Labels should request and submit the selected stateless NPY export format");

    specforge::SourceCollectionSessionView inactive_view =
        active_view;
    inactive_view.labeling.has_active_task = false;
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLabelingExportFrame(panel, inactive_view);
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::
            LabelingExportRect(panel),
        "Export Labels should be hidden when no editable task is active");

    specforge::SourceCollectionSessionView structural_view =
        active_view;
    structural_view.labeling.output_format =
        specforge::SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    structural_view.labeling.can_export_label_values = false;
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    const LabelingExportFrameObservation structural =
        RenderLabelingExportFrame(panel, structural_view);
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::
             LabelingExportRect(panel) &&
            structural.export_path_request_count == 0 &&
            !structural.submitted_workflow_kind,
        "Export Labels should be hidden for a structural canonical task without authoritative values");
}

void TestLabelingPanelKeepsFormatOverrideUntilSourceChanges()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView npy_view =
        MakeLabelingPanelView(7, 'q');

    (void)RenderLabelingExportFrame(panel, npy_view);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
            specforge::SampleLabelExportFormat::Npy,
        "NPY source should default label export to NPY");

    SelectLabelExportFormatThroughUi(
        panel,
        npy_view,
        specforge::SampleLabelExportFormat::Csv);
    (void)RenderLabelingExportFrame(panel, npy_view);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
            specforge::SampleLabelExportFormat::Csv,
        "same-source rendering must preserve a manual CSV override");
    panel.ResetForSampleWorkflow();
    (void)RenderLabelingExportFrame(panel, npy_view);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
            specforge::SampleLabelExportFormat::Csv,
        "same-source workflow resets must not discard the export override");

    specforge::SourceCollectionSessionView next_npy_view =
        npy_view;
    next_npy_view.labeling.source_identity = "next-npy-source";
    (void)RenderLabelingExportFrame(
        panel,
        next_npy_view);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
            specforge::SampleLabelExportFormat::Npy,
        "source identity changes should reapply the NPY recommendation");

    specforge::SourceCollectionSessionView folder_view =
        npy_view;
    folder_view.labeling.source_identity = "folder-source";
    folder_view.labeling.source_kind = "folder";
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLabelingExportFrame(panel, folder_view);
    const auto export_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingExportRect(panel);
    Require(
        export_rect &&
            specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingExportFormat(panel) ==
                specforge::SampleLabelExportFormat::Csv,
        "folder source should default label export to CSV");

    const ImVec2 export_position(
        ((*export_rect)[0] + (*export_rect)[2]) * 0.5f,
        ((*export_rect)[1] + (*export_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        export_position.x,
        export_position.y);
    (void)RenderLabelingExportFrame(panel, folder_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingExportFrame(panel, folder_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation submitted =
        RenderLabelingExportFrame(panel, folder_view);
    Require(
        submitted.export_path_request_count == 1 &&
            submitted.requested_export_format ==
                specforge::SampleLabelExportFormat::Csv &&
            submitted.requested_suggested_filename ==
                "Quality.csv" &&
            submitted.submitted_export_format ==
                specforge::SampleLabelExportFormat::Csv,
        "folder default should drive both the chooser and export intent as CSV");

    SelectLabelExportFormatThroughUi(
        panel,
        folder_view,
        specforge::SampleLabelExportFormat::Npy);
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderLabelingExportFrame(panel, folder_view);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportFormat(panel) ==
            specforge::SampleLabelExportFormat::Npy,
        "same folder source should preserve a manual NPY override");
    ImGui::GetIO().AddMousePosEvent(
        export_position.x,
        export_position.y);
    (void)RenderLabelingExportFrame(panel, folder_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingExportFrame(panel, folder_view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingExportFrameObservation overridden =
        RenderLabelingExportFrame(panel, folder_view);
    Require(
        overridden.export_path_request_count == 1 &&
            overridden.requested_export_format ==
                specforge::SampleLabelExportFormat::Npy &&
            overridden.requested_suggested_filename ==
                "Quality.npy" &&
            overridden.submitted_export_format ==
                specforge::SampleLabelExportFormat::Npy,
        "folder NPY override should drive both the chooser and export intent");
}

void TestLabelExportFormatMatrixRoutesToChooserAndIntent()
{
    struct MatrixCase {
        std::string_view source_kind;
        std::optional<specforge::SampleLabelExportFormat>
            override_format;
        specforge::SampleLabelExportFormat expected_default;
        specforge::SampleLabelExportFormat expected_export;
    };
    constexpr std::array kCases = {
        MatrixCase{
            "npy",
            std::nullopt,
            specforge::SampleLabelExportFormat::Npy,
            specforge::SampleLabelExportFormat::Npy},
        MatrixCase{
            "folder",
            std::nullopt,
            specforge::SampleLabelExportFormat::Csv,
            specforge::SampleLabelExportFormat::Csv},
        MatrixCase{
            "npy",
            specforge::SampleLabelExportFormat::Csv,
            specforge::SampleLabelExportFormat::Npy,
            specforge::SampleLabelExportFormat::Csv},
        MatrixCase{
            "folder",
            specforge::SampleLabelExportFormat::Npy,
            specforge::SampleLabelExportFormat::Csv,
            specforge::SampleLabelExportFormat::Npy},
    };

    for (std::size_t case_index = 0;
         case_index < kCases.size();
         ++case_index) {
        const MatrixCase& matrix_case = kCases[case_index];
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView view =
            MakeLabelingPanelView(7, 'q');
        view.labeling.source_identity =
            "export-matrix-" +
            std::to_string(case_index);
        view.labeling.source_kind =
            matrix_case.source_kind;

        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        const LabelingExportFrameObservation initialized =
            RenderLabelingExportFrame(panel, view);
        Require(
            !initialized.requested_export_format &&
                !initialized.submitted_workflow_kind &&
                specforge::SampleWorkflowPanelUiTestAccess::
                        LabelingExportFormat(panel) ==
                    matrix_case.expected_default,
            "rendering an export format recommendation must not execute an export");

        if (matrix_case.override_format) {
            SelectLabelExportFormatThroughUi(
                panel,
                view,
                *matrix_case.override_format);
            const LabelingExportFrameObservation overridden =
                RenderLabelingExportFrame(panel, view);
            Require(
                !overridden.requested_export_format &&
                    !overridden.submitted_workflow_kind &&
                    specforge::SampleWorkflowPanelUiTestAccess::
                            LabelingExportFormat(panel) ==
                        matrix_case.expected_export,
                "switching the export format must remain a session-only preference until export is requested");
        }

        const auto export_rect =
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingExportRect(panel);
        Require(
            export_rect.has_value(),
            "each export matrix case should expose the export action");
        const ImVec2 export_position(
            ((*export_rect)[0] + (*export_rect)[2]) *
                0.5f,
            ((*export_rect)[1] + (*export_rect)[3]) *
                0.5f);
        ImGui::GetIO().AddMousePosEvent(
            export_position.x,
            export_position.y);
        (void)RenderLabelingExportFrame(panel, view);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        (void)RenderLabelingExportFrame(panel, view);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        const LabelingExportFrameObservation exported =
            RenderLabelingExportFrame(panel, view);
        const std::string expected_suggested_filename =
            matrix_case.expected_export ==
                    specforge::SampleLabelExportFormat::Csv
                ? "Quality.csv"
                : "Quality.npy";
        Require(
            exported.export_path_request_count == 1 &&
                exported.requested_export_format ==
                    matrix_case.expected_export &&
                exported.requested_suggested_filename ==
                    expected_suggested_filename &&
                exported.submitted_workflow_kind ==
                    specforge::
                        ActiveSampleWorkflowIntentKind::
                            ExportActiveLabels &&
                exported.submitted_export_format ==
                    matrix_case.expected_export,
            "the selected matrix format must reach both the path chooser and export intent");
    }
}

void TestLabelExportFormatControlsDefaultExtension()
{
    Require(
        specforge::RecommendedSampleLabelExportFormat(
            "folder") ==
                specforge::SampleLabelExportFormat::Csv &&
            specforge::RecommendedSampleLabelExportFormat(
                "npy") ==
                specforge::SampleLabelExportFormat::Npy,
        "source-aware recommendations should select CSV only for folder sources");
    Require(
        specforge::EnsureSampleLabelExportPathExtension(
            "labels",
            specforge::SampleLabelExportFormat::Npy) ==
                std::filesystem::path{"labels.npy"} &&
            specforge::EnsureSampleLabelExportPathExtension(
                "labels",
                specforge::SampleLabelExportFormat::Csv) ==
                std::filesystem::path{"labels.csv"},
        "missing export extensions should follow the selected format");
    Require(
        specforge::EnsureSampleLabelExportPathExtension(
            "labels.txt",
            specforge::SampleLabelExportFormat::Csv) ==
            std::filesystem::path{"labels.txt"},
        "an explicit extension should not be silently rewritten");
}

void TestShortcutDisplayUsesKeyboardLegends()
{
    Require(specforge::FormatSampleLabelShortcut('q') == "Q", "lowercase shortcut should display as Q");
    Require(specforge::FormatSampleLabelShortcut('Q') == "Q", "uppercase input should display canonically");
    Require(specforge::FormatSampleLabelShortcut('3') == "3", "digit shortcut should retain its legend");
    Require(specforge::FormatSampleLabelShortcut('\0') == "None", "unbound shortcut should display as None");
}

void TestAddSortSourcePopupLocalizesBuiltInSampleName()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view;
    view.sorting.has_active_source = true;
    view.sorting.available_sources.push_back(
        {
            .id = "sample-name",
            .name = "Sample name",
        });

    SortingPopupFrameObservation observation =
        RenderSortingPopupFrame(panel, view);
    Require(
        observation.add_source_button_id != 0,
        "integration fixture should find the add-sort-source button");
    ImGui::ActivateItemByID(
        observation.add_source_button_id);
    observation = RenderSortingPopupFrame(panel, view);
    Require(
        observation.popup_open &&
            observation.localized_sample_name_item_id != 0,
        "integration fixture should open the add-sort-source popup");

    ImGui::ActivateItemByID(
        observation.localized_sample_name_item_id);
    observation = RenderSortingPopupFrame(panel, view);
    Require(
        observation.submission_count == 1,
        "the Chinese add-sort-source popup should expose the built-in sample-name item as 样本名称");
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

    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "integration fixture should expose a deterministic labeling task selector rectangle");
    const ImVec2 selector_position(
        ((*selector_rect)[0] + (*selector_rect)[2]) * 0.5f,
        ((*selector_rect)[1] + (*selector_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(selector_position.x, selector_position.y);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    Require(observation.selector_hovered, "integration fixture should locate the labeling task selector");
    ImGui::GetIO().AddMousePosEvent(selector_position.x, selector_position.y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    Require(observation.popup_open, "labeling task selector should open for the integration fixture");

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false);
    const auto temporary_action_rect =
        specforge::SampleWorkflowPanelUiTestAccess::TemporaryLabelingActionRect(panel);
    Require(
        temporary_action_rect.has_value(),
        "integration fixture should expose a deterministic temporary task action rectangle");
    const ImVec2 temporary_action_position(
        ((*temporary_action_rect)[0] + (*temporary_action_rect)[2]) * 0.5f,
        ((*temporary_action_rect)[1] + (*temporary_action_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(temporary_action_position.x, temporary_action_position.y);
    observation = RenderLabelingTaskSwitchFrame(panel, inactive_view, latest_view, activated_view, false);
    Require(
        observation.temporary_action_hovered,
        "integration fixture should locate the temporary task action");
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

void TestLabelingPanelUsesIdentityForSingleDraftResume()
{
    const specforge::SourceCollectionSessionView stale_view =
        MakeRecoveryPanelView();
    specforge::SourceCollectionSessionView activated_view = stale_view;
    activated_view.labeling.has_active_task = true;
    activated_view.labeling.active_task_is_temporary = true;
    activated_view.labeling.task_id =
        stale_view.labeling.recovery_drafts.front().task_id;
    activated_view.labeling.task_name =
        stale_view.labeling.recovery_drafts.front().task_name;

    const auto submit_single_draft_resume = [
        &stale_view,
        &activated_view](
        const specforge::SourceCollectionSessionView& rejected_view,
        specforge::SampleLabelingOperationResult::Issue issue) {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = stale_view;

        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        LabelingTaskSwitchFrameObservation observation =
            RenderLabelingTaskSwitchFrame(
                panel,
                stale_view,
                latest_view,
                activated_view,
                true);
        const auto selector_rect =
            specforge::SampleWorkflowPanelUiTestAccess::LabelingSelectorRect(panel);
        Require(
            selector_rect.has_value(),
            "single-draft resume fixture should expose a deterministic selector rectangle");
        const ImVec2 selector_position(
            ((*selector_rect)[0] + (*selector_rect)[2]) * 0.5f,
            ((*selector_rect)[1] + (*selector_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            selector_position.x,
            selector_position.y);
        observation = RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        Require(
            observation.selector_hovered,
            "single-draft resume fixture should locate the task selector");
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        (void)RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        observation = RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        Require(
            observation.popup_open,
            "single-draft resume fixture should open the task selector");

        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        observation = RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        const auto temporary_action_rect =
            specforge::SampleWorkflowPanelUiTestAccess::TemporaryLabelingActionRect(panel);
        Require(
            temporary_action_rect.has_value(),
            "single-draft resume fixture should expose a deterministic generic action rectangle");
        const ImVec2 resume_position(
            ((*temporary_action_rect)[0] + (*temporary_action_rect)[2]) * 0.5f,
            ((*temporary_action_rect)[1] + (*temporary_action_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            resume_position.x,
            resume_position.y);
        observation = RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        Require(
            observation.temporary_action_hovered,
            "single-draft resume fixture should locate its generic action");
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        (void)RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        observation = RenderLabelingTaskSwitchFrame(
            panel,
            stale_view,
            latest_view,
            activated_view,
            false,
            std::string{},
            issue,
            &rejected_view);
        return observation;
    };

    specforge::SourceCollectionSessionView missing_view = stale_view;
    missing_view.labeling.has_temporary_task = false;
    missing_view.labeling.recovery_drafts.clear();
    const LabelingTaskSwitchFrameObservation missing =
        submit_single_draft_resume(
            missing_view,
            specforge::SampleLabelingOperationResult::Issue::
                EditTargetChanged);
    Require(
        missing.submission_count == 1 &&
            missing.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RecoverTemporaryLabelingTask &&
            missing.submitted_source_identity ==
                stale_view.labeling.source_identity &&
            missing.submitted_task_id ==
                stale_view.labeling.recovery_drafts.front().task_id,
        "a missing single draft must submit identity-checked recovery");

    specforge::SourceCollectionSessionView formalized_view =
        MakeLabelingPanelView(8, 'g');
    formalized_view.labeling.source_identity =
        stale_view.labeling.source_identity;
    formalized_view.labeling.task_id = "formalized-task";
    formalized_view.labeling.task_name = "Formalized task";
    const LabelingTaskSwitchFrameObservation formalized =
        submit_single_draft_resume(
            formalized_view,
            specforge::SampleLabelingOperationResult::Issue::
                EditTargetChanged);
    Require(
        formalized.submission_count == 1 &&
            formalized.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RecoverTemporaryLabelingTask &&
            formalized.submitted_source_identity ==
                stale_view.labeling.source_identity &&
            formalized.submitted_task_id ==
                stale_view.labeling.recovery_drafts.front().task_id,
        "a formalized single draft must submit identity-checked recovery");
}

void TestLabelingPanelRoutesTemporaryDraftRecoveryActions()
{
    const specforge::SourceCollectionSessionView recovery_view =
        MakeRecoveryPanelView();
    const ImVec2 compact_window_size(650.0f, 700.0f);

    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = recovery_view;
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            compact_window_size);
        const auto recover_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                recovery_view,
                0,
                "SpecForgeRecoverTemporaryDraft");
        const auto keep_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                recovery_view,
                0,
                "SpecForgeKeepTemporaryDraft");
        const auto delete_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                recovery_view,
                0,
                "SpecForgeDeleteTemporaryDraft");
        Require(
            recover_rect.has_value() && keep_rect.has_value() && delete_rect.has_value(),
            "the recovery list should expose deterministic Recover, Keep, and Delete rectangles");
        const ImVec2 recover_position(
            ((*recover_rect)[0] + (*recover_rect)[2]) * 0.5f,
            ((*recover_rect)[1] + (*recover_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            recover_position.x,
            recover_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        const RecoveryFrameObservation observation =
            RenderRecoveryFrame(
                panel,
                recovery_view,
                latest_view,
                false,
                specforge::UiLanguage::English,
                compact_window_size);
        Require(
            observation.submission_count == 1 &&
                observation.submitted_workflow_kind ==
                    specforge::ActiveSampleWorkflowIntentKind::
                        RecoverTemporaryLabelingTask &&
                observation.submitted_source_identity ==
                    recovery_view.labeling.source_identity &&
                observation.submitted_task_id ==
                    recovery_view.labeling.recovery_drafts.front().task_id,
            "Recover should submit the exact source and task identity");
    }

    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = recovery_view;
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            compact_window_size);
        const auto keep_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                recovery_view,
                0,
                "SpecForgeKeepTemporaryDraft");
        Require(
            keep_rect.has_value(),
            "the recovery list should expose a deterministic Keep rectangle");
        const ImVec2 keep_position(
            ((*keep_rect)[0] + (*keep_rect)[2]) * 0.5f,
            ((*keep_rect)[1] + (*keep_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            keep_position.x,
            keep_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        const RecoveryFrameObservation observation =
            RenderRecoveryFrame(
                panel,
                recovery_view,
                latest_view,
                false,
                specforge::UiLanguage::English,
                compact_window_size);
        Require(
            observation.submission_count == 0 &&
                specforge::SampleWorkflowPanelUiTestAccess::
                    IsRecoveryDraftRetained(
                        panel,
                        recovery_view.labeling.source_identity,
                        recovery_view.labeling.recovery_drafts.front().task_id),
            "Keep draft should acknowledge the recovery row locally without a workflow mutation");
        specforge::SourceCollectionSessionView other_source_view =
            recovery_view;
        other_source_view.labeling.source_identity = "source/other";
        (void)RenderRecoveryFrame(
            panel,
            other_source_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "switching sources must not clear a session-local Keep acknowledgement");
        panel.ResetForSampleWorkflow();
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "resetting workflow presentation must not end the UI session acknowledgement");
        specforge::SourceCollectionSessionView unrelated_view = recovery_view;
        unrelated_view.labeling.recovery_revision =
            recovery_view.labeling.recovery_revision + 1;
        unrelated_view.labeling.recovery_drafts.push_back(
            {
                .task_id = "draft-2",
                .task_name = "Unrelated draft",
                .status = specforge::SampleLabelingRecoveryDraftStatus::Stale,
                .labeled_count = 0,
                .sample_count = 5,
            });
        (void)RenderRecoveryFrame(
            panel,
            unrelated_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "an unrelated recovery revision should not clear this draft's Keep acknowledgement");
        specforge::SourceCollectionSessionView changed_view = recovery_view;
        changed_view.labeling.recovery_drafts.front().labeled_count = 3;
        (void)RenderRecoveryFrame(
            panel,
            changed_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            !specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "a changed recovery row should clear its Keep acknowledgement");
        specforge::SourceCollectionSessionView empty_view = recovery_view;
        empty_view.labeling.recovery_drafts.clear();
        (void)RenderRecoveryFrame(
            panel,
            empty_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            !specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "removing a recovery row should clear its local Keep acknowledgement");
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        Require(
            !specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetained(
                    panel,
                    recovery_view.labeling.source_identity,
                    recovery_view.labeling.recovery_drafts.front().task_id),
            "a same-ID replacement draft should not inherit the old Keep acknowledgement");
    }

    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = recovery_view;
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            compact_window_size);
        const auto delete_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                recovery_view,
                0,
                "SpecForgeDeleteTemporaryDraft");
        Require(
            delete_rect.has_value(),
            "the recovery list should expose a deterministic Delete rectangle");
        const ImVec2 delete_position(
            ((*delete_rect)[0] + (*delete_rect)[2]) * 0.5f,
            ((*delete_rect)[1] + (*delete_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            delete_position.x,
            delete_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        RecoveryFrameObservation observation =
            RenderRecoveryFrame(
                panel,
                recovery_view,
                latest_view,
                false,
                specforge::UiLanguage::English,
                compact_window_size);
        Require(
            observation.submission_count == 0 && observation.popup_open,
            "Delete draft should open the existing confirmation modal");
        Require(
            observation.popup_confirm_id != 0,
            "Delete draft confirmation should expose its stable button ID");

        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        observation = RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size);
        const auto confirm_rect =
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingDeleteConfirmationRect(panel);
        Require(
            confirm_rect.has_value(),
            "Delete draft fixture should expose a deterministic confirmation rectangle");
        const ImVec2 confirm_position(
            ((*confirm_rect)[0] + (*confirm_rect)[2]) * 0.5f,
            ((*confirm_rect)[1] + (*confirm_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            confirm_position.x,
            confirm_position.y);
        const RecoveryFrameObservation confirm_hovered =
            RenderRecoveryFrame(
                panel,
                recovery_view,
                latest_view,
                false,
                specforge::UiLanguage::English,
                compact_window_size);
        Require(
            confirm_hovered.popup_open &&
                confirm_hovered.hovered_id == confirm_hovered.popup_confirm_id &&
                confirm_hovered.popup_confirm_id == observation.popup_confirm_id,
            "Delete draft fixture should locate the confirmation button");
        ImGui::GetIO().AddMousePosEvent(
            confirm_position.x,
            confirm_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size,
            specforge::SampleLabelingOperationResult::Issue::EditTargetChanged);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        const RecoveryFrameObservation confirmed = RenderRecoveryFrame(
            panel,
            recovery_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            compact_window_size,
            specforge::SampleLabelingOperationResult::Issue::EditTargetChanged);
        Require(
            confirmed.submission_count == 1 &&
                confirmed.submitted_workflow_kind ==
                    specforge::ActiveSampleWorkflowIntentKind::
                        DeleteTemporaryLabelingTask &&
                confirmed.submitted_source_identity ==
                    recovery_view.labeling.source_identity &&
                confirmed.submitted_task_id ==
                    recovery_view.labeling.recovery_drafts.front().task_id,
            "Delete draft confirmation should submit the exact source and task identity");
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingOperationMessage(panel) ==
                specforge::UiText(
                    specforge::UiLanguage::English,
                    specforge::UiTextId::LabelingDeleteTargetChanged),
            "Delete target changes should use delete-specific feedback");
    }
}

void TestLabelingPanelKeepsDuplicateRecoveryRowsIndependently()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    const specforge::SourceCollectionSessionView duplicate_view =
        MakeDuplicateRecoveryPanelView();
    specforge::SourceCollectionSessionView latest_view = duplicate_view;
    const ImVec2 compact_window_size(650.0f, 900.0f);

    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        compact_window_size);
    const auto second_keep_rect =
        specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
            panel,
            duplicate_view,
            1,
            "SpecForgeKeepTemporaryDraft");
    Require(
        second_keep_rect.has_value(),
        "duplicate recovery rows should expose a deterministic second Keep rectangle");
    const ImVec2 second_keep_position(
        ((*second_keep_rect)[0] + (*second_keep_rect)[2]) * 0.5f,
        ((*second_keep_rect)[1] + (*second_keep_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        second_keep_position.x,
        second_keep_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetainedAt(
                    panel,
                    duplicate_view,
                    1) &&
            !specforge::SampleWorkflowPanelUiTestAccess::
                IsRecoveryDraftRetainedAt(
                    panel,
                    duplicate_view,
                    0),
        "Keep should acknowledge only the selected duplicate task row");

    specforge::SourceCollectionSessionView changed_view = duplicate_view;
    changed_view.labeling.recovery_drafts[1].labeled_count = 3;
    latest_view = changed_view;
    (void)RenderRecoveryFrame(
        panel,
        changed_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::
            IsRecoveryDraftRetainedAt(panel, changed_view, 1),
        "changing a kept duplicate row should clear only its row token");

    latest_view = duplicate_view;
    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    const auto first_keep_rect =
        specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
            panel,
            duplicate_view,
            0,
            "SpecForgeKeepTemporaryDraft");
    Require(
        first_keep_rect.has_value(),
        "the first duplicate recovery row should expose a deterministic Keep rectangle");
    const ImVec2 first_keep_position(
        ((*first_keep_rect)[0] + (*first_keep_rect)[2]) * 0.5f,
        ((*first_keep_rect)[1] + (*first_keep_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        first_keep_position.x,
        first_keep_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderRecoveryFrame(
        panel,
        duplicate_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
            IsRecoveryDraftRetainedAt(panel, duplicate_view, 0),
        "the other duplicate row should remain independently retainable");

    specforge::SourceCollectionSessionView removed_view = duplicate_view;
    removed_view.labeling.recovery_drafts.erase(
        removed_view.labeling.recovery_drafts.begin());
    latest_view = removed_view;
    (void)RenderRecoveryFrame(
        panel,
        removed_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::
            IsRecoveryDraftRetainedAt(panel, duplicate_view, 0),
        "a removed duplicate row should clear its own acknowledgement");
}

void TestLabelingPanelDisablesAmbiguousRecoveryActionsAfterRepair()
{
    const specforge::SourceCollectionSessionView duplicate_view =
        MakeDuplicateRecoveryPanelView();
    specforge::SourceCollectionSessionView repaired_view = duplicate_view;
    repaired_view.labeling.recovery_drafts.erase(
        repaired_view.labeling.recovery_drafts.begin() + 1);
    const ImVec2 compact_window_size(650.0f, 900.0f);
    const auto assert_ambiguous_action_is_blocked = [
        &duplicate_view,
        &repaired_view,
        &compact_window_size](std::string_view stable_id) {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = duplicate_view;
        (void)RenderRecoveryFrame(
            panel,
            duplicate_view,
            latest_view,
            true,
            specforge::UiLanguage::SimplifiedChinese,
            compact_window_size);
        latest_view = repaired_view;
        const RecoveryFrameObservation initial = RenderRecoveryFrame(
            panel,
            duplicate_view,
            latest_view,
            false,
            specforge::UiLanguage::SimplifiedChinese,
            compact_window_size,
            specforge::SampleLabelingOperationResult::Issue::None,
            true,
            &repaired_view);
        const std::string_view conflict_text = specforge::UiText(
            specforge::UiLanguage::SimplifiedChinese,
            specforge::UiTextId::TemporaryDraftDuplicateIdentity);
        Require(
            initial.logged_text.find(std::string(conflict_text)) !=
                std::string::npos,
            "ambiguous duplicate rows should show localized conflict feedback");
        const auto action_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                duplicate_view,
                1,
                stable_id);
        Require(
            action_rect.has_value(),
            "ambiguous duplicate actions should expose deterministic rectangles");
        const ImVec2 action_position(
            ((*action_rect)[0] + (*action_rect)[2]) * 0.5f,
            ((*action_rect)[1] + (*action_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            action_position.x,
            action_position.y);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        (void)RenderRecoveryFrame(
            panel,
            duplicate_view,
            latest_view,
            false,
            specforge::UiLanguage::SimplifiedChinese,
            compact_window_size,
            specforge::SampleLabelingOperationResult::Issue::None,
            false,
            &repaired_view);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        const RecoveryFrameObservation released = RenderRecoveryFrame(
            panel,
            duplicate_view,
            latest_view,
            false,
            specforge::UiLanguage::SimplifiedChinese,
            compact_window_size,
            specforge::SampleLabelingOperationResult::Issue::None,
            false,
            &repaired_view);
        Require(
            released.submission_count == 0 &&
                !released.popup_open,
            "a repaired same-ID cache must not submit ambiguous Recover/Delete");
    };

    assert_ambiguous_action_is_blocked(
        "SpecForgeRecoverTemporaryDraft");
    assert_ambiguous_action_is_blocked(
        "SpecForgeDeleteTemporaryDraft");
}

void TestLabelingPanelRequiresIdentityForMultipleRecoveryDrafts()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.source_identity = "source/multiple-drafts";
    view.labeling.task_id = "formal-task";
    view.labeling.task_name = "Formal task";
    view.labeling.active_task_is_temporary = false;
    view.labeling.has_temporary_task = true;
    view.labeling.recovery_drafts = {
        {
            .task_id = "draft-1",
            .task_name = "Draft one",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Recoverable,
            .sample_count = 1,
        },
        {
            .task_id = "draft-2",
            .task_name = "Draft two",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Conflicting,
            .sample_count = 1,
        },
    };
    specforge::SourceCollectionSessionView latest_view = view;

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    LabelingTaskSwitchFrameObservation observation =
        RenderLabelingTaskSwitchFrame(
            panel,
            view,
            latest_view,
            view,
            true);
    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "multiple-draft fixture should expose a deterministic selector rectangle");
    const ImVec2 selector_position(
        ((*selector_rect)[0] + (*selector_rect)[2]) * 0.5f,
        ((*selector_rect)[1] + (*selector_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        selector_position.x,
        selector_position.y);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        view,
        latest_view,
        view,
        false);
    Require(
        observation.selector_hovered,
        "multiple-draft fixture should locate the task selector");
    ImGui::GetIO().AddMousePosEvent(
        selector_position.x,
        selector_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingTaskSwitchFrame(
        panel,
        view,
        latest_view,
        view,
        false);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        view,
        latest_view,
        view,
        false);
    Require(
        observation.popup_open,
        "multiple-draft fixture should open the task selector");

    const auto temporary_action_rect =
        specforge::SampleWorkflowPanelUiTestAccess::TemporaryLabelingActionRect(panel);
    const bool generic_resume_hovered = temporary_action_rect.has_value();
    Require(
        !generic_resume_hovered && observation.submission_count == 0,
        "multiple recovery drafts must not expose an identity-free Resume labeling draft action");
}

void TestLabelingPanelDisablesFormalAndTemporaryIdentityAmbiguity()
{
    const specforge::SourceCollectionSessionView ambiguous_view =
        MakeFormalTaskWithAmbiguousRecoveryDraftView();

    const auto assert_row_action_disabled = [
        &ambiguous_view](std::string_view stable_id,
                         std::string_view message) {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = ambiguous_view;
        const ImVec2 window_size(650.0f, 700.0f);
        (void)RenderRecoveryFrame(
            panel,
            ambiguous_view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            window_size);
        const auto action_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                ambiguous_view,
                0,
                stable_id);
        Require(
            action_rect.has_value(),
            message);
        const ImVec2 action_position(
            ((*action_rect)[0] + (*action_rect)[2]) * 0.5f,
            ((*action_rect)[1] + (*action_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            action_position.x,
            action_position.y);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        (void)RenderRecoveryFrame(
            panel,
            ambiguous_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        const RecoveryFrameObservation released = RenderRecoveryFrame(
            panel,
            ambiguous_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size);
        Require(
            released.submission_count == 0 && !released.popup_open,
            message);
    };

    assert_row_action_disabled(
        "SpecForgeRecoverTemporaryDraft",
        "a formal/temp duplicate ID should disable recovery");
    assert_row_action_disabled(
        "SpecForgeDeleteTemporaryDraft",
        "a formal/temp duplicate ID should disable deletion");

    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView latest_view = ambiguous_view;
    const ImVec2 window_size(650.0f, 700.0f);
    (void)RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        window_size);
    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "a formal/temp duplicate ID should expose a selector rectangle");
    const ImVec2 selector_position(
        ((*selector_rect)[0] + (*selector_rect)[2]) * 0.5f,
        ((*selector_rect)[1] + (*selector_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        selector_position.x,
        selector_position.y);
    (void)RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    RecoveryFrameObservation selector_released = RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    Require(
        selector_released.popup_open,
        "a formal/temp duplicate ID should open the selector for the disabled Resume check");
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    selector_released = RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    const auto temporary_action_rect =
        specforge::SampleWorkflowPanelUiTestAccess::TemporaryLabelingActionRect(panel);
    Require(
        temporary_action_rect.has_value(),
        "a formal/temp duplicate ID should expose the disabled Resume rectangle");
    const ImVec2 temporary_action_position(
        ((*temporary_action_rect)[0] + (*temporary_action_rect)[2]) * 0.5f,
        ((*temporary_action_rect)[1] + (*temporary_action_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        temporary_action_position.x,
        temporary_action_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const RecoveryFrameObservation resume_released = RenderRecoveryFrame(
        panel,
        ambiguous_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    Require(
        resume_released.submission_count == 0,
        "a formal/temp duplicate ID should disable generic Resume");
}

void TestLabelingPanelDeleteModalShowsRecoveryIdentity()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    view.labeling.recovery_drafts.push_back(
        {
            .task_id = "draft-2",
            .task_name = "Second draft",
            .status = specforge::SampleLabelingRecoveryDraftStatus::Stale,
            .sample_count = 5,
    });
    specforge::SourceCollectionSessionView latest_view = view;
    const ImVec2 compact_window_size(650.0f, 700.0f);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        compact_window_size);
    const auto delete_rect =
        specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
            panel,
            view,
            0,
            "SpecForgeDeleteTemporaryDraft");
    Require(
        delete_rect.has_value(),
        "multiple-draft fixture should expose a deterministic selected draft Delete rectangle");
    const ImVec2 delete_position(
        ((*delete_rect)[0] + (*delete_rect)[2]) * 0.5f,
        ((*delete_rect)[1] + (*delete_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        delete_position.x,
        delete_position.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const RecoveryFrameObservation opened = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        compact_window_size,
        specforge::SampleLabelingOperationResult::Issue::None,
        true);
    Require(
        opened.popup_open &&
            opened.logged_text.find("Recovered draft") != std::string::npos &&
            opened.logged_text.find("draft-1") != std::string::npos &&
            opened.logged_text.find("source/recovery") != std::string::npos,
        "recovery delete confirmation should show the selected draft name and identity");
}

void TestLabelingPanelPlacesSelectorBeforeRecoveryList()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    const specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    specforge::SourceCollectionSessionView latest_view = view;
    const RecoveryFrameObservation observation = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        ImVec2(650.0f, 700.0f),
        specforge::SampleLabelingOperationResult::Issue::None,
        true);
    const std::size_t selector_position = observation.logged_text.find(
        "Select labeling task");
    const std::size_t recovery_position = observation.logged_text.find(
        std::string(
            specforge::UiText(
                specforge::UiLanguage::English,
                specforge::UiTextId::TemporaryDraftRecovery)));
    Require(
        selector_position != std::string::npos &&
            recovery_position != std::string::npos &&
            selector_position < recovery_position,
        "the labeling selector should be rendered before the recovery list");
}

void TestLabelingPanelKeepsSelectorRowAboveRecoveryListAtAllWidths()
{
    const std::array<ImVec2, 2> window_sizes = {
        ImVec2(900.0f, 900.0f),
        ImVec2(650.0f, 900.0f),
    };
    const auto assert_selector_row_layout = [
        &window_sizes](
        const specforge::SourceCollectionSessionView& view,
        bool expect_recovery_action) {
        for (const ImVec2 window_size : window_sizes) {
            ScopedImGuiContext context;
            specforge::SampleWorkflowPanelUi panel;
            specforge::SourceCollectionSessionView latest_view = view;
            const RecoveryFrameObservation rendered = RenderRecoveryFrame(
                panel,
                view,
                latest_view,
                true,
                specforge::UiLanguage::English,
                window_size);
            const auto selector_rect =
                specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingSelectorRect(panel);
            const auto pause_rect =
                specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingPauseRect(panel);
            const auto delete_rect =
                specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingDeleteRect(panel);
            const auto recovery_rect =
                specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingRecoveryRect(panel);
            Require(
                selector_rect && pause_rect && delete_rect && recovery_rect,
                "labeling layout fixture should expose all first-row and recovery rectangles");
            Require(
                std::fabs((*selector_rect)[1] - (*pause_rect)[1]) <= 1.0f &&
                    std::fabs((*pause_rect)[1] - (*delete_rect)[1]) <= 1.0f,
                "selector, Pause, and Delete should share the first-row baseline");
            Require(
                (*recovery_rect)[1] >=
                    std::max((*pause_rect)[3], (*delete_rect)[3]) &&
                    (*recovery_rect)[3] > (*recovery_rect)[1],
                "the recovery rectangle should begin below the complete selector row");
            Require(
                (*selector_rect)[0] >= 20.0f &&
                    (*selector_rect)[2] <= 20.0f + window_size.x &&
                    (*pause_rect)[0] >= 20.0f &&
                    (*delete_rect)[2] <= 20.0f + window_size.x,
                "selector row controls should remain inside the labeling window");
            if (expect_recovery_action) {
                Require(
                    (*recovery_rect)[1] > (*delete_rect)[3],
                    "formal-task recovery actions should begin below Pause and Delete");
            }
            (void)rendered;
        }
    };

    assert_selector_row_layout(
        MakeActiveTemporaryRecoveryPanelView(),
        false);
    assert_selector_row_layout(
        MakeFormalTaskWithRecoveryDraftView(),
        true);
}

void TestLabelingPanelRendersCurrentTaskDeleteModalOnce()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView active_view =
        MakeLabelingPanelView(8, 'g');
    active_view.labeling.source_identity = "source/active";
    specforge::SourceCollectionSessionView latest_view = active_view;
    const auto render_active_frame = [&](bool request_initial_focus) {
        return RenderRecoveryFrame(
            panel,
            active_view,
            latest_view,
            request_initial_focus,
            specforge::UiLanguage::English,
            ImVec2(700.0f, 500.0f));
    };

    (void)render_active_frame(true);
    const auto delete_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingDeleteRect(panel);
    Require(
        delete_rect.has_value(),
        "active delete integration fixture should expose a deterministic delete rectangle");
    const ImVec2 delete_task_position(
        ((*delete_rect)[0] + (*delete_rect)[2]) * 0.5f,
        ((*delete_rect)[1] + (*delete_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        delete_task_position.x,
        delete_task_position.y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const RecoveryFrameObservation opened =
        render_active_frame(false);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Require(
        !opened.popup_open,
        "the current-task delete control should open its modal for the next frame");
    const RecoveryFrameObservation rendered =
        render_active_frame(false);
    Require(
        rendered.popup_open,
        "the current-task delete path should render one confirmation modal");
}

void TestLabelingPanelRejectsCrossFrameCurrentTaskDelete()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView frame_view =
        MakeLabelingPanelView(8, 'g');
    frame_view.labeling.source_identity = "source/old";
    frame_view.labeling.task_id = "task-old";
    specforge::SourceCollectionSessionView latest_view = frame_view;
    auto render_active_frame = [&]() {
        return RenderRecoveryFrame(
            panel,
            frame_view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            ImVec2(700.0f, 500.0f));
    };

    (void)render_active_frame();
    const auto delete_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingDeleteRect(panel);
    Require(
        delete_rect.has_value(),
        "cross-frame delete fixture should expose a deterministic delete rectangle");
    const ImVec2 delete_task_position(
        ((*delete_rect)[0] + (*delete_rect)[2]) * 0.5f,
        ((*delete_rect)[1] + (*delete_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        delete_task_position.x,
        delete_task_position.y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)render_active_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const RecoveryFrameObservation opened = render_active_frame();
    Require(opened.popup_open, "cross-frame delete fixture should open the confirmation modal");

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)render_active_frame();
    latest_view = frame_view;
    latest_view.labeling.source_identity = "source/new";
    latest_view.labeling.task_id = "task-new";
    const auto confirm_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingDeleteConfirmationRect(panel);
    Require(
        confirm_rect.has_value(),
        "cross-frame delete fixture should expose a deterministic confirmation rectangle");
    const ImVec2 confirm_position(
        ((*confirm_rect)[0] + (*confirm_rect)[2]) * 0.5f,
        ((*confirm_rect)[1] + (*confirm_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        confirm_position.x,
        confirm_position.y);
    const RecoveryFrameObservation confirm_hovered = render_active_frame();
    Require(
        confirm_hovered.popup_open &&
            confirm_hovered.hovered_id == confirm_hovered.popup_confirm_id &&
            confirm_hovered.popup_confirm_id != 0,
        "cross-frame delete fixture should locate the confirmation button");
    ImGui::GetIO().AddMousePosEvent(
        confirm_position.x,
        confirm_position.y);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)render_active_frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const RecoveryFrameObservation confirmed = render_active_frame();
    Require(
        confirmed.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::LabelingOperationMessage(panel) ==
                specforge::UiText(
                    specforge::UiLanguage::English,
                    specforge::UiTextId::LabelingDeleteTargetChanged),
        "a current-task delete confirmation must reject a changed source/task identity");
}

void TestLabelingPanelPreservesEditingStateWhenDeletingRecoveryDraft()
{
    const auto make_view = []() {
        specforge::SourceCollectionSessionView view =
            MakeLabelingPanelView(8, 'g');
        view.labeling.source_identity = "source/shared";
        view.labeling.task_id = "formal-task";
        view.labeling.task_name = "Formal task";
        view.labeling.active_task_is_temporary = false;
        view.labeling.recovery_drafts =
            MakeRecoveryPanelView().labeling.recovery_drafts;
        return view;
    };
    const auto find_and_confirm_delete = [](
                                           specforge::SampleWorkflowPanelUi& panel,
                                           const specforge::SourceCollectionSessionView& view,
                                           specforge::SourceCollectionSessionView& latest_view,
                                           specforge::SampleLabelingOperationResult::Issue issue) {
        const ImVec2 window_size(233.0f, 700.0f);
        (void)RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            window_size);
        specforge::SampleWorkflowPanelUiTestAccess::SetActiveTaskId(
            panel,
            view.labeling.task_id);
        const auto delete_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                view,
                0,
                "SpecForgeDeleteTemporaryDraft");
        Require(
            delete_rect.has_value(),
            "editing-state fixture should expose a deterministic recovery Delete rectangle");
        const ImVec2 delete_position(
            ((*delete_rect)[0] + (*delete_rect)[2]) * 0.5f,
            ((*delete_rect)[1] + (*delete_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(delete_position.x, delete_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        RecoveryFrameObservation opened = RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size);
        Require(opened.popup_open, "editing-state fixture should open recovery deletion confirmation");
        ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
        opened = RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size);
        specforge::SampleWorkflowPanelUiTestAccess::SetEditingLabelCode(
            panel,
            8);
        specforge::SampleWorkflowPanelUiTestAccess::SetShortcutCaptureActive(
            panel,
            true);

        const auto confirm_rect =
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingDeleteConfirmationRect(panel);
        Require(
            confirm_rect.has_value(),
            "editing-state fixture should expose a deterministic recovery confirmation rectangle");
        const ImVec2 confirm_position(
            ((*confirm_rect)[0] + (*confirm_rect)[2]) * 0.5f,
            ((*confirm_rect)[1] + (*confirm_rect)[3]) * 0.5f);
        ImGui::GetIO().AddMousePosEvent(
            confirm_position.x,
            confirm_position.y);
        const RecoveryFrameObservation confirm_hovered = RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size,
            issue);
        Require(
            confirm_hovered.popup_open &&
                confirm_hovered.popup_confirm_id != 0 &&
                confirm_hovered.hovered_id == confirm_hovered.popup_confirm_id,
            "editing-state fixture should locate recovery confirmation");
        ImGui::GetIO().AddMousePosEvent(confirm_position.x, confirm_position.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        (void)RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size,
            issue);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        RecoveryFrameObservation confirmed = RenderRecoveryFrame(
            panel,
            view,
            latest_view,
            false,
            specforge::UiLanguage::English,
            window_size,
            issue);
        Require(
            confirmed.submission_count == 1 &&
                confirmed.submitted_workflow_kind ==
                    specforge::ActiveSampleWorkflowIntentKind::
                        DeleteTemporaryLabelingTask &&
                confirmed.submitted_source_identity ==
                    view.labeling.source_identity &&
                confirmed.submitted_task_id ==
                    view.labeling.recovery_drafts.front().task_id,
            "editing-state delete confirmation must submit the exact delete operation and draft identity");
        return confirmed;
    };

    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        const specforge::SourceCollectionSessionView view = make_view();
        specforge::SourceCollectionSessionView latest_view = view;
        (void)find_and_confirm_delete(
            panel,
            view,
            latest_view,
            specforge::SampleLabelingOperationResult::Issue::None);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::IsEditingLabelCode(panel) &&
                specforge::SampleWorkflowPanelUiTestAccess::IsShortcutCaptureActive(panel),
            "deleting an unrelated recovery draft must preserve active-task editing state after success");
    }

    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        const specforge::SourceCollectionSessionView view = make_view();
        specforge::SourceCollectionSessionView latest_view = view;
        (void)find_and_confirm_delete(
            panel,
            view,
            latest_view,
            specforge::SampleLabelingOperationResult::Issue::EditLeaseUnavailable);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::IsEditingLabelCode(panel) &&
                specforge::SampleWorkflowPanelUiTestAccess::IsShortcutCaptureActive(panel),
            "a rejected unrelated recovery deletion must preserve active-task editing state");
    }
}

void TestLabelingPanelKeepsRecoveryActionsUsableAtDefaultDockWidth()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    const specforge::SourceCollectionSessionView view = MakeRecoveryPanelView();
    specforge::SourceCollectionSessionView latest_view = view;
    const ImVec2 default_labeling_dock_size(233.0f, 700.0f);
    const RecoveryFrameObservation rendered = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::SimplifiedChinese,
        default_labeling_dock_size);
    const std::array<std::string_view, 3> action_ids = {
        "SpecForgeRecoverTemporaryDraft",
        "SpecForgeKeepTemporaryDraft",
        "SpecForgeDeleteTemporaryDraft",
    };
    Require(
        rendered.cursor_max_y < default_labeling_dock_size.y,
        "the default labeling dock should keep all recovery actions visible in a narrow layout");
    for (const std::string_view action_id : action_ids) {
        const auto action_rect =
            specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
                panel,
                view,
                0,
                action_id);
        Require(
            action_rect.has_value(),
            "narrow recovery actions should expose deterministic rectangles");
        Require(
            (*action_rect)[0] >= 0.0f &&
                (*action_rect)[2] <= default_labeling_dock_size.x &&
                (*action_rect)[1] >= 0.0f &&
                (*action_rect)[3] <= default_labeling_dock_size.y,
            "narrow recovery actions should remain inside the dock");
    }
}

void TestLabelingPanelKeepsTaskNameEditorForDraftAndFormalTasks()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "28f66393-e877-400a-a748-563d623cbd47";
    view.labeling.task_name = "Formal review";

    const LabelingTaskNameFrameObservation formal =
        RenderLabelingTaskNameFrame(panel, view, true);
    Require(
        formal.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingTaskNameRect(panel)
                .has_value() &&
            specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingTaskIdCopyRect(panel)
                .has_value(),
        "a formal task should always render the name editor and task ID copy control");
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditTaskId(panel) == view.labeling.task_id &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) == view.labeling.task_name &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBaseline(panel) == view.labeling.task_name,
        "the name editor should initialize from the formal task projection");

    view.labeling.active_task_is_temporary = true;
    view.labeling.task_id =
        "3d7420df-a7a1-4f00-a963-fba1865612fe";
    view.labeling.task_name = "Draft triage";
    const LabelingTaskNameFrameObservation draft =
        RenderLabelingTaskNameFrame(panel, view, true);
    Require(
        draft.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                    LabelingTaskNameRect(panel)
                .has_value() &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) == "Draft triage",
        "an outputless draft should use the same persistent task-name editor with its real name");
    Require(
        draft.logged_text.find("Draft triage") !=
                std::string::npos &&
            draft.logged_text.find(
                specforge::UiText(
                    specforge::UiLanguage::English,
                    specforge::UiTextId::TemporaryLabelingTask)) ==
                std::string::npos,
        "the draft selector should show its persisted name rather than a fixed temporary-task label");
}

void TestLabelingPanelOpenSelectorUsesRealDraftName()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeActiveTemporaryRecoveryPanelView();
    view.labeling.task_name = "Review ## batch ### alpha";
    view.labeling.recovery_drafts.front().task_name =
        view.labeling.task_name;
    specforge::SourceCollectionSessionView latest_view = view;
    const ImVec2 window_size(650.0f, 700.0f);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        window_size);
    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "the active draft should expose its task selector rectangle");
    const ImVec2 selector_center = RectCenter(*selector_rect);
    ImGui::GetIO().AddMousePosEvent(
        selector_center.x,
        selector_center.y);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMousePosEvent(
        selector_center.x,
        selector_center.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const RecoveryFrameObservation opened = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size,
        specforge::SampleLabelingOperationResult::Issue::None,
        true);

    const std::size_t preview_name = opened.logged_text.find(
        view.labeling.task_name);
    const std::size_t selectable_name = preview_name == std::string::npos
        ? std::string::npos
        : opened.logged_text.find(
              view.labeling.task_name,
              preview_name + view.labeling.task_name.size());
    Require(
        opened.popup_open &&
            selectable_name != std::string::npos &&
            opened.logged_text.find(std::string(specforge::UiText(
                specforge::UiLanguage::English,
                specforge::UiTextId::TemporaryLabelingDraft))) !=
                std::string::npos,
        "the open selector should use the real draft name as its selectable label and show temporary ownership separately");
}

void TestLabelingPanelOpenSelectorUsesRealFormalName()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_name = "Formal ## review ### alpha";
    view.labeling.output_path = "formal-review.asdf";
    view.navigation.current_annotations.push_back(
        {
            .name = view.labeling.task_name,
            .path = *view.labeling.output_path,
            .relationship =
                specforge::SampleAnnotationWorkflowRelationship::
                    LocalLabelingTask,
            .can_activate_labeling = true,
        });
    specforge::SourceCollectionSessionView latest_view = view;
    const ImVec2 window_size(650.0f, 700.0f);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        window_size);
    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "the formal task should expose its task selector rectangle");
    const ImVec2 selector_center = RectCenter(*selector_rect);
    ImGui::GetIO().AddMousePosEvent(
        selector_center.x,
        selector_center.y);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMousePosEvent(
        selector_center.x,
        selector_center.y);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const RecoveryFrameObservation opened = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        window_size,
        specforge::SampleLabelingOperationResult::Issue::None,
        true);

    const std::size_t preview_name = opened.logged_text.find(
        view.labeling.task_name);
    const std::size_t selectable_name = preview_name == std::string::npos
        ? std::string::npos
        : opened.logged_text.find(
              view.labeling.task_name,
              preview_name + view.labeling.task_name.size());
    Require(
        opened.popup_open &&
            selectable_name != std::string::npos,
        "the open selector should render a formal task name containing ## or ### verbatim");
}

void TestLabelingPanelSubmitsTaskNameOnlyOnEnter()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "5fd04692-4eb7-4b1c-b356-39db30b13799";
    FocusLabelingTaskNameField(panel, view);

    const std::string requested_name =
        "  中日韩 review  ";
    ReplaceFocusedText(panel, view, requested_name);
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, view);
    Require(
        typed.submission_count == 0,
        "typing a task name must not submit a per-keystroke rename");
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
            TaskNameEditBuffer(panel) == requested_name,
        "the task-name editor should preserve whitespace and UTF-8 text exactly");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    const LabelingTaskNameFrameObservation submitted =
        RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    const LabelingTaskNameFrameObservation released =
        RenderLabelingTaskNameFrame(panel, view);
    const LabelingTaskNameFrameObservation settled =
        RenderLabelingTaskNameFrame(panel, view);
    Require(
        submitted.submission_count == 1 &&
            submitted.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RenameActiveLabelingTask &&
            submitted.submitted_task_id == view.labeling.task_id &&
            submitted.submitted_name == requested_name,
        "Enter should submit one exact rename intent with the expected task ID");
    Require(
        released.submission_count == 0 &&
            settled.submission_count == 0,
        "the Enter release and ordinary refresh must not duplicate a rename");
}

void TestLabelingPanelSubmitsTaskNameOnceOnBlurAndCopiesFullId()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "825508a7-7e87-41c4-8d2b-5eecaa3b518b";
    FocusLabelingTaskNameField(panel, view);
    ReplaceFocusedText(panel, view, "Blur rename");
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, view);
    Require(
        typed.submission_count == 0,
        "editing before blur must not emit a rename");

    const auto copy_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingTaskIdCopyRect(panel);
    Require(
        copy_rect.has_value(),
        "the full task ID should have a copy button beside it");
    const ImVec2 copy_center = RectCenter(*copy_rect);
    ImGui::GetIO().AddMousePosEvent(
        copy_center.x,
        copy_center.y);
    const LabelingTaskNameFrameObservation hovered =
        RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    const LabelingTaskNameFrameObservation pressed =
        RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingTaskNameFrameObservation released =
        RenderLabelingTaskNameFrame(panel, view);
    const LabelingTaskNameFrameObservation settled =
        RenderLabelingTaskNameFrame(panel, view);
    Require(
        hovered.submission_count + pressed.submission_count +
                released.submission_count ==
            1 &&
            settled.submission_count == 0,
        "leaving the edited field should submit exactly one rename");
    Require(
        context.clipboard_text() == view.labeling.task_id,
        "the copy control should copy the complete task UUID");
}

void TestLabelingPanelSettlesTaskNameWhenWindowCollapses()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "b5402a3a-aef5-4cfe-8cb2-e58189602ea5";
    FocusLabelingTaskNameField(panel, view);
    ReplaceFocusedText(panel, view, "Collapsed rename");
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, view);
    const LabelingTaskNameFrameObservation collapsed =
        RenderLabelingTaskNameFrame(panel, view, false, true);
    const LabelingTaskNameFrameObservation reopened =
        RenderLabelingTaskNameFrame(panel, view);

    Require(
        typed.submission_count == 0 &&
            collapsed.submission_count == 1 &&
            collapsed.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RenameActiveLabelingTask &&
            collapsed.submitted_task_id == view.labeling.task_id &&
            collapsed.submitted_name == "Collapsed rename" &&
            reopened.submission_count == 0,
        "collapsing or hiding the panel should settle one pending task-name edit without duplicating it on reopen");
}

void TestLabelingPanelKeepsTaskNameEditAcrossZeroMatchFilter()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "84f767c2-f7b8-4f8c-8e03-367a9457c695";
    FocusLabelingTaskNameField(panel, view);
    constexpr std::string_view requested_name =
        "Zero-match review";
    ReplaceFocusedText(
        panel,
        view,
        requested_name);
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, view);
    const auto copy_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingTaskIdCopyRect(panel);
    Require(
        typed.submission_count == 0 &&
            copy_rect.has_value(),
        "the zero-match fixture should begin with an unsubmitted task-name edit");

    const ImVec2 copy_center = RectCenter(*copy_rect);
    ImGui::GetIO().AddMousePosEvent(
        copy_center.x,
        copy_center.y);
    const LabelingTaskNameFrameObservation hovered =
        RenderLabelingTaskNameFrame(panel, view);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    const LabelingTaskNameFrameObservation pressed =
        RenderLabelingTaskNameFrame(panel, view);

    specforge::SourceCollectionSessionView zero_match_view = view;
    zero_match_view.labeling.current_index.reset();
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const LabelingTaskNameFrameObservation released =
        RenderLabelingTaskNameFrame(
            panel,
            zero_match_view);
    zero_match_view.labeling.task_name =
        std::string(requested_name);
    const LabelingTaskNameFrameObservation settled =
        RenderLabelingTaskNameFrame(
            panel,
            zero_match_view);

    Require(
        hovered.submission_count + pressed.submission_count +
                released.submission_count ==
            1 &&
            released.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RenameActiveLabelingTask &&
            released.submitted_task_id == view.labeling.task_id &&
            released.submitted_name == requested_name &&
            settled.submission_count == 0,
        "a zero-match filter should allow the deferred blur rename to submit exactly once");
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                LabelingTaskNameRect(panel)
                .has_value() &&
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingTaskIdCopyRect(panel)
                .has_value() &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditTaskId(panel) ==
                zero_match_view.labeling.task_id &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) == requested_name,
        "an active task should retain its task-name and task-ID controls when filters match no samples");
}

void TestLabelingPanelFinalizesTaskNameWhenRenderingStops()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "f33d04d4-c6b0-47d2-b92f-98cd1963308c";
    FocusLabelingTaskNameField(panel, view);
    ReplaceFocusedText(panel, view, "Hidden panel rename");
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, view);
    const LabelingTaskNameFrameObservation hidden =
        FinalizeLabelingTaskNameEdit(panel, view);
    const LabelingTaskNameFrameObservation repeated =
        FinalizeLabelingTaskNameEdit(panel, view);

    Require(
        typed.submission_count == 0 &&
            hidden.submission_count == 1 &&
            hidden.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RenameActiveLabelingTask &&
            hidden.submitted_task_id == view.labeling.task_id &&
            hidden.submitted_name == "Hidden panel rename" &&
            repeated.submission_count == 0,
        "stopping Labeling rendering should finalize one guarded rename without requiring the panel to reopen");

    specforge::SampleWorkflowPanelUi stale_panel;
    FocusLabelingTaskNameField(stale_panel, view);
    ReplaceFocusedText(stale_panel, view, "Stale hidden rename");
    (void)RenderLabelingTaskNameFrame(stale_panel, view);
    specforge::SourceCollectionSessionView switched_view = view;
    switched_view.labeling.task_id =
        "743a894d-d942-4050-8e45-c5a766464d4d";
    switched_view.labeling.task_name = "Switched task";
    const LabelingTaskNameFrameObservation stale =
        FinalizeLabelingTaskNameEdit(
            stale_panel,
            switched_view);
    Require(
        stale.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditTaskId(stale_panel).empty(),
        "hidden-panel finalization should discard the edit when the expected task ID is stale");
}

void TestLabelingPanelKeepsCopyIdReachableInNarrowDock()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeLabelingPanelView(8, 'g');
    view.labeling.task_id =
        "752d5d70-c699-4f39-9f32-e8b5b496e2e7";
    constexpr float window_x = 20.0f;
    constexpr float narrow_width = 233.0f;
    const ImVec2 narrow_size(narrow_width, 700.0f);
    const LabelingTaskNameFrameObservation rendered =
        RenderLabelingTaskNameFrame(
            panel,
            view,
            true,
            false,
            narrow_size);
    const auto copy_rect =
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingTaskIdCopyRect(panel);
    Require(
        copy_rect.has_value() &&
            (*copy_rect)[0] >= window_x &&
            (*copy_rect)[2] <= window_x + narrow_width &&
            rendered.logged_text.find(view.labeling.task_id) !=
                std::string::npos,
        "the default narrow dock should wrap the full task ID and keep Copy ID inside the visible panel");

    const ImVec2 copy_center = RectCenter(*copy_rect);
    ImGui::GetIO().AddMousePosEvent(
        copy_center.x,
        copy_center.y);
    (void)RenderLabelingTaskNameFrame(
        panel,
        view,
        false,
        false,
        narrow_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingTaskNameFrame(
        panel,
        view,
        false,
        false,
        narrow_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    (void)RenderLabelingTaskNameFrame(
        panel,
        view,
        false,
        false,
        narrow_size);
    Require(
        context.clipboard_text() == view.labeling.task_id,
        "the visible narrow-dock Copy ID button should copy the complete UUID");
}

void TestLabelingPanelRecoverySwitchDiscardsBlurredTaskName()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView frame_view =
        MakeFormalTaskWithRecoveryDraftView();
    specforge::SourceCollectionSessionView latest_view = frame_view;
    specforge::SourceCollectionSessionView recovered_view = frame_view;
    recovered_view.labeling.active_task_is_temporary = true;
    recovered_view.labeling.task_id =
        frame_view.labeling.recovery_drafts.front().task_id;
    recovered_view.labeling.task_name =
        frame_view.labeling.recovery_drafts.front().task_name;
    recovered_view.labeling.recovery_drafts.front().status =
        specforge::SampleLabelingRecoveryDraftStatus::Current;

    FocusLabelingTaskNameField(panel, frame_view);
    ReplaceFocusedText(panel, frame_view, "Must not rename formal task");
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, frame_view);
    Require(
        typed.submission_count == 0,
        "editing the old task should remain local before a recovery switch");

    const ImVec2 recovery_window_size(650.0f, 700.0f);
    const RecoveryFrameObservation recovery_layout = RenderRecoveryFrame(
        panel,
        frame_view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        recovery_window_size);
    Require(
        recovery_layout.submission_count == 0,
        "laying out the recovery controls should not settle a focused edit");

    const auto recovery_rect =
        specforge::SampleWorkflowPanelUiTestAccess::RecoveryActionRect(
            panel,
            frame_view,
            0,
            "SpecForgeRecoverTemporaryDraft");
    Require(
        recovery_rect.has_value(),
        "the recovery switch should expose its deterministic action rectangle");
    const ImVec2 recovery_center = RectCenter(*recovery_rect);
    ImGui::GetIO().AddMousePosEvent(
        recovery_center.x,
        recovery_center.y);
    const RecoveryFrameObservation hovered = RenderRecoveryFrame(
        panel,
        frame_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        recovery_window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    const RecoveryFrameObservation pressed = RenderRecoveryFrame(
        panel,
        frame_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        recovery_window_size);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    const RecoveryFrameObservation released = RenderRecoveryFrame(
        panel,
        frame_view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        recovery_window_size,
        specforge::SampleLabelingOperationResult::Issue::None,
        false,
        &recovered_view);

    Require(
        hovered.submission_count == 0 &&
            pressed.submission_count == 0 &&
            released.submission_count == 1 &&
            released.submitted_workflow_kind ==
                specforge::ActiveSampleWorkflowIntentKind::
                    RecoverTemporaryLabelingTask &&
            released.submitted_task_id ==
                recovered_view.labeling.task_id,
        "clicking Recover should switch tasks without first renaming the blurred old task");
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditTaskId(panel) != frame_view.labeling.task_id &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) !=
                    "Must not rename formal task",
        "the switched task should discard the old task's pending edit buffer");
}

void TestLabelingPanelRejectsWhitespaceTaskNameAndAbortsOnSwitch()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView first_view =
        MakeLabelingPanelView(8, 'g');
    first_view.labeling.task_id =
        "f18a69c5-408e-47df-8504-4803f9cc1634";
    FocusLabelingTaskNameField(panel, first_view);
    const std::string whitespace_name = " \xE3\x80\x80 ";
    ReplaceFocusedText(panel, first_view, whitespace_name);
    const LabelingTaskNameFrameObservation typed =
        RenderLabelingTaskNameFrame(panel, first_view);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    const LabelingTaskNameFrameObservation rejected =
        RenderLabelingTaskNameFrame(panel, first_view);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    (void)RenderLabelingTaskNameFrame(panel, first_view);
    Require(
        typed.submission_count == 0 &&
            rejected.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) == whitespace_name &&
            !specforge::SampleWorkflowPanelUiTestAccess::
                 TaskNameValidationMessage(panel)
                 .empty(),
        "Unicode-whitespace-only names should remain visible with an inline error and no command");
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::ValidateTaskName(
             specforge::UiLanguage::English,
             std::string("bad\xFF", 4))
             .empty(),
        "the task-name validator should reject malformed UTF-8 before submission");

    FocusLabelingTaskNameField(panel, first_view);
    ReplaceFocusedText(panel, first_view, "Unsubmitted first task name");
    (void)RenderLabelingTaskNameFrame(panel, first_view);
    specforge::SourceCollectionSessionView refreshed_view = first_view;
    refreshed_view.labeling.task_name = "Ordinary view refresh";
    const LabelingTaskNameFrameObservation refreshed =
        RenderLabelingTaskNameFrame(panel, refreshed_view);
    Require(
        refreshed.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) ==
                "Unsubmitted first task name",
        "an ordinary same-task view refresh must not overwrite an edit in progress");

    specforge::SourceCollectionSessionView second_view = first_view;
    second_view.labeling.task_id =
        "237ba83f-814b-4a77-b814-8627f82cc884";
    second_view.labeling.task_name = "Second task";
    const LabelingTaskNameFrameObservation switched =
        RenderLabelingTaskNameFrame(panel, second_view);
    Require(
        switched.submission_count == 0 &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditTaskId(panel) == second_view.labeling.task_id &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBuffer(panel) == "Second task" &&
            specforge::SampleWorkflowPanelUiTestAccess::
                TaskNameEditBaseline(panel) == "Second task",
        "switching tasks should discard the old edit buffer without targeting either task");
}

void TestLabelingPanelLocalizesBuiltInRecoveryPresentation()
{
    const std::string built_in_name(specforge::kTemporarySampleLabelingTaskName);
    Require(
        specforge::SampleWorkflowTemporaryDraftTaskName(
            specforge::UiLanguage::SimplifiedChinese,
            built_in_name) == built_in_name,
        "recovery rows should show the draft's persisted name instead of replacing it from ownership state");
    Require(
        specforge::SampleWorkflowTemporaryDraftTaskName(
            specforge::UiLanguage::SimplifiedChinese,
            "Historical review") == "Historical review",
        "a historical custom recovery task name must remain unchanged");
}

void TestLabelingPanelShowsFullRecoveryTaskIdentityTooltip()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView view =
        MakeRecoveryPanelView();
    const std::string long_task_id =
        "historical-draft-task-id-with-a-long-stable-identity-"
        "0123456789abcdef0123456789abcdef0123456789abcdef";
    view.labeling.recovery_drafts.front().task_id = long_task_id;
    specforge::SourceCollectionSessionView latest_view = view;
    const ImVec2 narrow_window_size(233.0f, 700.0f);

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    (void)RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        narrow_window_size);
    const auto identity_rect =
        specforge::SampleWorkflowPanelUiTestAccess::RecoveryIdentityRect(
            panel,
            view,
            0);
    Require(
        identity_rect.has_value(),
        "a recovery row should expose a deterministic task identity rectangle");
    const ImVec2 identity_position(
        (*identity_rect)[0] + 4.0f,
        ((*identity_rect)[1] + (*identity_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        identity_position.x,
        identity_position.y);
    const RecoveryFrameObservation hovered = RenderRecoveryFrame(
        panel,
        view,
        latest_view,
        false,
        specforge::UiLanguage::English,
        narrow_window_size,
        specforge::SampleLabelingOperationResult::Issue::None,
        true);
    const std::size_t first_identity =
        hovered.logged_text.find(long_task_id);
    Require(
        first_identity != std::string::npos &&
            hovered.logged_text.find(
                long_task_id,
                first_identity + long_task_id.size()) != std::string::npos,
        "hovering a narrow recovery identity should render the complete task ID tooltip");
}

void TestLabelingPanelShowsPausedDraftSaveFailure()
{
    const specforge::SourceCollectionSessionView failed_view =
        MakeFailedRecoveryPanelView();
    float healthy_height = 0.0f;
    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        const specforge::SourceCollectionSessionView healthy_view =
            MakeRecoveryPanelView();
        specforge::SourceCollectionSessionView latest_view = healthy_view;
        healthy_height = RenderRecoveryFrame(
            panel,
            healthy_view,
            latest_view,
            true)
                             .cursor_max_y;
    }

    float failed_height = 0.0f;
    RecoveryFrameObservation failed_observation;
    {
        ScopedImGuiContext context;
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionView latest_view = failed_view;
        failed_observation = RenderRecoveryFrame(
            panel,
            failed_view,
            latest_view,
            true,
            specforge::UiLanguage::English,
            ImVec2(900.0f, 500.0f),
            specforge::SampleLabelingOperationResult::Issue::None,
            true);
        failed_height = failed_observation.cursor_max_y;
    }

    const auto& failed_state =
        failed_view.labeling.recovery_drafts.front().save_state;
    const std::string english_status =
        specforge::SampleWorkflowSaveStateText(
            specforge::UiLanguage::English,
            failed_state);
    const std::string chinese_status =
        specforge::SampleWorkflowSaveStateText(
            specforge::UiLanguage::SimplifiedChinese,
            failed_state);
    Require(
        failed_height > healthy_height &&
            english_status.find("1") != std::string::npos &&
            english_status.find(
                specforge::UiText(
                    specforge::UiLanguage::English,
                    specforge::UiTextId::SaveFailedValue)) != std::string::npos &&
            chinese_status.find(
                specforge::UiText(
                    specforge::UiLanguage::SimplifiedChinese,
                    specforge::UiTextId::SaveFailedValue)) != std::string::npos &&
            specforge::SampleWorkflowSaveMessageText(
                specforge::UiLanguage::English,
                failed_state) == failed_state.message &&
            failed_observation.logged_text.find(english_status) !=
                std::string::npos &&
            failed_observation.logged_text.find(failed_state.message) !=
                std::string::npos,
        "a paused failed draft should render localized status, pending count, and its retained error detail");

    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView warning_view =
        MakeRecoveryPanelView();
    warning_view.labeling.state_save_failed = true;
    warning_view.labeling.state_save_error = "save warning detail";
    warning_view.labeling.state_load_warning = "load warning detail";
    specforge::SourceCollectionSessionView latest_view = warning_view;
    const RecoveryFrameObservation warning = RenderRecoveryFrame(
        panel,
        warning_view,
        latest_view,
        true,
        specforge::UiLanguage::English,
        ImVec2(900.0f, 500.0f),
        specforge::SampleLabelingOperationResult::Issue::None,
        true);
    Require(
        warning.logged_text.find("save warning detail") != std::string::npos &&
            warning.logged_text.find("load warning detail") != std::string::npos,
        "a no-active-task recovery view should render state save/load diagnostics");
}

void TestLabelingPanelSurfacesRejectedWorkflowMessage()
{
    ScopedImGuiContext context;
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionView inactive_view;
    inactive_view.labeling.has_active_source = true;
    inactive_view.labeling.current_index = 0;
    inactive_view.labeling.sample_count = 1;
    specforge::SourceCollectionSessionView latest_view =
        inactive_view;
    const specforge::SourceCollectionSessionView activated_view =
        MakeLabelingPanelView(8, 'g');
    const std::string conflict_message =
        "This labeling target is already being edited by another SpecForge instance.";

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    LabelingTaskSwitchFrameObservation observation =
        RenderLabelingTaskSwitchFrame(
            panel,
            inactive_view,
            latest_view,
            activated_view,
            true,
            conflict_message);

    const auto selector_rect =
        specforge::SampleWorkflowPanelUiTestAccess::LabelingSelectorRect(panel);
    Require(
        selector_rect.has_value(),
        "message integration fixture should expose a deterministic selector rectangle");
    const ImVec2 selector_position(
        ((*selector_rect)[0] + (*selector_rect)[2]) * 0.5f,
        ((*selector_rect)[1] + (*selector_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        selector_position.x,
        selector_position.y);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    Require(
        observation.selector_hovered,
        "message integration fixture should locate the labeling task selector");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    Require(
        observation.popup_open,
        "message integration fixture should open the task selector");

    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    const auto temporary_action_rect =
        specforge::SampleWorkflowPanelUiTestAccess::TemporaryLabelingActionRect(panel);
    Require(
        temporary_action_rect.has_value(),
        "message integration fixture should expose a deterministic labeling action rectangle");
    const ImVec2 temporary_action_position(
        ((*temporary_action_rect)[0] + (*temporary_action_rect)[2]) * 0.5f,
        ((*temporary_action_rect)[1] + (*temporary_action_rect)[3]) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(
        temporary_action_position.x,
        temporary_action_position.y);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    Require(
        observation.temporary_action_hovered,
        "message integration fixture should locate the labeling action");
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        true);
    (void)RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    ImGui::GetIO().AddMouseButtonEvent(
        ImGuiMouseButton_Left,
        false);
    observation = RenderLabelingTaskSwitchFrame(
        panel,
        inactive_view,
        latest_view,
        activated_view,
        false,
        conflict_message);
    Require(
        observation.submission_count == 1 &&
            observation.operation_message ==
                conflict_message,
        "rejected labeling outcome should remain visibly available in the panel that submitted it");
}

void TestLabelingPanelClearsNoticeAfterActionOnlySuccess()
{
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionResult rejected;
    rejected.message =
        "This labeling target is already being edited by another SpecForge instance.";
    rejected.labeling_issue =
        specforge::SampleLabelingOperationResult::Issue::
            EditLeaseUnavailable;
    specforge::SampleWorkflowPanelUiTestAccess::
        CaptureLabelingOperationResult(
            panel,
            rejected,
            specforge::UiLanguage::English);
    Require(
        !specforge::SampleWorkflowPanelUiTestAccess::
             LabelingOperationMessage(panel)
             .empty(),
        "rejected operation should establish a visible notice");

    specforge::SourceCollectionSessionResult succeeded;
    succeeded.action.workflow_changed = true;
    succeeded.view_invalidated = true;
    specforge::SampleWorkflowPanelUiTestAccess::
        CaptureLabelingOperationResult(
            panel,
            succeeded,
            specforge::UiLanguage::English);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingOperationMessage(panel)
            .empty(),
        "a successful action-only labeling transition should clear the previous failure notice");
}

void TestLabelingPanelLocalizesLeaseNotices()
{
    specforge::SampleWorkflowPanelUi panel;
    specforge::SourceCollectionSessionResult rejected;
    rejected.message =
        "This labeling target is already being edited by another SpecForge instance.";
    rejected.labeling_issue =
        specforge::SampleLabelingOperationResult::Issue::
            EditLeaseUnavailable;
    specforge::SampleWorkflowPanelUiTestAccess::
        CaptureLabelingOperationResult(
            panel,
            rejected,
            specforge::UiLanguage::SimplifiedChinese);
    Require(
        specforge::SampleWorkflowPanelUiTestAccess::
            LabelingOperationMessage(panel) ==
        specforge::UiText(
            specforge::UiLanguage::SimplifiedChinese,
            specforge::UiTextId::
                LabelingEditLeaseUnavailable),
        "lease notices should use the selected UI language instead of coordinator-authored English text");
}

void TestLabelingPanelLocalizesMigrationNotices()
{
    using Issue = specforge::SampleLabelingOperationResult::Issue;
    constexpr std::array kCases = {
        std::pair{
            Issue::OutputPathAlreadyUsed,
            specforge::UiTextId::OutputPathAlreadyUsed},
        std::pair{
            Issue::OutputMigrationCheckpointFailed,
            specforge::UiTextId::LabelingMigrationCheckpointFailed},
        std::pair{
            Issue::OutputMigrationPublicationFailed,
            specforge::UiTextId::LabelingMigrationPublicationFailed},
        std::pair{
            Issue::OutputMigrationOwnerSwitchFailed,
            specforge::UiTextId::LabelingMigrationOwnerSwitchFailed},
    };
    for (const auto& [issue, text_id] : kCases) {
        specforge::SampleWorkflowPanelUi panel;
        specforge::SourceCollectionSessionResult rejected;
        rejected.labeling_issue = issue;
        rejected.message = "unlocalized diagnostic must not be displayed";
        specforge::SampleWorkflowPanelUiTestAccess::
            CaptureLabelingOperationResult(
                panel,
                rejected,
                specforge::UiLanguage::SimplifiedChinese);
        Require(
            specforge::SampleWorkflowPanelUiTestAccess::
                LabelingOperationMessage(panel) ==
            specforge::UiText(
                specforge::UiLanguage::SimplifiedChinese,
                text_id),
            "migration notices should use stable localized issue text instead of diagnostics");
    }
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
    TestCanonicalAnnotationActivationUsesSingleFileConfirmation();
    TestCanonicalOutputActionDistinguishesDraftMigrationAndCanonicalOwner();
    TestLabelingPanelSuggestsCanonicalFilenameWithoutRewritingChosenPath();
    TestLabelingPanelRoutesExportLabelsAsASeparateIntent();
    TestLabelingPanelKeepsFormatOverrideUntilSourceChanges();
    TestLabelExportFormatMatrixRoutesToChooserAndIntent();
    TestLabelExportFormatControlsDefaultExtension();
    TestShortcutDisplayUsesKeyboardLegends();
    TestAddSortSourcePopupLocalizesBuiltInSampleName();
    TestShortcutCaptureAcceptsLettersAndKeypadDigits();
    TestShortcutCaptureRejectsModifiedAndReservedKeys();
    TestShortcutCaptureSupportsClearAndCancel();
    TestShortcutConflictRequiresTheSameKeyTwice();
    TestNavigationAndLabelCommandsShareOneRouter();
    TestLabelingPanelRoutesTheLatestSessionProjection();
    TestLabelingPanelTaskSwitchRegistersTheNewShortcutInTheSelectionFrame();
    TestLabelingPanelUsesIdentityForSingleDraftResume();
    TestLabelingPanelRoutesTemporaryDraftRecoveryActions();
    TestLabelingPanelKeepsDuplicateRecoveryRowsIndependently();
    TestLabelingPanelDisablesAmbiguousRecoveryActionsAfterRepair();
    TestLabelingPanelRequiresIdentityForMultipleRecoveryDrafts();
    TestLabelingPanelDisablesFormalAndTemporaryIdentityAmbiguity();
    TestLabelingPanelDeleteModalShowsRecoveryIdentity();
    TestLabelingPanelPlacesSelectorBeforeRecoveryList();
    TestLabelingPanelKeepsSelectorRowAboveRecoveryListAtAllWidths();
    TestLabelingPanelRendersCurrentTaskDeleteModalOnce();
    TestLabelingPanelRejectsCrossFrameCurrentTaskDelete();
    TestLabelingPanelPreservesEditingStateWhenDeletingRecoveryDraft();
    TestLabelingPanelKeepsRecoveryActionsUsableAtDefaultDockWidth();
    TestLabelingPanelKeepsTaskNameEditorForDraftAndFormalTasks();
    TestLabelingPanelOpenSelectorUsesRealDraftName();
    TestLabelingPanelOpenSelectorUsesRealFormalName();
    TestLabelingPanelSubmitsTaskNameOnlyOnEnter();
    TestLabelingPanelSubmitsTaskNameOnceOnBlurAndCopiesFullId();
    TestLabelingPanelSettlesTaskNameWhenWindowCollapses();
    TestLabelingPanelKeepsTaskNameEditAcrossZeroMatchFilter();
    TestLabelingPanelFinalizesTaskNameWhenRenderingStops();
    TestLabelingPanelKeepsCopyIdReachableInNarrowDock();
    TestLabelingPanelRecoverySwitchDiscardsBlurredTaskName();
    TestLabelingPanelRejectsWhitespaceTaskNameAndAbortsOnSwitch();
    TestLabelingPanelLocalizesBuiltInRecoveryPresentation();
    TestLabelingPanelShowsFullRecoveryTaskIdentityTooltip();
    TestLabelingPanelShowsPausedDraftSaveFailure();
    TestLabelingPanelSurfacesRejectedWorkflowMessage();
    TestLabelingPanelLocalizesLeaseNotices();
    TestLabelingPanelLocalizesMigrationNotices();
    TestLabelingPanelClearsNoticeAfterActionOnlySuccess();
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
