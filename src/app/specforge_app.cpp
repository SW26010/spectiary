#include "app/specforge_app.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace specforge {
namespace {

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 820;
constexpr float kDefaultWindowsDpi = 96.0f;
constexpr std::array<float, 4> kClearColor = {0.08f, 0.09f, 0.10f, 1.0f};
constexpr DWORD kOcclusionFallbackPollMilliseconds = 250;
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

std::string HResultHex(HRESULT result)
{
    std::ostringstream message;
    message << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(result);
    return message.str();
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

double RatioHz(UINT numerator, UINT denominator)
{
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

double PeriodMillisecondsFromHz(double hz)
{
    if (hz <= 0.0) {
        return 0.0;
    }
    return 1000.0 / hz;
}

double QpcTicksToMilliseconds(std::int64_t ticks)
{
    LARGE_INTEGER frequency = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0.0;
    }
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency.QuadPart);
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
    case WM_MOUSEMOVE:
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

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        if (metadata[index].key == key) {
            return metadata[index].value;
        }
    }
    return {};
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

int SpecForgeApp::Run(
    HINSTANCE instance,
    int show_command,
    std::optional<std::filesystem::path> initial_source)
{
    Initialize(instance, show_command, initial_source);

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

        if (minimized_ || !window_visible_) {
            WaitForRenderWake();
            continue;
        }

        if (occluded_ && !TryResumeFromOcclusion()) {
            WaitForRenderWake();
            continue;
        }

        RenderFrame();
    }

    Shutdown();
    return static_cast<int>(message.wParam);
}

