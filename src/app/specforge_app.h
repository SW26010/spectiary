#pragma once

#include "app/render_wake_scheduler.h"
#include "platform/win32_compositor_clock.h"
#include "platform/win32_message_render_observer.h"
#include "platform/win32_window.h"
#include "platform/win32_touchpad_gesture_source.h"
#include "profile/profile_sink.h"
#include "renderer/d3d11_imgui_viewport_renderer.h"
#include "renderer/d3d11_renderer.h"
#include "ui/shell_ui.h"

#include <Windows.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace specforge {

class SpecForgeApp {
public:
    SpecForgeApp() = default;
    ~SpecForgeApp();

    SpecForgeApp(const SpecForgeApp&) = delete;
    SpecForgeApp& operator=(const SpecForgeApp&) = delete;

    int Run(
        HINSTANCE instance,
        int show_command,
        std::optional<std::filesystem::path> initial_source = std::nullopt);

private:
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
    void Shutdown();
    void RenderFrame();
    void UpdateCompositorClockBoost(bool window_renderable, bool touchpad_active);
    static void InvalidateRenderFromWin32Message(void* context) noexcept;
    void RequestMessageRender() noexcept;
    void ApplyPendingResize();
    void ApplyUiScale(float dpi_scale);
    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void ToggleImmersivePlotMode();
    void EnterImmersivePlotMode();
    void ExitImmersivePlotMode();
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

    LRESULT HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void LogInputMessage(UINT message, WPARAM wparam, LPARAM lparam);

    Win32Window window_;
    D3D11Renderer renderer_;
    D3D11ImGuiViewportRenderer viewport_renderer_;
    ProfileSink profile_;
    Win32TouchpadGestureSource touchpad_gestures_;
    Win32CompositorClock compositor_clock_;
    Win32MessageRenderObserver message_render_observer_;
    RenderWakeScheduler render_wake_scheduler_;
    ShellUi ui_{&touchpad_gestures_};

    std::string imgui_ini_path_utf8_;
    bool imgui_initialized_ = false;
    bool running_ = true;
    bool minimized_ = false;
    bool window_visible_ = true;
    bool compositor_clock_tick_ready_ = false;
    ImGuiStyle base_imgui_style_;
    float ui_dpi_scale_ = 1.0f;
    std::optional<PendingResize> pending_resize_;
    std::optional<WindowedPlacement> fullscreen_restore_;
    bool immersive_plot_entered_fullscreen_ = false;
    std::uint64_t frame_index_ = 0;
};

}  // namespace specforge
