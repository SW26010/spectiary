#include "renderer/d3d11_renderer.h"

#include <iterator>

namespace specforge {

D3D11Renderer::~D3D11Renderer()
{
    Shutdown();
}

bool D3D11Renderer::Initialize(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_chain_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = hwnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level = D3D_FEATURE_LEVEL_11_0;

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        swap_chain_.GetAddressOf(),
        device_.GetAddressOf(),
        &selected_feature_level,
        device_context_.GetAddressOf());

    if (FAILED(result)) {
        return false;
    }

    if (!CreateRenderTarget()) {
        return false;
    }

    RegisterOcclusionStatusEvent();
    return true;
}

void D3D11Renderer::Shutdown()
{
    ReleaseRenderTarget();
    UnregisterOcclusionStatusEvent();

    if (swap_chain_ != nullptr) {
        swap_chain_->SetFullscreenState(FALSE, nullptr);
    }

    swap_chain_.Reset();
    device_context_.Reset();
    device_.Reset();
}

bool D3D11Renderer::Resize(UINT width, UINT height)
{
    if (swap_chain_ == nullptr || width == 0 || height == 0) {
        return false;
    }

    ReleaseRenderTarget();

    const HRESULT result = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        return false;
    }

    return CreateRenderTarget();
}

void D3D11Renderer::BeginFrame(const std::array<float, 4>& clear_color)
{
    ID3D11RenderTargetView* target = render_target_.Get();
    device_context_->OMSetRenderTargets(1, &target, nullptr);
    device_context_->ClearRenderTargetView(render_target_.Get(), clear_color.data());
}

HRESULT D3D11Renderer::Present()
{
    return swap_chain_->Present(kPresentSyncInterval, 0);
}

HRESULT D3D11Renderer::PresentTest()
{
    if (swap_chain_ == nullptr) {
        return E_FAIL;
    }
    return swap_chain_->Present(0, DXGI_PRESENT_TEST);
}

bool D3D11Renderer::GetSwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc) const
{
    if (swap_chain_ == nullptr) {
        return false;
    }
    return SUCCEEDED(swap_chain_->GetDesc(&desc));
}

bool D3D11Renderer::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(result)) {
        return false;
    }

    result = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target_.GetAddressOf());
    return SUCCEEDED(result);
}

void D3D11Renderer::ReleaseRenderTarget()
{
    render_target_.Reset();
}

void D3D11Renderer::RegisterOcclusionStatusEvent()
{
    UnregisterOcclusionStatusEvent();
    occlusion_status_registration_result_ = E_PENDING;

    if (device_ == nullptr) {
        occlusion_status_registration_result_ = E_FAIL;
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT result = device_.As(&dxgi_device);
    if (FAILED(result)) {
        occlusion_status_registration_result_ = result;
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    result = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(result)) {
        occlusion_status_registration_result_ = result;
        return;
    }

    result = adapter->GetParent(IID_PPV_ARGS(occlusion_factory_.GetAddressOf()));
    if (FAILED(result)) {
        occlusion_status_registration_result_ = result;
        occlusion_factory_.Reset();
        return;
    }

    occlusion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (occlusion_event_ == nullptr) {
        occlusion_status_registration_result_ = HRESULT_FROM_WIN32(GetLastError());
        occlusion_factory_.Reset();
        return;
    }

    result = occlusion_factory_->RegisterOcclusionStatusEvent(occlusion_event_, &occlusion_cookie_);
    if (FAILED(result)) {
        occlusion_status_registration_result_ = result;
        CloseHandle(occlusion_event_);
        occlusion_event_ = nullptr;
        occlusion_cookie_ = 0;
        occlusion_factory_.Reset();
        return;
    }

    occlusion_registered_ = true;
    occlusion_status_registration_result_ = S_OK;
}

void D3D11Renderer::UnregisterOcclusionStatusEvent() noexcept
{
    if (occlusion_registered_ && occlusion_factory_ != nullptr) {
        occlusion_factory_->UnregisterOcclusionStatus(occlusion_cookie_);
    }

    occlusion_registered_ = false;
    occlusion_cookie_ = 0;
    occlusion_factory_.Reset();

    if (occlusion_event_ != nullptr) {
        CloseHandle(occlusion_event_);
        occlusion_event_ = nullptr;
    }
}

}  // namespace specforge
