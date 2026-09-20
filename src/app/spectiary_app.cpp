#include "app/spectiary_app.h"
#include "profile/presentation_trace_fields.h"

#include "app/initial_source.h"
#include "app/native_window_title.h"
#include "app/runtime_paths.h"
#include "platform/win32_message_wait.h"
#include "platform/win32_application_icon.h"
#include "platform/win32_text.h"
#include "platform/win32_system_theme.h"
#include "platform/win32_shell_identity.h"
#include "ui/profile_recording_ui_state.h"
#include "ui/ui_font.h"
#include "ui/ui_scale_settings.h"
#include "ui/ui_text.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace spectiary {


namespace {

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 820;
constexpr UINT kCompositorClockTickMessage = WM_APP + 0x54U;
constexpr UINT kProfileRecorderStateChangedMessage = WM_APP + 0x55U;
constexpr UINT kSourceLoadCompletionReadyMessage = WM_APP + 0x56U;
constexpr UINT kAutomationCommandReadyMessage = WM_APP + 0x57U;
constexpr auto kAutomationIdlePollInterval =
    std::chrono::milliseconds(25);
constexpr UINT_PTR kPresentationRefreshTimer = 0x5350U;
constexpr UINT kPresentationRefreshDelayMs = 500U;
constexpr float kDefaultWindowsDpi = 96.0f;
constexpr DWORD kDwmUseImmersiveDarkModeAttribute = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacyAttribute = 19;

struct AutomationSystemThemeFixtureResult {
    bool present = false;
    std::optional<ThemeId> theme;
};

AutomationSystemThemeFixtureResult
ReadAutomationSystemThemeFixture(
    const std::optional<AutomationStartupConfiguration>&
        automation)
{
    if (!automation) {
        return {};
    }

    const std::filesystem::path fixture_path =
        automation->state_root /
        kAutomationSystemThemeTestFixtureName;
    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(
            fixture_path,
            exists_error) ||
        exists_error) {
        return {};
    }

    std::ifstream fixture(
        fixture_path,
        std::ios::binary);
    if (!fixture) {
        return {.present = true};
    }
    std::string value{
        std::istreambuf_iterator<char>(fixture),
        std::istreambuf_iterator<char>()};
    while (!value.empty() &&
           (value.back() == '\r' ||
            value.back() == '\n' ||
            value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }
    const std::size_t first =
        value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {.present = true};
    }
    value.erase(0, first);

    if (value == kBuiltInDarkThemeStableId) {
        return {
            .present = true,
            .theme = BuiltInDarkThemeId(),
        };
    }
    if (value == kBuiltInLightThemeStableId) {
        return {
            .present = true,
            .theme = BuiltInLightThemeId(),
        };
    }
    return {.present = true};
}

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

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FormatProfileStatusMessage(
    UiLanguage language,
    const ProfileRecordingStatus& status)
{
    UiTextId text_id = UiTextId::UseDiagnosticsToRecord;
    switch (status.kind) {
    case ProfileRecordingStatusKind::UseDiagnosticsToRecord:
        text_id = UiTextId::UseDiagnosticsToRecord;
        break;
    case ProfileRecordingStatusKind::StartedByEnvironment:
        text_id = UiTextId::RecordingStartedByEnvironment;
        break;
    case ProfileRecordingStatusKind::StartFailed:
        text_id = UiTextId::CouldNotStartRecording;
        break;
    case ProfileRecordingStatusKind::Recording:
        text_id = UiTextId::RecordingPerformanceDiagnostics;
        break;
    case ProfileRecordingStatusKind::EventsDroppedUnderPressure:
        text_id = UiTextId::EventsDroppedPressure;
        break;
    case ProfileRecordingStatusKind::Finishing:
        text_id = UiTextId::FinishingRecording;
        break;
    case ProfileRecordingStatusKind::Failed:
        text_id = UiTextId::RecordingFailed;
        break;
    case ProfileRecordingStatusKind::FailedWhileWriting:
        text_id = UiTextId::RecordingFailedWhileWriting;
        break;
    case ProfileRecordingStatusKind::SavedAfterDurationLimit:
        text_id = UiTextId::RecordingSavedAfterDuration;
        break;
    case ProfileRecordingStatusKind::SavedAfterFileSizeLimit:
        text_id = UiTextId::RecordingSavedAfterSize;
        break;
    case ProfileRecordingStatusKind::Saved:
        text_id = UiTextId::RecordingSaved;
        break;
    case ProfileRecordingStatusKind::Stopped:
        text_id = UiTextId::RecordingStopped;
        break;
    }

    std::string message;
    if (text_id == UiTextId::EventsDroppedPressure) {
        message = std::to_string(status.dropped_events);
        message += UiText(language, text_id);
        return message;
    }

    message = UiText(language, text_id);
    if ((text_id == UiTextId::CouldNotStartRecording ||
         text_id == UiTextId::RecordingFailed) &&
        !status.detail.empty()) {
        message += status.detail;
    }
    if (status.dropped_events != 0 &&
        text_id != UiTextId::EventsDroppedPressure) {
        message += " ";
        message += std::to_string(status.dropped_events);
        message += UiText(
            language,
            UiTextId::EventsDropped);
    }
    return message;
}

std::filesystem::path FrameCaptureOutputPath(
    const std::filesystem::path& directory,
    std::uint64_t frame_index)
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    std::wostringstream name;
    name << L"Spectiary-frame-"
         << std::setfill(L'0')
         << std::setw(4) << now.wYear
         << std::setw(2) << now.wMonth
         << std::setw(2) << now.wDay
         << L'-'
         << std::setw(2) << now.wHour
         << std::setw(2) << now.wMinute
         << std::setw(2) << now.wSecond
         << L'-'
         << std::setw(3) << now.wMilliseconds
         << L"-p" << GetCurrentProcessId()
         << L"-f" << frame_index
         << L".png";
    return directory / name.str();
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

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        if (metadata[index].key == key) {
            return metadata[index].value;
        }
    }
    return {};
}

