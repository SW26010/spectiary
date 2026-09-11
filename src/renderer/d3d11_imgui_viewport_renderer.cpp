#include "renderer/d3d11_imgui_viewport_renderer.h"
#include "profile/presentation_trace.h"
#include "platform/win32_native_size_trace.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <utility>

namespace specforge {
namespace {

struct ViewportRendererData {
    D3D11WindowPresentation presentation;
    HMONITOR monitor = nullptr;
    bool frame_acquired = false;
};

[[nodiscard]] HWND ViewportWindowHandle(const ImGuiViewport& viewport) noexcept
{
    return static_cast<HWND>(viewport.PlatformHandleRaw != nullptr ? viewport.PlatformHandleRaw : viewport.PlatformHandle);
}

}  // namespace

D3D11ImGuiViewportRenderer* D3D11ImGuiViewportRenderer::active_instance_ = nullptr;

D3D11ImGuiViewportRenderer::~D3D11ImGuiViewportRenderer()
{
    Shutdown();
}

bool D3D11ImGuiViewportRenderer::Initialize(
    IDXGIFactory2* factory,
    ID3D11Device* device,
    ID3D11DeviceContext* device_context)
{
    if (factory == nullptr || device == nullptr || device_context == nullptr || ImGui::GetCurrentContext() == nullptr ||
        active_instance_ != nullptr) {
        return false;
    }

    factory_ = factory;
    device_ = device;
    device_context_ = device_context;
    last_error_ = {};
    presentation_updates_.clear();
    present_completions_.clear();
    active_instance_ = this;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_set_position_ = platform_io.Platform_SetWindowPos;
    platform_set_size_ = platform_io.Platform_SetWindowSize;
    if (platform_set_position_) platform_io.Platform_SetWindowPos = TracePlatformWindowPosition;
    if (platform_set_size_) platform_io.Platform_SetWindowSize = TracePlatformWindowSize;
    platform_io.Renderer_CreateWindow = CreateViewportWindow;
    platform_io.Renderer_DestroyWindow = DestroyViewportWindow;
    platform_io.Renderer_SetWindowSize = SetViewportWindowSize;
    platform_io.Renderer_RenderWindow = RenderViewportWindow;
    platform_io.Renderer_SwapBuffers = SwapViewportBuffers;
    return true;
}

void D3D11ImGuiViewportRenderer::Shutdown() noexcept
{
    if (active_instance_ == this) {
        if (ImGui::GetCurrentContext() != nullptr) {
            auto& platform_io = ImGui::GetPlatformIO();
            if (platform_io.Platform_SetWindowPos == TracePlatformWindowPosition)
                platform_io.Platform_SetWindowPos = platform_set_position_;
            if (platform_io.Platform_SetWindowSize == TracePlatformWindowSize)
                platform_io.Platform_SetWindowSize = platform_set_size_;
        }
        active_instance_ = nullptr;
    }
    platform_set_position_ = nullptr;
    platform_set_size_ = nullptr;
    device_context_.Reset();
    device_.Reset();
    factory_.Reset();
    last_error_ = {};
    presentation_updates_.clear();
    present_completions_.clear();
    native_window_theme_callback_ = {};
    compositor_clock_paced_ = false;
    feedback_acquire_only_ = false;
}

void D3D11ImGuiViewportRenderer::SetClearColor(
    const std::array<float, 4>& clear_color) noexcept
{
    clear_color_ = clear_color;
}

void D3D11ImGuiViewportRenderer::TracePlatformWindowPosition(ImGuiViewport* viewport, ImVec2 position)
{
    auto* instance = active_instance_;
    presentation_trace::Span span({.name = "platform_window_position",
        .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(ViewportWindowHandle(*viewport)))});
    instance->platform_set_position_(viewport, position);
}

void D3D11ImGuiViewportRenderer::TracePlatformWindowSize(ImGuiViewport* viewport, ImVec2 size)
{
    auto* instance = active_instance_;
    presentation_trace::Span span({.name = "platform_window_size",
        .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(ViewportWindowHandle(*viewport))),
        .new_width = size.x > 0 ? static_cast<unsigned>(size.x) : 0,
        .new_height = size.y > 0 ? static_cast<unsigned>(size.y) : 0});
    native_size_trace::Scope native_size;
    instance->platform_set_size_(viewport, size);
}

