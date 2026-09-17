#include "renderer/d3d11_composition_swap_chain.h"
#include "profile/presentation_trace.h"
#include "renderer/d3d11_sdr_swap_chain.h"
#include "renderer/d3d11_imgui_viewport_renderer.h"
#include "renderer/d3d11_renderer.h"
#include "renderer/d3d11_window_presentation.h"
#include "renderer/win32_display_refresh.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <wincodec.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {
struct D3D11CompositionSwapChainTestAccess {
    static bool Released(const D3D11CompositionSwapChain& chain) {
        for (const auto& buffer : chain.buffers_) {
            if (buffer.texture || buffer.render_target || buffer.presentation || buffer.available_event ||
                buffer.identity.generation != 0) return false;
        }
        return !chain.initialized() && !chain.surface_handle_ && !chain.statistics_event_ &&
            chain.selected_buffer_ == -1 && chain.bound_buffer_ == -1;
    }
    inline static unsigned fail_step = 0;
    static void Inject(D3D11CompositionSwapChain& chain, unsigned step) {
        fail_step = step;
        chain.allocation_checkpoint_ = [](unsigned current) -> HRESULT { return current == fail_step ? E_FAIL : S_OK; };
    }
    static void Inject(D3D11WindowPresentation& window, unsigned step) { Inject(window.composition_, step); }
    static auto Generations(const D3D11CompositionSwapChain& chain) {
        return std::array<std::uint64_t, 3>{chain.buffers_[0].identity.generation,
            chain.buffers_[1].identity.generation, chain.buffers_[2].identity.generation};
    }
};
}
namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kSwapChainTestWindowClass = L"SpecForge.SdrSwapChain.TestWindow";

void Require(bool condition, std::string_view message);

LRESULT CALLBACK SwapChainTestWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

class SwapChainTestWindow {
public:
    explicit SwapChainTestWindow(DWORD extended_style = 0)
    {
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = SwapChainTestWindowProc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = kSwapChainTestWindowClass;
        atom_ = RegisterClassW(&window_class);
        Require(atom_ != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "the swap-chain integration test should register its window class");

        hwnd_ = CreateWindowExW(
            extended_style,
            kSwapChainTestWindowClass,
            L"SpecForge SDR swap-chain integration test",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            320,
            240,
            nullptr,
            nullptr,
            instance_,
            nullptr);
        Require(hwnd_ != nullptr, "the swap-chain integration test should create a hidden HWND");
    }

    ~SwapChainTestWindow()
    {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
        if (atom_ != 0) {
            UnregisterClassW(kSwapChainTestWindowClass, instance_);
        }
    }

    SwapChainTestWindow(const SwapChainTestWindow&) = delete;
    SwapChainTestWindow& operator=(const SwapChainTestWindow&) = delete;

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    HINSTANCE instance_ = nullptr;
    ATOM atom_ = 0;
    HWND hwnd_ = nullptr;
};

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        path_ =
            std::filesystem::temp_directory_path() /
            (L"SpecForge-frame-capture-test-" +
             std::to_wstring(GetCurrentProcessId()) +
             L"-" +
             std::to_wstring(GetTickCount64()));
        std::error_code error;
        std::filesystem::create_directories(
            path_,
            error);
        Require(
            !error,
            "the frame-capture test should create an isolated temporary directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(
            path_,
            ignored);
    }

    TemporaryDirectory(
        const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(
        const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct TestFileIdentity {
    DWORD volume_serial_number = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;

    [[nodiscard]] bool operator==(
        const TestFileIdentity&) const = default;
};

std::optional<TestFileIdentity> FileIdentity(
    const std::filesystem::path& path)
{
    const HANDLE file =
        CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information = {};
    const bool queried =
        GetFileInformationByHandle(
            file,
            &information) != FALSE;
    CloseHandle(file);
    if (!queried) {
        return std::nullopt;
    }
    return TestFileIdentity{
        .volume_serial_number =
            information.dwVolumeSerialNumber,
        .file_index_high =
            information.nFileIndexHigh,
        .file_index_low =
            information.nFileIndexLow,
    };
}

bool IsReparseEntry(
    const std::filesystem::path& path)
{
    const HANDLE entry =
        CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information = {};
    const bool reparse =
        GetFileInformationByHandle(
            entry,
            &information) != FALSE &&
        (information.dwFileAttributes &
         FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    CloseHandle(entry);
    return reparse;
}

std::string ReadFileBytes(
    const std::filesystem::path& path)
{
    std::ifstream stream(
        path,
        std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>(),
    };
}

std::vector<std::filesystem::path>
CaptureTemporaryArtifacts(
    const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> artifacts;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(
             directory,
             error);
         !error &&
         entry !=
             std::filesystem::directory_iterator{};
         entry.increment(error)) {
        const std::wstring name =
            entry->path().filename().wstring();
        if (name.starts_with(
                L".specforge-capture-") &&
            name.ends_with(L".tmp")) {
            artifacts.push_back(entry->path());
        }
    }
    Require(
        !error,
        "capture temporary artifacts should be enumerable");
    return artifacts;
}

bool ReadPngObservation(
    const std::filesystem::path& path,
    UINT& width,
    UINT& height,
    std::array<BYTE, 4>& first_pixel)
{
    const HRESULT com_result = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED |
            COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(com_result);
    if (FAILED(com_result) &&
        com_result != RPC_E_CHANGED_MODE) {
        return false;
    }

    HRESULT result = E_FAIL;
    {
        ComPtr<IWICImagingFactory> factory;
        result = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(result)) {
            result = factory->CreateDecoderFromFilename(
                path.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf());
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(result)) {
            result = decoder->GetFrame(
                0,
                frame.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = frame->GetSize(
                &width,
                &height);
        }
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result)) {
            result = factory->CreateFormatConverter(
                converter.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(result)) {
            WICRect first_pixel_rect = {
                0,
                0,
                1,
                1,
            };
            result = converter->CopyPixels(
                &first_pixel_rect,
                static_cast<UINT>(
                    first_pixel.size()),
                static_cast<UINT>(
                    first_pixel.size()),
                first_pixel.data());
        }
    }
    if (uninitialize) {
        CoUninitialize();
    }
    return SUCCEEDED(result);
}

void RequirePngMatchesClientAreaAndPixel(
    HWND hwnd,
    const std::filesystem::path& path,
    const std::array<BYTE, 4>& expected_pixel)
{
    RECT client = {};
    Require(
        GetClientRect(hwnd, &client) != FALSE,
        "the capture test should query its client size");
    UINT captured_width = 0;
    UINT captured_height = 0;
    std::array<BYTE, 4> first_pixel = {};
    Require(
        ReadPngObservation(
            path,
            captured_width,
            captured_height,
            first_pixel) &&
            captured_width ==
                static_cast<UINT>(
                    client.right - client.left) &&
            captured_height ==
                static_cast<UINT>(
                    client.bottom - client.top),
        "the encoded PNG should be decodable and match the rendered client area");
    for (std::size_t channel = 0;
         channel < first_pixel.size();
         ++channel) {
        const int difference =
            static_cast<int>(first_pixel[channel]) -
            static_cast<int>(expected_pixel[channel]);
        Require(
            difference >= -1 && difference <= 1,
            "the PNG should contain the active frame's rendered RGBA pixels");
    }
}

HRESULT CreateTestDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context)
{
    constexpr std::array feature_levels = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels.data(),
        static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &selected_feature_level,
        context.GetAddressOf());
    if (FAILED(result)) {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels.data(),
            static_cast<UINT>(feature_levels.size()),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &selected_feature_level,
            context.GetAddressOf());
    }
    return result;
}