void ApplyTitleBarTheme(
    HWND hwnd,
    ThemeColorScheme color_scheme)
{
    const BOOL use_dark_title_bar =
        color_scheme == ThemeColorScheme::Dark
            ? TRUE
            : FALSE;
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

SpectiaryApp::SpectiaryApp(
    const SpectiaryStartup& startup,
    std::optional<AutomationStartupConfiguration>
        automation,
    StartupSourcePolicy startup_source_policy)
    : startup_(startup),
      imgui_layout_persistence_(
          startup.runtime_paths().imgui_ini_path),
      ui_(
          startup_,
          &touchpad_gestures_,
          automation
          ? (automation->allow_persistent_labeling_outputs
                 ? SampleLabelingStateCacheLoadPolicy::
                       AllowPersistentOutputs
                 : SampleLabelingStateCacheLoadPolicy::
                       InternalDraftsOnly)
          : SampleLabelingStateCacheLoadPolicy::
                AllowPersistentOutputs,
          startup_source_policy),
      automation_configuration_(
          std::move(automation))
{
}

SpectiaryApp::~SpectiaryApp()
{
    Shutdown();
}

int SpectiaryApp::Run(
    HINSTANCE instance,
    int show_command,
    std::optional<std::filesystem::path> initial_source)
{
    Initialize(instance, show_command, initial_source);

    MSG message = {};
    const auto next_maintenance_deadline = [this]() {
        auto deadline = ui_.NextMaintenanceDeadline();
        if (runtime_resource_workload_) {
            const auto workload_deadline =
                runtime_resource_workload_
                    ->next_deadline();
            if (workload_deadline &&
                (!deadline ||
                 *workload_deadline < *deadline)) {
                deadline = workload_deadline;
            }
        }
        const auto automation_deadline =
            NextAutomationDeadline();
        if (automation_deadline &&
            (!deadline ||
             *automation_deadline < *deadline)) {
            deadline = automation_deadline;
        }
        return deadline;
    };
    while (running_) {
        std::uint64_t message_count = 0;
        {
            presentation_trace::Span message_batch({.name = "message_pump_batch"});
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
                ++message_count;
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
                    render_wake_scheduler_.RequestTouchpadUpdate();
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            message_batch.Count(message_count);
        }

        if (!running_) {
            break;
        }

        for (const auto& request : external_open_router_.TakeRequests()) {
            ui_.OpenRoutedExternalSource(request.path, request.as_folder);
            render_wake_scheduler_.RequestFrame();
        }

        // The release-time panel chooses the ordinary in-app opener. Process
        // copied paths outside OLE callbacks and retain the shell's path order.
        if (shell_drop_target_) {
            const auto drop = shell_drop_target_->TakeDrop();
            if (!drop.paths.empty()) {
                ui_.OpenDroppedPaths(static_cast<ShellUi::FileDropDestination>(drop.destination), drop.paths);
                render_wake_scheduler_.RequestFrame();
            }
        }

        auto now = RenderWakeScheduler::Clock::now();
        auto maintenance_deadline =
            next_maintenance_deadline();
        if (maintenance_deadline && now >= *maintenance_deadline) {
            ui_.RunMaintenance(now);
            if (runtime_resource_workload_) {
                runtime_resource_workload_->Service(
                    ui_,
                    window_.hwnd(),
                    now);
            }
            ServiceAutomation();
            now = RenderWakeScheduler::Clock::now();
            render_wake_scheduler_.RequestFrame();
        }

        const bool window_renderable = !minimized_ && window_visible_;
        const bool capture_was_pending =
            frame_capture_.pending();
        frame_capture_.ObserveWindowRenderable(
            window_renderable);
        if (capture_was_pending &&
            !frame_capture_.pending()) {
            profile_.WriteEvent(
                "frame_capture",
                {
                    ProfileSink::Field::String(
                        "action",
                        "cancel"),
                    ProfileSink::Field::String(
                        "reason",
                        "window_not_renderable"),
                    ProfileSink::Field::Bool(
                        "image_produced",
                        false),
                });
        }
        bool touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
        UpdateCompositorClockBoost(window_renderable, touchpad_active);

        switch (render_wake_scheduler_.TakeAction(now, window_renderable)) {
        case RenderWakeAction::RenderFrame: {
            const RenderFrameOutcome outcome = RenderFrame();
            ServiceAutomation();
            const ImGuiIO& io = ImGui::GetIO();
            const bool popup_open = ImGui::IsPopupOpen(
                nullptr,
                ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
            UpdateCompositorClockBoost(window_renderable, touchpad_active);
            render_wake_scheduler_.CompleteFrame(
                RenderWakeScheduler::Clock::now(),
                {
                    .touchpad_active = touchpad_active,
                    .text_input_active = io.WantTextInput && io.ConfigInputTextCursorBlink,
                    .popup_open = popup_open,
                },
                outcome);
            break;
        }
        case RenderWakeAction::PumpTouchpadUpdates:
            touchpad_gestures_.PumpUpdates();
            touchpad_active = touchpad_gestures_.NeedsContinuousUpdates();
            UpdateCompositorClockBoost(window_renderable, touchpad_active);
            break;
        case RenderWakeAction::Wait:
            break;
        }

        ApplyLocalizedWindowTitle();
        if (jump_list_) {
            if (const auto sources = ui_.TakeShellSourceRoster()) {
                std::vector<JumpListSource> destinations;
                for (const auto& source : *sources) {
                    destinations.push_back({source.path, Utf8ToWide(source.display_name)});
                }
                jump_list_->Refresh(std::move(destinations),
                    Utf8ToWide(UiText(ui_.ui_language(), UiTextId::DataSources)));
            }
        }
        (void)WaitForWin32MessageOrDeadline(
            render_wake_scheduler_.NextWakeDeadline(
                window_renderable,
                next_maintenance_deadline()));
    }

    (void)SettleAutomationPanelCommandsForShutdown();
    SaveImGuiLayoutForShutdown();
    if (automation_server_) {
        CancelAutomationFrameCapture(
            "Application shutdown canceled the pending automation frame capture.");
        automation_server_->Shutdown(
            "app_shutdown");
        automation_execution_.SettleForShutdown();
        automation_idle_waits_.clear();
        automation_poll_deadline_.reset();
    }
    const ShellLocalStateFlushResult local_state_flush =
        ui_.FlushLocalState();
    if (!local_state_flush.all_saved() && !os_session_ending_) {
        const UiLanguage language = ui_.ui_language();
        const std::string failure_message =
            local_state_flush.FailureMessage(language);
        if (runtime_resource_workload_) {
            runtime_resource_workload_->
                RecordLocalStateFlushFailure(
                    failure_message);
        } else {
            const std::wstring wide_message =
                Utf8ToWide(failure_message);
            const std::wstring wide_title =
                Utf8ToWide(UiText(
                    language,
                    UiTextId::LocalStateWarningTitle));
            MessageBoxW(
                nullptr,
                wide_message.c_str(),
                wide_title.c_str(),
                MB_OK | MB_ICONWARNING);
        }
    }
    Shutdown();
    if (runtime_resource_workload_) {
        return runtime_resource_workload_->exit_code();
    }
    return static_cast<int>(message.wParam);
}

void SpectiaryApp::Initialize(
    HINSTANCE instance,
    int show_command,
    const std::optional<std::filesystem::path>& initial_source)
{
    const RuntimeResourceWorkloadConfigurationLoadResult
        workload_configuration =
            LoadRuntimeResourceWorkloadConfigurationFromEnvironment();
    if (!workload_configuration.error_message.empty()) {
        throw std::runtime_error(
            workload_configuration.error_message);
    }
    if (workload_configuration.configuration) {
        runtime_resource_workload_.emplace(
            *workload_configuration.configuration);
        profile_limits_.max_duration =
            std::chrono::steady_clock::duration::zero();
    }

    pan_pacing_ = ResolvePanPacingEnvironment();
    OnDemandFrameCaptureConfiguration
        frame_capture_configuration =
            ResolveOnDemandFrameCaptureEnvironment();
    if (automation_configuration_) {
        frame_capture_configuration.requested =
            "automation";
        frame_capture_configuration.enabled = true;
        frame_capture_configuration.recognized = true;
    }
    frame_capture_ = OnDemandFrameCapture(
        std::move(frame_capture_configuration));
    OpenInitialSource(ui_, initial_source);

    wchar_t windowed_capture[2] = {};
    if (!runtime_resource_workload_ &&
        GetEnvironmentVariableW(L"SPECTIARY_PROFILE_WINDOWED", windowed_capture, 2) == 1 &&
        windowed_capture[0] == L'1') {
        profile_limits_.max_duration = std::chrono::seconds(5);
    }
    profile_ = ProfileSink::CreateDefault(
        ui_.profile_output_directory(),
        profile_limits_);
    if (profile_.is_open()) {
        profile_status_.kind =
            ProfileRecordingStatusKind::StartedByEnvironment;
        LogProfileRecordingStarted("environment", "startup");
    } else if (!profile_.error_message().empty()) {
        profile_status_.kind =
            ProfileRecordingStatusKind::StartFailed;
        profile_status_.detail =
            profile_.error_message();
    }
    if (runtime_resource_workload_ &&
        !profile_.is_open()) {
        throw std::runtime_error(
            "Runtime resource workload requires SPECTIARY_PROFILE=1 and a writable profile output directory.");
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    const std::wstring window_title =
        FormatSpectiaryNativeWindowTitle(
            UiText(
                ui_.ui_language(),
                UiTextId::ApplicationWindowTitle));
    const bool window_created = window_.Create(
        instance,
        window_title.c_str(),
        kInitialWidth,
        kInitialHeight,
        [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            return HandleWindowMessage(hwnd, message, wparam, lparam);
        });
    if (!window_created) {
        throw std::runtime_error("Failed to create the Win32 window.");
    }
    applied_window_title_ = window_title;
    shell_drop_target_ = std::make_unique<Win32FileDropTarget>(
        [this](HWND window, float x, float y, bool all_files) {
            for (const auto id : ui_.FileDropViewports()) {
                const auto* viewport = ImGui::FindViewportByID(id);
                if (viewport && viewport->PlatformHandleRaw == window)
                    return static_cast<unsigned int>(ui_.HitTestFileDrop(id, x, y, all_files));
            }
            return 0U;
        },
        [this](unsigned int destination) {
            ui_.SetFileDropHovered(static_cast<ShellUi::FileDropDestination>(destination));
            RequestMessageRender();
        });
    if (!automation_configuration_ && !runtime_resource_workload_) {
        const auto app_id = ShellAppUserModelId(startup_.runtime_paths().config_root);
        // The drop target has initialized this thread's OLE apartment.
        (void)ConfigureShellWindowIdentity(window_.hwnd(), app_id, CurrentExecutablePath());
        jump_list_ = std::make_unique<Win32JumpList>(app_id, startup_.runtime_paths().config_root);
    }
    const HWND profile_state_window = window_.hwnd();
    profile_.SetStateChangeCallback([profile_state_window]() noexcept {
        (void)PostMessageW(profile_state_window, kProfileRecorderStateChangedMessage, 0, 0);
    });
    presentation_trace::context = this;
    presentation_trace::main_hwnd = reinterpret_cast<std::uintptr_t>(window_.hwnd());
    presentation_trace::enabled = [](void* context) {
        return presentation_trace::capture_ready &&
            static_cast<SpectiaryApp*>(context)->profile_.is_frame_recording_active();
    };
    presentation_trace::callback = [](void* context, const presentation_trace::Event& event) {
        auto* app = static_cast<SpectiaryApp*>(context);
        std::uint64_t viewport_id = 0;
        if (event.window.hwnd != 0 && ImGui::GetCurrentContext() != nullptr) {
            for (const ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports) {
                const auto handle = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
                if (reinterpret_cast<std::uintptr_t>(handle) == event.window.hwnd) {
                    viewport_id = viewport->ID;
                    break;
                }
            }
        }
        WritePresentationTrace(app->profile_, event,
            presentation_trace::Role(event.window), viewport_id);
    };
    ui_.RegisterSourceLoadCompletionReadyCallback([completion_window = window_.hwnd()]() noexcept {
        PostSourceLoadCompletionReady(completion_window);
    });
    (void)ResolveAndApplyTheme(true);

    const HRESULT renderer_result = renderer_.Initialize(
        window_.hwnd(),
        pan_pacing_.effective == PanPacingMode::Uncapped
            ? D3D11CompositionPolicy::Disabled
            : D3D11CompositionPolicy::Prefer,
        runtime_resource_workload_ &&
            runtime_resource_workload_->
                graphics_debug_requested());
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
            &SpectiaryApp::InvalidateRenderFromWin32Message,
            this,
            kCompositorClockTickMessage,
            &SpectiaryApp::ObserveWin32Message)) {
        throw std::runtime_error("Failed to observe Win32 messages for render invalidation.");
    }
    InitializeAutomation();
    window_.Show(
        show_command,
        automation_configuration_
            ? Win32WindowActivation::NoActivate
            : Win32WindowActivation::Default);
    LogDisplayEnvironment("startup");
    if (!automation_configuration_ && !runtime_resource_workload_) {
        (void)external_open_router_.Start(startup_.runtime_paths().config_root,
            [this]() {
                if (!running_ || os_session_ending_) return false;
                return ActivateExternalOpenWindow(window_.hwnd());
            },
            [this]() { PostMessageW(window_.hwnd(), kSourceLoadCompletionReadyMessage, 0, 0); });
    }
    LogPresentationUpdates();
    WritePanPacingState("startup", false);
    if (runtime_resource_workload_) {
        runtime_resource_workload_->Start(
            GetCurrentProcessId(),
            ui_);
        render_wake_scheduler_.RequestFrame();
    }
}

void SpectiaryApp::InitializeUiBackends()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    const RuntimePaths& runtime_paths =
        startup_.runtime_paths();
    std::error_code data_directory_error;
    std::filesystem::create_directories(runtime_paths.application_data_root, data_directory_error);
    if (data_directory_error) {
        throw std::runtime_error(
            "Failed to create Spectiary runtime data directory '" +
            PathToUtf8(runtime_paths.application_data_root) + "': " + data_directory_error.message());
    }
    io.IniFilename = nullptr;
    (void)imgui_layout_persistence_.Load();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    io.ConfigViewportsNoDecoration = true;
    io.ConfigViewportsNoAutoMerge = true;
    io.ConfigViewportsNoTaskBarIcon = true;
    io.ConfigViewportsNoDefaultParent = false;
    const UiFontSelection ui_fonts = AddUiFonts(io);
    ui_.SetSpectralLineLabelFont(ui_fonts.spectral_label_font);
    const std::optional<std::filesystem::path>& ui_font_path =
        ui_fonts.cjk_font ? ui_fonts.cjk_font : ui_fonts.scientific_font;

    ApplyImGuiThemeColors(
        ResolvedThemeDescriptor(),
        ImGui::GetStyle());
    ApplyImPlotThemeColors(
        ResolvedThemeDescriptor(),
        ImPlot::GetStyle());
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
    }
    base_imgui_style_ = ImGui::GetStyle();
    ApplyUiScale(
        ImGui_ImplWin32_GetDpiScaleForHwnd(window_.hwnd()),
        ui_.ui_scale_percentage());
    profile_.WriteEvent("dpi_config", {
                                           ProfileSink::Field::Number("dpi_scale", std::to_string(system_dpi_scale_)),
                                           ProfileSink::Field::Number("system_dpi_scale", std::to_string(system_dpi_scale_)),
                                           ProfileSink::Field::Number("user_scale", std::to_string(user_ui_scale_)),
                                           ProfileSink::Field::Number("effective_scale", std::to_string(effective_ui_scale_)),
                                           ProfileSink::Field::Number(
                                               "font_scale_dpi",
                                               std::to_string(ImGui::GetStyle().FontScaleDpi)),
                                           ProfileSink::Field::Number(
                                               "font_scale_main",
                                               std::to_string(ImGui::GetStyle().FontScaleMain)),
                                           ProfileSink::Field::String(
                                               "ui_font",
                                               ui_font_path ? PathToUtf8(*ui_font_path) : "imgui_default"),
                                       });

    if (!ImGui_ImplWin32_Init(window_.hwnd())) {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui Win32 backend.");
    }
    if (!InstallImGuiPlatformWindowIconHook()) {
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error(
            "Failed to install the Dear ImGui platform-window icon hook.");
    }

    if (!ImGui_ImplDX11_Init(renderer_.device(), renderer_.context())) {
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui DirectX 11 backend.");
    }

    viewport_renderer_.SetClearColor(
        ResolvedThemeDescriptor().clear_color);
    viewport_renderer_.SetNativeWindowThemeCallback(
        [this](HWND hwnd) {
            ApplyTitleBarTheme(
                hwnd,
                ResolvedThemeDescriptor().color_scheme);
        });
    if (!viewport_renderer_.Initialize(renderer_.factory(), renderer_.device(), renderer_.context())) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw std::runtime_error("Failed to initialize the Dear ImGui SDR viewport renderer.");
    }

#ifdef SPECTIARY_REDIRECTION_AB_BUILD
    wchar_t feedback_experiment[2] = {};
    viewport_renderer_.SetFeedbackAcquireOnlyExperiment(
        GetEnvironmentVariableW(L"SPECTIARY_EXPERIMENT_FEEDBACK_ACQUIRE_ONLY", feedback_experiment, 2) == 1 &&
        feedback_experiment[0] == L'1');
    wchar_t buffer_experiment[2] = {};
    viewport_renderer_.SetIncrementalBuffersForDiagnostics(
        GetEnvironmentVariableW(L"SPECTIARY_EXPERIMENT_INCREMENTAL_BUFFERS", buffer_experiment, 2) == 1 &&
        buffer_experiment[0] == L'1');
