#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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
    HRESULT PresentTest();

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return device_context_.Get(); }
    [[nodiscard]] HANDLE occlusion_event() const noexcept { return occlusion_event_; }
    [[nodiscard]] HRESULT occlusion_status_registration_result() const noexcept
    {
        return occlusion_status_registration_result_;
    }
    [[nodiscard]] UINT present_sync_interval() const noexcept { return kPresentSyncInterval; }
    [[nodiscard]] bool GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const;

private:
    static constexpr UINT kPresentSyncInterval = 1;

    bool CreateRenderTarget();
    void ReleaseRenderTarget();
    void RegisterOcclusionStatusEvent();
    void UnregisterOcclusionStatusEvent() noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> occlusion_factory_;
    HANDLE occlusion_event_ = nullptr;
    DWORD occlusion_cookie_ = 0;
    HRESULT occlusion_status_registration_result_ = E_PENDING;
    bool occlusion_registered_ = false;
};

}  // namespace specforge
