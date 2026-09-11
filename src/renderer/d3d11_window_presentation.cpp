#include "profile/presentation_trace.h"
#include "renderer/d3d11_window_presentation.h"

#include <utility>

namespace specforge {

const char* D3D11PresentationBackendName(
    D3D11PresentationBackend backend) noexcept
{
    switch (backend) {
    case D3D11PresentationBackend::Composition:
        return "composition";
    case D3D11PresentationBackend::Dxgi:
        return "dxgi";
    case D3D11PresentationBackend::None:
    default:
        return "none";
    }
}

const char* D3D11PresentModeName(D3D11PresentMode mode) noexcept
{
    switch (mode) {
    case D3D11PresentMode::CompositorClock:
        return "compositor_clock";
    case D3D11PresentMode::Immediate:
        return "immediate";
    case D3D11PresentMode::DisplayVSync:
    default:
        return "display_vsync";
    }
}

const char* D3D11PresentationDegradationName(
    D3D11PresentationDegradation degradation) noexcept
{
    switch (degradation) {
    case D3D11PresentationDegradation::TearingAtTargetRate:
        return "tearing_at_target_rate";
    case D3D11PresentationDegradation::TearingAllowed:
        return "tearing_allowed";
    case D3D11PresentationDegradation::ImmediateWithoutTearingSupport:
        return "immediate_without_tearing_support";
    case D3D11PresentationDegradation::ReducedRateTearFree:
        return "reduced_rate_tear_free";
    case D3D11PresentationDegradation::None:
    default:
        return "none";
    }
}

D3D11WindowPresentation::~D3D11WindowPresentation()
{
    Shutdown();
}

HRESULT D3D11WindowPresentation::Initialize(
    IDXGIFactory2* factory,
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    HWND hwnd,
    UINT width,
    UINT height,
    D3D11CompositionPolicy composition_policy)
{
    Shutdown();
    last_error_operation_ = {};
    if (factory == nullptr || device == nullptr || device_context == nullptr ||
        hwnd == nullptr || width == 0 || height == 0) {
        return RecordFailure(
            "D3D11WindowPresentation::Initialize arguments",
            E_INVALIDARG);
    }

    factory_ = factory;
    device_ = device;
    device_context_ = device_context;
    hwnd_ = hwnd;
    presentation_trace::Register(reinterpret_cast<std::uintptr_t>(hwnd), width, height);
    width_ = width;
    height_ = height;
    return presentation_trace::Measure(
        presentation_trace::Event{
            .name = "viewport_initialize",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_))},
        [&]() -> HRESULT {
            tearing_supported_ = DxgiFactorySupportsTearing(factory);

            const HRESULT refresh_result = QueryWin32DisplayRefreshState(
                hwnd_,
                refresh_state_);
            if (composition_policy == D3D11CompositionPolicy::Prefer &&
                SUCCEEDED(refresh_result)) {
                const HRESULT composition_result = composition_.Initialize(
                    device_,
                    hwnd_,
                    width_,
                    height_,
                    refresh_state_);
                if (SUCCEEDED(composition_result)) {
                    backend_ = D3D11PresentationBackend::Composition;
                    transition_ = {
                        D3D11PresentationBackend::None,
                        backend_,
                        S_OK,
                        "D3D11CompositionSwapChain::Initialize",
                    };
                    return S_OK;
                }
                return ActivateDxgiFallback(
                    composition_result,
                    composition_.last_error_operation(),
                    width_,
                    height_);
            }

            return ActivateDxgiFallback(
                composition_policy == D3D11CompositionPolicy::Prefer
                    ? refresh_result
                    : S_FALSE,
                composition_policy == D3D11CompositionPolicy::Prefer
                    ? "QueryWin32DisplayRefreshState"
                    : "composition disabled by presentation options",
                width_,
                height_);
        });
}

void D3D11WindowPresentation::Shutdown() noexcept
{
    {
        presentation_trace::Span shutdown({.name = "viewport_shutdown",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_)),
            .backend = D3D11PresentationBackendName(backend_)});
        if (device_context_ != nullptr) {
            device_context_->OMSetRenderTargets(0, nullptr, nullptr);
        }
        composition_.Shutdown();
        dxgi_.Shutdown();
    }
    factory_ = nullptr;
    device_ = nullptr;
    device_context_ = nullptr;
    presentation_trace::Unregister(reinterpret_cast<std::uintptr_t>(hwnd_));
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
    tearing_supported_ = false;
    backend_ = D3D11PresentationBackend::None;
    refresh_state_ = {};
    transition_ = {};
    frame_active_ = false;
}