#endif
    profile_.WriteEvent("feedback_experiment", {
        ProfileSink::Field::Bool("acquire_only", viewport_renderer_.FeedbackAcquireOnlyExperiment()),
    });
    // Retain the legacy event name for existing capture analyzers; this now also
    // reports the production detached policy, independently of backend fallback.
    profile_.WriteEvent("buffer_replacement_experiment", {
        ProfileSink::Field::Bool("enabled", viewport_renderer_.IncrementalBuffersEnabled()),
    });
    imgui_initialized_ = true;
}

void SpectiaryApp::SaveImGuiLayoutForShutdown()
{
    if (!imgui_initialized_ || imgui_layout_saved_for_shutdown_) {
        return;
    }

    std::string imgui_layout_error;
    if (imgui_layout_persistence_.SaveNow(
            &imgui_layout_error)) {
        imgui_layout_saved_for_shutdown_ = true;
        return;
    }
    if (!imgui_layout_error.empty()) {
        profile_.WriteEvent(
            "imgui_layout_save",
            {
                ProfileSink::Field::String("phase", "shutdown"),
                ProfileSink::Field::String(
                    "error",
                    imgui_layout_error),
            });
    }
}

void SpectiaryApp::Shutdown()
{
    shell_drop_target_.reset();
    external_open_router_.Stop();
    if (shutdown_complete_) {
        return;
    }
    SaveImGuiLayoutForShutdown();
    const bool restored_panel_state_requires_flush =
        SettleAutomationPanelCommandsForShutdown();
    if (restored_panel_state_requires_flush) {
        (void)ui_.FlushLocalState();
    }
    if (automation_server_) {
        CancelAutomationFrameCapture(
            "Application shutdown canceled the pending automation frame capture.");
        automation_server_->Shutdown(
            "app_shutdown");
        automation_execution_.SettleForShutdown();
        automation_idle_waits_.clear();
        automation_poll_deadline_.reset();
    }
    ui_.UnregisterSourceLoadCompletionReadyCallback();
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
    if (runtime_resource_workload_) {
        runtime_resource_workload_->
            RecordGraphicsDiagnostics(
                renderer_.live_object_report(),
                ui_);
    }
    pending_resize_.reset();
    fullscreen_restore_.reset();
    immersive_plot_entered_fullscreen_ = false;
    profile_.SetStateChangeCallback({});
    presentation_trace::CancelInvalidation();
    profile_.Stop();
    presentation_trace::callback = nullptr;
    presentation_trace::enabled = nullptr;
    presentation_trace::context = nullptr;
    presentation_trace::main_hwnd = 0;
    window_.ClearMessageHandler();
    window_.Destroy();
    shutdown_complete_ = true;
}

RenderFrameOutcome SpectiaryApp::RenderFrame()
{
    if (!presentation_trace::capture_ready && profile_.is_frame_recording_active()) {
        presentation_trace::capture_ready = true;
        presentation_trace::CaptureBoundary(true);
        profile_.WriteEvent("buffer_replacement_experiment", {
            ProfileSink::Field::Bool("enabled", viewport_renderer_.IncrementalBuffersEnabled()),
        });
        profile_.WriteEvent("feedback_experiment", {
            ProfileSink::Field::Bool("acquire_only", viewport_renderer_.FeedbackAcquireOnlyExperiment()),
        });
    }
    presentation_trace::RenderFrame(frame_index_ + 1);
    profile_.BeginFrame();
    struct ProfileFrameFinalizationGuard {
        ProfileSink& sink;
        ~ProfileFrameFinalizationGuard() {
            if (sink.is_frame_finalization_pending() && presentation_trace::capture_ready) {
                presentation_trace::CancelInvalidation();
                presentation_trace::CaptureBoundary(false);
                presentation_trace::capture_ready = false;
            }
            presentation_trace::frame = 0;
            sink.CompleteFrameFinalization();
        }
    } profile_frame_finalization{profile_};
    presentation_trace::Span render_frame_span({.name = "render_frame"});

    ApplyPendingResize();
    ApplyPendingApplicationSettings(
        "user_scale_changed",
        false);

    ++frame_index_;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        ProfileTimer timer(profile_, "view_update", frame_index_);
        RefreshProfileRecordingStatus();
        const ProfileSink::LifecycleSnapshot
            profile_state =
                profile_.lifecycle_snapshot();
        ShellStatus status;
        status.profile_open = profile_state.open;
        status.profile_stopping = profile_state.stopping;
        status.latency_trace_recording_active = profile_state.frame_recording_active;
        status.profile = &profile_;
        status.profile_path = profile_.path().empty() ? nullptr : &profile_.path();
        const std::string profile_status_message =
            FormatProfileStatusMessage(
                ui_.ui_language(),
                profile_status_);
        status.profile_status_message =
            profile_status_message;
        status.frame_capture_enabled =
            frame_capture_.enabled();
        status.frame_capture_pending =
            frame_capture_.pending();
        status.window_renderable =
            !minimized_ && window_visible_;
        status.frame_capture_output_directory =
            frame_capture_.enabled()
                ? &startup_.runtime_paths()
                       .frame_capture_directory
                : nullptr;
        status.last_frame_capture_path =
            frame_capture_.last_output_path()
                ? &*frame_capture_.last_output_path()
                : nullptr;
        status.frame_capture_status_message =
            frame_capture_.status_message();
        status.frame_capture_status =
            frame_capture_.status();
        status.frame_capture_status_operation =
            frame_capture_.status_operation();
        status.frame_capture_status_result =
            frame_capture_.status_result();
        status.client_width = window_.client_width();
        status.client_height = window_.client_height();
        const ImGuiIO& io = ImGui::GetIO();
        const std::optional<TopBarFrameTimingSample> current_frame_timing_sample =
            TryMakeTopBarFrameTimingSample(io.DeltaTime, frame_index_ == 1);
        if (current_frame_timing_sample) {
            application_frame_timing_sample_ = current_frame_timing_sample;
        }
        status.application_frame_timing_sample =
            application_frame_timing_sample_;
        status.frame_index = frame_index_;
        ui_.Render(status);
        (void)ui_.TakeAppliedUiLanguage();
        if (ui_.TakeImmersivePlotModeToggleRequest()) {
            ToggleImmersivePlotMode();
        }
        if (ui_.TakeProfileRecordingToggleRequest()) {
            ToggleProfileRecording();
        }
        if (ui_.TakeFrameCaptureRequest()) {
            RequestFrameCapture();
        }
    }

    {
        ProfileTimer timer(profile_, "draw_submission", frame_index_);
        ImGui::Render();
    }

    std::string imgui_layout_error;
    if (!imgui_layout_persistence_.SaveIfRequested(
            &imgui_layout_error) &&
        !imgui_layout_error.empty()) {
        profile_.WriteEvent(
            "imgui_layout_save",
            {
                ProfileSink::Field::String("phase", "frame"),
                ProfileSink::Field::String(
                    "error",
                    imgui_layout_error),
            });
    }

    {
        ProfileTimer timer(profile_, "render_pass", frame_index_);
        const HRESULT begin_result = renderer_.BeginFrame(
            ResolvedThemeDescriptor().clear_color);
        if (begin_result == DXGI_ERROR_WAS_STILL_DRAWING) {
            LogPresentationUpdates();
            return RenderFrameOutcome::AcquireRetry;
        }
        if (FAILED(begin_result)) {
            throw std::runtime_error(
                HResultMessage(renderer_.last_error_operation(), begin_result));
        }
        {
            presentation_trace::Span draw({.name = "viewport_draw_submission",
                .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(window_.hwnd()))});
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        if (frame_capture_.ShouldCapture(
                frame_index_)) {
            CaptureRequestedFrame();
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            viewport_renderer_.SetCompositorClockPaced(compositor_clock_.boost_active());
            {
                presentation_trace::Span update({.name = "platform_windows_update"});
                ImGui::UpdatePlatformWindows();
                std::array<HWND, 2> drop_windows{};
                const auto viewport_ids = ui_.FileDropViewports();
                for (std::size_t index = 0; index < viewport_ids.size(); ++index) {
                    const auto* viewport = ImGui::FindViewportByID(viewport_ids[index]);
                    if (viewport) drop_windows[index] = static_cast<HWND>(viewport->PlatformHandleRaw);
                }
                shell_drop_target_->SetWindows(drop_windows);
            }
            {
                presentation_trace::Span render({.name = "platform_windows_render"});
                ImGui::RenderPlatformWindowsDefault();
            }
            const D3D11RendererError viewport_error = viewport_renderer_.TakeLastError();
            if (FAILED(viewport_error.result)) {
                throw std::runtime_error(HResultMessage(viewport_error.operation, viewport_error.result));
            }
        }
    }

    std::vector<NavigationLatencyPresentation> latency_presentations;
    for (const D3D11ViewportPresentCompletion& completion :
         viewport_renderer_.TakePresentCompletions()) {
        latency_presentations.push_back({completion.viewport_id, completion.completed_at});
    }

    const D3D11PresentMode present_mode = MainPresentMode();
    HRESULT present_result = S_OK;
    NavigationLatencyTimePoint present_completed_at;
    const bool profile_frame_active_at_present =
        profile_.lifecycle_snapshot()
            .frame_recording_active;
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
    if (present_result == S_OK) {
        latency_presentations.push_back({ImGui::GetMainViewport()->ID, present_completed_at});
    }
    std::vector<ShellAutomationViewportPresentationState>
        viewport_states;
    const ImGuiPlatformIO& platform_io =
        ImGui::GetPlatformIO();
    viewport_states.reserve(
        static_cast<std::size_t>(
            platform_io.Viewports.Size));
    for (ImGuiViewport* viewport :
         platform_io.Viewports) {
        if (viewport != nullptr) {
            viewport_states.push_back({
                .viewport_id = viewport->ID,
                .renderable =
                    (viewport->Flags &
                     ImGuiViewportFlags_IsMinimized) ==
                    0,
            });
        }
    }
    ui_.PresentFrame(
        frame_index_,
        latency_presentations,
        viewport_states);
    LogPresentationUpdates();
    ApplyPendingApplicationSettings(
        "user_scale_changed");
    return present_result == S_FALSE
               ? RenderFrameOutcome::PresentRetry
               : RenderFrameOutcome::Presented;
}