void SpecForgeApp::Initialize(
    HINSTANCE instance,
    int show_command,
    const std::optional<std::filesystem::path>& initial_source)
{
    if (initial_source) {
        ui_.OpenSource(*initial_source);
    }

    profile_ = ProfileSink::CreateDefault();
    const SpectrumSnapshotHandle startup_snapshot = ui_.current_snapshot();
    const std::string source_type(
        startup_snapshot ? MetadataValue(startup_snapshot->source.metadata, "source_type") : std::string_view{});
    profile_.WriteEvent("runtime_config", {
                                            ProfileSink::Field::String("target", "win32_dx11_imgui_implot"),
                                            ProfileSink::Field::String("profile_path", profile_.path().string()),
                                            ProfileSink::Field::String(
                                                "source",
                                                startup_snapshot ? startup_snapshot->source.display_name : ""),
                                            ProfileSink::Field::String("source_type", source_type),
                                            ProfileSink::Field::Bool(
                                                "can_plot_current_spectrum",
                                                startup_snapshot &&
                                                    startup_snapshot->capabilities.can_plot_current_spectrum),
                                            ProfileSink::Field::Number(
                                                "spectrum_count",
                                                std::to_string(
                                                    startup_snapshot
                                                        ? startup_snapshot->collection.spectrum_count
                                                        : 0)),
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
    const HRESULT occlusion_status_result = renderer_.occlusion_status_registration_result();
    profile_.WriteEvent("dxgi_occlusion_status_event", {
                                                        ProfileSink::Field::Bool(
                                                            "available",
                                                            renderer_.occlusion_event() != nullptr &&
                                                                SUCCEEDED(occlusion_status_result)),
                                                        ProfileSink::Field::String(
                                                            "registration_result",
                                                            HResultHex(occlusion_status_result)),
                                                    });

    InitializeUiBackends();
    window_.Show(show_command);
    LogDisplayEnvironment("startup");
}

void SpecForgeApp::InitializeUiBackends()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "specforge-imgui-v2.ini";
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
        status.profile = &profile_;
        status.profile_path = status.profile_open ? &profile_.path() : nullptr;
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

    HRESULT present_result = S_OK;
    if (profile_.is_open()) {
        const auto present_start = std::chrono::steady_clock::now();
        present_result = renderer_.Present();
        const auto present_elapsed = std::chrono::steady_clock::now() - present_start;
        profile_.WriteDuration(
            "present",
            frame_index_,
            std::chrono::duration<double, std::milli>(present_elapsed).count());
    } else {
        present_result = renderer_.Present();
    }

    if (present_result == DXGI_STATUS_OCCLUDED) {
        if (!occluded_) {
            profile_.WriteEvent("render_idle", {ProfileSink::Field::String("reason", "dxgi_occluded")});
        }
        occluded_ = true;
        return;
    }

    if (FAILED(present_result)) {
        throw std::runtime_error(HResultMessage("Present", present_result));
    }
}

bool SpecForgeApp::TryResumeFromOcclusion()
{
    const HRESULT present_test = renderer_.PresentTest();
    if (present_test == DXGI_STATUS_OCCLUDED) {
        return false;
    }

    if (FAILED(present_test)) {
        throw std::runtime_error(HResultMessage("Present test", present_test));
    }

    occluded_ = false;
    profile_.WriteEvent("render_resume", {ProfileSink::Field::String("reason", "dxgi_present_test")});
    return true;
}

void SpecForgeApp::WaitForRenderWake()
{
    if (!running_) {
        return;
    }

    if (occluded_) {
        HANDLE occlusion_event = renderer_.occlusion_event();
        if (occlusion_event != nullptr) {
            const DWORD result = MsgWaitForMultipleObjects(1, &occlusion_event, FALSE, INFINITE, QS_ALLINPUT);
            if (result != WAIT_FAILED) {
                return;
            }
        }

        const DWORD result =
            MsgWaitForMultipleObjectsEx(0, nullptr, kOcclusionFallbackPollMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result != WAIT_FAILED) {
            return;
        }
    }

    WaitMessage();
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
    LogDisplayEnvironment("resize");
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
    LogDisplayEnvironment("fullscreen_enter");
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
    LogDisplayEnvironment("fullscreen_exit");
}

void SpecForgeApp::LogDisplayEnvironment(std::string_view reason)
{
    if (!profile_.is_open()) {
        return;
    }

    HWND hwnd = window_.hwnd();

    MONITORINFOEXW monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    HMONITOR monitor = nullptr;
    bool monitor_ok = false;
    if (hwnd != nullptr) {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        monitor_ok = monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info);
    }

    DEVMODEW display_mode = {};
    display_mode.dmSize = sizeof(display_mode);
    const bool display_mode_ok =
        monitor_ok && EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &display_mode);

    DWM_TIMING_INFO dwm_timing = {};
    dwm_timing.cbSize = sizeof(dwm_timing);
    HRESULT dwm_result = hwnd != nullptr ? DwmGetCompositionTimingInfo(hwnd, &dwm_timing) : E_HANDLE;
    std::string dwm_query_target = "window";
    if (FAILED(dwm_result)) {
        dwm_timing = {};
        dwm_timing.cbSize = sizeof(dwm_timing);
        dwm_result = DwmGetCompositionTimingInfo(nullptr, &dwm_timing);
        dwm_query_target = "desktop";
    }
    const bool dwm_ok = SUCCEEDED(dwm_result);
    const double dwm_refresh_hz =
        dwm_ok ? RatioHz(dwm_timing.rateRefresh.uiNumerator, dwm_timing.rateRefresh.uiDenominator) : 0.0;
    const double dwm_compose_hz =
        dwm_ok ? RatioHz(dwm_timing.rateCompose.uiNumerator, dwm_timing.rateCompose.uiDenominator) : 0.0;

    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    const bool swapchain_ok = renderer_.GetSwapChainDesc(swap_chain_desc);
    const double swapchain_desc_hz = swapchain_ok
                                         ? RatioHz(
                                               swap_chain_desc.BufferDesc.RefreshRate.Numerator,
                                               swap_chain_desc.BufferDesc.RefreshRate.Denominator)
                                         : 0.0;

    profile_.WriteEvent("display_environment", {
                                                  ProfileSink::Field::String("reason", std::string(reason)),
                                                  ProfileSink::Field::Bool("monitor_ok", monitor_ok),
                                                  ProfileSink::Field::String(
                                                      "monitor_device",
                                                      monitor_ok ? WideToUtf8(monitor_info.szDevice) : ""),
                                                  ProfileSink::Field::Bool(
                                                      "monitor_primary",
                                                      monitor_ok &&
                                                          (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0),
                                                  ProfileSink::Field::Number(
                                                      "monitor_left",
                                                      std::to_string(monitor_ok ? monitor_info.rcMonitor.left : 0)),
                                                  ProfileSink::Field::Number(
                                                      "monitor_top",
                                                      std::to_string(monitor_ok ? monitor_info.rcMonitor.top : 0)),
                                                  ProfileSink::Field::Number(
                                                      "monitor_width",
                                                      std::to_string(
                                                          monitor_ok
                                                              ? monitor_info.rcMonitor.right -
                                                                    monitor_info.rcMonitor.left
                                                              : 0)),
                                                  ProfileSink::Field::Number(
                                                      "monitor_height",
                                                      std::to_string(
                                                          monitor_ok
                                                              ? monitor_info.rcMonitor.bottom -
                                                                    monitor_info.rcMonitor.top
                                                              : 0)),
                                                  ProfileSink::Field::Bool("display_mode_ok", display_mode_ok),
                                                  ProfileSink::Field::Number(
                                                      "display_mode_frequency_hz",
                                                      std::to_string(
                                                          display_mode_ok ? display_mode.dmDisplayFrequency : 0)),
                                                  ProfileSink::Field::Number(
                                                      "display_mode_width",
                                                      std::to_string(display_mode_ok ? display_mode.dmPelsWidth : 0)),
                                                  ProfileSink::Field::Number(
                                                      "display_mode_height",
                                                      std::to_string(display_mode_ok ? display_mode.dmPelsHeight : 0)),
                                                  ProfileSink::Field::Number(
                                                      "display_mode_position_x",
                                                      std::to_string(display_mode_ok ? display_mode.dmPosition.x : 0)),
                                                  ProfileSink::Field::Number(
                                                      "display_mode_position_y",
                                                      std::to_string(display_mode_ok ? display_mode.dmPosition.y : 0)),
                                                  ProfileSink::Field::Bool("dwm_timing_ok", dwm_ok),
                                                  ProfileSink::Field::String("dwm_query_target", dwm_query_target),
                                                  ProfileSink::Field::String(
                                                      "dwm_result",
                                                      HResultHex(dwm_result)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_refresh_numerator",
                                                      std::to_string(
                                                          dwm_ok ? dwm_timing.rateRefresh.uiNumerator : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_refresh_denominator",
                                                      std::to_string(
                                                          dwm_ok ? dwm_timing.rateRefresh.uiDenominator : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_refresh_hz",
                                                      std::to_string(dwm_refresh_hz)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_refresh_period_ms",
                                                      std::to_string(PeriodMillisecondsFromHz(dwm_refresh_hz))),
                                                  ProfileSink::Field::Number(
                                                      "dwm_qpc_refresh_period_ms",
                                                      std::to_string(
                                                          dwm_ok
                                                              ? QpcTicksToMilliseconds(dwm_timing.qpcRefreshPeriod)
                                                              : 0.0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_compose_numerator",
                                                      std::to_string(
                                                          dwm_ok ? dwm_timing.rateCompose.uiNumerator : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_compose_denominator",
                                                      std::to_string(
                                                          dwm_ok ? dwm_timing.rateCompose.uiDenominator : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_compose_hz",
                                                      std::to_string(dwm_compose_hz)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_compose_period_ms",
                                                      std::to_string(PeriodMillisecondsFromHz(dwm_compose_hz))),
                                                  ProfileSink::Field::Number(
                                                      "dwm_frames_late",
                                                      std::to_string(dwm_ok ? dwm_timing.cFramesLate : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_frames_dropped",
                                                      std::to_string(dwm_ok ? dwm_timing.cFramesDropped : 0)),
                                                  ProfileSink::Field::Number(
                                                      "dwm_frames_missed",
                                                      std::to_string(dwm_ok ? dwm_timing.cFramesMissed : 0)),
                                                  ProfileSink::Field::Bool("swapchain_ok", swapchain_ok),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_present_sync_interval",
                                                      std::to_string(renderer_.present_sync_interval())),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_desc_refresh_numerator",
                                                      std::to_string(
                                                          swapchain_ok
                                                              ? swap_chain_desc.BufferDesc.RefreshRate.Numerator
                                                              : 0)),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_desc_refresh_denominator",
                                                      std::to_string(
                                                          swapchain_ok
                                                              ? swap_chain_desc.BufferDesc.RefreshRate.Denominator
                                                              : 0)),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_desc_refresh_hz",
                                                      std::to_string(swapchain_desc_hz)),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_desc_refresh_period_ms",
                                                      std::to_string(PeriodMillisecondsFromHz(swapchain_desc_hz))),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_buffer_count",
                                                      std::to_string(swapchain_ok ? swap_chain_desc.BufferCount : 0)),
                                                  ProfileSink::Field::Bool(
                                                      "swapchain_windowed",
                                                      swapchain_ok && swap_chain_desc.Windowed == TRUE),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_swap_effect",
                                                      std::to_string(
                                                          swapchain_ok ? swap_chain_desc.SwapEffect : 0)),
                                              });
}