HRESULT D3D11WindowPresentation::Resize(UINT width, UINT height)
{
    return presentation_trace::Measure(
        presentation_trace::Event{
            .name = "viewport_resize",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_)),
            .new_width = width,
            .new_height = height,
            .backend = D3D11PresentationBackendName(backend_)},
        [&]() -> HRESULT {
            if (width == 0 || height == 0 || device_ == nullptr ||
                device_context_ == nullptr) {
                return RecordFailure(
                    "D3D11WindowPresentation::Resize arguments",
                    E_INVALIDARG);
            }
            if (auto it = presentation_trace::windows.find(reinterpret_cast<std::uintptr_t>(hwnd_)); it != presentation_trace::windows.end()) {
                it->second.width = width;
                it->second.height = height;
            }
            width_ = width;
            height_ = height;
            frame_active_ = false;

            if (backend_ == D3D11PresentationBackend::Composition) {
                const HRESULT result = composition_.Resize(
                    device_,
                    device_context_,
                    width_,
                    height_);
                if (FAILED(result)) {
                    return ActivateDxgiFallback(
                        result,
                        composition_.last_error_operation(),
                        width_,
                        height_);
                }
                last_error_operation_ = {};
                return S_OK;
            }
            if (backend_ == D3D11PresentationBackend::Dxgi) {
                const HRESULT result = dxgi_.Resize(
                    device_,
                    device_context_,
                    width_,
                    height_);
                if (FAILED(result)) {
                    return RecordFailure(dxgi_.last_error_operation(), result);
                }
                last_error_operation_ = {};
                return S_OK;
            }
            return RecordFailure("D3D11WindowPresentation::Resize backend", E_FAIL);
        });
}

HRESULT D3D11WindowPresentation::RefreshTarget()
{
    Win32DisplayRefreshState refreshed;
    const HRESULT query_result = QueryWin32DisplayRefreshState(hwnd_, refreshed);
    if (FAILED(query_result)) {
        return RecordFailure("QueryWin32DisplayRefreshState", query_result);
    }
    refresh_state_ = std::move(refreshed);

    if (backend_ == D3D11PresentationBackend::Composition) {
        const HRESULT result = composition_.UpdatePreferredDuration(refresh_state_);
        if (FAILED(result)) {
            return ActivateDxgiFallback(
                result,
                composition_.last_error_operation(),
                width_,
                height_);
        }
    }
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11WindowPresentation::BeginFrame(
    const float clear_color[4],
    bool clear,
    DWORD availability_timeout_ms)
{
    return presentation_trace::Measure(
        presentation_trace::Event{
            .name = "viewport_acquire",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_)),
            .backend = D3D11PresentationBackendName(backend_),
            .timeout_ms = availability_timeout_ms},
        [&]() -> HRESULT {
            frame_active_ = false;
            if (device_context_ == nullptr || (clear && clear_color == nullptr)) {
                return RecordFailure(
                    "D3D11WindowPresentation::BeginFrame arguments",
                    E_INVALIDARG);
            }
            if (backend_ == D3D11PresentationBackend::Composition) {
                const HRESULT result = composition_.BeginFrame(
                    device_context_,
                    clear_color,
                    clear,
                    availability_timeout_ms);
                if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
                    last_error_operation_ = {};
                    return result;
                }
                if (FAILED(result)) {
                    const HRESULT fallback_result = ActivateDxgiFallback(
                        result,
                        composition_.last_error_operation(),
                        width_,
                        height_);
                    if (FAILED(fallback_result)) {
                        return fallback_result;
                    }
                    dxgi_.Bind(device_context_);
                    if (clear) {
                        dxgi_.Clear(device_context_, clear_color);
                    }
                    frame_active_ = true;
                    return S_FALSE;
                }
                last_error_operation_ = {};
                frame_active_ = true;
                return S_OK;
            }
            if (backend_ == D3D11PresentationBackend::Dxgi) {
                dxgi_.Bind(device_context_);
                if (clear) {
                    dxgi_.Clear(device_context_, clear_color);
                }
                last_error_operation_ = {};
                frame_active_ = true;
                return S_OK;
            }
            return RecordFailure("D3D11WindowPresentation::BeginFrame backend", E_FAIL);
        });
}