void SpectiaryApp::UpdateCompositorClockBoost(bool window_renderable, bool touchpad_active)
{
    const bool plot_interaction_active = ui_.latency_sensitive_plot_interaction_active();
    const bool uncapped_pan_active = UncappedPanActive(window_renderable);
    const ImGuiIO& io = ImGui::GetIO();
    const bool imgui_drag_active = ShouldBoostForImGuiDrag(
        io.WantCaptureMouse,
        ImGui::IsMouseDragging(ImGuiMouseButton_Left),
        uncapped_pan_active);
    render_wake_scheduler_.SetContinuousRendering(uncapped_pan_active);
    if (uncapped_pan_active != uncapped_pan_active_) {
        uncapped_pan_active_ = uncapped_pan_active;
        WritePanPacingState("pan_state_changed", uncapped_pan_active_);
    }

    const bool requested =
        window_renderable &&
        ((plot_interaction_active &&
          pan_pacing_.effective == PanPacingMode::Display) ||
         (touchpad_active && !uncapped_pan_active) ||
         imgui_drag_active);
    if (requested == compositor_clock_.boost_requested()) {
        render_wake_scheduler_.SetCompositorClockPaced(
            compositor_clock_.boost_active());
        return;
    }

    if (!requested) {
        render_wake_scheduler_.CancelTouchpadUpdate();
    }
    (void)compositor_clock_.SetBoostRequested(requested);
    render_wake_scheduler_.SetCompositorClockPaced(
        compositor_clock_.boost_active());
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
                                                  ProfileSink::Field::Bool(
                                                      "imgui_drag_active",
                                                      imgui_drag_active),
                                                  ProfileSink::Field::String(
                                                      "presentation_backend",
                                                      D3D11PresentationBackendName(
                                                          renderer_.presentation_backend())),
                                                  ProfileSink::Field::String(
                                                      "presentation_degradation",
                                                      D3D11PresentationDegradationName(
                                                          renderer_.presentation_degradation(
                                                              MainPresentMode()))),
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

bool SpectiaryApp::UncappedPanActive(bool window_renderable) const
{
    return pan_pacing_.effective == PanPacingMode::Uncapped &&
           window_renderable &&
           ui_.latency_sensitive_plot_interaction_active();
}

D3D11PresentMode SpectiaryApp::MainPresentMode() const
{
    if (UncappedPanActive(!minimized_ && window_visible_)) {
        return D3D11PresentMode::Immediate;
    }
    return compositor_clock_.boost_active()
               ? D3D11PresentMode::CompositorClock
               : D3D11PresentMode::DisplayVSync;
}

void SpectiaryApp::WritePanPacingState(
    std::string_view reason,
    bool active)
{
    if (!profile_.is_open()) {
        return;
    }
    const D3D11PresentMode present_mode =
        active ? D3D11PresentMode::Immediate : MainPresentMode();
    profile_.WriteEvent("pan_pacing", {
                                                ProfileSink::Field::String(
                                                    "reason",
                                                    std::string(reason)),
                                                ProfileSink::Field::String(
                                                    "requested",
                                                    pan_pacing_.requested),
                                                ProfileSink::Field::String(
                                                    "effective",
                                                    PanPacingModeName(
                                                        pan_pacing_.effective)),
                                                ProfileSink::Field::Bool(
                                                    "recognized",
                                                    pan_pacing_.recognized),
                                                ProfileSink::Field::Bool(
                                                    "pan_active",
                                                    active),
                                                ProfileSink::Field::Bool(
                                                    "continuous_rendering",
                                                    active),
                                                ProfileSink::Field::String(
                                                    "backend",
                                                    D3D11PresentationBackendName(
                                                        renderer_.presentation_backend())),
                                                ProfileSink::Field::String(
                                                    "present_mode",
                                                    D3D11PresentModeName(
                                                        present_mode)),
                                                ProfileSink::Field::Number(
                                                    "sync_interval",
                                                    std::to_string(
                                                        D3D11PresentSyncInterval(
                                                            present_mode,
                                                            renderer_
                                                                .tearing_supported()))),
                                                ProfileSink::Field::Number(
                                                    "present_flags",
                                                    std::to_string(
                                                        D3D11PresentFlags(
                                                            present_mode,
                                                            renderer_
                                                                .tearing_supported()))),
                                                ProfileSink::Field::Bool(
                                                    "tearing_supported",
                                                    renderer_.tearing_supported()),
                                                ProfileSink::Field::String(
                                                    "display_feedback",
                                                    renderer_.presentation_backend() ==
                                                            D3D11PresentationBackend::Composition
                                                        ? "composition"
                                                        : "unavailable"),
                                            });
}

void SpectiaryApp::InvalidateRenderFromWin32Message(void* context) noexcept
{
    static_cast<SpectiaryApp*>(context)->RequestMessageRender();
}

void SpectiaryApp::ObserveWin32Message(
    void* context,
    const Win32ObservedMessage& message) noexcept
{
    auto* app = static_cast<SpectiaryApp*>(context);
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

void SpectiaryApp::PostSourceLoadCompletionReady(HWND hwnd) noexcept
{
    if (hwnd != nullptr) {
        (void)PostMessageW(hwnd, kSourceLoadCompletionReadyMessage, 0, 0);
    }
}

void SpectiaryApp::RequestMessageRender() noexcept
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

void SpectiaryApp::ApplyPendingResize()
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
    if (runtime_resource_workload_) {
        runtime_resource_workload_->
            RecordRenderTargetResize();
    }

    profile_.WriteEvent("render_target_resize", {
                                                    ProfileSink::Field::Number("width", std::to_string(resize.width)),
                                                    ProfileSink::Field::Number("height", std::to_string(resize.height)),
                                                });
    LogDisplayEnvironment("resize");
}

void SpectiaryApp::ApplyUiScale(
    float system_dpi_scale,
    int user_scale_percentage)
{
    const UiScaleFactors scales =
        CalculateUiScaleFactors(
            NormalizeDpiScale(system_dpi_scale),
            user_scale_percentage);
    system_dpi_scale_ = scales.system;
    user_ui_scale_ = scales.user;
    effective_ui_scale_ = scales.effective;
    user_ui_scale_percentage_ =
        IsValidUiScalePercentage(user_scale_percentage)
        ? user_scale_percentage
        : kDefaultUiScalePercentage;

    ApplyUiScaleToImGuiStyle(
        ImGui::GetStyle(),
        base_imgui_style_,
        scales);
}

void SpectiaryApp::ApplyLocalizedWindowTitle()
{
    if (window_.hwnd() == nullptr) {
        return;
    }
    const ShellWindowTitleView view =
        ui_.WindowTitleView();
    const bool loading =
        view.loading_source_path != nullptr &&
        !view.loading_source_path->empty();
    const NativeWindowTitleView title_view{
        .product_name = UiText(
            ui_.ui_language(),
            UiTextId::ApplicationWindowTitle),
        .source_path =
            loading ? view.loading_source_path
                    : view.source_path,
        .loading = loading,
        .loading_text = UiText(
            ui_.ui_language(),
            UiTextId::LoadingSource),
        .sample_present = view.sample_present,
        .sample_name = view.sample_name,
        .sample_index = view.sample_index,
        .sample_count = view.sample_count,
    };
    if (applied_window_title_key_ &&
        applied_window_title_key_->Matches(title_view)) {
        return;
    }
    const std::wstring title =
        FormatSpectiaryNativeWindowTitle(title_view);
    if (title == applied_window_title_) {
        applied_window_title_key_.emplace(title_view);
        return;
    }
    if (SetWindowTextW(
            window_.hwnd(),
            title.c_str()) != FALSE) {
        applied_window_title_ = title;
        applied_window_title_key_.emplace(title_view);
    }
}

void SpectiaryApp::WriteDpiConfiguration(
    std::string_view reason)
{
    if (!profile_.is_open() ||
        ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    profile_.WriteEvent("dpi_config", {
                                           ProfileSink::Field::String(
                                               "reason",
                                               std::string(reason)),
                                           ProfileSink::Field::Number(
                                               "dpi_scale",
                                               std::to_string(system_dpi_scale_)),
                                           ProfileSink::Field::Number(
                                               "system_dpi_scale",
                                               std::to_string(system_dpi_scale_)),
                                           ProfileSink::Field::Number(
                                               "user_scale",
                                               std::to_string(user_ui_scale_)),
                                           ProfileSink::Field::Number(
                                               "effective_scale",
                                               std::to_string(effective_ui_scale_)),
                                           ProfileSink::Field::Number(
                                               "font_scale_dpi",
                                               std::to_string(style.FontScaleDpi)),
                                           ProfileSink::Field::Number(
                                               "font_scale_main",
                                               std::to_string(style.FontScaleMain)),
                                       });
}

void SpectiaryApp::ToggleFullscreen()
{
    if (fullscreen_restore_) {
        ExitFullscreen();
    } else {
        EnterFullscreen();
    }
}

void SpectiaryApp::EnterFullscreen()
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

void SpectiaryApp::ExitFullscreen()
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
    ApplyTitleBarTheme(
        hwnd,
        ResolvedThemeDescriptor().color_scheme);
    profile_.WriteEvent("fullscreen", {ProfileSink::Field::Bool("enabled", false)});
    LogDisplayEnvironment("fullscreen_exit");
}

void SpectiaryApp::ToggleImmersivePlotMode()
{
    if (ui_.immersive_plot_mode()) {
        ExitImmersivePlotMode();
    } else {
        EnterImmersivePlotMode();
    }
}

void SpectiaryApp::EnterImmersivePlotMode()
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

void SpectiaryApp::ExitImmersivePlotMode()
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

void SpectiaryApp::ToggleProfileRecording()
{
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    switch (ResolveProfileRecordingToggleAction(
        lifecycle.open,
        lifecycle.stopping)) {
    case ProfileRecordingToggleAction::Stop:
        StopProfileRecording("user_toggle");
        break;
    case ProfileRecordingToggleAction::Start:
        (void)StartProfileRecording("user_toggle");
        break;
    case ProfileRecordingToggleAction::None:
        return;
    }
    render_wake_scheduler_.RequestFrame();
}

void SpectiaryApp::RequestFrameCapture()
{
    const OnDemandFrameCaptureRequestOutcome outcome =
        frame_capture_.Request(
            frame_index_,
            !minimized_ && window_visible_);
    profile_.WriteEvent(
        "frame_capture",
        {
            ProfileSink::Field::String(
                "action",
                outcome ==
                        OnDemandFrameCaptureRequestOutcome::
                            Accepted
                    ? "request"
                    : "request_rejected"),
            ProfileSink::Field::String(
                "outcome",
                outcome ==
                        OnDemandFrameCaptureRequestOutcome::
                            Accepted
                    ? "accepted"
                : outcome ==
                        OnDemandFrameCaptureRequestOutcome::
                            WindowNotRenderable
                    ? "window_not_renderable"
                : outcome ==
                        OnDemandFrameCaptureRequestOutcome::
                            AlreadyPending
                    ? "already_pending"
                    : "disabled"),
            ProfileSink::Field::Number(
                "requested_after_frame",
                std::to_string(frame_index_)),
            ProfileSink::Field::Bool(
                "profile_interference",
                true),
        });
    if (outcome ==
        OnDemandFrameCaptureRequestOutcome::Accepted) {
        render_wake_scheduler_.RequestFrame();
    }
}

void SpectiaryApp::CaptureRequestedFrame()
{
    const std::optional<std::filesystem::path>
        requested_output =
            frame_capture_.requested_output_path();
    if (requested_output &&
        automation_capture_command_ &&
        frame_index_ <=
            automation_capture_command_
                    ->accepted_frame +
                1U) {
        return;
    }
    std::filesystem::path output_path;
    std::filesystem::path frame_capture_directory;
    if (requested_output) {
        if (!AutomationFrameCaptureRequestActive()) {
            CancelAutomationFrameCapture(
                "The automation request became inactive before capture.");
            return;
        }
        if (!automation_configuration_) {
            frame_capture_.Fail(
                "Explicit frame-capture paths require an automation instance.");
            return;
        }
        const AutomationCapturePathValidation validation =
            ValidateAutomationCapturePath(
                automation_configuration_->state_root,
                *requested_output);
        if (!validation.valid) {
            frame_capture_.Fail(
                validation.error_message);
            return;
        }
        output_path = validation.normalized_path;
        frame_capture_directory =
            output_path.parent_path();
    } else {
        frame_capture_directory =
            startup_.runtime_paths()
                .frame_capture_directory;
        output_path =
            FrameCaptureOutputPath(
                frame_capture_directory,
                frame_index_);
    }
    if (!requested_output) {
        std::error_code directory_error;
        std::filesystem::create_directories(
            frame_capture_directory,
            directory_error);
        if (directory_error) {
            frame_capture_.
                FailPreparingOutputDirectory();
            profile_.WriteEvent(
                "frame_capture",
                {
                    ProfileSink::Field::String(
                        "action",
                        "failed"),
                    ProfileSink::Field::String(
                        "stage",
                        "create_output_directory"),
                    ProfileSink::Field::String(
                        "detail",
                        directory_error.message()),
                    ProfileSink::Field::Bool(
                        "image_produced",
                        false),
                });
            render_wake_scheduler_.RequestFrame();
            return;
        }
    }

    if (requested_output) {
        if (!AutomationFrameCaptureRequestActive()) {
            CancelAutomationFrameCapture(
                "The automation request became inactive before PNG encoding.");
            return;
        }
        const AutomationCapturePathValidation validation =
            ValidateAutomationCapturePath(
                automation_configuration_->state_root,
                output_path);
        if (!validation.valid) {
            frame_capture_.Fail(
                validation.error_message);
            render_wake_scheduler_.RequestFrame();
            return;
        }
        output_path = validation.normalized_path;
    }
    if (requested_output &&
        !AutomationFrameCaptureRequestActive()) {
        CancelAutomationFrameCapture(
            "The automation request became inactive before PNG encoding.");
        return;
    }
    AutomationNamedPipeServer::
        FrameCaptureFinalizationResult
            automation_finalization;
    bool automation_finalization_attempted = false;
    AutomationQueuedCommand capture_command;
    AutomationCommandResult capture_result;
    if (requested_output &&
        automation_capture_command_) {
        capture_command =
            automation_capture_command_->command;
        capture_result = AutomationFrameCaptureResult{
            PathToUtf8(output_path), frame_index_, window_.client_width(), window_.client_height()};
    }
    const HRESULT result =
        renderer_.CaptureFrameToPng(
            output_path,
            requested_output
            ? D3D11FrameCaptureFinalizer{
                  [this,
                   capture_command,
                   capture_result,
                   &automation_finalization,
                   &automation_finalization_attempted](
                      const std::function<HRESULT()>&
                          publish) {
                      automation_finalization_attempted =
                          true;
                      automation_finalization =
                          automation_server_
                              ->TryFinalizeFrameCapture(
                                  capture_command,
                                  capture_result,
                              publish);
                      return automation_finalization
                          .result;
                  }}
            : D3D11FrameCaptureFinalizer{},
            requested_output
            ? std::optional<std::filesystem::path>(
                  automation_configuration_
                      ->state_root)
            : std::nullopt);
    if (FAILED(result)) {
        if (requested_output &&
            automation_finalization_attempted) {
            automation_capture_last_path_ =
                output_path;
            if (automation_finalization.state ==
                AutomationNamedPipeServer::
                    FrameCaptureFinalizationState::
                        Inactive) {
                CancelAutomationFrameCapture(
                    "The automation request became inactive before PNG publication.");
            } else {
                frame_capture_.FailCapture(
                    "Finalize frame capture output",
                    HResultHex(result));
                automation_capture_last_result_ =
                    "failed";
            }
            automation_capture_command_.reset();
            render_wake_scheduler_.RequestFrame();
            return;
        }
        if (requested_output &&
            !AutomationFrameCaptureRequestActive()) {
            CancelAutomationFrameCapture(
                "The automation request became inactive during PNG encoding.");
            automation_capture_command_.reset();
            return;
        }
        const std::string operation(
            renderer_.last_error_operation());
        frame_capture_.FailCapture(
            operation.empty()
                ? std::string(
                    "Direct3D/WIC capture")
                : operation,
            HResultHex(result));
        profile_.WriteEvent(
            "frame_capture",
            {
                ProfileSink::Field::String(
                    "action",
                    "failed"),
                ProfileSink::Field::String(
                    "stage",
                    operation),
                ProfileSink::Field::String(
                    "result",
                    HResultHex(result)),
                ProfileSink::Field::Bool(
                    "image_produced",
                    false),
                ProfileSink::Field::Number(
                    "frame",
                    std::to_string(frame_index_)),
            });
        render_wake_scheduler_.RequestFrame();
        return;
    }

    frame_capture_.Complete(output_path);
    if (requested_output &&
        automation_finalization_attempted) {
        automation_capture_last_result_ =
            "succeeded";
        automation_capture_last_path_ =
            output_path;
        automation_capture_command_.reset();
    }
    profile_.WriteEvent(
        "frame_capture",
        {
            ProfileSink::Field::String(
                "action",
                "captured"),
            ProfileSink::Field::String(
                "scope",
                "main_viewport"),
            ProfileSink::Field::String(
                "path",
                PathToUtf8(output_path)),
            ProfileSink::Field::Bool(
                "image_produced",
                true),
            ProfileSink::Field::Number(
                "frame",
                std::to_string(frame_index_)),
            ProfileSink::Field::Number(
                "width",
                std::to_string(
                    window_.client_width())),
            ProfileSink::Field::Number(
                "height",
                std::to_string(
                    window_.client_height())),
        });
    render_wake_scheduler_.RequestFrame();
}

bool SpectiaryApp::StartProfileRecording(
    std::string_view trigger,
    std::optional<AutomationPreparedProfileOutput>
        prepared_output)
{
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (lifecycle.open || lifecycle.stopping) {
        return false;
    }
    profile_.ResetStoppedOutcome();
    displayed_profile_stop_reason_ =
        ProfileSink::StopReason::None;
    bool started = false;
    if (prepared_output) {
        if (prepared_output->valid()) {
            started = profile_.StartPrepared(
                std::move(prepared_output->path),
                profile_limits_,
                std::move(prepared_output->stream),
                std::move(
                    prepared_output->discard_output));
        } else {
            profile_status_ = {
                .kind =
                    ProfileRecordingStatusKind::
                        StartFailed,
                .detail =
                    prepared_output->error_message,
            };
            return false;
        }
    } else {
        started = profile_.StartDefault(
            ui_.profile_output_directory(),
            profile_limits_);
    }
    if (!started) {
        profile_status_ = {
            .kind = ProfileRecordingStatusKind::StartFailed,
            .detail = profile_.error_message(),
        };
        return false;
    }

    displayed_profile_stop_reason_ = ProfileSink::StopReason::None;
    profile_status_ = {
        .kind = ProfileRecordingStatusKind::Recording,
    };
    presentation_trace::capture_ready = false;
    LogProfileRecordingStarted(trigger, "recording_started");
    WriteDpiConfiguration("recording_started");
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
    WritePanPacingState(
        "recording_started",
        UncappedPanActive(!minimized_ && window_visible_));
    return true;
}

void SpectiaryApp::StopProfileRecording(std::string_view trigger)
{
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (!lifecycle.open) {
        return;
    }
    profile_.WriteEvent("profile_recording", {
                                                   ProfileSink::Field::String("action", "stop"),
                                                   ProfileSink::Field::String("trigger", std::string(trigger)),
                                                   ProfileSink::Field::Number(
                                                       "dropped_events",
                                                       std::to_string(lifecycle.dropped_events)),
    });
    displayed_profile_stop_reason_ = ProfileSink::StopReason::None;
    profile_status_ = {
        .kind = ProfileRecordingStatusKind::Finishing,
    };
    profile_.RequestStopAfterFrame();
}

AutomationSettingResult AutomationStringSettingResult(
    std::string_view name, std::string_view value,
    std::optional<bool> changed = std::nullopt)
{
    return {std::string(name), std::string(value), changed};
}

AutomationSettingResult AutomationIntegerSettingResult(
    std::string_view name, int value,
    std::optional<bool> changed = std::nullopt)
{
    return {std::string(name), value, changed};
}

void SpectiaryApp::PostAutomationCommandReady(
    HWND hwnd) noexcept
{
    if (hwnd != nullptr) {
        (void)PostMessageW(
            hwnd,
            kAutomationCommandReadyMessage,
            0,
            0);
    }
}

void SpectiaryApp::InitializeAutomation()
{
    if (!automation_configuration_) {
        return;
    }
    automation_server_ =
        std::make_unique<AutomationNamedPipeServer>(
            automation_configuration_->pipe_name,
            automation_configuration_->nonce,
            automation_configuration_->instance_id);
    std::string error_message;
    const HWND automation_window = window_.hwnd();
    if (!automation_server_->Start(
            [automation_window]() noexcept {
                PostAutomationCommandReady(
                    automation_window);
            },
            error_message)) {
        automation_server_.reset();
        throw std::runtime_error(
            "Could not start the local automation control pipe: " +
            error_message);
    }
}

void SpectiaryApp::ApplyPendingApplicationSettings(
    std::string_view scale_reason,
    bool request_frame)
{
    bool applied = false;
    if (ui_.TakeAppliedThemeSelection()) {
        (void)ResolveAndApplyTheme(true);
        applied = true;
    }
    if (const std::optional<int> percentage =
            ui_.TakeAppliedUiScalePercentage()) {
        ApplyUiScale(
            system_dpi_scale_,
            *percentage);
        WriteDpiConfiguration(scale_reason);
        applied = true;
    }
    if (ui_.TakeAppliedUiLanguage()) {
        ApplyLocalizedWindowTitle();
        ui_.RequestShellSourceRosterRefresh();
        applied = true;
    }
    if (applied && request_frame) {
        render_wake_scheduler_.RequestFrame();
    }
}

bool SpectiaryApp::ResolveAndApplyTheme(
    bool refresh_native_windows)
{
    const ThemeSelection selection =
        ui_.theme_selection();
    std::optional<ThemeId> system_theme;
    if (selection.policy ==
        ThemeSelectionPolicy::FollowSystem) {
        const AutomationSystemThemeFixtureResult
            fixture =
                ReadAutomationSystemThemeFixture(
                    automation_configuration_);
        system_theme = fixture.present
            ? fixture.theme
            : ReadWindowsSystemTheme();
    }
    const ThemeDescriptor& next_theme =
        ResolveBuiltInThemeDescriptor(
            selection,
            system_theme);
    const bool changed =
        !theme_initialized_ ||
        next_theme.id != resolved_theme_id_;
    resolved_theme_id_ = next_theme.id;
    theme_initialized_ = true;

    const ThemeDescriptor& theme =
        ResolvedThemeDescriptor();
    if (changed) {
        ActivateTheme(theme);
        viewport_renderer_.SetClearColor(
            theme.clear_color);
        if (ImGui::GetCurrentContext() != nullptr) {
            ApplyImGuiThemeColors(
                theme,
                ImGui::GetStyle());
            if (ImPlot::GetCurrentContext() != nullptr) {
                ApplyImPlotThemeColors(
                    theme,
                    ImPlot::GetStyle());
            }
            if (imgui_initialized_) {
                ApplyImGuiThemeColors(
                    theme,
                    base_imgui_style_);
            }
        }
    }

    if ((changed || refresh_native_windows) &&
        window_.hwnd() != nullptr) {
        ApplyTitleBarTheme(
            window_.hwnd(),
            theme.color_scheme);
    }
    if ((changed || refresh_native_windows) &&
        imgui_initialized_) {
        viewport_renderer_.RefreshNativeWindowThemes();
    }
    return changed;
}

const ThemeDescriptor&
SpectiaryApp::ResolvedThemeDescriptor() const
{
    if (const ThemeDescriptor* descriptor =
            FindBuiltInThemeDescriptor(
                resolved_theme_id_)) {
        return *descriptor;
    }
    return *FindBuiltInThemeDescriptor(
        BuiltInDarkThemeId());
}

void SpectiaryApp::ServiceAutomationSettingGet(
    const AutomationQueuedCommand& command)
{
    if (!automation_server_->IsRequestActive(
            command.request_id)) {
        return;
    }
    const auto* parameters =
        std::get_if<AutomationSettingGetParameters>(
            &command.parameters);
    if (parameters == nullptr) {
        automation_server_->Fail(
            command,
            "invalid_params",
            "setting.get parameters were not decoded.");
        return;
    }

    if (parameters->name ==
        kAutomationUiLanguageSettingName) {
        automation_server_->Complete(
            command,
            AutomationStringSettingResult(
                parameters->name,
                UiLanguageSettingValue(
                    ui_.ui_language())));
        return;
    }
    if (parameters->name ==
        kAutomationUiScaleSettingName) {
        automation_server_->Complete(
            command,
            AutomationIntegerSettingResult(
                parameters->name,
                ui_.ui_scale_percentage()));
        return;
    }
    if (parameters->name ==
        kAutomationUiThemeSettingName) {
        automation_server_->Complete(
            command,
            AutomationStringSettingResult(
                parameters->name,
                ThemeSelectionStableValue(
                    ui_.theme_selection())));
        return;
    }
    automation_server_->Fail(
        command,
        "unsupported_setting",
        "The requested automation setting is not supported.");
}

void SpectiaryApp::ServiceAutomationSettingSet(
    const AutomationQueuedCommand& command)
{
    const auto* parameters =
        std::get_if<AutomationSettingSetParameters>(
            &command.parameters);
    if (parameters == nullptr) {
        automation_server_->Fail(
            command,
            "invalid_params",
            "setting.set parameters were not decoded.");
        return;
    }

    enum class SupportedSetting {
        Language,
        UiScale,
        Theme,
    };
    SupportedSetting setting =
        SupportedSetting::Language;
    UiLanguage requested_language =
        UiLanguage::Count;
    int requested_scale = 0;
    ThemeSelection requested_theme =
        ThemeSelection::FollowSystem();
    if (parameters->name ==
        kAutomationUiLanguageSettingName) {
        setting = SupportedSetting::Language;
        const auto* value =
            std::get_if<std::string>(
                &parameters->value);
        if (value == nullptr) {
            automation_server_->Fail(
                command,
                "setting_type_mismatch",
                "ui.language requires a string value.");
            return;
        }
        requested_language =
            ParseUiLanguageSettingValue(*value)
                .value_or(UiLanguage::Count);
    } else if (
        parameters->name ==
        kAutomationUiScaleSettingName) {
        setting = SupportedSetting::UiScale;
        const auto* value =
            std::get_if<std::int64_t>(
                &parameters->value);
        if (value == nullptr) {
            automation_server_->Fail(
                command,
                "setting_type_mismatch",
                "ui.scale requires an integer value.");
            return;
        }
        if (*value <
                (std::numeric_limits<int>::min)() ||
            *value >
                (std::numeric_limits<int>::max)()) {
            automation_server_->Fail(
                command,
                "setting_value_rejected",
                "The UI scale must be from 80% through 150%.");
            return;
        }
        requested_scale =
            static_cast<int>(*value);
    } else if (
        parameters->name ==
        kAutomationUiThemeSettingName) {
        setting = SupportedSetting::Theme;
        const auto* value =
            std::get_if<std::string>(
                &parameters->value);
        if (value == nullptr) {
            automation_server_->Fail(
                command,
                "setting_type_mismatch",
                "ui.theme requires a string value.");
            return;
        }
        requested_theme =
            ParseThemeSelectionStableValue(*value)
                .value_or(
                    ThemeSelection::Explicit(
                        ThemeId(*value)));
    } else {
        automation_server_->Fail(
            command,
            "unsupported_setting",
            "The requested automation setting is not supported.");
        return;
    }

    if (!automation_server_->TryClaimExecution(
            command)) {
        return;
    }

    const UiLanguage previous_language =
        ui_.ui_language();
    const int previous_scale =
        ui_.ui_scale_percentage();
    const ThemeSelection previous_theme =
        ui_.theme_selection();
    ApplicationSettingsResult result;
    switch (setting) {
    case SupportedSetting::Language:
        result = ui_.SetUiLanguageForAutomation(
            requested_language);
        break;
    case SupportedSetting::UiScale:
        result = ui_.SetUiScaleForAutomation(
            requested_scale);
        break;
    case SupportedSetting::Theme:
        result = ui_.SetThemeSelectionForAutomation(
            requested_theme);
        break;
    }
    if (result.outcome ==
        ApplicationSettingsOutcome::Rejected) {
        automation_server_->Fail(
            command,
            "setting_value_rejected",
            result.detail);
        return;
    }
    if (result.outcome ==
        ApplicationSettingsOutcome::
            PersistenceFailed) {
        automation_server_->Fail(
            command,
            "setting_persistence_failed",
            result.detail);
        return;
    }

    ApplyPendingApplicationSettings(
        "automation_setting_changed");

    AutomationCommandResult result_payload;
    if (setting == SupportedSetting::Language) {
        const UiLanguage current =
            ui_.ui_language();
        result_payload = AutomationStringSettingResult(
            parameters->name,
            UiLanguageSettingValue(current),
            current != previous_language);
    } else if (setting == SupportedSetting::UiScale) {
        const int current =
            ui_.ui_scale_percentage();
        result_payload = AutomationIntegerSettingResult(
            parameters->name,
            current,
            current != previous_scale);
    } else {
        const ThemeSelection current =
            ui_.theme_selection();
        result_payload = AutomationStringSettingResult(
            parameters->name,
            ThemeSelectionStableValue(current),
            current != previous_theme);
    }
    automation_server_->Complete(
        command,
        result_payload);
}

void SpectiaryApp::ServiceAutomationPanelGet(
    const AutomationQueuedCommand& command)
{
    automation_panel_coordinator_.ServicePanelGet(
        command,
        AutomationPanelCoordinatorCallbacks());
}

void SpectiaryApp::ServiceAutomationPanelSet(
    const AutomationQueuedCommand& command)
{
    automation_panel_coordinator_.ServicePanelSet(
        command,
        frame_index_,
        {
            .window_renderable = !minimized_ && window_visible_,
            .immersive_plot_mode = ui_.immersive_plot_mode(),
            .presentation_available =
                !minimized_ &&
                window_visible_ &&
                !ui_.immersive_plot_mode(),
        },
        AutomationPanelCoordinatorCallbacks());
}

AutomationPanelCommandCoordinator::Callbacks
SpectiaryApp::AutomationPanelCoordinatorCallbacks()
{
    return {
        .is_request_active = [this](std::string_view request_id) {
            return automation_server_ &&
                   automation_server_->IsRequestActive(request_id);
        },
        .try_claim_execution = [this](
                                    const AutomationQueuedCommand& command) {
            return automation_server_ &&
                   automation_server_->TryClaimExecution(command);
        },
        .current_visibility = [this]() {
            return ui_.PanelVisibilityForAutomation();
        },
        .apply_visibility = [this](ApplicationPanel panel, bool visible) {
            const ApplicationSettingsResult result =
                ui_.SetPanelVisibilityForAutomation(panel, visible);
            AutomationPanelCommandCoordinator::ApplyOutcome outcome =
                AutomationPanelCommandCoordinator::ApplyOutcome::Unchanged;
            switch (result.outcome) {
            case ApplicationSettingsOutcome::Unchanged:
                outcome =
                    AutomationPanelCommandCoordinator::ApplyOutcome::
                        Unchanged;
                break;
            case ApplicationSettingsOutcome::Applied:
                outcome =
                    AutomationPanelCommandCoordinator::ApplyOutcome::Applied;
                break;
            case ApplicationSettingsOutcome::Rejected:
                outcome =
                    AutomationPanelCommandCoordinator::ApplyOutcome::Rejected;
                break;
            case ApplicationSettingsOutcome::PersistenceFailed:
                outcome = AutomationPanelCommandCoordinator::ApplyOutcome::
                    PersistenceFailed;
                break;
            }
            return AutomationPanelCommandCoordinator::ApplyResult{
                .outcome = outcome,
                .detail = result.detail,
            };
        },
        .complete = [this](
                        const AutomationQueuedCommand& command,
                        const AutomationCommandResult& result) {
            if (automation_server_) {
                automation_server_->Complete(command, result);
            }
        },
        .fail = [this](
                    const AutomationQueuedCommand& command,
                    std::string_view error_code,
                    std::string_view error_message) {
            if (automation_server_) {
                automation_server_->Fail(
                    command,
                    error_code,
                    error_message);
            }
        },
        .request_poll = [this]() {
            automation_poll_deadline_ =
                RenderWakeScheduler::Clock::now() +
                kAutomationIdlePollInterval;
            render_wake_scheduler_.RequestFrame();
        },
    };
}

AutomationExecution::Callbacks SpectiaryApp::AutomationExecutionCallbacks()
{
    return {
        .is_request_active = [this](std::string_view id) { return automation_server_->IsRequestActive(id); },
        .try_claim_execution = [this](const auto& command) { return automation_server_->TryClaimExecution(command); },
        .complete = [this](const auto& command, const auto& result) { automation_server_->Complete(command, result); },
        .fail = [this](const auto& command, auto code, auto message) { automation_server_->Fail(command, code, message); },
        .open_source = [this](const auto& path) { return ui_.BeginSourceOpenForAutomation(path); },
        .goto_spectrum = [this](auto index, auto name) { return ui_.GotoSpectrumForAutomation(index, name); },
        .assign_label = [this](int code) { return ui_.AssignLabelForAutomation(code); },
        .activation_generation = [this]() { return ui_.ActivationGenerationForAutomation(); },
        .presented_spectrum = [this]() { return ui_.PresentedSpectrumForAutomation(); },
        .view = [this]() { return ui_.AutomationView(); },
        .shell_idle = [this]() { return ui_.runtime_resource_observation().idle(); },
        .request_frame = [this]() { render_wake_scheduler_.RequestFrame(); },
    };
}

void SpectiaryApp::BeginAutomationFrameCapture(
    const AutomationQueuedCommand& command)
{
    const auto* parameters =
        std::get_if<AutomationFrameCaptureParameters>(
            &command.parameters);
    if (parameters == nullptr ||
        !automation_configuration_) {
        automation_capture_last_result_ =
            "failed";
        automation_capture_last_path_.clear();
        automation_server_->Fail(
            command,
            "invalid_params",
            "frame.capture parameters were not decoded.");
        return;
    }
    const std::filesystem::path requested_path(
        Utf8ToWide(parameters->path));
    if (automation_capture_command_ ||
        frame_capture_.pending()) {
        automation_capture_last_result_ =
            "failed";
        automation_capture_last_path_ =
            requested_path;
        automation_server_->Fail(
            command,
            "capture_busy",
            "A frame capture is already pending.");
        return;
    }
    const AutomationCapturePathValidation validation =
        ValidateAutomationCapturePath(
            automation_configuration_->state_root,
            requested_path);
    if (!validation.valid) {
        automation_capture_last_result_ =
            "failed";
        automation_capture_last_path_ =
            requested_path;
        automation_server_->Fail(
            command,
            validation.error_code,
            validation.error_message);
        return;
    }

    const OnDemandFrameCaptureRequestOutcome outcome =
        frame_capture_.Request(
            frame_index_,
            !minimized_ && window_visible_,
            validation.normalized_path);
    if (outcome ==
        OnDemandFrameCaptureRequestOutcome::
            WindowNotRenderable) {
        automation_capture_last_result_ =
            "failed";
        automation_capture_last_path_ =
            validation.normalized_path;
        automation_server_->Fail(
            command,
            "window_not_renderable",
            "The main application window is hidden or minimized.");
        return;
    }
    if (outcome !=
        OnDemandFrameCaptureRequestOutcome::Accepted) {
        automation_capture_last_result_ =
            "failed";
        automation_capture_last_path_ =
            validation.normalized_path;
        automation_server_->Fail(
            command,
            outcome ==
                    OnDemandFrameCaptureRequestOutcome::
                        AlreadyPending
                ? "capture_busy"
                : "capture_failed",
            "The application capture seam rejected the request.");
        return;
    }
    automation_capture_command_ =
        AutomationCaptureCommand{
            .command = command,
            .output_path =
                validation.normalized_path,
            .accepted_frame = frame_index_,
        };
    render_wake_scheduler_.RequestFrame();
}

void SpectiaryApp::ServiceAutomationProfileStart(
    const AutomationQueuedCommand& command)
{
    if (!automation_configuration_) {
        automation_server_->Fail(
            command,
            "profile_start_unavailable",
            "Performance recording is unavailable outside an isolated automation instance.");
        return;
    }
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (lifecycle.open) {
        automation_server_->Fail(
            command,
            "profile_recording_active",
            "Performance recording is already active.");
        return;
    }
    if (lifecycle.stopping ||
        automation_profile_stop_command_) {
        automation_server_->Fail(
            command,
            "profile_stop_in_progress",
            "The previous performance recording is still finishing.");
        return;
    }
    if (!automation_server_->TryClaimExecution(command)) {
        return;
    }

    const std::filesystem::path output_directory =
        ui_.profile_output_directory();
    const AutomationStateOwnedPathValidation validation =
        ValidateAutomationStateOwnedPath(
            automation_configuration_->state_root,
            output_directory);
    if (!validation.valid) {
        AutomationPreparedProfileOutput rejected_output{
            .error_message = validation.error_message,
        };
        (void)StartProfileRecording(
            "automation",
            std::optional<AutomationPreparedProfileOutput>(
                std::move(rejected_output)));
        automation_server_->Fail(
            command,
            "profile_output_outside_state_root",
            "The resolved performance output directory must remain below the isolated automation state root without reparse points.");
        render_wake_scheduler_.RequestFrame();
        return;
    }
    AutomationPreparedProfileOutput prepared_output =
        AutomationProfileOutputFactory::Create(
            automation_configuration_->state_root,
            validation.normalized_path);
    if (!StartProfileRecording(
            "automation",
            std::optional<AutomationPreparedProfileOutput>(
                std::move(prepared_output)))) {
        automation_server_->Fail(
            command,
            "profile_start_failed",
            "The production performance recorder could not start.");
        render_wake_scheduler_.RequestFrame();
        return;
    }

    automation_server_->Complete(command,
        AutomationProfileStartResult{PathToUtf8(profile_.path())});
    render_wake_scheduler_.RequestFrame();
}

void SpectiaryApp::BeginAutomationProfileStop(
    const AutomationQueuedCommand& command)
{
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (automation_profile_stop_command_ ||
        lifecycle.stopping) {
        automation_server_->Fail(
            command,
            "profile_stop_in_progress",
            "Performance recording is already finishing.");
        return;
    }
    if (!lifecycle.open) {
        automation_server_->Fail(
            command,
            "profile_not_recording",
            "Performance recording is not active.");
        return;
    }
    if (!automation_server_->TryClaimExecution(command)) {
        return;
    }

    automation_profile_stop_command_ = command;
    StopProfileRecording("automation");
    automation_poll_deadline_ =
        RenderWakeScheduler::Clock::now() +
        kAutomationIdlePollInterval;
    render_wake_scheduler_.RequestFrame();
}

void SpectiaryApp::PollAutomationProfileStop()
{
    if (!automation_profile_stop_command_) {
        return;
    }

    RefreshProfileRecordingStatus();
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (lifecycle.stopping) {
        return;
    }

    const AutomationQueuedCommand command =
        *automation_profile_stop_command_;
    automation_profile_stop_command_.reset();
    CompleteAutomationProfileStopTerminal(
        *automation_server_,
        command,
        lifecycle.stop_reason,
        profile_.path(),
        lifecycle.dropped_events);
}

bool SpectiaryApp::ServiceAutomationAppQuit(
    const AutomationQueuedCommand& command)
{
    const AutomationQueuedCommand quit_command =
        command;
    using ClaimResult =
        AutomationNamedPipeServer::
            AppQuitClaimResult;
    const ClaimResult claim =
        automation_server_->TryBeginAppQuit(
            quit_command);
    if (claim ==
        ClaimResult::WaitingForEarlierExecution) {
        automation_pending_quit_command_ =
            quit_command;
        automation_poll_deadline_ =
            RenderWakeScheduler::Clock::now() +
            kAutomationIdlePollInterval;
        return false;
    }
    automation_pending_quit_command_.reset();
    if (claim != ClaimResult::Claimed) {
        return true;
    }

    automation_idle_waits_.clear();
    automation_execution_.SettleForShutdown();
    CancelAutomationFrameCapture(
        "app.quit canceled the pending automation frame capture.");
    automation_capture_command_.reset();
    automation_poll_deadline_.reset();
    automation_shutdown_requested_ = true;
    const bool posted =
        window_.hwnd() != nullptr &&
        PostMessageW(
            window_.hwnd(),
            WM_CLOSE,
            0,
            0) != FALSE;
    if (posted) {
        automation_server_->Complete(quit_command);
    } else {
        automation_server_->Fail(
            quit_command,
            "close_request_failed",
            "Could not post the normal Win32 close request.");
    }
    return true;
}

bool SpectiaryApp::
    AutomationFrameCaptureRequestActive() const
{
    return automation_server_ &&
           automation_capture_command_ &&
           automation_server_->IsRequestActive(
               automation_capture_command_
                   ->command.request_id) &&
           frame_capture_.requested_output_path() &&
           *frame_capture_.requested_output_path() ==
               automation_capture_command_
                   ->output_path;
}

void SpectiaryApp::CancelAutomationFrameCapture(
    std::string_view reason)
{
    if (!automation_capture_command_ &&
        !frame_capture_.requested_output_path()) {
        return;
    }
    if (automation_capture_command_) {
        automation_capture_last_path_ =
            automation_capture_command_
                ->output_path;
    } else if (
        frame_capture_.requested_output_path()) {
        automation_capture_last_path_ =
            *frame_capture_
                 .requested_output_path();
    }
    automation_capture_last_result_ =
        "canceled";
    frame_capture_.Cancel(
        std::string(reason));
}

void SpectiaryApp::PollAutomationBusinessOperations()
{
    PollAutomationProfileStop();
    PollAutomationPanelCommands();
    automation_execution_.Poll(AutomationExecutionCallbacks());

    if (automation_capture_command_) {
        AutomationCaptureCommand& pending =
            *automation_capture_command_;
        if (!automation_server_->IsRequestActive(
                pending.command.request_id)) {
            CancelAutomationFrameCapture(
                "The automation request was disconnected or canceled.");
            automation_capture_command_.reset();
        } else if (!frame_capture_.pending()) {
            if (frame_capture_.status() ==
                OnDemandFrameCaptureStatus::
                    WindowUnavailable) {
                automation_server_->Fail(
                    pending.command,
                    "window_not_renderable",
                    "The main application window became hidden or minimized.");
                automation_capture_last_result_ =
                    "failed";
                automation_capture_last_path_ =
                    pending.output_path;
            } else if (
                frame_capture_.status() ==
                OnDemandFrameCaptureStatus::
                    FailedPreparingOutputDirectory) {
                automation_server_->Fail(
                    pending.command,
                    "capture_directory_failed",
                    "The capture output directory could not be prepared.");
                automation_capture_last_result_ =
                    "failed";
                automation_capture_last_path_ =
                    pending.output_path;
            } else {
                automation_server_->Fail(
                    pending.command,
                    "capture_failed",
                    "The application-rendered PNG capture failed.");
                automation_capture_last_result_ =
                    frame_capture_.status() ==
                            OnDemandFrameCaptureStatus::
                                Canceled
                        ? "canceled"
                        : "failed";
                automation_capture_last_path_ =
                    pending.output_path;
            }
            automation_capture_command_.reset();
        }
    }
}

bool SpectiaryApp::SettleAutomationPanelCommandsForShutdown()
{
    return automation_panel_coordinator_.SettleForShutdown(
        AutomationPanelCoordinatorCallbacks());
}

void SpectiaryApp::PollAutomationPanelCommands()
{
    automation_panel_coordinator_.Poll(
        ui_.PresentedPanelVisibilityForAutomation(),
        ui_.PanelPresentationStatusForAutomation(),
        {
            .window_renderable = !minimized_ && window_visible_,
            .immersive_plot_mode = ui_.immersive_plot_mode(),
            .presentation_available =
                !minimized_ &&
                window_visible_ &&
                !ui_.immersive_plot_mode(),
        },
        AutomationPanelCoordinatorCallbacks());
}

bool SpectiaryApp::AutomationBusinessIdle() const noexcept
{
    return automation_panel_coordinator_.Idle() &&
           automation_execution_.Idle() &&
           !automation_capture_command_ &&
           !automation_profile_stop_command_ &&
           !frame_capture_.pending();
}

void SpectiaryApp::ServiceAutomation()
{
    if (!automation_server_) {
        return;
    }

    RefreshProfileRecordingStatus();
    ApplyPendingApplicationSettings(
        "user_scale_changed");
    PollAutomationBusinessOperations();
    if (automation_pending_quit_command_) {
        if (!ServiceAutomationAppQuit(
                *automation_pending_quit_command_)) {
            return;
        }
        if (automation_shutdown_requested_) {
            return;
        }
    }
    std::vector<AutomationQueuedCommand> commands;
    if (automation_idle_waits_.empty()) {
        commands =
            automation_server_->TakePendingCommands();
    }
    bool stop_dispatch = false;
    for (const AutomationQueuedCommand& command :
         commands) {
        switch (command.command) {
        case AutomationCommandKind::StateGet:
            if (!automation_server_->IsRequestActive(
                    command.request_id)) {
                break;
            }
            automation_server_->CompleteState(command, AutomationState());
            break;
        case AutomationCommandKind::WaitIdle:
            if (!automation_server_->IsRequestActive(
                    command.request_id)) {
                break;
            }
            automation_idle_waits_.push_back(
                command);
            break;
        case AutomationCommandKind::SettingGet:
            ServiceAutomationSettingGet(command);
            break;
        case AutomationCommandKind::SettingSet:
            ServiceAutomationSettingSet(command);
            break;
        case AutomationCommandKind::PanelGet:
            ServiceAutomationPanelGet(command);
            break;
        case AutomationCommandKind::PanelSet:
            ServiceAutomationPanelSet(command);
            break;
        case AutomationCommandKind::SourceOpen:
            automation_execution_.BeginAutomationSourceOpen(command, AutomationExecutionCallbacks());
            break;
        case AutomationCommandKind::SpectrumGoto:
            automation_execution_.BeginAutomationSpectrumGoto(command, AutomationExecutionCallbacks());
            break;
        case AutomationCommandKind::LabelAssign:
            automation_execution_.BeginAutomationLabelAssign(command, AutomationExecutionCallbacks());
            break;
        case AutomationCommandKind::FrameCapture:
            BeginAutomationFrameCapture(command);
            break;
        case AutomationCommandKind::ProfileStart:
            ServiceAutomationProfileStart(command);
            break;
        case AutomationCommandKind::ProfileStop:
            BeginAutomationProfileStop(command);
            break;
        case AutomationCommandKind::AppQuit:
            (void)ServiceAutomationAppQuit(
                command);
            stop_dispatch = true;
            break;
        }
        if (stop_dispatch) {
            break;
        }
    }

    PollAutomationBusinessOperations();
    if (automation_pending_quit_command_) {
        if (!ServiceAutomationAppQuit(
                *automation_pending_quit_command_)) {
            return;
        }
        if (automation_shutdown_requested_) {
            return;
        }
    }
    automation_idle_waits_.erase(
        std::remove_if(
            automation_idle_waits_.begin(),
            automation_idle_waits_.end(),
            [this](const auto& wait) {
                return !automation_server_
                            ->IsRequestActive(
                                wait.request_id);
            }),
        automation_idle_waits_.end());
    if (automation_idle_waits_.empty() &&
        AutomationBusinessIdle()) {
        automation_poll_deadline_.reset();
        return;
    }

    const ShellRuntimeResourceObservation observation =
        ui_.runtime_resource_observation();
    bool idle_wait_completed = false;
    for (auto wait = automation_idle_waits_.begin();
         wait != automation_idle_waits_.end();) {
        if (AutomationBusinessIdle() &&
            observation.idle() &&
            automation_server_->TryCompleteIdleWaits(
                {wait->request_id})) {
            idle_wait_completed = true;
            wait =
                automation_idle_waits_.erase(
                    wait);
        } else {
            ++wait;
        }
    }
    if (idle_wait_completed) {
        PostAutomationCommandReady(
            window_.hwnd());
    }
    if (automation_idle_waits_.empty() &&
        AutomationBusinessIdle()) {
        automation_poll_deadline_.reset();
        return;
    }
    automation_poll_deadline_ =
        RenderWakeScheduler::Clock::now() +
        kAutomationIdlePollInterval;
}

std::optional<RenderWakeScheduler::TimePoint>
SpectiaryApp::NextAutomationDeadline() const
{
    const auto execution_deadline = automation_execution_.NextDeadline();
    if (!automation_poll_deadline_) return execution_deadline;
    if (!execution_deadline) return automation_poll_deadline_;
    return std::min(*automation_poll_deadline_, *execution_deadline);
}

AutomationStateSnapshot
SpectiaryApp::AutomationState()
{
    AutomationStateSnapshot state;
    if (!automation_configuration_ ||
        !automation_server_) {
        return state;
    }
    state.instance_id =
        automation_configuration_->instance_id;
    state.control =
        automation_server_->queue_snapshot();

    const ShellRuntimeResourceObservation shell =
        ui_.runtime_resource_observation();
    state.shell = {
        .idle = shell.idle(),
        .source_load_idle =
            shell.source_load_idle(),
        .pending_completion_idle =
            shell.pending_completion_idle(),
        .background_retirement_idle =
            shell.background_retirement_idle(),
        .active_load_count =
            shell.load_activity.active_task_count,
        .completed_load_count =
            shell.load_activity.completed_count,
        .pending_load_count =
            shell.pending_load_count,
        .retirement_queued_count =
            shell.load_activity
                .retirement_queued_count,
        .retirement_in_flight_count =
            shell.load_activity
                .retirement_in_flight_count,
    };
    const ShellAutomationView& automation =
        ui_.PresentedAutomationView();
    state.shell.current_source_id =
        shell.active_source_id;
    state.shell.current_source_path =
        shell.active_source_path;
    const std::string_view language =
        UiLanguageSettingValue(
            ui_.ui_language());
    state.settings = {
        .language = language.empty()
            ? "en"
            : std::string(language),
        .ui_scale_percentage =
            user_ui_scale_percentage_,
    };
    state.panels =
        ui_.PanelVisibilityForAutomation();
    state.presented_source = {
        .present =
            !automation.source_id.empty() ||
            !automation.source_path.empty(),
        .id = automation.source_id,
        .path = automation.source_path,
    };
    state.spectrum = {
        .present = automation.spectrum.present,
        .index = automation.spectrum.index,
        .name = automation.spectrum.name,
        .count = automation.spectrum.count,
    };
    state.labeling = {
        .has_active_task =
            automation.labeling.has_active_task,
        .task_id =
            automation.labeling.task_id,
        .task_name =
            automation.labeling.task_name,
        .task_ids =
            automation.labeling.task_ids,
        .current_spectrum_code =
            automation.labeling
                .current_spectrum_code,
    };
    state.capture = {
        .pending =
            automation_capture_command_ &&
            frame_capture_.pending(),
        .current_path =
            automation_capture_command_
            ? automation_capture_command_
                  ->output_path
            : std::filesystem::path{},
        .last_result =
            automation_capture_last_result_,
        .last_path =
            automation_capture_last_path_,
    };
    const ProfileSink::LifecycleSnapshot profile_state =
        profile_.lifecycle_snapshot();
    const ProfileSink::StopReason profile_stop_reason =
        profile_state.stop_reason;
    std::string profile_status = "inactive";
    if (profile_state.open) {
        profile_status = "recording";
    } else if (profile_state.stopping) {
        profile_status = "stopping";
    } else if (
        profile_stop_reason ==
            ProfileSink::StopReason::WriteFailure ||
        profile_status_.kind ==
            ProfileRecordingStatusKind::StartFailed ||
        profile_status_.kind ==
            ProfileRecordingStatusKind::Failed ||
        profile_status_.kind ==
            ProfileRecordingStatusKind::FailedWhileWriting) {
        profile_status = "failed";
    } else if (
        profile_stop_reason !=
        ProfileSink::StopReason::None) {
        profile_status = "succeeded";
    }
    state.profile = {
        .status = std::move(profile_status),
        .path = profile_.path(),
        .stop_reason =
            ProfileSink::StopReasonName(
                profile_stop_reason),
        .dropped_events = profile_state.dropped_events,
    };
    state.window = {
        .visible = window_visible_,
        .minimized = minimized_,
        .client_width = window_.client_width(),
        .client_height =
            window_.client_height(),
    };
    state.runtime = {
        .running = running_,
        .shutting_down =
            automation_shutdown_requested_,
        .frame_index = frame_index_,
    };
    return state;
}

void SpectiaryApp::LogProfileRecordingStarted(
    std::string_view trigger,
    std::string_view configuration_reason)
{
    if (!profile_.is_open()) {
        return;
    }
    profile_.WriteEvent("profile_recording", {
                                                   ProfileSink::Field::String("action", "start"),
                                                   ProfileSink::Field::String("trigger", std::string(trigger)),
                                                   ProfileSink::Field::String("writer", "bounded_async_jsonl"),
                                                   ProfileSink::Field::Number(
                                                       "max_queue_bytes",
                                                       std::to_string(profile_limits_.max_queue_bytes)),
                                                   ProfileSink::Field::Number(
                                                       "max_file_bytes",
                                                       std::to_string(profile_limits_.max_file_bytes)),
                                                   ProfileSink::Field::Number(
                                                       "max_duration_seconds",
                                                       std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                                          profile_limits_.max_duration)
                                                                          .count())),
                                               });
    WriteRuntimeConfiguration(configuration_reason);
}

