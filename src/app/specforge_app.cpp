#include "app/specforge_app.h"

#include "app/runtime_paths.h"
#include "platform/win32_message_wait.h"
#include "profile/profile_recording_status.h"
#include "ui/profile_recording_ui_state.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>
#include <shlobj_core.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <optional>
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
constexpr UINT kCompositorClockTickMessage = WM_APP + 0x54U;
constexpr UINT kProfileRecorderStateChangedMessage = WM_APP + 0x55U;
constexpr UINT_PTR kPresentationRefreshTimer = 0x5350U;
constexpr UINT kPresentationRefreshDelayMs = 500U;
constexpr float kDefaultWindowsDpi = 96.0f;
constexpr std::array<float, 4> kClearColor = {0.08f, 0.09f, 0.10f, 1.0f};
constexpr DWORD kDwmUseImmersiveDarkModeAttribute = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacyAttribute = 19;
constexpr const wchar_t* kPersonalizeRegistryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kAppsUseLightThemeRegistryValue = L"AppsUseLightTheme";

std::string HResultMessage(std::string_view action, HRESULT result)
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

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::optional<std::filesystem::path> WindowsFontsDirectory()
{
    PWSTR fonts_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, KF_FLAG_DEFAULT, nullptr, &fonts_path)) ||
        fonts_path == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path result(fonts_path);
    CoTaskMemFree(fonts_path);
    return result;
}

std::optional<std::filesystem::path> AddUiFont(ImGuiIO& io)
{
    io.Fonts->AddFontDefaultVector();
    const std::optional<std::filesystem::path> fonts_directory = WindowsFontsDirectory();
    if (!fonts_directory) {
        return std::nullopt;
    }

    constexpr std::array<const wchar_t*, 5> kPreferredFonts = {
        L"NotoSansSC-VF.ttf",
        L"Deng.ttf",
        L"simhei.ttf",
        L"msyh.ttc",
        L"simsun.ttc",
    };
    static constexpr ImWchar kCjkGlyphRanges[] = {
        0x2000, 0x206f,  // General punctuation.
        0x2e80, 0x2eff,  // CJK radicals supplement.
        0x2f00, 0x2fdf,  // Kangxi radicals.
        0x3000, 0x30ff,  // CJK symbols, punctuation, hiragana, and katakana.
        0x31f0, 0x31ff,  // Katakana phonetic extensions.
        0x3400, 0x4dbf,  // CJK unified ideographs extension A.
        0x4e00, 0x9fff,  // CJK unified ideographs.
        0xf900, 0xfaff,  // CJK compatibility ideographs.
        0xff00, 0xffef,  // Halfwidth and fullwidth forms.
        0,
    };

    ImFontConfig font_config;
    font_config.MergeMode = true;
    font_config.OversampleH = 2;
    font_config.OversampleV = 1;

    for (const wchar_t* filename : kPreferredFonts) {
        const std::filesystem::path candidate = *fonts_directory / filename;
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            continue;
        }

        const std::string font_path = PathToUtf8(candidate);
        // ImGui 1.92 requires merged fonts to keep the same implicit reference-size mode
        // as the default vector font; passing LegacySize here trips a runtime assertion.
        if (io.Fonts->AddFontFromFileTTF(
                font_path.c_str(),
                0.0f,
                &font_config,
                kCjkGlyphRanges) != nullptr) {
            return candidate;
        }
    }

    return std::nullopt;
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
            const bool direct_manipulation_attached = touchpad_gestures_.OwnsWindow(
                reinterpret_cast<std::uintptr_t>(message.hwnd));
            const Win32TouchpadQueuedMessageAction touchpad_message_action =
                ClassifyWin32TouchpadQueuedMessage(
                    message.message,
                    direct_manipulation_attached);
            if (touchpad_message_action ==
                Win32TouchpadQueuedMessageAction::InvalidateRender) {
                message_render_observer_.ObserveQueuedMessage(Win32ObservedMessage{
                    reinterpret_cast<std::uintptr_t>(message.hwnd),
                    message.message,
                    static_cast<std::uintptr_t>(message.wParam),
                    static_cast<std::intptr_t>(message.lParam)});
            } else {
                touchpad_update_pending_ = true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!running_) {
            break;
        }

        auto now = RenderWakeScheduler::Clock::now();
        const auto maintenance_deadline = ui_.NextMaintenanceDeadline();
        if (maintenance_deadline && now >= *maintenance_deadline) {
            ui_.RunMaintenance(now);
            now = RenderWakeScheduler::Clock::now();
            render_wake_scheduler_.RequestFrame();
        }

        const bool window_renderable = !minimized_ && window_visible_;
        bool touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
        UpdateCompositorClockBoost(window_renderable, touchpad_active);
        const bool compositor_clock_paced = compositor_clock_.boost_active();
        const bool render_permitted = !compositor_clock_paced || compositor_clock_tick_ready_;
        const bool should_render =
            window_renderable && render_wake_scheduler_.ShouldRender(now, render_permitted);
        if (should_render) {
            if (compositor_clock_paced) {
                compositor_clock_tick_ready_ = false;
                touchpad_update_tick_ready_ = false;
            }
            touchpad_update_pending_ = false;
            render_wake_scheduler_.BeginFrame(now);
            RenderFrame();
            const ImGuiIO& io = ImGui::GetIO();
            const bool popup_open = ImGui::IsPopupOpen(
                nullptr,
                ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
            UpdateCompositorClockBoost(window_renderable, touchpad_active);
            render_wake_scheduler_.EndFrame(
                RenderWakeScheduler::Clock::now(),
                {
                    .touchpad_active = touchpad_active,
                    .compositor_clock_paced = compositor_clock_.boost_active(),
                    .text_input_active = io.WantTextInput && io.ConfigInputTextCursorBlink,
                    .popup_open = popup_open,
                });
        } else if (
            window_renderable && compositor_clock_paced && touchpad_update_pending_ &&
            touchpad_update_tick_ready_) {
            touchpad_update_pending_ = false;
            touchpad_update_tick_ready_ = false;
            touchpad_gestures_.PumpUpdates();
            touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
            UpdateCompositorClockBoost(window_renderable, touchpad_active);
        }

        const bool wait_render_permitted =
            !compositor_clock_.boost_active() || compositor_clock_tick_ready_;
        (void)WaitForWin32MessageOrDeadline(render_wake_scheduler_.NextWakeDeadline(
            window_renderable,
            ui_.NextMaintenanceDeadline(),
            wait_render_permitted));
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
    if (profile_.is_open()) {
        profile_status_message_ = "Recording started by SPECFORGE_PROFILE.";
        LogProfileRecordingStarted("environment", "startup");
    } else if (!profile_.error_message().empty()) {
        profile_status_message_ = "Could not start recording: " + profile_.error_message();
    }

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
    const HWND profile_state_window = window_.hwnd();
    profile_.SetStateChangeCallback([profile_state_window]() noexcept {
        (void)PostMessageW(profile_state_window, kProfileRecorderStateChangedMessage, 0, 0);
    });
    ApplyTitleBarTheme(window_.hwnd());

    const HRESULT renderer_result = renderer_.Initialize(window_.hwnd());
    if (FAILED(renderer_result)) {
        throw std::runtime_error(HResultMessage(renderer_.last_error_operation(), renderer_result));
    }

    const bool compositor_clock_available =
        compositor_clock_.Initialize(window_.hwnd(), kCompositorClockTickMessage);
    profile_.WriteEvent("compositor_clock", {
                                                  ProfileSink::Field::String("action", "initialize"),
                                                  ProfileSink::Field::Bool("available", compositor_clock_available),
                                                  ProfileSink::Field::String(
                                                      "result",
                                                      HResultHex(compositor_clock_.last_boost_result())),
                                                  ProfileSink::Field::Number(
                                                      "last_wait_result",
                                                      std::to_string(compositor_clock_.last_wait_result())),
                                                  ProfileSink::Field::Number(
                                                      "tick_count",
                                                      std::to_string(compositor_clock_.tick_count())),
                                              });

    InitializeUiBackends();
    if (!message_render_observer_.Start(
            &SpecForgeApp::InvalidateRenderFromWin32Message,
            this,
            kCompositorClockTickMessage,
            &SpecForgeApp::ObserveWin32Message)) {
        throw std::runtime_error("Failed to observe Win32 messages for render invalidation.");
    }
    window_.Show(show_command);
    LogDisplayEnvironment("startup");
    LogPresentationUpdates();
}

