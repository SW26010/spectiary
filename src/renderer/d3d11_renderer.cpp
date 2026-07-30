#include "renderer/d3d11_renderer.h"

#include "renderer/d3d11_frame_capture.h"

#include <d3d11sdklayers.h>

#include <cstddef>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace specforge {
namespace {

bool IsAllowedLiveObjectMessage(D3D11_MESSAGE_ID id)
{
    switch (id) {
    case D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY:
    case D3D11_MESSAGE_ID_LIVE_DEVICE:
    case D3D11_MESSAGE_ID_LIVE_CONTEXT:
    case D3D11_MESSAGE_ID_LIVE_DEVICE_WIN7:
    case D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY_WIN7:
        return true;
    default:
        return false;
    }
}

std::string HResultText(HRESULT result)
{
    char buffer[16] = {};
    (void)std::snprintf(
        buffer,
        std::size(buffer),
        "0x%08lx",
        static_cast<unsigned long>(result));
    return buffer;
}

}  // namespace

D3D11Renderer::~D3D11Renderer()
{
    Shutdown();
}

HRESULT D3D11Renderer::Initialize(
    HWND hwnd,
    D3D11CompositionPolicy composition_policy,
    bool enable_debug_layer)
{
    Shutdown();
    last_error_operation_ = {};
    debug_layer_requested_ = enable_debug_layer;
    debug_layer_enabled_ = false;
    debug_layer_enable_result_ = S_OK;
    live_object_report_ = {};
    live_object_report_.requested = enable_debug_layer;
    if (hwnd == nullptr) {
        return RecordFailure("D3D11Renderer::Initialize arguments", E_INVALIDARG);
    }

    constexpr D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_feature_level = D3D_FEATURE_LEVEL_11_0;

    const UINT ordinary_creation_flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const UINT requested_creation_flags =
        ordinary_creation_flags |
        (enable_debug_layer
             ? D3D11_CREATE_DEVICE_DEBUG
             : 0U);
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        requested_creation_flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        &selected_feature_level,
        device_context_.GetAddressOf());
    debug_layer_enable_result_ = result;
    if (FAILED(result) && enable_debug_layer) {
        device_context_.Reset();
        device_.Reset();
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            ordinary_creation_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &selected_feature_level,
            device_context_.GetAddressOf());
    } else if (SUCCEEDED(result) && enable_debug_layer) {
        debug_layer_enabled_ = true;
    }
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
        static_cast<UINT>(client_rect.bottom - client_rect.top),
        composition_policy);
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
    CollectLiveObjectReport();
    factory_.Reset();
    device_context_.Reset();
    device_.Reset();
}

void D3D11Renderer::CollectLiveObjectReport()
{
    if (!debug_layer_requested_ ||
        live_object_report_.available ||
        !live_object_report_.detail.empty()) {
        return;
    }

    live_object_report_.requested = true;
    if (!debug_layer_enabled_ || device_ == nullptr) {
        live_object_report_.detail =
            "D3D11 debug layer unavailable (" +
            HResultText(debug_layer_enable_result_) + ").";
        return;
    }

    if (device_context_ != nullptr) {
        device_context_->ClearState();
        device_context_->Flush();
    }

    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> info_queue;
    const HRESULT debug_result = device_.As(&debug);
    const HRESULT queue_result = device_.As(&info_queue);
    if (FAILED(debug_result) || FAILED(queue_result)) {
        live_object_report_.detail =
            "D3D11 debug interfaces unavailable.";
        return;
    }

    info_queue->ClearStoredMessages();
    const HRESULT report_result =
        debug->ReportLiveDeviceObjects(
            static_cast<D3D11_RLDO_FLAGS>(
                D3D11_RLDO_DETAIL |
                D3D11_RLDO_IGNORE_INTERNAL));
    if (FAILED(report_result)) {
        live_object_report_.detail =
            "ID3D11Debug::ReportLiveDeviceObjects failed (" +
            HResultText(report_result) + ").";
        return;
    }

    live_object_report_.available = true;
    const UINT64 message_count =
        info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    constexpr std::size_t kMaximumStoredDescriptions = 32;
    for (UINT64 index = 0; index < message_count; ++index) {
        SIZE_T message_size = 0;
        if (FAILED(info_queue->GetMessage(
                index,
                nullptr,
                &message_size)) ||
            message_size < sizeof(D3D11_MESSAGE)) {
            ++live_object_report_.unexpected_live_object_messages;
            continue;
        }

        std::vector<std::byte> storage(message_size);
        auto* message =
            reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(info_queue->GetMessage(
                index,
                message,
                &message_size))) {
            ++live_object_report_.unexpected_live_object_messages;
            continue;
        }

        if (IsAllowedLiveObjectMessage(message->ID)) {
            ++live_object_report_.allowed_live_object_messages;
            continue;
        }

        ++live_object_report_.unexpected_live_object_messages;
        if (live_object_report_.unexpected_messages.size() <
                kMaximumStoredDescriptions &&
            message->pDescription != nullptr) {
            live_object_report_.unexpected_messages.emplace_back(
                message->pDescription,
                message->DescriptionByteLength > 0
                    ? message->DescriptionByteLength - 1
                    : 0);
        }
    }
    live_object_report_.detail =
        live_object_report_.unexpected_live_object_messages == 0
        ? "D3D11 debug-layer live-object report contains only the allowed device/context summary."
        : "D3D11 debug-layer live-object report contains unexpected live objects.";
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

HRESULT D3D11Renderer::CaptureFrameToPng(
    const std::filesystem::path& output_path,
    const D3D11FrameCaptureFinalizer& finalizer,
    const std::optional<std::filesystem::path>&
        allowed_root)
{
    ID3D11Texture2D* source =
        presentation_.active_render_texture();
    if (source == nullptr) {
        return RecordFailure(
            "D3D11Renderer::CaptureFrameToPng without active frame",
            DXGI_ERROR_INVALID_CALL);
    }

    const D3D11FrameCaptureResult capture =
        CaptureD3D11TextureToPng(
            device_.Get(),
            device_context_.Get(),
            source,
            output_path,
            finalizer,
            allowed_root);
    if (!capture.succeeded()) {
        return RecordFailure(
            capture.operation,
            capture.result);
    }
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11Renderer::Present(D3D11PresentMode mode)
{
    const HRESULT result = presentation_.Present(mode);
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
