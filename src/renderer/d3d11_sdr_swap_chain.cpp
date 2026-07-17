#include "renderer/d3d11_sdr_swap_chain.h"

namespace specforge {

DXGI_SWAP_CHAIN_DESC1 MakeSdrSwapChainDesc(UINT width, UINT height, bool allow_tearing) noexcept
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = kSdrSwapChainFormat;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    return desc;
}

bool DxgiFactorySupportsTearing(IDXGIFactory2* factory) noexcept
{
    if (factory == nullptr) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(factory5.GetAddressOf())))) {
        return false;
    }

    BOOL supported = FALSE;
    return SUCCEEDED(factory5->CheckFeatureSupport(
               DXGI_FEATURE_PRESENT_ALLOW_TEARING,
               &supported,
               sizeof(supported))) &&
           supported == TRUE;
}

D3D11SdrSwapChain::~D3D11SdrSwapChain()
{
    Shutdown();
}

HRESULT D3D11SdrSwapChain::Initialize(
    IDXGIFactory2* factory,
    ID3D11Device* device,
    HWND hwnd,
    UINT width,
    UINT height)
{
    Shutdown();
    last_error_operation_ = {};
    if (factory == nullptr || device == nullptr || hwnd == nullptr) {
        return RecordFailure("D3D11SdrSwapChain::Initialize arguments", E_INVALIDARG);
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain1> base_swap_chain;
    tearing_supported_ = DxgiFactorySupportsTearing(factory);
    const DXGI_SWAP_CHAIN_DESC1 desc = MakeSdrSwapChainDesc(width, height, tearing_supported_);
    HRESULT result = factory->CreateSwapChainForHwnd(
        device,
        hwnd,
        &desc,
        nullptr,
        nullptr,
        base_swap_chain.GetAddressOf());
    if (FAILED(result)) {
        return RecordFailure("IDXGIFactory2::CreateSwapChainForHwnd", result);
    }

    result = base_swap_chain.As(&swap_chain_);
    if (FAILED(result)) {
        RecordFailure("IDXGISwapChain1::QueryInterface(IDXGISwapChain3)", result);
        Shutdown();
        return result;
    }

    result = ApplySdrColorSpace();
    if (FAILED(result)) {
        Shutdown();
        return result;
    }

    result = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(result)) {
        RecordFailure("IDXGIFactory2::MakeWindowAssociation", result);
        Shutdown();
        return result;
    }

    result = CreateRenderTarget(device);
    if (FAILED(result)) {
        Shutdown();
    } else {
        last_error_operation_ = {};
    }
    return result;
}

void D3D11SdrSwapChain::Shutdown() noexcept
{
    render_target_.Reset();
    swap_chain_.Reset();
    color_space_ = DXGI_COLOR_SPACE_CUSTOM;
    color_space_set_ = false;
    tearing_supported_ = false;
}

HRESULT D3D11SdrSwapChain::Resize(
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    UINT width,
    UINT height)
{
    last_error_operation_ = {};
    if (device == nullptr || device_context == nullptr || swap_chain_ == nullptr || width == 0 || height == 0) {
        return RecordFailure("D3D11SdrSwapChain::Resize arguments", E_INVALIDARG);
    }

    device_context->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_.Reset();
    const UINT flags = tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
    HRESULT result = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(result)) {
        return RecordFailure("IDXGISwapChain3::ResizeBuffers", result);
    }

    result = ApplySdrColorSpace();
    if (FAILED(result)) {
        return result;
    }
    result = CreateRenderTarget(device);
    if (SUCCEEDED(result)) {
        last_error_operation_ = {};
    }
    return result;
}

void D3D11SdrSwapChain::Bind(ID3D11DeviceContext* device_context) const noexcept
{
    if (device_context == nullptr) {
        return;
    }
    ID3D11RenderTargetView* target = render_target_.Get();
    device_context->OMSetRenderTargets(1, &target, nullptr);
}

void D3D11SdrSwapChain::Clear(ID3D11DeviceContext* device_context, const float clear_color[4]) const noexcept
{
    if (device_context == nullptr || render_target_ == nullptr || clear_color == nullptr) {
        return;
    }
    device_context->ClearRenderTargetView(render_target_.Get(), clear_color);
}

HRESULT D3D11SdrSwapChain::Present(UINT sync_interval, UINT flags) noexcept
{
    if (swap_chain_ == nullptr) {
        return RecordFailure("IDXGISwapChain3::Present", E_FAIL);
    }
    const HRESULT result = swap_chain_->Present(sync_interval, flags);
    if (FAILED(result)) {
        return RecordFailure("IDXGISwapChain3::Present", result);
    }
    last_error_operation_ = {};
    return result;
}

bool D3D11SdrSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC& desc) const noexcept
{
    if (swap_chain_ == nullptr) {
        return false;
    }
    return SUCCEEDED(swap_chain_->GetDesc(&desc));
}

bool D3D11SdrSwapChain::GetConfiguredColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const noexcept
{
    if (swap_chain_ == nullptr || !color_space_set_) {
        return false;
    }
    color_space = color_space_;
    return true;
}

HRESULT D3D11SdrSwapChain::RecordFailure(std::string_view operation, HRESULT result) noexcept
{
    last_error_operation_ = operation;
    return result;
}

HRESULT D3D11SdrSwapChain::ApplySdrColorSpace() noexcept
{
    if (swap_chain_ == nullptr) {
        return RecordFailure("D3D11SdrSwapChain::ApplySdrColorSpace", E_FAIL);
    }

    color_space_ = DXGI_COLOR_SPACE_CUSTOM;
    color_space_set_ = false;
    UINT support = 0;
    HRESULT result = swap_chain_->CheckColorSpaceSupport(kSdrSwapChainColorSpace, &support);
    if (FAILED(result)) {
        return RecordFailure("IDXGISwapChain3::CheckColorSpaceSupport", result);
    }
    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        return RecordFailure("IDXGISwapChain3::CheckColorSpaceSupport(PRESENT)", DXGI_ERROR_UNSUPPORTED);
    }
    result = swap_chain_->SetColorSpace1(kSdrSwapChainColorSpace);
    if (SUCCEEDED(result)) {
        color_space_ = kSdrSwapChainColorSpace;
        color_space_set_ = true;
    } else {
        RecordFailure("IDXGISwapChain3::SetColorSpace1", result);
    }
    return result;
}

HRESULT D3D11SdrSwapChain::CreateRenderTarget(ID3D11Device* device)
{
    if (device == nullptr || swap_chain_ == nullptr) {
        return RecordFailure("D3D11SdrSwapChain::CreateRenderTarget arguments", E_INVALIDARG);
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(result)) {
        return RecordFailure("IDXGISwapChain3::GetBuffer", result);
    }
    result = device->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target_.GetAddressOf());
    if (FAILED(result)) {
        return RecordFailure("ID3D11Device::CreateRenderTargetView", result);
    }
    return result;
}

}  // namespace specforge