void SpecForgeApp::InitializeUiBackends()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    const RuntimePaths runtime_paths = DefaultRuntimePaths();
    std::error_code data_directory_error;
    std::filesystem::create_directories(runtime_paths.local_user_state_root, data_directory_error);
    if (data_directory_error) {
        throw std::runtime_error(
            "Failed to create SpecForge runtime data directory '" +
            PathToUtf8(runtime_paths.local_user_state_root) + "': " + data_directory_error.message());
    }
    imgui_ini_path_utf8_ = PathToUtf8(runtime_paths.imgui_ini_path);
    io.IniFilename = imgui_ini_path_utf8_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDecoration = true;
    const std::optional<std::filesystem::path> ui_font_path = AddUiFont(io);

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
                                           ProfileSink::Field::String(
                                               "ui_font",
                                               ui_font_path ? PathToUtf8(*ui_font_path) : "imgui_default"),
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

    if (!viewport_renderer_.Initialize(renderer_.factory(), renderer_.device(), renderer_.context())) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui SDR viewport renderer.");
    }

    imgui_initialized_ = true;
}

void SpecForgeApp::Shutdown()
{
    if (window_.hwnd() != nullptr) {
        KillTimer(window_.hwnd(), kPresentationRefreshTimer);
    }
    if (compositor_clock_.boost_requested()) {
        (void)compositor_clock_.SetBoostRequested(false);
        profile_.WriteEvent("compositor_clock", {
                                                  ProfileSink::Field::String("action", "shutdown_release"),
                                                  ProfileSink::Field::Bool("requested", false),
                                                  ProfileSink::Field::Bool(
                                                      "active",
                                                      compositor_clock_.boost_active()),
                                                  ProfileSink::Field::String(
                                                      "result",
                                                      HResultHex(compositor_clock_.last_boost_result())),
                                                  ProfileSink::Field::Number(
                                                      "last_wait_result",
                                                      std::to_string(compositor_clock_.last_wait_result())),
                                                  ProfileSink::Field::Number(
                                                      "tick_count",
                                                      std::to_string(compositor_clock_.tick_count())),
                                              });
    }
    compositor_clock_.Shutdown();
    message_render_observer_.Stop();
    touchpad_gestures_.ClearTarget();

    if (imgui_initialized_) {
        profile_.WriteEvent("shutdown");
        ImGui_ImplDX11_Shutdown();
        viewport_renderer_.Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }

    renderer_.Shutdown();
    pending_resize_.reset();
    fullscreen_restore_.reset();
    immersive_plot_entered_fullscreen_ = false;
    profile_.SetStateChangeCallback({});
    profile_.Stop();
    window_.ClearMessageHandler();
    window_.Destroy();
}

