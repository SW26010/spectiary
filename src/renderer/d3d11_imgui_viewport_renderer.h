#pragma once

#include "renderer/d3d11_window_presentation.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <functional>
#include <string_view>
#include <vector>

struct ImGuiViewport;
struct ImVec2;

namespace spectiary {

struct D3D11RendererError {
    HRESULT result = S_OK;
    std::string_view operation;
};

struct D3D11ViewportPresentationUpdate {
    unsigned int viewport_id = 0;
    HWND hwnd = nullptr;
    D3D11PresentationBackend backend = D3D11PresentationBackend::None;
    D3D11PresentationDegradation degradation =
        D3D11PresentationDegradation::None;
    Win32DisplayRefreshState refresh_state;
    D3D11PresentationTransition transition;
    D3D11CompositionFeedback feedback;
};

struct D3D11ViewportPresentCompletion {
    unsigned int viewport_id = 0;
    std::chrono::steady_clock::time_point completed_at;
};

class D3D11ImGuiViewportRenderer {
public:
    D3D11ImGuiViewportRenderer() = default;
    ~D3D11ImGuiViewportRenderer();

    D3D11ImGuiViewportRenderer(const D3D11ImGuiViewportRenderer&) = delete;
    D3D11ImGuiViewportRenderer& operator=(const D3D11ImGuiViewportRenderer&) = delete;

    bool Initialize(IDXGIFactory2* factory, ID3D11Device* device, ID3D11DeviceContext* device_context);
    void Shutdown() noexcept;
    void SetCompositorClockPaced(bool paced) noexcept { compositor_clock_paced_ = paced; }
    // Diagnostic only: leave statistics draining at the existing BeginFrame boundary.
    void SetFeedbackAcquireOnlyExperiment(bool enabled) noexcept { feedback_acquire_only_ = enabled; }
    [[nodiscard]] bool FeedbackAcquireOnlyExperiment() const noexcept { return feedback_acquire_only_; }
    // Override only for isolated baseline comparisons, before creating viewports.
    void SetIncrementalBuffersForDiagnostics(bool enabled) noexcept { incremental_buffers_ = enabled; }
    [[nodiscard]] bool IncrementalBuffersEnabled() const noexcept { return incremental_buffers_; }
    void SetClearColor(
        const std::array<float, 4>& clear_color) noexcept;
    void SetNativeWindowThemeCallback(
        std::function<void(HWND)> callback);
    void RefreshNativeWindowThemes();
    void RefreshPresentationTargets();

    [[nodiscard]] D3D11RendererError TakeLastError() noexcept;
    [[nodiscard]] std::vector<D3D11ViewportPresentationUpdate>
    TakePresentationUpdates() noexcept;
    [[nodiscard]] std::vector<D3D11ViewportPresentCompletion>
    TakePresentCompletions() noexcept;

private:
    static void CreateViewportWindow(ImGuiViewport* viewport);
    static void DestroyViewportWindow(ImGuiViewport* viewport);
    static void SetViewportWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void RenderViewportWindow(ImGuiViewport* viewport, void* render_argument);
    static void SwapViewportBuffers(ImGuiViewport* viewport, void* render_argument);
    static void TracePlatformWindowPosition(ImGuiViewport* viewport, ImVec2 position);
    static void TracePlatformWindowSize(ImGuiViewport* viewport, ImVec2 size);

    void RecordFailure(HRESULT result, std::string_view operation) noexcept;
    void CollectPresentationUpdate(
        const ImGuiViewport& viewport,
        D3D11WindowPresentation& presentation);

    static D3D11ImGuiViewportRenderer* active_instance_;
    void (*platform_set_position_)(ImGuiViewport*, ImVec2) = nullptr;
    void (*platform_set_size_)(ImGuiViewport*, ImVec2) = nullptr;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    D3D11RendererError last_error_;
    std::vector<D3D11ViewportPresentationUpdate> presentation_updates_;
    std::vector<D3D11ViewportPresentCompletion> present_completions_;
    std::array<float, 4> clear_color_{0.0f, 0.0f, 0.0f, 1.0f};
    std::function<void(HWND)> native_window_theme_callback_;
    bool compositor_clock_paced_ = false;
    bool feedback_acquire_only_ = false;
    // Detached Composition policy; the main-window adapter retains its own default.
    bool incremental_buffers_ = true;
};

}  // namespace spectiary