HRESULT D3D11WindowPresentation::Present(D3D11PresentMode mode)
{
    return presentation_trace::Measure(
        presentation_trace::Event{
            .name = "viewport_present",
            .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_)),
            .backend = D3D11PresentationBackendName(backend_),
            .present_mode = D3D11PresentModeName(mode)},
        [&]() -> HRESULT {
            frame_active_ = false;
            if (backend_ == D3D11PresentationBackend::Composition) {
                const HRESULT result = composition_.Present(device_context_);
                if (FAILED(result)) {
                    const HRESULT fallback_result = ActivateDxgiFallback(
                        result,
                        composition_.last_error_operation(),
                        width_,
                        height_);
                    return FAILED(fallback_result) ? fallback_result : S_FALSE;
                }
                last_error_operation_ = {};
                return S_OK;
            }
            if (backend_ == D3D11PresentationBackend::Dxgi) {
                const UINT sync_interval =
                    D3D11PresentSyncInterval(mode, tearing_supported_);
                const UINT flags = D3D11PresentFlags(mode, tearing_supported_);
                const HRESULT result = dxgi_.Present(sync_interval, flags);
                if (FAILED(result)) {
                    return RecordFailure(dxgi_.last_error_operation(), result);
                }
                last_error_operation_ = {};
                return result;
            }
            return RecordFailure("D3D11WindowPresentation::Present backend", E_FAIL);
        });
}

ID3D11Texture2D*
D3D11WindowPresentation::active_render_texture() const noexcept
{
    if (!frame_active_) {
        return nullptr;
    }
    if (backend_ == D3D11PresentationBackend::Composition) {
        return composition_.active_render_texture();
    }
    if (backend_ == D3D11PresentationBackend::Dxgi) {
        return dxgi_.render_texture();
    }
    return nullptr;
}

D3D11PresentationTransition D3D11WindowPresentation::TakeTransition() noexcept
{
    return std::exchange(transition_, D3D11PresentationTransition{});
}

D3D11CompositionFeedback
D3D11WindowPresentation::TakeCompositionFeedback() noexcept
{
    presentation_trace::Span feedback({.name = "presentation_feedback_collect",
        .window = presentation_trace::Lookup(reinterpret_cast<std::uintptr_t>(hwnd_)),
        .backend = D3D11PresentationBackendName(backend_)});
    return composition_.TakeFeedback();
}

D3D11PresentationDegradation D3D11WindowPresentation::degradation(
    D3D11PresentMode mode) const noexcept
{
    if (backend_ == D3D11PresentationBackend::Composition) {
        return D3D11PresentationDegradation::None;
    }
    if (backend_ == D3D11PresentationBackend::Dxgi) {
        if (mode == D3D11PresentMode::Immediate) {
            if (tearing_supported_) {
                return D3D11PresentationDegradation::TearingAllowed;
            }
            return D3D11PresentationDegradation::ImmediateWithoutTearingSupport;
        }
        if (mode == D3D11PresentMode::CompositorClock && tearing_supported_) {
            return D3D11PresentationDegradation::TearingAtTargetRate;
        }
        return D3D11PresentationDegradation::ReducedRateTearFree;
    }
    return D3D11PresentationDegradation::ReducedRateTearFree;
}

bool D3D11WindowPresentation::GetSwapChainDesc(
    DXGI_SWAP_CHAIN_DESC& desc) const noexcept
{
    return backend_ == D3D11PresentationBackend::Dxgi && dxgi_.GetDesc(desc);
}

bool D3D11WindowPresentation::GetConfiguredSwapChainColorSpace(
    DXGI_COLOR_SPACE_TYPE& color_space) const noexcept
{
    if (backend_ == D3D11PresentationBackend::Composition) {
        color_space = kSdrSwapChainColorSpace;
        return true;
    }
    return backend_ == D3D11PresentationBackend::Dxgi &&
           dxgi_.GetConfiguredColorSpace(color_space);
}

HRESULT D3D11WindowPresentation::ActivateDxgiFallback(
    HRESULT reason,
    std::string_view operation,
    UINT width,
    UINT height)
{
    const D3D11PresentationBackend previous_backend = backend_;
    if (device_context_ != nullptr) {
        device_context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    composition_.Shutdown();
    dxgi_.Shutdown();
    backend_ = D3D11PresentationBackend::None;
    frame_active_ = false;

    const HRESULT result = dxgi_.Initialize(
        factory_,
        device_,
        hwnd_,
        width,
        height);
    if (FAILED(result)) {
        return RecordFailure(dxgi_.last_error_operation(), result);
    }

    backend_ = D3D11PresentationBackend::Dxgi;
    transition_ = {
        previous_backend,
        backend_,
        reason,
        operation.empty() ? "composition presentation unavailable" : operation,
    };
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11WindowPresentation::RecordFailure(
    std::string_view operation,
    HRESULT result) noexcept
{
    last_error_operation_ = operation.empty()
                                ? "D3D11 window presentation operation"
                                : operation;
    return result;
}

}  // namespace specforge