void SpecForgeApp::RenderFrame()
{
    profile_.BeginFrame();
    struct ProfileFrameFinalizationGuard {
        ProfileSink& sink;
        ~ProfileFrameFinalizationGuard() { sink.CompleteFrameFinalization(); }
    } profile_frame_finalization{profile_};

    ApplyPendingResize();

    ++frame_index_;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        ProfileTimer timer(profile_, "view_update", frame_index_);
        RefreshProfileRecordingStatus();
        const ProfileSink::StateSnapshot profile_state = profile_.state_snapshot();
        ShellStatus status;
        status.profile_open = profile_state.open;
        status.profile_stopping = profile_state.stopping;
        status.navigation_trace_recording_active = profile_state.frame_recording_active;
        status.profile = &profile_;
        status.profile_path = profile_.path().empty() ? nullptr : &profile_.path();
        status.profile_status_message = profile_status_message_;
        status.client_width = window_.client_width();
        status.client_height = window_.client_height();
        status.frame_index = frame_index_;
        ui_.Render(status);
        if (ui_.TakeImmersivePlotModeToggleRequest()) {
            ToggleImmersivePlotMode();
        }
        if (ui_.TakeProfileRecordingToggleRequest()) {
            ToggleProfileRecording();
        }
    }

    {
        ProfileTimer timer(profile_, "draw_submission", frame_index_);
        ImGui::Render();
    }

    {
        ProfileTimer timer(profile_, "render_pass", frame_index_);
        const HRESULT begin_result = renderer_.BeginFrame(kClearColor);
        if (FAILED(begin_result)) {
            throw std::runtime_error(
                HResultMessage(renderer_.last_error_operation(), begin_result));
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            viewport_renderer_.SetCompositorClockPaced(compositor_clock_.boost_active());
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            const D3D11RendererError viewport_error = viewport_renderer_.TakeLastError();
            if (FAILED(viewport_error.result)) {
                throw std::runtime_error(HResultMessage(viewport_error.operation, viewport_error.result));
            }
        }
    }

    std::vector<NavigationLatencyPresentation> navigation_presentations;
    for (const D3D11ViewportPresentCompletion& completion :
         viewport_renderer_.TakePresentCompletions()) {
        navigation_presentations.push_back({completion.viewport_id, completion.completed_at});
    }

    const D3D11PresentMode present_mode = compositor_clock_.boost_active()
                                                 ? D3D11PresentMode::CompositorClock
                                                 : D3D11PresentMode::DisplayVSync;
    HRESULT present_result = S_OK;
    NavigationLatencyTimePoint present_completed_at;
    const bool profile_frame_active_at_present =
        profile_.state_snapshot().frame_recording_active;
    if (profile_frame_active_at_present) {
        const auto present_start = std::chrono::steady_clock::now();
        present_result = renderer_.Present(present_mode);
        const auto present_elapsed = std::chrono::steady_clock::now() - present_start;
        profile_.WriteDuration(
            "present",
            frame_index_,
            std::chrono::duration<double, std::milli>(present_elapsed).count());
    } else {
        present_result = renderer_.Present(present_mode);
    }
    present_completed_at = NavigationLatencyTrace::Now();

    if (FAILED(present_result)) {
        throw std::runtime_error(HResultMessage(renderer_.last_error_operation(), present_result));
    }
    if (present_result == S_FALSE) {
        render_wake_scheduler_.RequestFrame();
    }
    if (present_result == S_OK) {
        navigation_presentations.push_back({ImGui::GetMainViewport()->ID, present_completed_at});
    }
    std::vector<NavigationLatencyReport> navigation_reports =
        ui_.CompleteFramePresentations(frame_index_, navigation_presentations);
    for (const NavigationLatencyReport& report : navigation_reports) {
        (void)WriteNavigationLatencyProfileEvent(profile_, report);
    }
    LogPresentationUpdates();
}