LRESULT SpecForgeApp::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (profile_.is_open() && IsProfiledInputMessage(message)) {
        LogInputMessage(message, wparam, lparam);
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
    case WM_SIZE: {
        if (wparam == SIZE_MINIMIZED) {
            if (!minimized_) {
                profile_.WriteEvent("render_idle", {ProfileSink::Field::String("reason", "minimized")});
            }
            minimized_ = true;
            pending_resize_.reset();
            return 0;
        }

        if (minimized_) {
            profile_.WriteEvent("render_resume", {ProfileSink::Field::String("reason", "restored")});
        }
        minimized_ = false;
        occluded_ = false;

        const UINT width = LOWORD(lparam);
        const UINT height = HIWORD(lparam);
        if (IsRenderableSize(width, height)) {
            pending_resize_ = PendingResize{width, height};
        }
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wparam == FALSE) {
            const bool was_visible = window_visible_;
            window_visible_ = false;
            if (was_visible) {
                profile_.WriteEvent("render_idle", {ProfileSink::Field::String("reason", "hidden")});
            }
            pending_resize_.reset();
        } else {
            const bool was_visible = window_visible_;
            window_visible_ = true;
            if (!was_visible) {
                profile_.WriteEvent("render_resume", {ProfileSink::Field::String("reason", "shown")});
            }
            occluded_ = false;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ui_.RefreshSystemColors();
        ApplyTitleBarTheme(hwnd);
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ui_.RefreshSystemColors();
        return 0;
    case WM_DISPLAYCHANGE:
        LogDisplayEnvironment("display_change");
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
        LogDisplayEnvironment("dpi_changed");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void SpecForgeApp::LogInputMessage(UINT message, WPARAM wparam, LPARAM lparam)
{
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);

    switch (message) {
    case WM_LBUTTONDOWN:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_press_left"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_RBUTTONDOWN:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_press_right"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_MBUTTONDOWN:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_press_middle"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_LBUTTONUP:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_release_left"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_RBUTTONUP:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_release_right"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_MBUTTONUP:
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_release_middle"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                     });
        break;
    case WM_MOUSEMOVE: {
        const bool left_down = (wparam & MK_LBUTTON) != 0;
        const bool right_down = (wparam & MK_RBUTTON) != 0;
        const bool middle_down = (wparam & MK_MBUTTON) != 0;
        if (!left_down && !right_down && !middle_down) {
            break;
        }
        profile_.WriteEvent("input", {
                                         ProfileSink::Field::String("kind", "pointer_move"),
                                         ProfileSink::Field::Number("x", std::to_string(x)),
                                         ProfileSink::Field::Number("y", std::to_string(y)),
                                         ProfileSink::Field::Bool("left_down", left_down),
                                         ProfileSink::Field::Bool("right_down", right_down),
                                         ProfileSink::Field::Bool("middle_down", middle_down),
                                     });
        break;
    }
    case WM_MOUSEWHEEL:
        profile_.WriteEvent("input", {
                                          ProfileSink::Field::String("kind", "wheel"),
                                          ProfileSink::Field::Number("x", std::to_string(x)),
                                          ProfileSink::Field::Number("y", std::to_string(y)),
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
