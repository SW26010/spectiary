#include "app/specforge_app.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>

#include <array>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace specforge {
namespace {

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 820;
constexpr float kDefaultWindowsDpi = 96.0f;
constexpr std::array<float, 4> kClearColor = {0.08f, 0.09f, 0.10f, 1.0f};
constexpr DWORD kDwmUseImmersiveDarkModeAttribute = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacyAttribute = 19;
constexpr const wchar_t* kPersonalizeRegistryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kAppsUseLightThemeRegistryValue = L"AppsUseLightTheme";

std::string HResultMessage(const char* action, HRESULT result)
{
    std::ostringstream message;
    message << action << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ".";
    return message.str();
}

bool IsProfiledInputMessage(UINT message)
{
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

float NormalizeDpiScale(float dpi_scale)
{
    return dpi_scale > 0.0f ? dpi_scale : 1.0f;
}

float DpiScaleFromWParam(WPARAM wparam)
{
    return NormalizeDpiScale(static_cast<float>(HIWORD(wparam)) / kDefaultWindowsDpi);
}

bool IsInitialKeyDown(LPARAM lparam)
{
    return (lparam & (1LL << 30)) == 0;
}

bool IsRenderableSize(UINT width, UINT height)
{
    return width > 0 && height > 0;
}

bool ShouldUseDarkTitleBar()
{
    DWORD apps_use_light_theme = 1;
    DWORD value_size = sizeof(apps_use_light_theme);
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER,
        kPersonalizeRegistryKey,
        kAppsUseLightThemeRegistryValue,
        RRF_RT_REG_DWORD,
        nullptr,
        &apps_use_light_theme,
        &value_size);

    return result == ERROR_SUCCESS && apps_use_light_theme == 0;
}

void ApplyTitleBarTheme(HWND hwnd)
{
    const BOOL use_dark_title_bar = ShouldUseDarkTitleBar() ? TRUE : FALSE;
    HRESULT result = DwmSetWindowAttribute(
        hwnd,
        kDwmUseImmersiveDarkModeAttribute,
        &use_dark_title_bar,
        sizeof(use_dark_title_bar));

    if (FAILED(result)) {
        DwmSetWindowAttribute(
            hwnd,
            kDwmUseImmersiveDarkModeLegacyAttribute,
            &use_dark_title_bar,
            sizeof(use_dark_title_bar));
    }
}

}  // namespace

SpecForgeApp::~SpecForgeApp()
{
    Shutdown();
}

int SpecForgeApp::Run(HINSTANCE instance, int show_command)
{
    Initialize(instance, show_command);

    MSG message = {};
    while (running_) {
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!running_) {
            break;
        }

        RenderFrame();
    }

    Shutdown();
    return static_cast<int>(message.wParam);
}

void SpecForgeApp::Initialize(HINSTANCE instance, int show_command)
{
    profile_ = ProfileSink::CreateDefault();
    profile_.WriteEvent("runtime_config", {
                                            ProfileSink::Field::String("target", "win32_dx11_imgui_implot"),
                                            ProfileSink::Field::String("profile_path", profile_.path().string()),
                                            ProfileSink::Field::Bool("synthetic_fixture", true),
                                        });

    ImGui_ImplWin32_EnableDpiAwareness();

    const bool window_created = window_.Create(
        instance,
        L"SpecForge",
        kInitialWidth,
        kInitialHeight,
        [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            return HandleWindowMessage(hwnd, message, wparam, lparam);
        });
    if (!window_created) {
        throw std::runtime_error("Failed to create the Win32 window.");
    }
    ApplyTitleBarTheme(window_.hwnd());

    if (!renderer_.Initialize(window_.hwnd())) {
        throw std::runtime_error("Failed to create the Direct3D 11 device and swap chain.");
    }

    InitializeUiBackends();
    window_.Show(show_command);
}

void SpecForgeApp::InitializeUiBackends()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDecoration = true;
    io.Fonts->AddFontDefaultVector();

    ImGui::StyleColorsDark();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    base_imgui_style_ = ImGui::GetStyle();
    ApplyUiScale(ImGui_ImplWin32_GetDpiScaleForHwnd(window_.hwnd()));
    profile_.WriteEvent("dpi_config", {
                                           ProfileSink::Field::Number("dpi_scale", std::to_string(ui_dpi_scale_)),
                                           ProfileSink::Field::Number(
                                               "font_scale_dpi",
                                               std::to_string(ImGui::GetStyle().FontScaleDpi)),
                                       });

    if (!ImGui_ImplWin32_Init(window_.hwnd())) {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui Win32 backend.");
    }

    if (!ImGui_ImplDX11_Init(renderer_.device(), renderer_.context())) {
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui DirectX 11 backend.");
    }

    imgui_initialized_ = true;
}

void SpecForgeApp::Shutdown()
{
    if (imgui_initialized_) {
        profile_.WriteEvent("shutdown");
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }

    renderer_.Shutdown();
    pending_resize_.reset();
    fullscreen_restore_.reset();
    window_.ClearMessageHandler();
    window_.Destroy();
}