void SpecForgeApp::UpdateCompositorClockBoost(bool window_renderable, bool touchpad_active)
{
    const bool plot_interaction_active = ui_.latency_sensitive_plot_interaction_active();
    const bool requested = window_renderable && (plot_interaction_active || touchpad_active);
    if (requested == compositor_clock_.boost_requested()) {
        return;
    }

    compositor_clock_tick_ready_ = false;
    touchpad_update_tick_ready_ = false;
    if (!requested) {
        touchpad_update_pending_ = false;
    }
    (void)compositor_clock_.SetBoostRequested(requested);
    profile_.WriteEvent("compositor_clock", {
                                                  ProfileSink::Field::String("action", "boost_request"),
                                                  ProfileSink::Field::Bool("available", compositor_clock_.available()),
                                                  ProfileSink::Field::Bool("requested", requested),
                                                  ProfileSink::Field::Bool(
                                                      "active",
                                                      compositor_clock_.boost_active()),
                                                  ProfileSink::Field::Bool(
                                                      "plot_interaction_active",
                                                      plot_interaction_active),
                                                  ProfileSink::Field::Bool(
                                                      "touchpad_active",
                                                      touchpad_active),
                                                  ProfileSink::Field::String(
                                                      "presentation_backend",
                                                      D3D11PresentationBackendName(
                                                          renderer_.presentation_backend())),
                                                  ProfileSink::Field::String(
                                                      "presentation_degradation",
                                                      D3D11PresentationDegradationName(
                                                          renderer_.presentation_degradation(
                                                              compositor_clock_.boost_active()
                                                                  ? D3D11PresentMode::CompositorClock
                                                                  : D3D11PresentMode::DisplayVSync))),
                                                  ProfileSink::Field::String(
                                                      "result",
                                                      HResultHex(compositor_clock_.last_boost_result())),
                                                  ProfileSink::Field::Number(
                                                      "last_wait_result",
                                                      std::to_string(compositor_clock_.last_wait_result())),
                                                  ProfileSink::Field::Number(
                                                      "tick_count",
                                                      std::to_string(compositor_clock_.tick_count())),
                                              });
}

void SpecForgeApp::InvalidateRenderFromWin32Message(void* context) noexcept
{
    static_cast<SpecForgeApp*>(context)->RequestMessageRender();
}

void SpecForgeApp::ObserveWin32Message(
    void* context,
    const Win32ObservedMessage& message) noexcept
{
    auto* app = static_cast<SpecForgeApp*>(context);
    constexpr std::intptr_t kPreviousKeyStateMask = 1LL << 30;
    if (!app->profile_.is_open() || message.message != WM_KEYDOWN ||
        (message.lparam & kPreviousKeyStateMask) != 0) {
        return;
    }
    if (message.wparam == VK_LEFT) {
        app->ui_.RecordNavigationKeyInput(NavigationLatencyInputKind::KeyboardPrevious);
    } else if (message.wparam == VK_RIGHT) {
        app->ui_.RecordNavigationKeyInput(NavigationLatencyInputKind::KeyboardNext);
    }
}

void SpecForgeApp::RequestMessageRender() noexcept
{
    std::optional<RenderWakeScheduler::Duration> settings_save_delay;
    if (imgui_initialized_ && ImGui::GetCurrentContext() != nullptr) {
        const float saving_rate_seconds = ImGui::GetIO().IniSavingRate;
        if (saving_rate_seconds > 0.0f) {
            settings_save_delay = std::chrono::duration_cast<RenderWakeScheduler::Duration>(
                std::chrono::duration<float>(saving_rate_seconds));
        }
    }
    render_wake_scheduler_.RequestFrame(settings_save_delay);
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

    const HRESULT resize_result = renderer_.Resize(resize.width, resize.height);
    if (FAILED(resize_result)) {
        throw std::runtime_error(HResultMessage(renderer_.last_error_operation(), resize_result));
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

void SpecForgeApp::ToggleImmersivePlotMode()
{
    if (ui_.immersive_plot_mode()) {
        ExitImmersivePlotMode();
    } else {
        EnterImmersivePlotMode();
    }
}

void SpecForgeApp::EnterImmersivePlotMode()
{
    if (ui_.immersive_plot_mode()) {
        return;
    }

    const bool was_fullscreen = fullscreen_restore_.has_value();
    ui_.EnterImmersivePlotMode();
    if (!was_fullscreen) {
        EnterFullscreen();
        immersive_plot_entered_fullscreen_ = fullscreen_restore_.has_value();
    } else {
        immersive_plot_entered_fullscreen_ = false;
    }

    profile_.WriteEvent("immersive_plot", {ProfileSink::Field::Bool("enabled", true)});
}

void SpecForgeApp::ExitImmersivePlotMode()
{
    const bool was_immersive = ui_.immersive_plot_mode();
    ui_.ExitImmersivePlotMode();
    if (immersive_plot_entered_fullscreen_ && fullscreen_restore_) {
        ExitFullscreen();
    }
    immersive_plot_entered_fullscreen_ = false;

    if (was_immersive) {
        profile_.WriteEvent("immersive_plot", {ProfileSink::Field::Bool("enabled", false)});
    }
}

void SpecForgeApp::ToggleProfileRecording()
{
    switch (ResolveProfileRecordingToggleAction(profile_.is_open(), profile_.is_stopping())) {
    case ProfileRecordingToggleAction::Stop:
        StopProfileRecording("user_toggle");
        break;
    case ProfileRecordingToggleAction::Start:
        StartProfileRecording("user_toggle");
        break;
    case ProfileRecordingToggleAction::None:
        return;
    }
    render_wake_scheduler_.RequestFrame();
}

void SpecForgeApp::StartProfileRecording(std::string_view trigger)
{
    if (profile_.is_open() || profile_.is_stopping()) {
        return;
    }
    if (!profile_.StartDefault()) {
        profile_status_message_ = "Could not start recording: " + profile_.error_message();
        displayed_profile_stop_reason_ = ProfileSink::StopReason::WriteFailure;
        return;
    }

    displayed_profile_stop_reason_ = ProfileSink::StopReason::None;
    profile_status_message_ = "Recording performance diagnostics.";
    LogProfileRecordingStarted(trigger, "recording_started");
    profile_.WriteEvent("compositor_clock", {
                                                  ProfileSink::Field::String("action", "recording_snapshot"),
                                                  ProfileSink::Field::Bool(
                                                      "available",
                                                      compositor_clock_.available()),
                                                  ProfileSink::Field::Bool(
                                                      "requested",
                                                      compositor_clock_.boost_requested()),
                                                  ProfileSink::Field::Bool(
                                                      "active",
                                                      compositor_clock_.boost_active()),
                                                  ProfileSink::Field::String(
                                                      "result",
                                                      HResultHex(compositor_clock_.last_boost_result())),
                                                  ProfileSink::Field::Number(
                                                      "last_wait_result",
                                                      std::to_string(compositor_clock_.last_wait_result())),
                                                  ProfileSink::Field::Number(
                                                      "tick_count",
                                                      std::to_string(compositor_clock_.tick_count())),
                                              });
    LogDisplayEnvironment("recording_started");
}

void SpecForgeApp::StopProfileRecording(std::string_view trigger)
{
    if (!profile_.is_open()) {
        return;
    }
    profile_.WriteEvent("profile_recording", {
                                                   ProfileSink::Field::String("action", "stop"),
                                                   ProfileSink::Field::String("trigger", std::string(trigger)),
                                                   ProfileSink::Field::Number(
                                                       "dropped_events",
                                                       std::to_string(profile_.dropped_event_count())),
                                               });
    displayed_profile_stop_reason_ = ProfileSink::StopReason::None;
    profile_status_message_ = "Finishing recording...";
    profile_.RequestStopAfterFrame();
}

void SpecForgeApp::LogProfileRecordingStarted(
    std::string_view trigger,
    std::string_view configuration_reason)
{
    if (!profile_.is_open()) {
        return;
    }
    const ProfileSink::Limits limits;
    profile_.WriteEvent("profile_recording", {
                                                   ProfileSink::Field::String("action", "start"),
                                                   ProfileSink::Field::String("trigger", std::string(trigger)),
                                                   ProfileSink::Field::String("writer", "bounded_async_jsonl"),
                                                   ProfileSink::Field::Number(
                                                       "max_queue_bytes",
                                                       std::to_string(limits.max_queue_bytes)),
                                                   ProfileSink::Field::Number(
                                                       "max_file_bytes",
                                                       std::to_string(limits.max_file_bytes)),
                                                   ProfileSink::Field::Number(
                                                       "max_duration_seconds",
                                                       std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                                          limits.max_duration)
                                                                          .count())),
                                               });
    WriteRuntimeConfiguration(configuration_reason);
}

