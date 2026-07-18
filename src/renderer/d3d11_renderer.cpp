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
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
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

    RECT client_rect = {};
    if (!GetClientRect(hwnd, &client_rect) || client_rect.right <= client_rect.left ||
        client_rect.bottom <= client_rect.top) {
        const DWORD error = GetLastError();
        Shutdown();
        return RecordFailure(
            "GetClientRect",
            HRESULT_FROM_WIN32(
                error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE));
    }

    result = presentation_.Initialize(
        factory_.Get(),
        device_.Get(),
        device_context_.Get(),
        hwnd,
        static_cast<UINT>(client_rect.right - client_rect.left),
        static_cast<UINT>(client_rect.bottom - client_rect.top));
    if (FAILED(result)) {
        const std::string_view operation = presentation_.last_error_operation();
        Shutdown();
        return RecordFailure(operation, result);
    }

    last_error_operation_ = {};
    return S_OK;
}

void D3D11Renderer::Shutdown()
{
    presentation_.Shutdown();
    factory_.Reset();
    device_context_.Reset();
    device_.Reset();
}

HRESULT D3D11Renderer::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0) {
        return RecordFailure("D3D11Renderer::Resize arguments", E_INVALIDARG);
    }
    const HRESULT result = presentation_.Resize(width, height);
    if (FAILED(result)) {
        return RecordFailure(presentation_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11Renderer::BeginFrame(const std::array<float, 4>& clear_color)
{
    const HRESULT result = presentation_.BeginFrame(clear_color.data());
    if (FAILED(result)) {
        return RecordFailure(presentation_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return result;
}

HRESULT D3D11Renderer::Present(D3D11PresentMode mode)
{
    const HRESULT result = presentation_.Present(
        mode == D3D11PresentMode::CompositorClock);
    if (FAILED(result)) {
        return RecordFailure(presentation_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return result;
}

HRESULT D3D11Renderer::RefreshPresentationTarget()
{
    const HRESULT result = presentation_.RefreshTarget();
    if (FAILED(result)) {
        return RecordFailure(presentation_.last_error_operation(), result);
    }
    last_error_operation_ = {};
    return result;
}

D3D11PresentationTransition
D3D11Renderer::TakePresentationTransition() noexcept
{
    return presentation_.TakeTransition();
}

D3D11CompositionFeedback D3D11Renderer::TakeCompositionFeedback() noexcept
{
    return presentation_.TakeCompositionFeedback();
}

bool D3D11Renderer::GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const
{
    return presentation_.GetSwapChainDesc(desc);
}

bool D3D11Renderer::GetConfiguredSwapChainColorSpace(DXGI_COLOR_SPACE_TYPE& color_space) const
{
    return presentation_.GetConfiguredSwapChainColorSpace(color_space);
}

HRESULT D3D11Renderer::RecordFailure(std::string_view operation, HRESULT result) noexcept
{
    last_error_operation_ = operation.empty() ? "Direct3D 11 renderer operation" : operation;
    return result;
}

}  // namespace specforge