void SpectiaryApp::WriteRuntimeConfiguration(std::string_view reason)
{
    if (!profile_.is_open()) {
        return;
    }
    const RuntimePaths& runtime_paths =
        startup_.runtime_paths();
    const SpectrumSnapshotHandle snapshot = ui_.current_snapshot();
    const std::string source_type(
        snapshot ? MetadataValue(snapshot->source.metadata, "source_type") : std::string_view{});
    profile_.WriteEvent("runtime_config", {
#ifdef SPECTIARY_REDIRECTION_AB_BUILD
                                            ProfileSink::Field::String("redirection_build", "creation_time_ab"),
#else
                                            ProfileSink::Field::String("redirection_build", "baseline"),
#endif
                                            ProfileSink::Field::String("reason", std::string(reason)),
                                            ProfileSink::Field::String("target", "win32_dx11_imgui_implot"),
                                            ProfileSink::Field::String(
                                                "pan_pacing_requested",
                                                pan_pacing_.requested),
                                            ProfileSink::Field::String(
                                                "pan_pacing_effective",
                                                PanPacingModeName(
                                                    pan_pacing_.effective)),
                                            ProfileSink::Field::Bool(
                                                "pan_pacing_recognized",
                                                pan_pacing_.recognized),
                                            ProfileSink::Field::String(
                                                "frame_capture_requested",
                                                frame_capture_.configuration().requested),
                                            ProfileSink::Field::Bool(
                                                "frame_capture_enabled",
                                                frame_capture_.enabled()),
                                            ProfileSink::Field::Bool(
                                                "frame_capture_recognized",
                                                frame_capture_.configuration().recognized),
                                            ProfileSink::Field::String(
                                                "distribution",
                                                DistributionName(runtime_paths.distribution)),
                                            ProfileSink::Field::String(
                                                "storage_profile",
                                                StorageProfileName(runtime_paths.storage_profile)),
                                            ProfileSink::Field::String(
                                                "package_root",
                                                PathToUtf8(runtime_paths.package_root)),
                                            ProfileSink::Field::String(
                                                "application_data_root",
                                                PathToUtf8(runtime_paths.application_data_root)),
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

void SpectiaryApp::RefreshProfileRecordingStatus()
{
    (void)profile_.TryFinalizeStop();
    const ProfileSink::LifecycleSnapshot lifecycle =
        profile_.lifecycle_snapshot();
    if (lifecycle.open) {
        if (lifecycle.dropped_events > 0) {
            profile_status_ = {
                .kind =
                    ProfileRecordingStatusKind::
                        EventsDroppedUnderPressure,
                .dropped_events =
                    lifecycle.dropped_events,
            };
        }
        return;
    }
    if (lifecycle.stopping) {
        profile_status_ = {
            .kind = ProfileRecordingStatusKind::Finishing,
        };
        return;
    }

    const ProfileSink::StopReason reason =
        lifecycle.stop_reason;
    if (reason == displayed_profile_stop_reason_) {
        return;
    }
    displayed_profile_stop_reason_ = reason;
    if (reason != ProfileSink::StopReason::None) {
        profile_status_ = DescribeProfileRecordingStop(
            reason,
            lifecycle.dropped_events,
            profile_.error_message());
    }
}

void SpectiaryApp::LogPresentationUpdates()
{
    const D3D11PresentMode present_mode = MainPresentMode();
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

void SpectiaryApp::WritePresentationUpdate(
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

void SpectiaryApp::SchedulePresentationTargetRefresh(HWND hwnd) noexcept
{
    if (hwnd != nullptr) {
        SetTimer(
            hwnd,
            kPresentationRefreshTimer,
            kPresentationRefreshDelayMs,
            nullptr);
    }
}

void SpectiaryApp::RefreshPresentationTargets(std::string_view reason)
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

void SpectiaryApp::LogDisplayEnvironment(std::string_view reason)
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
    const D3D11PresentMode present_mode = MainPresentMode();

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
                                                  ProfileSink::Field::String(
                                                      "pan_pacing_requested",
                                                      pan_pacing_.requested),
                                                  ProfileSink::Field::String(
                                                      "pan_pacing_effective",
                                                      PanPacingModeName(
                                                          pan_pacing_.effective)),
                                                  ProfileSink::Field::String(
                                                      "presentation_present_mode",
                                                      D3D11PresentModeName(
                                                          present_mode)),
                                                  ProfileSink::Field::Number(
                                                      "presentation_sync_interval",
                                                      std::to_string(
                                                          D3D11PresentSyncInterval(
                                                              present_mode,
                                                              renderer_
                                                                  .tearing_supported()))),
                                                  ProfileSink::Field::Number(
                                                      "presentation_flags",
                                                      std::to_string(
                                                          D3D11PresentFlags(
                                                              present_mode,
                                                              renderer_
                                                                  .tearing_supported()))),
                                                  ProfileSink::Field::String(
                                                      "presentation_feedback",
                                                      renderer_.presentation_backend() ==
                                                              D3D11PresentationBackend::Composition
                                                          ? "composition"
                                                          : "unavailable"),
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
                                                          D3D11PresentMode::DisplayVSync,
                                                          renderer_.tearing_supported()))),
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

LRESULT SpectiaryApp::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ACTIVATEAPP && wparam) {
        external_open_router_.MarkUsed();
        if (jump_list_) ui_.RequestShellSourceRosterRefresh();
    }
    if (message == kSourceLoadCompletionReadyMessage) {
        render_wake_scheduler_.RequestFrame();
        return 0;
    }
    if (message == kAutomationCommandReadyMessage) {
        ServiceAutomation();
        return 0;
    }

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
        const CompositorClockTickOutcome outcome =
            render_wake_scheduler_.OnCompositorClockTick(
                compositor_clock_.ConsumeTick(),
                compositor_clock_.boost_active());
        if (outcome == CompositorClockTickOutcome::FallbackFrameRequested) {
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
    case WM_QUERYENDSESSION:
        os_session_ending_ = true;
        return TRUE; // Unsaved drafts never veto OS shutdown/logoff.
    case WM_ENDSESSION:
        os_session_ending_ = wparam != FALSE;
        return 0;
    case WM_CLOSE:
        if (!os_session_ending_ && !automation_shutdown_requested_ &&
            !ui_.PrepareLabelingForInteractiveClose()) {
            const auto text = Utf8ToWide(UiText(ui_.ui_language(), UiTextId::LabelingCloseBlocked));
            const auto title = Utf8ToWide(UiText(ui_.ui_language(), UiTextId::LocalStateWarningTitle));
            MessageBoxW(hwnd, text.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
            return 0;
        }
        external_open_router_.Stop();
        break;
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
        if (ResolveAndApplyTheme(true)) {
            render_wake_scheduler_.RequestFrame();
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        (void)ResolveAndApplyTheme(true);
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
            ApplyUiScale(
                DpiScaleFromWParam(wparam),
                user_ui_scale_percentage_);
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
                                                ProfileSink::Field::Number("dpi_scale", std::to_string(system_dpi_scale_)),
                                                ProfileSink::Field::Number("system_dpi_scale", std::to_string(system_dpi_scale_)),
                                                ProfileSink::Field::Number("user_scale", std::to_string(user_ui_scale_)),
                                                ProfileSink::Field::Number("effective_scale", std::to_string(effective_ui_scale_)),
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

void SpectiaryApp::LogInputMessage(UINT message, WPARAM wparam, LPARAM lparam)
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

}  // namespace spectiary