void SpecForgeApp::WriteRuntimeConfiguration(std::string_view reason)
{
    if (!profile_.is_open()) {
        return;
    }
    const RuntimePaths runtime_paths = DefaultRuntimePaths();
    const SpectrumSnapshotHandle snapshot = ui_.current_snapshot();
    const std::string source_type(
        snapshot ? MetadataValue(snapshot->source.metadata, "source_type") : std::string_view{});
    profile_.WriteEvent("runtime_config", {
                                            ProfileSink::Field::String("reason", std::string(reason)),
                                            ProfileSink::Field::String("target", "win32_dx11_imgui_implot"),
                                            ProfileSink::Field::String(
                                                "release_profile",
                                                ReleaseProfileName(runtime_paths.release_profile)),
                                            ProfileSink::Field::String(
                                                "package_root",
                                                PathToUtf8(runtime_paths.package_root)),
                                            ProfileSink::Field::String(
                                                "local_user_state_root",
                                                PathToUtf8(runtime_paths.local_user_state_root)),
                                            ProfileSink::Field::String(
                                                "profile_path",
                                                PathToUtf8(profile_.path())),
                                            ProfileSink::Field::String(
                                                "source",
                                                snapshot ? snapshot->source.display_name : ""),
                                            ProfileSink::Field::String("source_type", source_type),
                                            ProfileSink::Field::Bool(
                                                "can_plot_current_spectrum",
                                                snapshot && snapshot->capabilities.can_plot_current_spectrum),
                                            ProfileSink::Field::Bool(
                                                "immersive_plot",
                                                ui_.immersive_plot_mode()),
                                            ProfileSink::Field::Bool(
                                                "fullscreen",
                                                fullscreen_restore_.has_value()),
                                            ProfileSink::Field::Number(
                                                "client_width",
                                                std::to_string(window_.client_width())),
                                            ProfileSink::Field::Number(
                                                "client_height",
                                                std::to_string(window_.client_height())),
                                            ProfileSink::Field::Number(
                                                "frame",
                                                std::to_string(frame_index_)),
                                            ProfileSink::Field::Number(
                                                "spectrum_count",
                                                std::to_string(snapshot ? snapshot->collection.spectrum_count : 0)),
                                        });
}