ComPtr<IDXGIFactory2> GetTestFactory(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgi_device;
    Require(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()))), "the D3D11 device should expose IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    Require(SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf())), "the DXGI device should expose its adapter");
    ComPtr<IDXGIFactory2> factory;
    Require(
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))),
        "the DXGI adapter should expose IDXGIFactory2");
    return factory;
}

struct ViewportCleanupObservation {
    bool viewport_destroyed = false;
    bool callbacks_cleared = false;
    bool renderer_shutdown = false;
    bool context_destroyed = false;
};

class ImGuiViewportTestFixture {
public:
    ImGuiViewportTestFixture(
        IDXGIFactory2* factory,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        HWND hwnd,
        ViewportCleanupObservation* cleanup_observation = nullptr)
        : previous_context_(ImGui::GetCurrentContext()), cleanup_observation_(cleanup_observation)
    {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        backend_initialized_ = context_ != nullptr &&
                               ImGui_ImplDX11_Init(device, context);
        renderer_initialized_ = backend_initialized_ &&
                                renderer_.Initialize(factory, device, context);
        viewport_.ID = 73;
        viewport_.PlatformHandle = viewport_.PlatformHandleRaw = hwnd;
        viewport_.Size = ImVec2(320.0f, 240.0f);
        draw_data_.DisplaySize = ImVec2(0.0f, 0.0f);
        viewport_.DrawData = &draw_data_;
    }

    ~ImGuiViewportTestFixture()
    {
        if (context_ == nullptr) {
            return;
        }

        ImGui::SetCurrentContext(context_);
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (viewport_.RendererUserData != nullptr && platform_io.Renderer_DestroyWindow != nullptr) {
            platform_io.Renderer_DestroyWindow(&viewport_);
        }
        if (cleanup_observation_ != nullptr) {
            cleanup_observation_->viewport_destroyed = viewport_.RendererUserData == nullptr;
        }

        platform_io.ClearRendererHandlers();
        if (cleanup_observation_ != nullptr) {
            cleanup_observation_->callbacks_cleared =
                platform_io.Renderer_CreateWindow == nullptr && platform_io.Renderer_DestroyWindow == nullptr &&
                platform_io.Renderer_SetWindowSize == nullptr && platform_io.Renderer_RenderWindow == nullptr &&
                platform_io.Renderer_SwapBuffers == nullptr;
        }

        renderer_.Shutdown();
        if (cleanup_observation_ != nullptr) {
            cleanup_observation_->renderer_shutdown = true;
        }
        if (backend_initialized_) {
            ImGui_ImplDX11_Shutdown();
            backend_initialized_ = false;
        }
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        ImGui::SetCurrentContext(previous_context_);
        if (cleanup_observation_ != nullptr) {
            cleanup_observation_->context_destroyed = ImGui::GetCurrentContext() == previous_context_;
        }
    }

    ImGuiViewportTestFixture(const ImGuiViewportTestFixture&) = delete;
    ImGuiViewportTestFixture& operator=(const ImGuiViewportTestFixture&) = delete;

    [[nodiscard]] bool renderer_initialized() const noexcept { return renderer_initialized_; }
    [[nodiscard]] bool has_viewport_swap_chain() const noexcept { return viewport_.RendererUserData != nullptr; }
    [[nodiscard]] specforge::D3D11RendererError TakeLastError() noexcept { return renderer_.TakeLastError(); }
    [[nodiscard]] std::vector<specforge::D3D11ViewportPresentationUpdate>
    TakePresentationUpdates() noexcept
    {
        return renderer_.TakePresentationUpdates();
    }
    [[nodiscard]] std::vector<specforge::D3D11ViewportPresentCompletion>
    TakePresentCompletions() noexcept
    {
        return renderer_.TakePresentCompletions();
    }

    void CreateViewport()
    {
        ImGui::GetPlatformIO().Renderer_CreateWindow(&viewport_);
    }

    void ResizeViewport(ImVec2 size)
    {
        ImGui::GetPlatformIO().Renderer_SetWindowSize(&viewport_, size);
    }

    void PresentViewport()
    {
        // Each invocation models a distinct application frame, including the
        // production one-replacement-per-ImGui-frame guard.
        ImGui::GetIO().DisplaySize = viewport_.Size;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImGui::EndFrame();
        ImGui::GetPlatformIO().Renderer_RenderWindow(&viewport_, nullptr);
        ImGui::GetPlatformIO().Renderer_SwapBuffers(&viewport_, nullptr);
    }

    void SetCompositorClockPaced(bool paced)
    {
        renderer_.SetCompositorClockPaced(paced);
    }
    void SetFeedbackAcquireOnly(bool enabled) { renderer_.SetFeedbackAcquireOnlyExperiment(enabled); }

    void DestroyViewport()
    {
        ImGui::GetPlatformIO().Renderer_DestroyWindow(&viewport_);
    }

private:
    ImGuiContext* previous_context_ = nullptr;
    ImGuiContext* context_ = nullptr;
    specforge::D3D11ImGuiViewportRenderer renderer_;
    ImGuiViewport viewport_;
    ImDrawData draw_data_;
    ViewportCleanupObservation* cleanup_observation_ = nullptr;
    bool backend_initialized_ = false;
    bool renderer_initialized_ = false;
};

void TestSdrSwapChainUsesModernSrgbPresentationContract()
{
    const DXGI_SWAP_CHAIN_DESC1 desc = specforge::MakeSdrSwapChainDesc(1280, 720);
    const DXGI_SWAP_CHAIN_DESC1 tearing_desc = specforge::MakeSdrSwapChainDesc(1280, 720, true);

    Require(desc.Width == 1280 && desc.Height == 720, "requested dimensions should be preserved");
    Require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "SDR presentation should use 8-bit RGBA UNORM");
    Require(desc.BufferCount == 2, "flip-model presentation requires at least two buffers");
    Require(desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD, "presentation should use flip discard");
    Require(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0, "swap chain should not be multisampled");
    Require(desc.BufferUsage == DXGI_USAGE_RENDER_TARGET_OUTPUT, "buffers should support render-target output");
    Require(desc.Scaling == DXGI_SCALING_STRETCH, "windowed swap chain should scale with the client area");
    Require(desc.AlphaMode == DXGI_ALPHA_MODE_IGNORE, "opaque Win32 windows should ignore swap-chain alpha");
    Require(desc.Flags == 0, "borderless fullscreen should not request a display mode switch");
    Require(
        tearing_desc.Flags == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
        "a capable system should opt the flip-model chain into variable-refresh presentation");
    Require(
        specforge::kSdrSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        "content color space should be explicitly tagged as sRGB/Rec.709 SDR");
    Require(
        specforge::D3D11PresentSyncInterval(
            specforge::D3D11PresentMode::DisplayVSync,
            true) == 1,
        "normal event-driven presentation should remain display-vsync synchronized");
    Require(
        specforge::D3D11PresentSyncInterval(
            specforge::D3D11PresentMode::CompositorClock,
            true) == 0,
        "compositor-clock-paced presentation should not wait again on virtualized DXGI vblank");
    Require(
        specforge::D3D11PresentSyncInterval(
            specforge::D3D11PresentMode::CompositorClock,
            false) == 1,
        "compositor-clock DXGI fallback should preserve tear-free vsync when tearing is unsupported");
    Require(
        specforge::D3D11PresentSyncInterval(
            specforge::D3D11PresentMode::Immediate,
            false) == 0 &&
            specforge::D3D11PresentSyncInterval(
                specforge::D3D11PresentMode::Immediate,
                true) == 0,
        "uncapped presentation should submit without waiting for DXGI vblank");
    Require(
        specforge::D3D11PresentFlags(specforge::D3D11PresentMode::DisplayVSync, true) == 0,
        "normal presentation should retain synchronized, tear-free semantics");
    Require(
        specforge::D3D11PresentFlags(specforge::D3D11PresentMode::CompositorClock, false) == 0,
        "compositor-clock presentation should fall back cleanly without tearing support");
    Require(
        specforge::D3D11PresentFlags(specforge::D3D11PresentMode::CompositorClock, true) ==
            DXGI_PRESENT_ALLOW_TEARING,
        "boosted presentation should opt into variable-refresh delivery when supported");
    Require(
        specforge::D3D11PresentFlags(specforge::D3D11PresentMode::Immediate, false) == 0 &&
            specforge::D3D11PresentFlags(specforge::D3D11PresentMode::Immediate, true) ==
                DXGI_PRESENT_ALLOW_TEARING,
        "uncapped presentation should use tearing only when the adapter supports it");
}

