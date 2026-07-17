#pragma once

#include "renderer/d3d11_sdr_swap_chain.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <string_view>

namespace specforge {

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    HRESULT Initialize(HWND hwnd);
    void Shutdown();

    HRESULT Resize(UINT width, UINT height);
    void BeginFrame(const std::array<float, 4>& clear_color);
    HRESULT Present();

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return device_context_.Get(); }
    [[nodiscard]] IDXGIFactory2* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] std::string_view last_error_operation() const noexcept { return last_error_operation_; }
    [[nodiscard]] UINT present_sync_interval() const noexcept { return kPresentSyncInterval; }
    [[nodiscard]] bool GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const;
    [[nodiscard]] bool GetConfiguredSwapChainColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const;

private:
    static constexpr UINT kPresentSyncInterval = 1;

    HRESULT RecordFailure(std::string_view operation, HRESULT result) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    D3D11SdrSwapChain swap_chain_;
    std::string_view last_error_operation_;
};

}  // namespace specforge
