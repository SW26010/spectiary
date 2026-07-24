#include "renderer/d3d11_composition_swap_chain.h"
#include "renderer/d3d11_sdr_swap_chain.h"
#include "renderer/d3d11_imgui_viewport_renderer.h"
#include "renderer/d3d11_renderer.h"
#include "renderer/d3d11_window_presentation.h"
#include "renderer/win32_display_refresh.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    SwapChainTestWindow()
    {
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = SwapChainTestWindowProc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = kSwapChainTestWindowClass;
        atom_ = RegisterClassW(&window_class);
        Require(atom_ != 0, "the swap-chain integration test should register its window class");

        hwnd_ = CreateWindowExW(
            0,
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

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
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

bool PresentationApiSupported(ID3D11Device* device)
{
    ComPtr<IPresentationFactory> factory;
    return SUCCEEDED(CreatePresentationFactory(
               device,
               IID_PPV_ARGS(factory.GetAddressOf()))) &&
           factory->IsPresentationSupported();
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
        ImGui::GetPlatformIO().Renderer_RenderWindow(&viewport_, nullptr);
        ImGui::GetPlatformIO().Renderer_SwapBuffers(&viewport_, nullptr);
    }

    void SetCompositorClockPaced(bool paced)
    {
        renderer_.SetCompositorClockPaced(paced);
    }

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
        specforge::D3D11PresentSyncInterval(specforge::D3D11PresentMode::DisplayVSync) == 1,
        "normal event-driven presentation should remain display-vsync synchronized");
    Require(
        specforge::D3D11PresentSyncInterval(specforge::D3D11PresentMode::CompositorClock) == 0,
        "compositor-clock-paced presentation should not wait again on virtualized DXGI vblank");
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
    if (PresentationApiSupported(device.Get())) {
        Require(
            presentation.backend() ==
                specforge::D3D11PresentationBackend::Composition,
            "a supported Presentation API device should select the composition backend");
    }
    const specforge::D3D11PresentationTransition initial_transition =
        presentation.TakeTransition();
    Require(
        initial_transition.current_backend == presentation.backend(),
        "initialization should expose the selected backend as a transition");

    constexpr float clear_color[4] = {0.08f, 0.09f, 0.10f, 1.0f};
    Require(
        SUCCEEDED(presentation.BeginFrame(clear_color)),
        "the selected backend should acquire and bind a render target");
    Require(
        SUCCEEDED(presentation.Present(false)),
        "the selected backend should submit an ordinary tear-free frame");
    Require(
        SUCCEEDED(presentation.Resize(640, 360)),
        "the selected backend should rebuild its buffers on resize");
    Require(
        SUCCEEDED(presentation.BeginFrame(clear_color)) &&
            SUCCEEDED(presentation.Present(true)),
        "the selected backend should present after resize under compositor pacing");
    Require(
        SUCCEEDED(presentation.RefreshTarget()),
        "the selected backend should refresh its per-monitor duration policy");
    presentation.Shutdown();

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
            SUCCEEDED(presentation.Present(false)),
        "the deterministic DXGI fallback should render and present");
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

void TestImGuiViewportSwapChainLifecycle()
{
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

    fixture.ResizeViewport(ImVec2(640.0f, 360.0f));
    Require(SUCCEEDED(fixture.TakeLastError().result), "detached viewport resize should succeed");

    fixture.PresentViewport();
    Require(SUCCEEDED(fixture.TakeLastError().result), "detached viewport present should succeed");
    const std::vector<specforge::D3D11ViewportPresentCompletion> ordinary_presentations =
        fixture.TakePresentCompletions();
    Require(
        ordinary_presentations.size() == 1 && ordinary_presentations[0].viewport_id == 73 &&
            ordinary_presentations[0].completed_at.time_since_epoch().count() > 0,
        "a real detached viewport Present should publish its viewport identity and completion time");

    fixture.SetCompositorClockPaced(true);
    fixture.PresentViewport();
    Require(
        SUCCEEDED(fixture.TakeLastError().result),
        "detached viewport compositor-clock present should use supported DXGI flags");
    Require(
        fixture.TakePresentCompletions().size() == 1,
        "each successful detached viewport Present should publish exactly one completion");

    fixture.DestroyViewport();
    Require(!fixture.has_viewport_swap_chain(), "redocking should destroy the viewport swap chain");
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

}  // namespace

int main()
{
    TestSdrSwapChainUsesModernSrgbPresentationContract();
    TestDisplayRefreshDurationPolicy();
    TestInvalidArgumentsPreserveDiagnosticStage();
    TestRealSwapChainInitializationColorSpaceAndResize();
    TestWindowPresentationLifecycleAndDeterministicFallback();
    TestImGuiViewportSwapChainLifecycle();
    TestImGuiViewportFixtureCleansUpDuringExceptionUnwind();
    return 0;
}
