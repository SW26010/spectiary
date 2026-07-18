#pragma once

#include "renderer/d3d11_window_presentation.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <string_view>

namespace specforge {

enum class D3D11PresentMode {
    DisplayVSync,
    CompositorClock,
};

[[nodiscard]] constexpr UINT D3D11PresentSyncInterval(D3D11PresentMode mode) noexcept
{
    return mode == D3D11PresentMode::CompositorClock ? 0U : 1U;
}

[[nodiscard]] constexpr UINT D3D11PresentFlags(D3D11PresentMode mode, bool tearing_supported) noexcept
{
    return mode == D3D11PresentMode::CompositorClock && tearing_supported
               ? DXGI_PRESENT_ALLOW_TEARING
               : 0U;
}

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    HRESULT Initialize(HWND hwnd);
    void Shutdown();

    HRESULT Resize(UINT width, UINT height);
    HRESULT BeginFrame(const std::array<float, 4>& clear_color);
    HRESULT Present(D3D11PresentMode mode = D3D11PresentMode::DisplayVSync);
    HRESULT RefreshPresentationTarget();

    [[nodiscard]] D3D11PresentationTransition TakePresentationTransition() noexcept;
    [[nodiscard]] D3D11CompositionFeedback TakeCompositionFeedback() noexcept;

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return device_context_.Get(); }
    [[nodiscard]] IDXGIFactory2* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] std::string_view last_error_operation() const noexcept { return last_error_operation_; }
    [[nodiscard]] bool GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const;
    [[nodiscard]] bool GetConfiguredSwapChainColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const;
    [[nodiscard]] bool tearing_supported() const noexcept
    {
        return presentation_.tearing_supported();
    }
    [[nodiscard]] D3D11PresentationBackend presentation_backend() const noexcept
    {
        return presentation_.backend();
    }
    [[nodiscard]] D3D11PresentationDegradation presentation_degradation(
        D3D11PresentMode mode) const noexcept
    {
        return presentation_.degradation(mode == D3D11PresentMode::CompositorClock);
    }
    [[nodiscard]] const Win32DisplayRefreshState& display_refresh_state() const noexcept
    {
        return presentation_.refresh_state();
    }
    [[nodiscard]] bool composition_independent_flip_supported() const noexcept
    {
        return presentation_.composition_independent_flip_supported();
    }
    [[nodiscard]] bool composition_statistics_available() const noexcept
    {
        return presentation_.composition_statistics_available();
    }

private:
    HRESULT RecordFailure(std::string_view operation, HRESULT result) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    D3D11WindowPresentation presentation_;
    std::string_view last_error_operation_;
};

}  // namespace specforge
