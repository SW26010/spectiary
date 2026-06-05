#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>

namespace specforge {

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();

    bool Resize(UINT width, UINT height);
    void BeginFrame(const std::array<float, 4>& clear_color);
    HRESULT Present();

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return device_context_.Get(); }

private:
    bool CreateRenderTarget();
    void ReleaseRenderTarget();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
};

}  // namespace specforge
