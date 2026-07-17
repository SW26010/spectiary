#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string_view>

namespace specforge {

inline constexpr DXGI_FORMAT kSdrSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_COLOR_SPACE_TYPE kSdrSwapChainColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

[[nodiscard]] DXGI_SWAP_CHAIN_DESC1 MakeSdrSwapChainDesc(UINT width = 0, UINT height = 0) noexcept;

class D3D11SdrSwapChain {
public:
    D3D11SdrSwapChain() = default;
    ~D3D11SdrSwapChain();

    D3D11SdrSwapChain(const D3D11SdrSwapChain&) = delete;
    D3D11SdrSwapChain& operator=(const D3D11SdrSwapChain&) = delete;

    HRESULT Initialize(
        IDXGIFactory2* factory,
        ID3D11Device* device,
        HWND hwnd,
        UINT width = 0,
        UINT height = 0);
    void Shutdown() noexcept;

    HRESULT Resize(ID3D11Device* device, ID3D11DeviceContext* device_context, UINT width, UINT height);
    void Bind(ID3D11DeviceContext* device_context) const noexcept;
    void Clear(ID3D11DeviceContext* device_context, const float clear_color[4]) const noexcept;
    HRESULT Present(UINT sync_interval, UINT flags) noexcept;

    [[nodiscard]] bool GetDesc(DXGI_SWAP_CHAIN_DESC& desc) const noexcept;
    [[nodiscard]] bool GetConfiguredColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const noexcept;
    [[nodiscard]] std::string_view last_error_operation() const noexcept { return last_error_operation_; }

private:
    HRESULT RecordFailure(std::string_view operation, HRESULT result) noexcept;
    HRESULT ApplySdrColorSpace() noexcept;
    HRESULT CreateRenderTarget(ID3D11Device* device);

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_CUSTOM;
    bool color_space_set_ = false;
    std::string_view last_error_operation_;
};

}  // namespace specforge