void SpecForgeApp::RefreshProfileRecordingStatus()
{
    (void)profile_.TryFinalizeStop();
    if (profile_.is_open()) {
        if (profile_.dropped_event_count() > 0) {
            profile_status_message_ = std::to_string(profile_.dropped_event_count()) +
                                      " events dropped under recorder queue pressure.";
        }
        return;
    }
    if (profile_.is_stopping()) {
        profile_status_message_ = "Finishing recording...";
        return;
    }

    const ProfileSink::StopReason reason = profile_.stop_reason();
    if (reason == displayed_profile_stop_reason_) {
        return;
    }
    displayed_profile_stop_reason_ = reason;
    if (reason != ProfileSink::StopReason::None) {
        profile_status_message_ = ProfileRecordingStatusMessage(
            reason,
            profile_.dropped_event_count(),
            profile_.error_message());
    }
}

void SpecForgeApp::LogPresentationUpdates()
{
    const D3D11PresentMode present_mode = compositor_clock_.boost_active()
                                                  ? D3D11PresentMode::CompositorClock
                                                  : D3D11PresentMode::DisplayVSync;
    WritePresentationUpdate(
        "main",
        0,
        window_.hwnd(),
        renderer_.presentation_backend(),
        renderer_.presentation_degradation(present_mode),
        renderer_.display_refresh_state(),
        renderer_.TakePresentationTransition(),
        renderer_.TakeCompositionFeedback());

    for (D3D11ViewportPresentationUpdate& update :
         viewport_renderer_.TakePresentationUpdates()) {
        WritePresentationUpdate(
            "viewport",
            update.viewport_id,
            update.hwnd,
            update.backend,
            update.degradation,
            update.refresh_state,
            update.transition,
            update.feedback);
    }
}

void SpecForgeApp::WritePresentationUpdate(
    std::string_view target,
    unsigned int viewport_id,
    HWND hwnd,
    D3D11PresentationBackend backend,
    D3D11PresentationDegradation degradation,
    const Win32DisplayRefreshState& refresh_state,
    const D3D11PresentationTransition& transition,
    const D3D11CompositionFeedback& feedback)
{
    if (!profile_.is_open()) {
        return;
    }

    if (!transition.empty()) {
        profile_.WriteEvent("presentation_state", {
                                                      ProfileSink::Field::String(
                                                          "target",
                                                          std::string(target)),
                                                      ProfileSink::Field::Number(
                                                          "viewport_id",
                                                          std::to_string(viewport_id)),
                                                      ProfileSink::Field::Number(
                                                          "hwnd",
                                                          std::to_string(
                                                              reinterpret_cast<std::uintptr_t>(hwnd))),
                                                      ProfileSink::Field::String(
                                                          "previous_backend",
                                                          D3D11PresentationBackendName(
                                                              transition.previous_backend)),
                                                      ProfileSink::Field::String(
                                                          "backend",
                                                          D3D11PresentationBackendName(backend)),
                                                      ProfileSink::Field::String(
                                                          "degradation",
                                                          D3D11PresentationDegradationName(
                                                              degradation)),
                                                      ProfileSink::Field::String(
                                                          "transition_operation",
                                                          std::string(transition.operation)),
                                                      ProfileSink::Field::String(
                                                          "transition_result",
                                                          HResultHex(transition.reason)),
                                                      ProfileSink::Field::Bool(
                                                          "drr_configured",
                                                          refresh_state.drr_configured()),
                                                      ProfileSink::Field::Bool(
                                                          "system_refresh_constrained",
                                                          refresh_state.system_refresh_constrained()),
                                                      ProfileSink::Field::Number(
                                                          "virtual_refresh_hz",
                                                          std::to_string(
                                                              refresh_state.virtual_refresh_hz())),
                                                      ProfileSink::Field::Number(
                                                          "physical_refresh_hz",
                                                          std::to_string(
                                                              refresh_state.physical_refresh_hz())),
                                                      ProfileSink::Field::Number(
                                                          "requested_refresh_hz",
                                                          std::to_string(
                                                              refresh_state.requested_refresh_hz())),
                                                      ProfileSink::Field::Number(
                                                          "preferred_duration",
                                                          std::to_string(
                                                              refresh_state.preferred_duration)),
                                                      ProfileSink::Field::Number(
                                                          "preferred_tolerance",
                                                          std::to_string(
                                                              refresh_state.preferred_tolerance)),
                                                  });
    }

    if (!feedback.empty()) {
        profile_.WriteEvent("presentation_feedback", {
                                                         ProfileSink::Field::String(
                                                             "target",
                                                             std::string(target)),
                                                         ProfileSink::Field::Number(
                                                             "viewport_id",
                                                             std::to_string(viewport_id)),
                                                         ProfileSink::Field::Number(
                                                             "hwnd",
                                                             std::to_string(
                                                                 reinterpret_cast<std::uintptr_t>(hwnd))),
                                                         ProfileSink::Field::String(
                                                             "backend",
                                                             D3D11PresentationBackendName(backend)),
                                                         ProfileSink::Field::Number(
                                                             "present_submissions",
                                                             std::to_string(
                                                                 feedback.present_submissions)),
                                                         ProfileSink::Field::Number(
                                                             "status_queued",
                                                             std::to_string(feedback.status_queued)),
                                                         ProfileSink::Field::Number(
                                                             "status_skipped",
                                                             std::to_string(feedback.status_skipped)),
                                                         ProfileSink::Field::Number(
                                                             "status_canceled",
                                                             std::to_string(feedback.status_canceled)),
                                                         ProfileSink::Field::Number(
                                                             "composition_frames",
                                                             std::to_string(feedback.composition_frames)),
                                                         ProfileSink::Field::Number(
                                                             "independent_flip_frames",
                                                             std::to_string(
                                                                 feedback.independent_flip_frames)),
                                                         ProfileSink::Field::Number(
                                                             "buffer_acquire_skipped",
                                                             std::to_string(
                                                                 feedback.buffer_acquire_skipped)),
                                                         ProfileSink::Field::Number(
                                                             "last_actual_duration",
                                                             std::to_string(
                                                                 feedback.last_actual_duration)),
                                                         ProfileSink::Field::Number(
                                                             "last_displayed_time",
                                                             std::to_string(
                                                                 feedback.last_displayed_time)),
                                                         ProfileSink::Field::String(
                                                             "statistics_result",
                                                             HResultHex(feedback.statistics_error)),
                                                     });
    }
}