void TestDisplayRefreshDurationPolicy()
{
    Require(
        specforge::PreferredPresentDuration(120, 1) == 83'333,
        "120 Hz should request an 8.3333 ms presentation duration");
    Require(
        specforge::PreferredPresentDuration(60, 1) == 166'667,
        "60 Hz should request a 16.6667 ms presentation duration");
    Require(
        specforge::PreferredPresentDuration(0, 1) == 0 &&
            specforge::PreferredPresentDuration(120, 0) == 0,
        "invalid refresh rationals should not create a duration request");
    Require(
        specforge::PreferredPresentTolerance(83'333) == 1'000,
        "high refresh durations should retain a practical minimum tolerance");
    Require(
        specforge::PreferredPresentTolerance(166'667) == 1'667,
        "longer durations should use the one-percent tolerance policy");
}

void TestWindowPresentationLifecycleAndDeterministicFallback()
{
    namespace trace = specforge::presentation_trace;
    std::vector<trace::Event> trace_events;
    trace_events.reserve(256);
    trace::context = &trace_events;
    trace::enabled = [](void*) { return true; };
    trace::callback = [](void* context, const trace::Event& event) {
        static_cast<std::vector<trace::Event>*>(context)->push_back(event);
    };
    struct TraceCleanup {
        ~TraceCleanup() { trace::callback = nullptr; trace::enabled = nullptr; trace::context = nullptr; }
    } trace_cleanup;
    SwapChainTestWindow window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(
        SUCCEEDED(CreateTestDevice(device, context)),
        "the presentation adapter test should create a BGRA-capable D3D11 device");
    ComPtr<IDXGIFactory2> factory = GetTestFactory(device.Get());

    specforge::D3D11WindowPresentation presentation;
    Require(
        SUCCEEDED(presentation.Initialize(
            factory.Get(),
            device.Get(),
            context.Get(),
            window.hwnd(),
            320,
            240)),
        "the per-window presentation adapter should initialize composition or its DXGI fallback");
    Require(
        presentation.backend() != specforge::D3D11PresentationBackend::None,
        "successful initialization should select a concrete presentation backend");
    const specforge::D3D11PresentationTransition initial_transition =
        presentation.TakeTransition();
    Require(
        initial_transition.previous_backend ==
                specforge::D3D11PresentationBackend::None &&
            initial_transition.current_backend ==
                presentation.backend(),
        "initialization should expose the selected backend as a transition");
    if (presentation.backend() ==
        specforge::D3D11PresentationBackend::Dxgi) {
        Require(
            FAILED(initial_transition.reason) &&
                !initial_transition.operation.empty(),
            "automatic DXGI fallback should retain the Composition failure reason");
    } else {
        Require(
            presentation.backend() ==
                    specforge::D3D11PresentationBackend::Composition &&
                SUCCEEDED(initial_transition.reason),
            "successful Composition initialization should be observable");
    }

    constexpr float clear_color[4] = {0.08f, 0.09f, 0.10f, 1.0f};
    Require(
        SUCCEEDED(presentation.BeginFrame(clear_color)),
        "the selected backend should acquire and bind a render target");
    Require(
        SUCCEEDED(presentation.Present(specforge::D3D11PresentMode::DisplayVSync)),
        "the selected backend should submit an ordinary tear-free frame");
    Require(
        SUCCEEDED(presentation.Resize(640, 360)),
        "the selected backend should rebuild its buffers on resize");
    Require(
        SUCCEEDED(presentation.BeginFrame(clear_color)) &&
            SUCCEEDED(presentation.Present(specforge::D3D11PresentMode::CompositorClock)),
        "the selected backend should present after resize under compositor pacing");
    Require(
        SUCCEEDED(presentation.RefreshTarget()),
        "the selected backend should refresh its per-monitor duration policy");
    const bool composition_selected = presentation.backend() == specforge::D3D11PresentationBackend::Composition;
    (void)presentation.TakeCompositionFeedback();
    presentation.Shutdown();

    bool resize_seen = false, present_seen = false, rebuild_seen = false;
    bool feedback_seen = false, release_seen = false, poll_seen = false;
    for (const auto& event : trace_events) {
        if (event.phase != "end") continue;
        if (event.name == "viewport_resize") {
            Require(event.window.hwnd == reinterpret_cast<std::uintptr_t>(window.hwnd()) &&
                event.window.lifetime != 0 && event.window.width == 320 && event.new_width == 640 &&
                event.result >= 0 && event.duration_ms >= 0, "real resize telemetry must preserve dimensions and result");
            resize_seen = true;
        }
        if (event.name == "viewport_present") {
            Require(event.window.lifetime != 0 && event.operation != 0 && event.result >= 0,
                "real per-viewport Present must complete with identity");
            present_seen = true;
        }
        if (event.name == "presentation_buffer_rebuild" && event.parent != 0) rebuild_seen = true;
        if (event.name == "presentation_feedback_collect") {
            Require(event.window.hwnd == reinterpret_cast<std::uintptr_t>(window.hwnd()) &&
                event.window.lifetime != 0, "feedback must retain viewport identity outside resize");
            feedback_seen = true;
        }
        if (event.name == "presentation_statistics_poll") {
            Require(event.parent != 0 && event.timeout_ms == 0 && event.result_valid,
                "statistics polling must expose the zero-timeout wait result");
            poll_seen = true;
        }
        if (event.name == "presentation_resource_release") {
            Require(event.window.hwnd == reinterpret_cast<std::uintptr_t>(window.hwnd()) &&
                event.window.lifetime != 0 && event.parent != 0 &&
                event.buffer_slot >= 0 && event.buffer_slot < 3 &&
                (event.resource_kind == "available_event" || event.resource_kind == "presentation_buffer" ||
                 event.resource_kind == "render_target_view" || event.resource_kind == "texture"),
                "real resource releases must retain viewport, slot and resource identity");
            release_seen = true;
        }
    }
    Require(resize_seen && present_seen && rebuild_seen, "production renderer trace coverage missing");
    Require(feedback_seen && (!composition_selected || (release_seen && poll_seen)),
        "feedback and Composition release trace coverage missing");

    Require(
        SUCCEEDED(presentation.Initialize(
            factory.Get(),
            device.Get(),
            context.Get(),
            window.hwnd(),
            320,
            240,
            specforge::D3D11CompositionPolicy::Disabled)),
        "the adapter should retain a deterministic DXGI fallback path");
    Require(
        presentation.backend() == specforge::D3D11PresentationBackend::Dxgi,
        "disabling composition should select DXGI explicitly");
    const specforge::D3D11PresentationTransition fallback_transition =
        presentation.TakeTransition();
    Require(
        fallback_transition.current_backend ==
                specforge::D3D11PresentationBackend::Dxgi &&
            fallback_transition.operation ==
                "composition disabled by presentation options",
        "the deterministic fallback should remain observable");
    Require(
        SUCCEEDED(presentation.BeginFrame(clear_color)) &&
            SUCCEEDED(presentation.Present(specforge::D3D11PresentMode::Immediate)),
        "the deterministic DXGI fallback should render and present");
}

void TestIncrementalBufferPlanAndFailure()
{
    using namespace specforge;
    std::array<IncrementalBufferSlot, 3> slots{{{320,240,1},{320,240,2},{320,240,3}}};
    Require(BufferPixelBytes(~0U, ~0U) == 0 && BufferPixelBytes(0, 1) == 0,
        "invalid and overflowing dimensions must fail before allocation");
    auto plan = PlanIncrementalBuffer(slots, {true,true,true}, 0, 640, 480);
    Require(plan.action == BufferPlanAction::Replace && plan.slot == 1 &&
        plan.peak_bytes == 3ULL * 320 * 240 * 4 + 640ULL * 480 * 4,
        "replacement must exclude bound slot and include temporary memory");
    Require(PlanIncrementalBuffer(slots, {true,false,false}, 0, 640, 480).action == BufferPlanAction::Skip,
        "only a bound or unavailable candidate must skip");
    Require(PlanIncrementalBuffer(slots, {false,true,false}, 0, 320, 240).action == BufferPlanAction::Select,
        "an exact-size available buffer must be reused");
    auto large = slots;
    for (auto& slot : large) slot = {4096,4096,1};
    Require(PlanIncrementalBuffer(large, {true,true,true}, -1, 4097,4096).action == BufferPlanAction::BudgetExceeded,
        "budget includes installed buffers until replacement commits");
    Require(PlanIncrementalBuffer(slots, {true,true,true}, -1, 640,480).slot == 0,
        "another viewport's planning must remain independent");

    SwapChainTestWindow window(WS_EX_NOREDIRECTIONBITMAP);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "incremental test requires device");
    Win32DisplayRefreshState refresh;
    Require(SUCCEEDED(QueryWin32DisplayRefreshState(window.hwnd(), refresh)), "incremental test requires display timing");
    D3D11CompositionSwapChain chain;
    const HRESULT initialized = chain.Initialize(device.Get(), window.hwnd(), 320,240,refresh,true);
    if (FAILED(initialized)) {
        std::printf("[SKIP] incremental Composition unavailable: 0x%08lx\n", static_cast<unsigned long>(initialized));
        return;
    }
    const auto original = D3D11CompositionSwapChainTestAccess::Generations(chain);
    constexpr float color[4] = {0.1f,0.1f,0.1f,1};
    for (unsigned step = 1; step <= 4; ++step) {
        D3D11CompositionSwapChainTestAccess::Inject(chain, step);
        Require(SUCCEEDED(chain.Resize(device.Get(), context.Get(), 640,360)), "resize only records request");
        Require(chain.BeginFrame(context.Get(), color, true, 0, step) == E_FAIL,
            "each partial allocation failure must retain its HRESULT");
        Require(D3D11CompositionSwapChainTestAccess::Generations(chain) == original,
            "partial allocation must leave all old generations installed");
    }
    D3D11CompositionSwapChainTestAccess::Inject(chain, 0);
    Require(SUCCEEDED(chain.Resize(device.Get(), context.Get(), 500,300)) &&
        SUCCEEDED(chain.Resize(device.Get(), context.Get(), 640,360)) &&
        SUCCEEDED(chain.BeginFrame(context.Get(), color, true,0,10)), "latest request must acquire");
    D3D11_TEXTURE2D_DESC desc{};
    chain.active_render_texture()->GetDesc(&desc);
    Require(desc.Width == 640 && desc.Height == 360, "render target must match latest dimensions");
    const auto replaced = D3D11CompositionSwapChainTestAccess::Generations(chain);
    unsigned changed = 0;
    for (int i = 0; i < 3; ++i) changed += original[i] != replaced[i];
    Require(changed == 1 && SUCCEEDED(chain.Present(context.Get())), "one slot replacement must present");
    Require(SUCCEEDED(chain.Resize(device.Get(),context.Get(),641,360)) &&
        chain.BeginFrame(context.Get(),color,true,0,10) == DXGI_ERROR_WAS_STILL_DRAWING,
        "a second replacement in the same frame must skip");
    chain.Shutdown();
    Require(SUCCEEDED(chain.Initialize(device.Get(),window.hwnd(),320,240,refresh,true)) &&
        SUCCEEDED(chain.Resize(device.Get(),context.Get(),640,360)) &&
        SUCCEEDED(chain.BeginFrame(context.Get(),color,true,0,10)), "recreated lifetime must not inherit frame suppression");
    chain.Shutdown();

    auto factory = GetTestFactory(device.Get());
    D3D11WindowPresentation presentation;
    Require(SUCCEEDED(presentation.Initialize(factory.Get(),device.Get(),context.Get(),window.hwnd(),320,240,
        D3D11CompositionPolicy::Prefer,true)), "experimental adapter initializes");
    D3D11CompositionSwapChainTestAccess::Inject(presentation,3);
    Require(SUCCEEDED(presentation.Resize(640,360)) && SUCCEEDED(presentation.BeginFrame(color,true,0,11)) &&
        presentation.backend() == D3D11PresentationBackend::Dxgi,
        "replacement failure must use the existing DXGI fallback");
}

void TestIncrementalBufferRepeatedLifetimes()
{
    using namespace specforge;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "repeated resize requires D3D11");
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)))
        adapter.As(&adapter3);
    // Observations, not exact reclamation assertions: DWM/driver retirement is asynchronous.
    const auto sample = [&](unsigned cycle) {
        DWORD handles = 0;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        Require(GetProcessHandleCount(GetCurrentProcess(), &handles) &&
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)),
            "resource observations must be available");
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
        const bool gpu = adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal));
        std::printf("[incremental resources] cycle=%u handles=%lu private_bytes=%llu gpu_available=%d local_bytes=%llu nonlocal_bytes=%llu\n",
            cycle, handles, static_cast<unsigned long long>(memory.PrivateUsage), gpu,
            static_cast<unsigned long long>(local.CurrentUsage), static_cast<unsigned long long>(nonlocal.CurrentUsage));
    };
    constexpr float color[4] = {0.1f, 0.1f, 0.1f, 1};
    constexpr std::array<std::array<UINT, 2>, 8> sizes{{
        {640,360}, {320,240}, {1280,720}, {480,800}, {320,240}, {960,540}, {640,360}, {320,240}}};
    const ULONGLONG deadline = GetTickCount64() + 10'000;
    unsigned submitted = 0;
    for (unsigned cycle = 0; cycle < 24; ++cycle) {
        {
            // Two simultaneous ordinary HWNDs, destroyed and recreated each cycle.
            SwapChainTestWindow first, second;
            D3D11CompositionSwapChain chains[2];
            const HWND windows[] = {first.hwnd(), second.hwnd()};
            for (unsigned viewport = 0; viewport < 2; ++viewport) {
                Win32DisplayRefreshState refresh;
                Require(SUCCEEDED(QueryWin32DisplayRefreshState(windows[viewport], refresh)), "display timing must be available");
                const HRESULT result = chains[viewport].Initialize(device.Get(), windows[viewport], 320,240,refresh,true);
                if (cycle == 0 && viewport == 0 && FAILED(result)) {
                    std::printf("[SKIP] repeated incremental Composition unavailable: 0x%08lx\n", static_cast<unsigned long>(result));
                    return;
                }
                Require(SUCCEEDED(result), "both ordinary viewports must initialize on every lifetime");
            }
            for (unsigned frame = 0; frame < sizes.size(); ++frame) {
                for (unsigned viewport = 0; viewport < 2; ++viewport) {
                    auto& chain = chains[viewport];
                    auto& other = chains[1 - viewport];
                    const auto other_generations = D3D11CompositionSwapChainTestAccess::Generations(other);
                    const auto size = sizes[(frame + viewport * 3) % sizes.size()];
                    Require(SUCCEEDED(chain.Resize(device.Get(), context.Get(), size[0], size[1])), "oscillating resize must succeed");
                    HRESULT acquired;
                    do {
                        Require(GetTickCount64() < deadline, "bounded resize workload must make progress within ten seconds");
                        acquired = chain.BeginFrame(context.Get(), color, true, 0, frame + 1);
                        if (acquired == DXGI_ERROR_WAS_STILL_DRAWING) Sleep(1);
                    } while (acquired == DXGI_ERROR_WAS_STILL_DRAWING);
                    Require(SUCCEEDED(acquired), "each viewport must acquire without fallback");
                    D3D11_TEXTURE2D_DESC desc{};
                    chain.active_render_texture()->GetDesc(&desc);
                    Require(desc.Width == size[0] && desc.Height == size[1], "oscillation must render at the latest size");
                    Require(SUCCEEDED(chain.Present(context.Get())), "each oscillating viewport must present");
                    ++submitted;
                    Require(D3D11CompositionSwapChainTestAccess::Generations(other) == other_generations,
                        "one viewport's replacement must not mutate another viewport");
                }
            }
            context->ClearState();
            for (auto& chain : chains) {
                chain.Shutdown();
                Require(D3D11CompositionSwapChainTestAccess::Released(chain), "shutdown must clear owned buffers, handles and selection");
            }
            context->Flush();
        }
        Sleep(100);
        sample(cycle + 1);
    }
    Require(submitted == 384, "both viewports must complete all bounded resize submissions");
}
void TestCreationTimeNoRedirectionCompatibility()
{
    // Isolated HWND experiment only: production HWND creation is unchanged.
    for (auto policy : {specforge::D3D11CompositionPolicy::Prefer, specforge::D3D11CompositionPolicy::Disabled}) {
        SwapChainTestWindow window(WS_EX_NOREDIRECTIONBITMAP);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Require(SUCCEEDED(CreateTestDevice(device, context)), "creation-time experiment requires D3D11");
        auto factory = GetTestFactory(device.Get());
        specforge::D3D11WindowPresentation presentation;
        Require(SUCCEEDED(presentation.Initialize(factory.Get(), device.Get(), context.Get(), window.hwnd(), 320, 240, policy)),
            "creation-time no-redirection HWND should support presentation initialization");
        std::printf("[redirection probe] backend=%s policy=%s\n",
            specforge::D3D11PresentationBackendName(presentation.backend()),
            policy == specforge::D3D11CompositionPolicy::Prefer ? "prefer" : "disabled");
        if (policy == specforge::D3D11CompositionPolicy::Disabled)
            Require(presentation.backend() == specforge::D3D11PresentationBackend::Dxgi, "probe must exercise deterministic DXGI");
        constexpr float color[4] = {0.08f, 0.09f, 0.1f, 1.0f};
        for (UINT size : {360U, 400U, 320U}) {
            Require(SetWindowPos(window.hwnd(), nullptr, 0, 0, size, 240,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "probe native resize must succeed");
            Require(SUCCEEDED(presentation.Resize(size, 240)) && SUCCEEDED(presentation.BeginFrame(color)) &&
                SUCCEEDED(presentation.Present(specforge::D3D11PresentMode::DisplayVSync)),
                "probe must resize, acquire and present with its creation-time style");
            Require((GetWindowLongPtrW(window.hwnd(), GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0,
                "creation-time experiment bit must persist through resize and Present");
        }
        presentation.Shutdown();
    }
}

void TestInvalidArgumentsPreserveDiagnosticStage()
{
    specforge::D3D11SdrSwapChain swap_chain;
    const HRESULT result = swap_chain.Initialize(nullptr, nullptr, nullptr);

    Require(result == E_INVALIDARG, "invalid initialization arguments should return E_INVALIDARG");
    Require(
        swap_chain.last_error_operation() == "D3D11SdrSwapChain::Initialize arguments",
        "swap-chain failures should preserve the failing operation");
}

void TestRealSwapChainInitializationColorSpaceAndResize()
{
    SwapChainTestWindow window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "the integration test should create a D3D11 device");

    ComPtr<IDXGIFactory2> factory = GetTestFactory(device.Get());
    const bool tearing_supported = specforge::DxgiFactorySupportsTearing(factory.Get());

    specforge::D3D11SdrSwapChain swap_chain;
    Require(
        SUCCEEDED(swap_chain.Initialize(factory.Get(), device.Get(), window.hwnd(), 320, 240)),
        "a real SDR flip-model swap chain should initialize");

    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
    Require(
        swap_chain.GetConfiguredColorSpace(color_space) && color_space == specforge::kSdrSwapChainColorSpace,
        "a real swap chain should record the explicit P709 SDR color space");

    constexpr float clear_color[4] = {0.08f, 0.09f, 0.10f, 1.0f};
    swap_chain.Bind(context.Get());
    swap_chain.Clear(context.Get(), clear_color);
    Require(
        SUCCEEDED(swap_chain.Resize(device.Get(), context.Get(), 640, 360)),
        "resize should explicitly unbind a bound back buffer instead of depending on Present");

    DXGI_SWAP_CHAIN_DESC resized_desc = {};
    Require(swap_chain.GetDesc(resized_desc), "the resized swap chain should expose its descriptor");
    Require(
        swap_chain.tearing_supported() == tearing_supported,
        "the swap chain should retain the factory's tearing capability");
    Require(
        ((resized_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) == tearing_supported,
        "resize should preserve the creation-time tearing flag");
    Require(
        resized_desc.BufferDesc.Width == 640 && resized_desc.BufferDesc.Height == 360,
        "resize should update the swap-chain dimensions");
    Require(
        swap_chain.GetConfiguredColorSpace(color_space) && color_space == specforge::kSdrSwapChainColorSpace,
        "resize should reapply the explicit P709 SDR color space");
}

void TestRendererDebugLayerRequestFallsBackAndReportsAvailability()
{
    SwapChainTestWindow window;
    specforge::D3D11Renderer renderer;
    Require(
        SUCCEEDED(renderer.Initialize(
            window.hwnd(),
            specforge::D3D11CompositionPolicy::Disabled,
            true)),
        "requesting live-object diagnostics should fall back to the ordinary device when Graphics Tools are unavailable");
    renderer.Shutdown();

    const specforge::D3D11LiveObjectReport& report =
        renderer.live_object_report();
    Require(
        report.requested,
        "the renderer should retain that live-object diagnostics were requested");
    Require(
        report.available || !report.detail.empty(),
        "the renderer should either produce a report or explain why the debug layer was unavailable");
    if (report.available) {
        Require(
            report.unexpected_live_object_messages == 0,
            "a clean renderer shutdown should not retain unexpected D3D11 objects");
    }
}

void TestRendererCapturesOnlyTheActiveDxgiFrameToValidPng()
{
    SwapChainTestWindow window;
    TemporaryDirectory temporary;
    specforge::D3D11Renderer renderer;
    Require(
        SUCCEEDED(renderer.Initialize(
            window.hwnd(),
            specforge::D3D11CompositionPolicy::
                Disabled)),
        "the frame-capture test should initialize the DXGI renderer");

    const std::filesystem::path before_frame =
        temporary.path() / L"before-frame.png";
    Require(
        FAILED(renderer.CaptureFrameToPng(
            before_frame)) &&
            !std::filesystem::exists(before_frame),
        "capture before BeginFrame should fail without producing an image");

    constexpr std::array<float, 4> clear_color = {
        0.25f,
        0.50f,
        0.75f,
        1.0f,
    };
    Require(
        SUCCEEDED(renderer.BeginFrame(clear_color)),
        "the capture test should begin and clear a real frame");

    const std::filesystem::path dangling_output =
        temporary.path() / L"dangling.png";
    const std::filesystem::path dangling_temporary =
        std::filesystem::path(
            dangling_output.wstring() + L".tmp");
    const std::filesystem::path escaped_output =
        temporary.path().parent_path() /
        (temporary.path().filename().wstring() +
         L"-escaped.png");
    std::filesystem::remove(escaped_output);
    const BOOL symlink_created =
        CreateSymbolicLinkW(
            dangling_temporary.c_str(),
            escaped_output.c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    const DWORD symlink_error = GetLastError();
    Require(
        symlink_created != FALSE ||
            symlink_error ==
                ERROR_PRIVILEGE_NOT_HELD,
        "the capture security regression should create a dangling temporary-file symlink or identify an unavailable symlink privilege");
    constexpr std::string_view
        kLegacySentinel =
            "legacy-fixed-temp-sentinel";
    std::optional<TestFileIdentity>
        sentinel_identity_before;
    if (symlink_created == FALSE) {
        const HANDLE temporary_sentinel =
            CreateFileW(
                dangling_temporary.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
        Require(
            temporary_sentinel !=
                INVALID_HANDLE_VALUE,
            "the no-symlink fallback should reserve the legacy fixed temporary-file name");
        DWORD written = 0;
        Require(
            WriteFile(
                temporary_sentinel,
                kLegacySentinel.data(),
                static_cast<DWORD>(
                    kLegacySentinel.size()),
                &written,
                nullptr) != FALSE &&
                written ==
                    kLegacySentinel.size(),
            "the no-symlink fallback should write an identity-bearing legacy temporary sentinel");
        CloseHandle(temporary_sentinel);
        sentinel_identity_before =
            FileIdentity(
                dangling_temporary);
        Require(
            sentinel_identity_before.has_value(),
            "the fallback sentinel identity should be observable before capture");
    }
    const HRESULT dangling_result =
        renderer.CaptureFrameToPng(
            dangling_output,
            {},
            temporary.path());
    const bool dangling_output_created =
        std::filesystem::exists(
            dangling_output);
    const bool escaped_output_created =
        std::filesystem::exists(
            escaped_output);
    const bool legacy_entry_preserved =
        symlink_created != FALSE
        ? IsReparseEntry(
              dangling_temporary)
        : FileIdentity(
              dangling_temporary) ==
                  sentinel_identity_before &&
              ReadFileBytes(
                  dangling_temporary) ==
                  kLegacySentinel;
    const bool no_dangling_capture_temporary =
        CaptureTemporaryArtifacts(
            temporary.path())
            .empty();
    Require(
        legacy_entry_preserved,
        "capture must preserve the dangling symlink or the identity and contents of the no-symlink fallback sentinel at the legacy fixed temporary name");
    (void)DeleteFileW(
        dangling_temporary.c_str());
    std::filesystem::remove(escaped_output);
    Require(
        SUCCEEDED(dangling_result) &&
            dangling_output_created &&
            !escaped_output_created &&
            no_dangling_capture_temporary,
        "capture must publish only through a random handle-bound file inside the allowed root without leaking a temporary artifact");

    const std::filesystem::path relocation_parent =
        temporary.path() / L"relocation-parent";
    const std::filesystem::path relocated_parent =
        temporary.path().parent_path() /
        (temporary.path().filename().wstring() +
         L"-relocated-parent");
    std::filesystem::create_directory(
        relocation_parent);
    std::filesystem::remove_all(
        relocated_parent);
    const std::filesystem::path relocation_output =
        relocation_parent / L"race.png";
    bool parent_relocated = false;
    const HRESULT relocation_result =
        renderer.CaptureFrameToPng(
            relocation_output,
            [&](const std::function<HRESULT()>&
                    publish) {
                parent_relocated =
                    MoveFileW(
                        relocation_parent.c_str(),
                        relocated_parent.c_str()) !=
                    FALSE;
                return publish();
            },
            temporary.path());
    const bool relocated_output_created =
        std::filesystem::exists(
            relocated_parent / L"race.png");
    const bool in_root_output_created =
        std::filesystem::exists(
            relocation_output);
    if (parent_relocated) {
        (void)MoveFileW(
            relocated_parent.c_str(),
            relocation_parent.c_str());
    }
    Require(
        (parent_relocated &&
         FAILED(relocation_result) &&
         !relocated_output_created &&
         !in_root_output_created) ||
            (!parent_relocated &&
             SUCCEEDED(relocation_result) &&
             !relocated_output_created &&
             in_root_output_created),
        "capture publication must either deny parent relocation through its live file handle or detect the moved pinned directory and delete the temporary file");

    const std::filesystem::path canceled =
        temporary.path() / L"canceled.png";
    Require(
        FAILED(renderer.CaptureFrameToPng(
            canceled,
            [](const std::function<HRESULT()>&) {
                return HRESULT_FROM_WIN32(
                    ERROR_CANCELLED);
            })) &&
            !std::filesystem::exists(canceled) &&
            !std::filesystem::exists(
                canceled.wstring() + L".tmp") &&
            CaptureTemporaryArtifacts(
                temporary.path())
                .empty(),
        "a final request-cancellation checkpoint should remove the encoded temporary PNG without publishing output");

    const std::filesystem::path nested_capture =
        temporary.path() / L"new" / L"nested" /
        L"captured.png";
    Require(
        SUCCEEDED(renderer.CaptureFrameToPng(
            nested_capture,
            {},
            temporary.path())) &&
            std::filesystem::is_regular_file(
                nested_capture) &&
            CaptureTemporaryArtifacts(
                nested_capture.parent_path())
                .empty(),
        "an allowed-root capture should create missing parent components handle-relatively without following reparse points");

    const std::filesystem::path captured =
        temporary.path() / L"captured.png";
    Require(
        SUCCEEDED(renderer.CaptureFrameToPng(
            captured)) &&
            std::filesystem::is_regular_file(captured),
        "capture during the active pre-Present frame should produce a PNG");

    RequirePngMatchesClientAreaAndPixel(
        window.hwnd(),
        captured,
        {64, 128, 191, 255});
    const auto captured_size =
        std::filesystem::file_size(captured);
    Require(
        FAILED(renderer.CaptureFrameToPng(
            captured)) &&
            std::filesystem::file_size(
                captured) == captured_size &&
            CaptureTemporaryArtifacts(
                temporary.path())
                .empty(),
        "frame capture publication must not replace an existing PNG");

    Require(
        SUCCEEDED(renderer.Present(
            specforge::D3D11PresentMode::
                Immediate)),
        "the captured frame should remain presentable");
    const std::filesystem::path after_present =
        temporary.path() / L"after-present.png";
    Require(
        FAILED(renderer.CaptureFrameToPng(
            after_present)) &&
            !std::filesystem::exists(after_present),
        "capture after Present should reject stale frame contents without producing an image");
}

void TestRendererCapturesDefaultCompositionFrameWhenSelected()
{
    SwapChainTestWindow window;
    TemporaryDirectory temporary;
    specforge::D3D11Renderer renderer;
    Require(
        SUCCEEDED(renderer.Initialize(window.hwnd())),
        "the default renderer should initialize");
    const specforge::D3D11PresentationTransition transition =
        renderer.TakePresentationTransition();
    Require(
        transition.previous_backend ==
                specforge::D3D11PresentationBackend::None &&
            transition.current_backend ==
                renderer.presentation_backend(),
        "default renderer initialization should expose the selected backend as a transition");
    if (renderer.presentation_backend() ==
        specforge::D3D11PresentationBackend::Dxgi) {
        Require(
            FAILED(transition.reason) &&
                !transition.operation.empty(),
            "Composition fallback should retain its diagnostic");
        std::fprintf(
            stderr,
            "[SKIP] TestRendererCapturesDefaultCompositionFrameWhenSelected: Composition capture unavailable; reason=0x%08lx operation=%.*s\n",
            static_cast<unsigned long>(transition.reason),
            static_cast<int>(transition.operation.size()),
            transition.operation.data());
        return;
    }
    Require(
        renderer.presentation_backend() ==
            specforge::D3D11PresentationBackend::
                Composition &&
            SUCCEEDED(transition.reason),
        "successful Composition initialization should be observable");

    constexpr std::array<float, 4> clear_color = {
        0.75f,
        0.25f,
        0.50f,
        1.0f,
    };
    Require(
        SUCCEEDED(renderer.BeginFrame(clear_color)),
        "the Composition capture test should begin and clear a real frame");

    const std::filesystem::path captured =
        temporary.path() / L"composition.png";
    Require(
        SUCCEEDED(renderer.CaptureFrameToPng(
            captured)) &&
            std::filesystem::is_regular_file(captured),
        "the default Composition active texture should be capturable before Present");
    RequirePngMatchesClientAreaAndPixel(
        window.hwnd(),
        captured,
        {191, 64, 128, 255});
    Require(
        SUCCEEDED(renderer.Present()),
        "the captured Composition frame should remain presentable");
}

void TestImGuiViewportSwapChainLifecycle()
{
    namespace trace = specforge::presentation_trace;
    std::vector<trace::Event> events;
    events.reserve(256);
    trace::context = &events;
    trace::enabled = [](void*) { return true; };
    trace::callback = [](void* context, const trace::Event& event) {
        static_cast<std::vector<trace::Event>*>(context)->push_back(event);
    };
    struct TraceCleanup {
        ~TraceCleanup() { trace::callback = nullptr; trace::enabled = nullptr; trace::context = nullptr; }
    } trace_cleanup;
    SwapChainTestWindow window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "the viewport test should create a D3D11 device");
    ComPtr<IDXGIFactory2> factory = GetTestFactory(device.Get());

    ImGuiViewportTestFixture fixture(factory.Get(), device.Get(), context.Get(), window.hwnd());
    Require(
        fixture.renderer_initialized(),
        "the custom ImGui viewport renderer should initialize");

    fixture.CreateViewport();
    Require(fixture.has_viewport_swap_chain(), "detaching should create a viewport swap chain");
    Require(SUCCEEDED(fixture.TakeLastError().result), "viewport creation should not record a DXGI error");
    const std::vector<specforge::D3D11ViewportPresentationUpdate>
        initial_updates = fixture.TakePresentationUpdates();
    Require(
        initial_updates.size() == 1 &&
            initial_updates[0].viewport_id == 73 &&
            initial_updates[0].backend !=
                specforge::D3D11PresentationBackend::None &&
            initial_updates[0].transition.current_backend ==
                initial_updates[0].backend,
        "viewport creation should report its selected presentation backend");
    const bool composition_selected =
        initial_updates[0].backend ==
        specforge::D3D11PresentationBackend::Composition;

    fixture.ResizeViewport(ImVec2(640.0f, 360.0f));
    Require(SUCCEEDED(fixture.TakeLastError().result), "detached viewport resize should succeed");

    fixture.PresentViewport();
    Require(SUCCEEDED(fixture.TakeLastError().result), "detached viewport present should succeed");
    const std::vector<specforge::D3D11ViewportPresentCompletion> ordinary_presentations =
        fixture.TakePresentCompletions();
    Require(
        (composition_selected
             ? ordinary_presentations.size() == 1
             : ordinary_presentations.size() <= 1) &&
            (ordinary_presentations.empty() ||
             (ordinary_presentations[0].viewport_id == 73 &&
              ordinary_presentations[0]
                      .completed_at.time_since_epoch()
                      .count() > 0)),
        "a completed detached viewport Present should publish exactly one valid completion while hidden DXGI occlusion may publish none");

    fixture.SetCompositorClockPaced(true);
    fixture.PresentViewport();
    Require(
        SUCCEEDED(fixture.TakeLastError().result),
        "detached viewport compositor-clock present should use supported DXGI flags");
    const std::vector<specforge::D3D11ViewportPresentCompletion>
        paced_presentations = fixture.TakePresentCompletions();
    Require(
        (composition_selected
             ? paced_presentations.size() == 1
             : paced_presentations.size() <= 1) &&
            (paced_presentations.empty() ||
             (paced_presentations[0].viewport_id == 73 &&
              paced_presentations[0]
                      .completed_at.time_since_epoch()
                      .count() > 0)),
        "each completed compositor-clock Present should publish exactly one valid completion while hidden DXGI occlusion may publish none");

    fixture.DestroyViewport();
    Require(!fixture.has_viewport_swap_chain(), "redocking should destroy the viewport swap chain");
    unsigned present_completions = 0;
    bool resize_seen = false, wait_seen = false, draw_seen = false;
    unsigned replacements = 0, rebuilds = 0;
    for (const auto& event : events) {
        if (event.phase != "end") continue;
        if (event.name == "viewport_resize") {
            Require(event.window.lifetime != 0 && event.new_width == 640 && event.new_height == 360,
                "detached resize must carry the presentation lifetime and dimensions");
            resize_seen = true;
        }
        if (event.name == "viewport_present" && event.result_valid && event.result == S_OK) ++present_completions;
        if (event.name == "viewport_draw_submission") {
            Require(event.window.lifetime != 0 && !event.result_valid,
                "real detached draw timing must retain identity without inventing an API result");
            draw_seen = true;
        }
        if (event.name == "presentation_available_wait") {
            Require(event.timeout_ms == 0 && event.count == 1 && event.parent != 0,
                "production detached acquisition must poll individual slots without blocking");
            wait_seen = true;
        }
        if (event.name == "presentation_buffer_rebuild") ++rebuilds;
        if (event.name == "presentation_buffer_replace") {
            Require(event.result_valid && event.result == S_OK &&
                event.new_width == 640 && event.new_height == 360 &&
                event.logical_bytes <= specforge::kIncrementalBufferBudget,
                "production replacement must use the requested dimensions within budget");
            ++replacements;
        }
    }
    Require(resize_seen && draw_seen && present_completions == ordinary_presentations.size() + paced_presentations.size(),
        "telemetry completions must match actual detached completion policy");
    Require(!composition_selected || wait_seen, "composition acquire must emit actual available-event waits");
    Require(!composition_selected || (rebuilds == 1 && replacements >= 1 && replacements <= 2),
        "two production frames must replace at most one slot each instead of rebuilding the initial buffer set");

    // Exercise the actual detached callbacks, including a recreated lifetime.
    fixture.SetFeedbackAcquireOnly(true);
    for (int lifetime = 0; lifetime < 2; ++lifetime) {
        events.clear();
        fixture.CreateViewport();
        fixture.ResizeViewport(ImVec2(640.0f, 360.0f));
        unsigned drains = 0;
        for (const auto& event : events)
            if (event.name == "presentation_statistics_drain" && event.phase == "end") ++drains;
        Require(drains == 0, "experimental creation and resize collection must not drain statistics");
        events.clear();
        fixture.PresentViewport();
        Require(SUCCEEDED(fixture.TakeLastError().result), "experimental detached presentation must succeed");
        drains = 0;
        for (const auto& event : events) {
            if (event.name != "presentation_statistics_drain" || event.phase != "end") continue;
            ++drains;
            bool acquire_parent = false;
            for (const auto& parent : events)
                if (parent.operation == event.parent && parent.name == "viewport_acquire") acquire_parent = true;
            Require(acquire_parent, "experimental drain must belong to buffer acquisition");
        }
        Require(!composition_selected || drains == 1, "experimental frame must drain exactly once");
        fixture.DestroyViewport();
    }
}

void TestPlatformTelemetryForwardsAndRestoresCallbacks()
{
    namespace trace = specforge::presentation_trace;
    struct State {
        bool recording = true;
        int positions = 0, sizes = 0;
        ImVec2 position, size;
        std::vector<trace::Event> events;
    } state;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "platform trace fixture requires a device");
    auto factory = GetTestFactory(device.Get());
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::CreateContext();
    struct Cleanup {
        ImGuiContext* previous;
        ~Cleanup() {
            trace::callback = nullptr;
            trace::enabled = nullptr;
            trace::context = nullptr;
            trace::Unregister(42);
            ImGui::DestroyContext();
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{previous};
    auto& io = ImGui::GetPlatformIO();
    const auto position_callback = +[](ImGuiViewport* viewport, ImVec2 value) {
        auto& state = *static_cast<State*>(viewport->PlatformUserData);
        ++state.positions;
        state.position = value;
    };
    const auto size_callback = +[](ImGuiViewport* viewport, ImVec2 value) {
        auto& state = *static_cast<State*>(viewport->PlatformUserData);
        ++state.sizes;
        state.size = value;
    };
    io.Platform_SetWindowPos = position_callback;
    io.Platform_SetWindowSize = size_callback;
    specforge::D3D11ImGuiViewportRenderer renderer;
    Require(renderer.Initialize(factory.Get(), device.Get(), context.Get()), "trace wrapper should initialize");
    ImGuiViewport viewport;
    struct ClearViewportUserData {
        ImGuiViewport& viewport;
        ~ClearViewportUserData() { viewport.PlatformUserData = nullptr; }
    } clear_viewport_user_data{viewport};
    viewport.PlatformHandleRaw = reinterpret_cast<void*>(42);
    viewport.PlatformUserData = &state;
    trace::Register(42, 320, 240);
    trace::context = &state;
    trace::enabled = [](void* data) { return static_cast<State*>(data)->recording; };
    trace::callback = [](void* data, const trace::Event& event) {
        static_cast<State*>(data)->events.push_back(event);
    };
    {
        trace::Span update({.name = "platform_windows_update"});
        io.Platform_SetWindowPos(&viewport, ImVec2(-10.5f, 200.25f));
        io.Platform_SetWindowSize(&viewport, ImVec2(640.5f, 480.25f));
    }
    Require(state.positions == 1 && state.sizes == 1 && state.position.x == -10.5f &&
        state.position.y == 200.25f && state.size.x == 640.5f && state.size.y == 480.25f,
        "instrumentation must forward each exact platform argument once");
    Require(state.events.size() == 10 && state.events[1].parent == state.events[0].operation &&
        state.events[3].parent == state.events[0].operation &&
        state.events[3].window.lifetime != 0 && state.events[8].phase == "end" &&
        !state.events[8].result_valid && state.events[8].new_width == 640,
        "platform callbacks must emit nested durations without invented return status");
    state.recording = false;
    io.Platform_SetWindowSize(&viewport, ImVec2(321, 123));
    Require(state.sizes == 2 && state.events.size() == 10 && state.size.x == 321,
        "disabled telemetry must preserve callbacks without events");
    renderer.Shutdown();
    Require(io.Platform_SetWindowPos == position_callback && io.Platform_SetWindowSize == size_callback,
        "shutdown must restore original platform handlers");
    Require(renderer.Initialize(factory.Get(), device.Get(), context.Get()), "wrapper should reinstall");
    Require(renderer.IncrementalBuffersEnabled(), "reinitialized renderer must retain production buffer policy");
    io.Platform_SetWindowPos = nullptr; // Another owner replaced the handler.
    renderer.Shutdown();
    Require(io.Platform_SetWindowPos == nullptr && io.Platform_SetWindowSize == size_callback,
        "shutdown must not overwrite a later handler owner");
}

void TestImGuiViewportFixtureCleansUpDuringExceptionUnwind()
{
    struct ExpectedFailure {
    };

    SwapChainTestWindow window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(CreateTestDevice(device, context)), "the exception cleanup test should create a D3D11 device");
    ComPtr<IDXGIFactory2> factory = GetTestFactory(device.Get());
    ViewportCleanupObservation cleanup;

    bool caught_expected_failure = false;
    try {
        ImGuiViewportTestFixture fixture(factory.Get(), device.Get(), context.Get(), window.hwnd(), &cleanup);
        Require(fixture.renderer_initialized(), "the exception cleanup fixture should initialize");
        fixture.CreateViewport();
        Require(fixture.has_viewport_swap_chain(), "the exception cleanup fixture should own a viewport swap chain");
        throw ExpectedFailure{};
    } catch (const ExpectedFailure&) {
        caught_expected_failure = true;
    }

    Require(caught_expected_failure, "the test should exercise exception unwinding");
    Require(cleanup.viewport_destroyed, "exception unwinding should destroy the viewport swap chain");
    Require(cleanup.callbacks_cleared, "exception unwinding should clear ImGui renderer callbacks");
    Require(cleanup.renderer_shutdown, "exception unwinding should shut down the viewport renderer");
    Require(cleanup.context_destroyed, "exception unwinding should destroy the ImGui context");

    ImGuiViewportTestFixture retry_fixture(factory.Get(), device.Get(), context.Get(), window.hwnd());
    Require(
        retry_fixture.renderer_initialized(),
        "exception cleanup should release global renderer state for a subsequent fixture");
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main()
{
    constexpr TestCase tests[] = {
        {"TestIncrementalBufferRepeatedLifetimes", TestIncrementalBufferRepeatedLifetimes},
        {"TestPlatformTelemetryForwardsAndRestoresCallbacks", TestPlatformTelemetryForwardsAndRestoresCallbacks},
        {
            "TestSdrSwapChainUsesModernSrgbPresentationContract",
            TestSdrSwapChainUsesModernSrgbPresentationContract,
        },
        {
            "TestDisplayRefreshDurationPolicy",
            TestDisplayRefreshDurationPolicy,
        },
        {
            "TestIncrementalBufferPlanAndFailure",
            TestIncrementalBufferPlanAndFailure,
        },
        {
            "TestCreationTimeNoRedirectionCompatibility",
            TestCreationTimeNoRedirectionCompatibility,
        },
        {
            "TestInvalidArgumentsPreserveDiagnosticStage",
            TestInvalidArgumentsPreserveDiagnosticStage,
        },
        {
            "TestRealSwapChainInitializationColorSpaceAndResize",
            TestRealSwapChainInitializationColorSpaceAndResize,
        },
        {
            "TestRendererDebugLayerRequestFallsBackAndReportsAvailability",
            TestRendererDebugLayerRequestFallsBackAndReportsAvailability,
        },
        {
            "TestRendererCapturesOnlyTheActiveDxgiFrameToValidPng",
            TestRendererCapturesOnlyTheActiveDxgiFrameToValidPng,
        },
        {
            "TestRendererCapturesDefaultCompositionFrameWhenSelected",
            TestRendererCapturesDefaultCompositionFrameWhenSelected,
        },
        {
            "TestWindowPresentationLifecycleAndDeterministicFallback",
            TestWindowPresentationLifecycleAndDeterministicFallback,
        },
        {
            "TestImGuiViewportSwapChainLifecycle",
            TestImGuiViewportSwapChainLifecycle,
        },
        {
            "TestImGuiViewportFixtureCleansUpDuringExceptionUnwind",
            TestImGuiViewportFixtureCleansUpDuringExceptionUnwind,
        },
    };

    for (const TestCase& test : tests) {
        try {
            test.run();
        } catch (const std::exception& error) {
            std::fprintf(
                stderr,
                "[FAIL] %s: %s\n",
                test.name,
                error.what());
            return 1;
        } catch (...) {
            std::fprintf(
                stderr,
                "[FAIL] %s: unknown exception\n",
                test.name);
            return 1;
        }
    }
    return 0;
}
