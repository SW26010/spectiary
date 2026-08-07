#pragma once

#include "app/automation_panel_command_coordinator.h"
#include "app/native_window_title.h"
#include "app/on_demand_frame_capture.h"
#include "app/pan_pacing.h"
#include "app/imgui_layout_persistence.h"
#include "app/render_wake_scheduler.h"
#include "app/runtime_paths.h"
#include "app/runtime_resource_workload.h"
#include "automation/automation_named_pipe.h"
#include "automation/automation_startup.h"
#include "automation/automation_state.h"
#include "platform/win32_compositor_clock.h"
#include "platform/win32_message_render_observer.h"
#include "platform/win32_window.h"
#include "platform/win32_touchpad_gesture_source.h"
#include "profile/profile_recording_status.h"
#include "profile/profile_sink.h"
#include "renderer/d3d11_imgui_viewport_renderer.h"
#include "renderer/d3d11_renderer.h"
#include "ui/shell_ui.h"
#include "ui/top_bar_status_layout.h"

#include <Windows.h>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

class SpecForgeApp {
public:
    explicit SpecForgeApp(
        const SpecForgeStartup& startup,
        std::optional<AutomationStartupConfiguration>
            automation = std::nullopt);
    ~SpecForgeApp();

    SpecForgeApp(const SpecForgeApp&) = delete;
    SpecForgeApp& operator=(const SpecForgeApp&) = delete;

    int Run(
        HINSTANCE instance,
        int show_command,
        std::optional<std::filesystem::path> initial_source = std::nullopt);

private:
    struct AutomationSourceCommand {
        AutomationQueuedCommand command;
        SourceCollectionActivationTransaction::
            SourceOpenOperation operation;
    };

    struct AutomationGotoCommand {
        AutomationQueuedCommand command;
        std::string source_id;
        std::size_t target_index = 0;
        std::uint64_t activation_generation = 0;
        std::uint64_t presented_sequence_before = 0;
        bool changed = false;
    };

    struct AutomationLabelCommand {
        enum class Phase {
            NavigatingToTarget,
            WaitingForAutoAdvance,
        };

        AutomationQueuedCommand command;
        Phase phase = Phase::NavigatingToTarget;
        std::string source_id;
        std::size_t target_index = 0;
        std::uint64_t activation_generation = 0;
        std::uint64_t presented_sequence_before = 0;
        std::optional<
            ShellAutomationLabelAssignmentResult>
            assignment;
    };

    struct AutomationCaptureCommand {
        AutomationQueuedCommand command;
        std::filesystem::path output_path;
        std::uint64_t accepted_frame = 0;
    };

    struct PendingResize {
        UINT width = 0;
        UINT height = 0;
    };

    struct WindowedPlacement {
        DWORD style = 0;
        DWORD extended_style = 0;
        WINDOWPLACEMENT placement = {};
    };

    void Initialize(
        HINSTANCE instance,
        int show_command,
        const std::optional<std::filesystem::path>& initial_source);
    void InitializeUiBackends();
    void SaveImGuiLayoutForShutdown();
    void Shutdown();
    [[nodiscard]] RenderFrameOutcome RenderFrame();
    void UpdateCompositorClockBoost(bool window_renderable, bool touchpad_active);
    [[nodiscard]] bool UncappedPanActive(bool window_renderable) const;
    [[nodiscard]] D3D11PresentMode MainPresentMode() const;
    void WritePanPacingState(std::string_view reason, bool active);
    static void InvalidateRenderFromWin32Message(void* context) noexcept;
    static void ObserveWin32Message(
        void* context,
        const Win32ObservedMessage& message) noexcept;
    static void PostSourceLoadCompletionReady(HWND hwnd) noexcept;
    void RequestMessageRender() noexcept;
    void ApplyPendingResize();
    void ApplyUiScale(
        float system_dpi_scale,
        int user_scale_percentage);
    void ApplyLocalizedWindowTitle();
    void WriteDpiConfiguration(std::string_view reason);
    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void ToggleImmersivePlotMode();
    void EnterImmersivePlotMode();
    void ExitImmersivePlotMode();
    void ToggleProfileRecording();
    void RequestFrameCapture();
    void CaptureRequestedFrame();
    [[nodiscard]] bool StartProfileRecording(
        std::string_view trigger,
        std::optional<AutomationPreparedProfileOutput>
            prepared_output = std::nullopt);
    void StopProfileRecording(std::string_view trigger);
    void LogProfileRecordingStarted(std::string_view trigger, std::string_view configuration_reason);
    void WriteRuntimeConfiguration(std::string_view reason);
    void RefreshProfileRecordingStatus();
    void LogDisplayEnvironment(std::string_view reason);
    void LogPresentationUpdates();
    void WritePresentationUpdate(
        std::string_view target,
        unsigned int viewport_id,
        HWND hwnd,
        D3D11PresentationBackend backend,
        D3D11PresentationDegradation degradation,
        const Win32DisplayRefreshState& refresh_state,
        const D3D11PresentationTransition& transition,
        const D3D11CompositionFeedback& feedback);
    void SchedulePresentationTargetRefresh(HWND hwnd) noexcept;
    void RefreshPresentationTargets(std::string_view reason);
    void InitializeAutomation();
    void ServiceAutomation();
    void ApplyPendingApplicationSettings(
        std::string_view scale_reason,
        bool request_frame = true);
    void ServiceAutomationSettingGet(
        const AutomationQueuedCommand& command);
    void ServiceAutomationSettingSet(
        const AutomationQueuedCommand& command);
    void ServiceAutomationPanelGet(
        const AutomationQueuedCommand& command);
    void ServiceAutomationPanelSet(
        const AutomationQueuedCommand& command);
    void BeginAutomationSourceOpen(
        const AutomationQueuedCommand& command);
    void BeginAutomationSpectrumGoto(
        const AutomationQueuedCommand& command);
    void BeginAutomationLabelAssign(
        const AutomationQueuedCommand& command);
    void ContinueAutomationLabelAssign(
        AutomationLabelCommand& operation);
    void BeginAutomationFrameCapture(
        const AutomationQueuedCommand& command);
    void ServiceAutomationProfileStart(
        const AutomationQueuedCommand& command);
    void BeginAutomationProfileStop(
        const AutomationQueuedCommand& command);
    void PollAutomationProfileStop();
    [[nodiscard]] bool
    ServiceAutomationAppQuit(
        const AutomationQueuedCommand& command);
    void CancelAutomationFrameCapture(
        std::string_view reason);
    [[nodiscard]] bool
    AutomationFrameCaptureRequestActive() const;
    void PollAutomationBusinessOperations();
    void PollAutomationPanelCommands();
    [[nodiscard]] bool
    SettleAutomationPanelCommandsForShutdown();
    [[nodiscard]] AutomationPanelCommandCoordinator::Callbacks
    AutomationPanelCoordinatorCallbacks();
    [[nodiscard]] bool
    AutomationBusinessIdle() const noexcept;
    [[nodiscard]] std::optional<
        RenderWakeScheduler::TimePoint>
    NextAutomationDeadline() const;
    [[nodiscard]] AutomationStateSnapshot
    AutomationState();
    static void PostAutomationCommandReady(
        HWND hwnd) noexcept;