void SpecForgeApp::SchedulePresentationTargetRefresh(HWND hwnd) noexcept
{
    if (hwnd != nullptr) {
        SetTimer(
            hwnd,
            kPresentationRefreshTimer,
            kPresentationRefreshDelayMs,
            nullptr);
    }
}

void SpecForgeApp::RefreshPresentationTargets(std::string_view reason)
{
    const HRESULT result = renderer_.RefreshPresentationTarget();
    if (FAILED(result)) {
        profile_.WriteEvent("presentation_refresh_failed", {
                                                                 ProfileSink::Field::String(
                                                                     "target",
                                                                     "main"),
                                                                 ProfileSink::Field::String(
                                                                     "reason",
                                                                     std::string(reason)),
                                                                 ProfileSink::Field::String(
                                                                     "operation",
                                                                     std::string(
                                                                         renderer_.last_error_operation())),
                                                                 ProfileSink::Field::String(
                                                                     "result",
                                                                     HResultHex(result)),
                                                             });
    }
    viewport_renderer_.RefreshPresentationTargets();
    const D3D11RendererError viewport_error = viewport_renderer_.TakeLastError();
    if (FAILED(viewport_error.result)) {
        profile_.WriteEvent("presentation_refresh_failed", {
                                                                 ProfileSink::Field::String(
                                                                     "target",
                                                                     "viewport"),
                                                                 ProfileSink::Field::String(
                                                                     "reason",
                                                                     std::string(reason)),
                                                                 ProfileSink::Field::String(
                                                                     "operation",
                                                                     std::string(
                                                                         viewport_error.operation)),
                                                                 ProfileSink::Field::String(
                                                                     "result",
                                                                     HResultHex(
                                                                         viewport_error.result)),
                                                             });
    }
    LogPresentationUpdates();
    LogDisplayEnvironment(reason);
    render_wake_scheduler_.RequestFrame();
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
    DXGI_COLOR_SPACE_TYPE swap_chain_color_space = DXGI_COLOR_SPACE_CUSTOM;
    const bool swapchain_color_space_configured =
        renderer_.GetConfiguredSwapChainColorSpace(swap_chain_color_space);
    const double swapchain_desc_hz = swapchain_ok
                                         ? RatioHz(
                                               swap_chain_desc.BufferDesc.RefreshRate.Numerator,
                                               swap_chain_desc.BufferDesc.RefreshRate.Denominator)
                                         : 0.0;
    const Win32DisplayRefreshState& refresh_state =
        renderer_.display_refresh_state();
    const D3D11PresentMode present_mode = compositor_clock_.boost_active()
                                                  ? D3D11PresentMode::CompositorClock
                                                  : D3D11PresentMode::DisplayVSync;

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
                                                  ProfileSink::Field::String(
                                                      "presentation_backend",
                                                      D3D11PresentationBackendName(
                                                          renderer_.presentation_backend())),
                                                  ProfileSink::Field::String(
                                                      "presentation_degradation",
                                                      D3D11PresentationDegradationName(
                                                          renderer_.presentation_degradation(
                                                              present_mode))),
                                                  ProfileSink::Field::Bool(
                                                      "composition_independent_flip_supported",
                                                      renderer_
                                                          .composition_independent_flip_supported()),
                                                  ProfileSink::Field::Bool(
                                                      "composition_statistics_available",
                                                      renderer_.composition_statistics_available()),
                                                  ProfileSink::Field::Bool(
                                                      "display_config_path_found",
                                                      refresh_state.path_found),
                                                  ProfileSink::Field::Number(
                                                      "display_config_result",
                                                      std::to_string(
                                                          refresh_state.display_config_result)),
                                                  ProfileSink::Field::Number(
                                                      "display_config_query_flags",
                                                      std::to_string(
                                                          refresh_state
                                                              .display_config_query_flags)),
                                                  ProfileSink::Field::Bool(
                                                      "display_config_virtual_refresh_aware",
                                                      refresh_state.virtual_refresh_rate_aware),
                                                  ProfileSink::Field::Bool(
                                                      "display_config_drr_configured",
                                                      refresh_state.drr_configured()),
                                                  ProfileSink::Field::Bool(
                                                      "system_refresh_constrained",
                                                      refresh_state
                                                          .system_refresh_constrained()),
                                                  ProfileSink::Field::Number(
                                                      "max_available_refresh_hz",
                                                      std::to_string(
                                                          refresh_state
                                                              .max_available_refresh_hz)),
                                                  ProfileSink::Field::Number(
                                                      "virtual_refresh_hz",
                                                      std::to_string(
                                                          refresh_state.virtual_refresh_hz())),
                                                  ProfileSink::Field::Number(
                                                      "physical_refresh_hz",
                                                      std::to_string(
                                                          refresh_state.physical_refresh_hz())),
                                                  ProfileSink::Field::Number(
                                                      "presentation_requested_refresh_hz",
                                                      std::to_string(
                                                          refresh_state.requested_refresh_hz())),
                                                  ProfileSink::Field::Number(
                                                      "presentation_preferred_duration",
                                                      std::to_string(
                                                          refresh_state.preferred_duration)),
                                                  ProfileSink::Field::Number(
                                                      "presentation_preferred_tolerance",
                                                      std::to_string(
                                                          refresh_state.preferred_tolerance)),
                                                  ProfileSink::Field::String(
                                                      "presentation_duration_basis",
                                                      DisplayRefreshDurationBasisName(
                                                          refresh_state.duration_basis)),
                                                  ProfileSink::Field::Bool(
                                                      "swapchain_color_space_configured",
                                                      swapchain_color_space_configured),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_color_space",
                                                      std::to_string(
                                                          swapchain_color_space_configured
                                                              ? swap_chain_color_space
                                                              : 0)),
                                                  ProfileSink::Field::Bool(
                                                      "swapchain_srgb_sdr_explicit",
                                                      swapchain_color_space_configured &&
                                                          swap_chain_color_space ==
                                                              DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_present_sync_interval",
                                                      std::to_string(D3D11PresentSyncInterval(
                                                          D3D11PresentMode::DisplayVSync))),
                                                  ProfileSink::Field::Bool(
                                                      "swapchain_tearing_supported",
                                                      renderer_.tearing_supported()),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_compositor_clock_present_flags",
                                                      std::to_string(D3D11PresentFlags(
                                                          D3D11PresentMode::CompositorClock,
                                                          renderer_.tearing_supported()))),
                                                  ProfileSink::Field::Bool(
                                                      "compositor_clock_available",
                                                      compositor_clock_.available()),
                                                  ProfileSink::Field::Bool(
                                                      "compositor_clock_boost_requested",
                                                      compositor_clock_.boost_requested()),
                                                  ProfileSink::Field::Bool(
                                                      "compositor_clock_boost_active",
                                                      compositor_clock_.boost_active()),
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
                                                  ProfileSink::Field::Number(
                                                      "swapchain_format",
                                                      std::to_string(
                                                          swapchain_ok ? swap_chain_desc.BufferDesc.Format : 0)),
                                                  ProfileSink::Field::Bool(
                                                      "swapchain_windowed",
                                                      swapchain_ok && swap_chain_desc.Windowed == TRUE),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_swap_effect",
                                                      std::to_string(
                                                          swapchain_ok ? swap_chain_desc.SwapEffect : 0)),
                                                  ProfileSink::Field::Number(
                                                      "swapchain_flags",
                                                      std::to_string(swapchain_ok ? swap_chain_desc.Flags : 0)),
                                              });
}

