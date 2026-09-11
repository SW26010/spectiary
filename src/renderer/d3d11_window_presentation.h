#pragma once

#include "renderer/d3d11_composition_swap_chain.h"
#include "renderer/d3d11_sdr_swap_chain.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string_view>

namespace specforge {

enum class D3D11PresentationBackend {
    None,
    Composition,
    Dxgi,
};

enum class D3D11PresentMode {
    DisplayVSync,
    CompositorClock,
    Immediate,
};

enum class D3D11PresentationDegradation {
    None,
    TearingAtTargetRate,
    TearingAllowed,
    ImmediateWithoutTearingSupport,
    ReducedRateTearFree,
};

enum class D3D11CompositionPolicy {
    Prefer,
    Disabled,
};

struct D3D11PresentationTransition {
    D3D11PresentationBackend previous_backend = D3D11PresentationBackend::None;
    D3D11PresentationBackend current_backend = D3D11PresentationBackend::None;
    HRESULT reason = S_OK;
    std::string_view operation;

    [[nodiscard]] bool empty() const noexcept
    {
        return previous_backend == current_backend && SUCCEEDED(reason);
    }
};

[[nodiscard]] const char* D3D11PresentationBackendName(
    D3D11PresentationBackend backend) noexcept;
[[nodiscard]] const char* D3D11PresentModeName(
    D3D11PresentMode mode) noexcept;
[[nodiscard]] const char* D3D11PresentationDegradationName(
    D3D11PresentationDegradation degradation) noexcept;
[[nodiscard]] constexpr UINT D3D11PresentSyncInterval(
    D3D11PresentMode mode,
    bool tearing_supported) noexcept
{
    if (mode == D3D11PresentMode::DisplayVSync) {
        return 1U;
    }
    if (mode == D3D11PresentMode::CompositorClock &&
        !tearing_supported) {
        return 1U;
    }
    return 0U;
}
[[nodiscard]] constexpr UINT D3D11PresentFlags(
    D3D11PresentMode mode,
    bool tearing_supported) noexcept
{
    return mode != D3D11PresentMode::DisplayVSync && tearing_supported
               ? DXGI_PRESENT_ALLOW_TEARING
               : 0U;
}

class D3D11WindowPresentation {
public:
    D3D11WindowPresentation() = default;
    ~D3D11WindowPresentation();

    D3D11WindowPresentation(const D3D11WindowPresentation&) = delete;
    D3D11WindowPresentation& operator=(const D3D11WindowPresentation&) = delete;

    HRESULT Initialize(
        IDXGIFactory2* factory,
        ID3D11Device* device,
        ID3D11DeviceContext* device_context,
        HWND hwnd,
        UINT width,
        UINT height,
        D3D11CompositionPolicy composition_policy =
            D3D11CompositionPolicy::Prefer,
        bool incremental_buffers = false);
    void Shutdown() noexcept;

    HRESULT Resize(UINT width, UINT height);
    HRESULT RefreshTarget();
    HRESULT BeginFrame(
        const float clear_color[4],
        bool clear = true,
        DWORD availability_timeout_ms = 1'000,
        std::uint64_t frame_id = 0);
    HRESULT Present(D3D11PresentMode mode);
    [[nodiscard]] ID3D11Texture2D*
    active_render_texture() const noexcept;

    [[nodiscard]] D3D11PresentationTransition TakeTransition() noexcept;
    [[nodiscard]] D3D11CompositionFeedback TakeCompositionFeedback(bool drain = true) noexcept;
    [[nodiscard]] D3D11PresentationBackend backend() const noexcept
    {
        return backend_;
    }
    [[nodiscard]] D3D11PresentationDegradation degradation(
        D3D11PresentMode mode) const noexcept;
    [[nodiscard]] const Win32DisplayRefreshState& refresh_state() const noexcept
    {
        return refresh_state_;
    }
    [[nodiscard]] bool tearing_supported() const noexcept
    {
        return tearing_supported_;
    }
    [[nodiscard]] bool composition_independent_flip_supported() const noexcept
    {
        return composition_.independent_flip_supported();
    }
    [[nodiscard]] bool composition_statistics_available() const noexcept
    {
        return composition_.statistics_available();
    }
    [[nodiscard]] bool GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const noexcept;
    [[nodiscard]] bool GetConfiguredSwapChainColorSpace(
        DXGI_COLOR_SPACE_TYPE& color_space) const noexcept;
    [[nodiscard]] std::string_view last_error_operation() const noexcept
    {
        return last_error_operation_;
    }

private:
    friend struct D3D11CompositionSwapChainTestAccess;
    HRESULT ActivateDxgiFallback(
        HRESULT reason,
        std::string_view operation,
        UINT width,
        UINT height);
    HRESULT RecordFailure(std::string_view operation, HRESULT result) noexcept;

    IDXGIFactory2* factory_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    bool tearing_supported_ = false;
    D3D11PresentationBackend backend_ = D3D11PresentationBackend::None;
    Win32DisplayRefreshState refresh_state_;
    D3D11CompositionSwapChain composition_;
    D3D11SdrSwapChain dxgi_;
    D3D11PresentationTransition transition_;
    std::string_view last_error_operation_;
    bool frame_active_ = false;
};

}  // namespace specforge