void D3D11ImGuiViewportRenderer::SetNativeWindowThemeCallback(
    std::function<void(HWND)> callback)
{
    native_window_theme_callback_ = std::move(callback);
}

void D3D11ImGuiViewportRenderer::RefreshNativeWindowThemes()
{
    if (!native_window_theme_callback_ ||
        ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const ImGuiPlatformIO& platform_io =
        ImGui::GetPlatformIO();
    for (ImGuiViewport* viewport : platform_io.Viewports) {
        if (viewport == nullptr) {
            continue;
        }
        if (const HWND hwnd = ViewportWindowHandle(*viewport);
            hwnd != nullptr) {
            native_window_theme_callback_(hwnd);
        }
    }
}

D3D11RendererError D3D11ImGuiViewportRenderer::TakeLastError() noexcept
{
    return std::exchange(last_error_, D3D11RendererError{});
}

std::vector<D3D11ViewportPresentationUpdate>
D3D11ImGuiViewportRenderer::TakePresentationUpdates() noexcept
{
    return std::exchange(
        presentation_updates_,
        std::vector<D3D11ViewportPresentationUpdate>{});
}

std::vector<D3D11ViewportPresentCompletion>
D3D11ImGuiViewportRenderer::TakePresentCompletions() noexcept
{
    return std::exchange(
        present_completions_,
        std::vector<D3D11ViewportPresentCompletion>{});
}

void D3D11ImGuiViewportRenderer::RefreshPresentationTargets()
{
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    for (int index = 0; index < platform_io.Viewports.Size; ++index) {
        ImGuiViewport* viewport = platform_io.Viewports[index];
        if (viewport == nullptr) {
            continue;
        }
        auto* data = static_cast<ViewportRendererData*>(viewport->RendererUserData);
        if (data == nullptr) {
            continue;
        }
        const HRESULT result = data->presentation.RefreshTarget();
        RecordFailure(result, data->presentation.last_error_operation());
        if (SUCCEEDED(result)) {
            data->monitor = data->presentation.refresh_state().monitor;
        }
        CollectPresentationUpdate(*viewport, data->presentation);
    }
}

void D3D11ImGuiViewportRenderer::CreateViewportWindow(ImGuiViewport* viewport)
{
    D3D11ImGuiViewportRenderer* instance = active_instance_;
    if (instance == nullptr || viewport == nullptr) {
        return;
    }

    auto* data = new ViewportRendererData();
    const HWND hwnd = ViewportWindowHandle(*viewport);
    const HRESULT result = data->presentation.Initialize(
        instance->factory_.Get(),
        instance->device_.Get(),
        instance->device_context_.Get(),
        hwnd,
        static_cast<UINT>(viewport->Size.x),
        static_cast<UINT>(viewport->Size.y));
    if (FAILED(result)) {
        instance->RecordFailure(result, data->presentation.last_error_operation());
        delete data;
        return;
    }
    data->monitor = data->presentation.refresh_state().monitor;
    viewport->RendererUserData = data;
    if (instance->native_window_theme_callback_ &&
        hwnd != nullptr) {
        instance->native_window_theme_callback_(hwnd);
    }
    instance->CollectPresentationUpdate(*viewport, data->presentation);
}

void D3D11ImGuiViewportRenderer::DestroyViewportWindow(ImGuiViewport* viewport)
{
    if (viewport == nullptr) {
        return;
    }
    auto* data = static_cast<ViewportRendererData*>(viewport->RendererUserData);
    delete data;
    viewport->RendererUserData = nullptr;
}

void D3D11ImGuiViewportRenderer::SetViewportWindowSize(ImGuiViewport* viewport, ImVec2 size)
{
    D3D11ImGuiViewportRenderer* instance = active_instance_;
    if (instance == nullptr || viewport == nullptr || size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    auto* data = static_cast<ViewportRendererData*>(viewport->RendererUserData);
    if (data == nullptr) {
        return;
    }

    const HRESULT result = data->presentation.Resize(
        static_cast<UINT>(size.x),
        static_cast<UINT>(size.y));
    data->frame_acquired = false;
    instance->RecordFailure(result, data->presentation.last_error_operation());
    instance->CollectPresentationUpdate(*viewport, data->presentation);
}

void D3D11ImGuiViewportRenderer::RenderViewportWindow(ImGuiViewport* viewport, void*)
{
    D3D11ImGuiViewportRenderer* instance = active_instance_;
    if (instance == nullptr || viewport == nullptr) {
        return;
    }
    auto* data = static_cast<ViewportRendererData*>(viewport->RendererUserData);
    if (data == nullptr) {
        return;
    }

    const HMONITOR current_monitor = MonitorFromWindow(
        ViewportWindowHandle(*viewport),
        MONITOR_DEFAULTTONEAREST);
    if (current_monitor != nullptr && current_monitor != data->monitor) {
        const HRESULT refresh_result = data->presentation.RefreshTarget();
        instance->RecordFailure(
            refresh_result,
            data->presentation.last_error_operation());
        if (SUCCEEDED(refresh_result)) {
            data->monitor = current_monitor;
        }
        instance->CollectPresentationUpdate(*viewport, data->presentation);
    }

    const HRESULT begin_result = data->presentation.BeginFrame(
        instance->clear_color_.data(),
        (viewport->Flags & ImGuiViewportFlags_NoRendererClear) == 0,
        0);
    if (begin_result == DXGI_ERROR_WAS_STILL_DRAWING) {
        data->frame_acquired = false;
        instance->CollectPresentationUpdate(*viewport, data->presentation);
        return;
    }
    instance->RecordFailure(
        begin_result,
        data->presentation.last_error_operation());
    if (FAILED(begin_result)) {
        data->frame_acquired = false;
        return;
    }
    data->frame_acquired = true;
    {
        presentation_trace::Span draw({.name = "viewport_draw_submission",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(ViewportWindowHandle(*viewport)))});
        ImGui_ImplDX11_RenderDrawData(viewport->DrawData);
    }
    instance->CollectPresentationUpdate(*viewport, data->presentation);
}

void D3D11ImGuiViewportRenderer::SwapViewportBuffers(ImGuiViewport* viewport, void*)
{
    D3D11ImGuiViewportRenderer* instance = active_instance_;
    if (instance == nullptr || viewport == nullptr) {
        return;
    }
    auto* data = static_cast<ViewportRendererData*>(viewport->RendererUserData);
    if (data == nullptr) {
        return;
    }
    if (!data->frame_acquired) {
        return;
    }
    data->frame_acquired = false;
    const HRESULT result = data->presentation.Present(
        instance->compositor_clock_paced_
            ? D3D11PresentMode::CompositorClock
            : D3D11PresentMode::DisplayVSync);
    const auto completed_at = std::chrono::steady_clock::now();
    if (result == S_OK) {
        instance->present_completions_.push_back({viewport->ID, completed_at});
    }
    instance->RecordFailure(result, data->presentation.last_error_operation());
    instance->CollectPresentationUpdate(*viewport, data->presentation);
}

void D3D11ImGuiViewportRenderer::RecordFailure(HRESULT result, std::string_view operation) noexcept
{
    if (FAILED(result) && SUCCEEDED(last_error_.result)) {
        last_error_ = D3D11RendererError{result, operation.empty() ? "Dear ImGui viewport renderer" : operation};
    }
}

void D3D11ImGuiViewportRenderer::CollectPresentationUpdate(
    const ImGuiViewport& viewport,
    D3D11WindowPresentation& presentation)
{
    D3D11PresentationTransition transition = presentation.TakeTransition();
    D3D11CompositionFeedback feedback = presentation.TakeCompositionFeedback(!feedback_acquire_only_);
    if (transition.empty() && feedback.empty()) {
        return;
    }
    presentation_updates_.push_back(D3D11ViewportPresentationUpdate{
        .viewport_id = viewport.ID,
        .hwnd = ViewportWindowHandle(viewport),
        .backend = presentation.backend(),
        .degradation = presentation.degradation(
            compositor_clock_paced_
                ? D3D11PresentMode::CompositorClock
                : D3D11PresentMode::DisplayVSync),
        .refresh_state = presentation.refresh_state(),
        .transition = transition,
        .feedback = feedback,
    });
}

}  // namespace specforge