LRESULT SpecForgeApp::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (Win32MessageCanInvalidateRender(
            message,
            kCompositorClockTickMessage)) {
        RequestMessageRender();
    }

    if (message == kProfileRecorderStateChangedMessage) {
        RequestMessageRender();
        return 0;
    }

    if (profile_.is_open() && IsProfiledInputMessage(message)) {
        LogInputMessage(message, wparam, lparam);
    }
    if (message == kCompositorClockTickMessage) {
        const CompositorClockTickAction action = ClassifyCompositorClockTick(
            compositor_clock_.ConsumeTick(),
            compositor_clock_.boost_active());
        if (action == CompositorClockTickAction::GrantFramePermission) {
            compositor_clock_tick_ready_ = true;
            touchpad_update_tick_ready_ = true;
        } else if (action == CompositorClockTickAction::RequestFallbackFrame) {
            compositor_clock_tick_ready_ = false;
            touchpad_update_tick_ready_ = false;
            render_wake_scheduler_.RequestFrame();
            profile_.WriteEvent("compositor_clock", {
                                                          ProfileSink::Field::String(
                                                              "action",
                                                              "waiter_failure_fallback"),
                                                          ProfileSink::Field::Bool(
                                                              "requested",
                                                              compositor_clock_.boost_requested()),
                                                          ProfileSink::Field::Bool("active", false),
                                                          ProfileSink::Field::Number(
                                                              "last_wait_result",
                                                              std::to_string(
                                                                  compositor_clock_.last_wait_result())),
                                                      });
        }
        return 0;
    }

    if (message == WM_KEYDOWN && wparam == VK_F11 && IsInitialKeyDown(lparam)) {
        ToggleImmersivePlotMode();
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && ui_.immersive_plot_mode() && IsInitialKeyDown(lparam)) {
        ExitImmersivePlotMode();
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
        SchedulePresentationTargetRefresh(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        SchedulePresentationTargetRefresh(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == kPresentationRefreshTimer) {
            KillTimer(hwnd, kPresentationRefreshTimer);
            RefreshPresentationTargets("display_refresh_settled");
            return 0;
        }
        break;
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
        SchedulePresentationTargetRefresh(hwnd);
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