void SpecForgeApp::RenderFrame()
{
    ApplyPendingResize();

    ++frame_index_;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        ProfileTimer timer(profile_, "view_update", frame_index_);
        ShellStatus status;
        status.profile_open = profile_.is_open();
        status.profile_path = profile_.path();
        status.client_width = window_.client_width();
        status.client_height = window_.client_height();
        status.frame_index = frame_index_;
        ui_.Render(status);
    }

    {
        ProfileTimer timer(profile_, "draw_submission", frame_index_);
        ImGui::Render();
    }

    {
        ProfileTimer timer(profile_, "render_pass", frame_index_);
        renderer_.BeginFrame(kClearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    const auto present_start = std::chrono::steady_clock::now();
    const HRESULT present_result = renderer_.Present();
    const auto present_elapsed = std::chrono::steady_clock::now() - present_start;
    profile_.WriteDuration(
        "present",
        frame_index_,
        std::chrono::duration<double, std::milli>(present_elapsed).count());

    if (FAILED(present_result)) {
        throw std::runtime_error(HResultMessage("Present", present_result));
    }
}

void SpecForgeApp::ApplyPendingResize()
{
    if (!pending_resize_) {
        return;
    }

    const PendingResize resize = *pending_resize_;
    pending_resize_.reset();

    if (!IsRenderableSize(resize.width, resize.height)) {
        return;
    }

    if (!renderer_.Resize(resize.width, resize.height)) {
        throw std::runtime_error("Failed to resize the Direct3D 11 render target.");
    }

    profile_.WriteEvent("render_target_resize", {
                                                    ProfileSink::Field::Number("width", std::to_string(resize.width)),
                                                    ProfileSink::Field::Number("height", std::to_string(resize.height)),
                                                });
}

void SpecForgeApp::ApplyUiScale(float dpi_scale)
{
    ui_dpi_scale_ = NormalizeDpiScale(dpi_scale);

    ImGuiStyle& style = ImGui::GetStyle();
    style = base_imgui_style_;
    style.ScaleAllSizes(ui_dpi_scale_);
    style.FontScaleDpi = ui_dpi_scale_;
}

void SpecForgeApp::ToggleFullscreen()
{
    if (fullscreen_restore_) {
        ExitFullscreen();
    } else {
        EnterFullscreen();
    }
}

void SpecForgeApp::EnterFullscreen()
{
    HWND hwnd = window_.hwnd();
    if (hwnd == nullptr || fullscreen_restore_) {
        return;
    }

    WindowedPlacement restore = {};
    restore.style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    restore.extended_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    restore.placement.length = sizeof(restore.placement);
    if (!GetWindowPlacement(hwnd, &restore.placement)) {
        return;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        return;
    }

    fullscreen_restore_ = restore;

    const DWORD fullscreen_style = restore.style & ~WS_OVERLAPPEDWINDOW;
    const DWORD fullscreen_extended_style =
        restore.extended_style & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(fullscreen_style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(fullscreen_extended_style));

    const RECT& monitor_rect = monitor_info.rcMonitor;
    SetWindowPos(
        hwnd,
        HWND_TOP,
        monitor_rect.left,
        monitor_rect.top,
        monitor_rect.right - monitor_rect.left,
        monitor_rect.bottom - monitor_rect.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    profile_.WriteEvent("fullscreen", {ProfileSink::Field::Bool("enabled", true)});
}

void SpecForgeApp::ExitFullscreen()
{
    HWND hwnd = window_.hwnd();
    if (hwnd == nullptr || !fullscreen_restore_) {
        return;
    }

    const WindowedPlacement restore = *fullscreen_restore_;
    fullscreen_restore_.reset();

    SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(restore.style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(restore.extended_style));
    SetWindowPlacement(hwnd, &restore.placement);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    ApplyTitleBarTheme(hwnd);
    profile_.WriteEvent("fullscreen", {ProfileSink::Field::Bool("enabled", false)});
}

LRESULT SpecForgeApp::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (IsProfiledInputMessage(message)) {
        LogInputMessage(message, wparam);
    }

    if (message == WM_KEYDOWN && wparam == VK_F11 && IsInitialKeyDown(lparam)) {
        ToggleFullscreen();
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && fullscreen_restore_ && IsInitialKeyDown(lparam)) {
        ExitFullscreen();
        return 0;
    }

    if (imgui_initialized_ && ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
        return 1;
    }

    switch (message) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            const UINT width = LOWORD(lparam);
            const UINT height = HIWORD(lparam);
            if (IsRenderableSize(width, height)) {
                pending_resize_ = PendingResize{width, height};
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyTitleBarTheme(hwnd);
        return 0;
    case WM_DPICHANGED:
        if (imgui_initialized_) {
            ApplyUiScale(DpiScaleFromWParam(wparam));
        }
        if (lparam != 0) {
            const auto* suggested_rect = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(
                hwnd,
                nullptr,
                suggested_rect->left,
                suggested_rect->top,
                suggested_rect->right - suggested_rect->left,
                suggested_rect->bottom - suggested_rect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        profile_.WriteEvent("dpi_changed", {
                                                ProfileSink::Field::Number("dpi", std::to_string(HIWORD(wparam))),
                                                ProfileSink::Field::Number("dpi_scale", std::to_string(ui_dpi_scale_)),
                                            });
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void SpecForgeApp::LogInputMessage(UINT message, WPARAM wparam)
{
    switch (message) {
    case WM_LBUTTONDOWN:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_press_left")});
        break;
    case WM_RBUTTONDOWN:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_press_right")});
        break;
    case WM_MBUTTONDOWN:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_press_middle")});
        break;
    case WM_LBUTTONUP:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_release_left")});
        break;
    case WM_RBUTTONUP:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_release_right")});
        break;
    case WM_MBUTTONUP:
        profile_.WriteEvent("input", {ProfileSink::Field::String("kind", "pointer_release_middle")});
        break;
    case WM_MOUSEWHEEL:
        profile_.WriteEvent("input", {
                                          ProfileSink::Field::String("kind", "wheel"),
                                          ProfileSink::Field::Number(
                                              "wheel_delta",
                                              std::to_string(GET_WHEEL_DELTA_WPARAM(wparam))),
                                      });
        break;
    default:
        break;
    }
}

}  // namespace specforge
