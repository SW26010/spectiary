#include "renderer/d3d11_imgui_viewport_renderer.h"

#include "renderer/d3d11_sdr_swap_chain.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <utility>

namespace specforge {
namespace {

struct ViewportRendererData {
    D3D11SdrSwapChain swap_chain;
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
    active_instance_ = this;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
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
        active_instance_ = nullptr;
    }
    device_context_.Reset();
    device_.Reset();
    factory_.Reset();
    last_error_ = {};
    compositor_clock_paced_ = false;
}

D3D11RendererError D3D11ImGuiViewportRenderer::TakeLastError() noexcept
{
    return std::exchange(last_error_, D3D11RendererError{});
}

void D3D11ImGuiViewportRenderer::CreateViewportWindow(ImGuiViewport* viewport)
{
    D3D11ImGuiViewportRenderer* instance = active_instance_;
    if (instance == nullptr || viewport == nullptr) {
        return;
    }

    auto* data = new ViewportRendererData();
    const HRESULT result = data->swap_chain.Initialize(
        instance->factory_.Get(),
        instance->device_.Get(),
        ViewportWindowHandle(*viewport),
        static_cast<UINT>(viewport->Size.x),
        static_cast<UINT>(viewport->Size.y));
    if (FAILED(result)) {
        instance->RecordFailure(result, data->swap_chain.last_error_operation());
        delete data;
        return;
    }
    viewport->RendererUserData = data;
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

    const HRESULT result = data->swap_chain.Resize(
        instance->device_.Get(),
        instance->device_context_.Get(),
        static_cast<UINT>(size.x),
        static_cast<UINT>(size.y));
    instance->RecordFailure(result, data->swap_chain.last_error_operation());
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

    constexpr float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    data->swap_chain.Bind(instance->device_context_.Get());
    if ((viewport->Flags & ImGuiViewportFlags_NoRendererClear) == 0) {
        data->swap_chain.Clear(instance->device_context_.Get(), clear_color);
    }
    ImGui_ImplDX11_RenderDrawData(viewport->DrawData);
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
    const UINT flags = instance->compositor_clock_paced_ && data->swap_chain.tearing_supported()
                           ? DXGI_PRESENT_ALLOW_TEARING
                           : 0U;
    const HRESULT result = data->swap_chain.Present(0, flags);
    instance->RecordFailure(result, data->swap_chain.last_error_operation());
}

void D3D11ImGuiViewportRenderer::RecordFailure(HRESULT result, std::string_view operation) noexcept
{
    if (FAILED(result) && SUCCEEDED(last_error_.result)) {
        last_error_ = D3D11RendererError{result, operation.empty() ? "Dear ImGui viewport renderer" : operation};
    }
}

}  // namespace specforge
