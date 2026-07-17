#include "renderer/d3d11_renderer.h"

#include <iterator>

namespace specforge {

D3D11Renderer::~D3D11Renderer()
{
    Shutdown();
}

HRESULT D3D11Renderer::Initialize(HWND hwnd)
{
    Shutdown();
    last_error_operation_ = {};
    if (hwnd == nullptr) {
        return RecordFailure("D3D11Renderer::Initialize arguments", E_INVALIDARG);
    }

    constexpr D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level = D3D_FEATURE_LEVEL_11_0;

    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        &selected_feature_level,
        device_context_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("D3D11CreateDevice", result);
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    result = device_.As(&dxgi_device);
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("ID3D11Device::QueryInterface(IDXGIDevice)", result);
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    result = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDXGIDevice::GetAdapter", result);
    }

    result = adapter->GetParent(IID_PPV_ARGS(factory_.GetAddressOf()));
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDXGIAdapter::GetParent(IDXGIFactory2)", result);
    }

    result = swap_chain_.Initialize(factory_.Get(), device_.Get(), hwnd);
    if (FAILED(result)) {
        const std::string_view operation = swap_chain_.last_error_operation();
        Shutdown();
        return RecordFailure(operation, result);
    }

    last_error_operation_ = {};
    return S_OK;
}

void D3D11Renderer::Shutdown()
{
    swap_chain_.Shutdown();
    factory_.Reset();
    device_context_.Reset();
    device_.Reset();
}

HRESULT D3D11Renderer::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return RecordFailure("D3D11Renderer::Resize arguments", E_INVALIDARG);
    }
    const HRESULT result = swap_chain_.Resize(device_.Get(), device_context_.Get(), width, height);
    if (FAILED(result)) {
        return RecordFailure(swap_chain_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return S_OK;
}

void D3D11Renderer::BeginFrame(const std::array<float, 4>& clear_color)
{
    swap_chain_.Bind(device_context_.Get());
    swap_chain_.Clear(device_context_.Get(), clear_color.data());
}

HRESULT D3D11Renderer::Present()
{
    const HRESULT result = swap_chain_.Present(kPresentSyncInterval, 0);
    if (FAILED(result)) {
        return RecordFailure(swap_chain_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return result;
}

bool D3D11Renderer::GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const
{
    return swap_chain_.GetDesc(desc);
}

bool D3D11Renderer::GetConfiguredSwapChainColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const
{
    return swap_chain_.GetConfiguredColorSpace(color_space);
}

HRESULT D3D11Renderer::RecordFailure(std::string_view operation, HRESULT result) noexcept
{
    last_error_operation_ = operation.empty() ? "Direct3D 11 renderer operation" : operation;
    return result;
}

}  // namespace specforge