    LRESULT HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void LogInputMessage(UINT message, WPARAM wparam, LPARAM lparam);

    SpecForgeStartup startup_;
    Win32Window window_;
    D3D11Renderer renderer_;
    D3D11ImGuiViewportRenderer viewport_renderer_;
    ProfileSink profile_;
    ProfileSink::Limits profile_limits_;
    Win32TouchpadGestureSource touchpad_gestures_;
    Win32CompositorClock compositor_clock_;
    Win32MessageRenderObserver message_render_observer_;
    RenderWakeScheduler render_wake_scheduler_;
    ImGuiLayoutPersistence imgui_layout_persistence_;
    ShellUi ui_;
    PanPacingConfiguration pan_pacing_;
    OnDemandFrameCapture frame_capture_;
    std::optional<AutomationStartupConfiguration>
        automation_configuration_;
    AutomationPanelCommandCoordinator
        automation_panel_coordinator_;
    std::unique_ptr<AutomationNamedPipeServer>
        automation_server_;
    std::vector<AutomationQueuedCommand>
        automation_idle_waits_;
    std::vector<AutomationSourceCommand>
        automation_source_commands_;
    std::optional<AutomationGotoCommand>
        automation_goto_command_;
    std::optional<AutomationLabelCommand>
        automation_label_command_;
    std::optional<AutomationCaptureCommand>
        automation_capture_command_;
    std::optional<AutomationQueuedCommand>
        automation_profile_stop_command_;
    std::optional<AutomationQueuedCommand>
        automation_pending_quit_command_;
    std::string automation_capture_last_result_ =
        "none";
    std::filesystem::path
        automation_capture_last_path_;
    std::optional<RenderWakeScheduler::TimePoint>
        automation_poll_deadline_;
    bool automation_shutdown_requested_ = false;

    std::wstring applied_window_title_;
    std::optional<NativeWindowTitleSemanticKey>
        applied_window_title_key_;
    bool imgui_initialized_ = false;
    bool imgui_layout_saved_for_shutdown_ = false;
    bool running_ = true;
    bool minimized_ = false;
    bool window_visible_ = true;
    bool uncapped_pan_active_ = false;
    ImGuiStyle base_imgui_style_;
    float system_dpi_scale_ = 1.0f;
    float user_ui_scale_ = 1.0f;
    float effective_ui_scale_ = 1.0f;
    int user_ui_scale_percentage_ = 100;
    std::optional<PendingResize> pending_resize_;
    std::optional<WindowedPlacement> fullscreen_restore_;
    bool immersive_plot_entered_fullscreen_ = false;
    bool shutdown_complete_ = false;
    ProfileSink::StopReason displayed_profile_stop_reason_ = ProfileSink::StopReason::None;
    ProfileRecordingStatus profile_status_;
    std::uint64_t frame_index_ = 0;
    // Keep the last valid sample while event-driven rendering is idle.
    std::optional<TopBarFrameTimingSample>
        application_frame_timing_sample_;
    std::optional<RuntimeResourceWorkload>
        runtime_resource_workload_;
};

}  // namespace specforge
