#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string_view>

struct ImGuiViewport;
struct ImVec2;

namespace specforge {

struct D3D11RendererError {
    HRESULT result = S_OK;
    std::string_view operation;
};

class D3D11ImGuiViewportRenderer {
public:
    D3D11ImGuiViewportRenderer() = default;
    ~D3D11ImGuiViewportRenderer();

    D3D11ImGuiViewportRenderer(const D3D11ImGuiViewportRenderer&) = delete;
    D3D11ImGuiViewportRenderer& operator=(const D3D11ImGuiViewportRenderer&) = delete;

    bool Initialize(IDXGIFactory2* factory, ID3D11Device* device, ID3D11DeviceContext* device_context);
    void Shutdown() noexcept;

    [[nodiscard]] D3D11RendererError TakeLastError() noexcept;

private:
    static void CreateViewportWindow(ImGuiViewport* viewport);
    static void DestroyViewportWindow(ImGuiViewport* viewport);
    static void SetViewportWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void RenderViewportWindow(ImGuiViewport* viewport, void* render_argument);
    static void SwapViewportBuffers(ImGuiViewport* viewport, void* render_argument);

    void RecordFailure(HRESULT result, std::string_view operation) noexcept;

    static D3D11ImGuiViewportRenderer* active_instance_;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    D3D11RendererError last_error_;
};

}  // namespace specforge
