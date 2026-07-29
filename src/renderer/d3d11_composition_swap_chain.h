#pragma once

#include "renderer/win32_display_refresh.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <presentation.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace specforge {

struct D3D11CompositionFeedback {
    std::uint64_t present_submissions = 0;
    std::uint64_t status_queued = 0;
    std::uint64_t status_skipped = 0;
    std::uint64_t status_canceled = 0;
    std::uint64_t composition_frames = 0;
    std::uint64_t independent_flip_frames = 0;
    std::uint64_t buffer_acquire_skipped = 0;
    std::uint64_t last_actual_duration = 0;
    std::int64_t last_displayed_time = 0;
    HRESULT statistics_error = S_OK;

    [[nodiscard]] bool empty() const noexcept;
};

class D3D11CompositionSwapChain {
public:
    D3D11CompositionSwapChain() = default;
    ~D3D11CompositionSwapChain();

    D3D11CompositionSwapChain(const D3D11CompositionSwapChain&) = delete;
    D3D11CompositionSwapChain& operator=(const D3D11CompositionSwapChain&) = delete;

    HRESULT Initialize(
        ID3D11Device* device,
        HWND hwnd,
        UINT width,
        UINT height,
        const Win32DisplayRefreshState& refresh_state);
    void Shutdown() noexcept;

    HRESULT Resize(
        ID3D11Device* device,
        ID3D11DeviceContext* device_context,
        UINT width,
        UINT height);
    HRESULT UpdatePreferredDuration(
        const Win32DisplayRefreshState& refresh_state) noexcept;
    HRESULT BeginFrame(
        ID3D11DeviceContext* device_context,
        const float clear_color[4],
        bool clear = true,
        DWORD availability_timeout_ms = 1'000);
    HRESULT Present(ID3D11DeviceContext* device_context);

    [[nodiscard]] D3D11CompositionFeedback TakeFeedback() noexcept;
    [[nodiscard]] ID3D11Texture2D*
    active_render_texture() const noexcept
    {
        return selected_buffer_ >= 0
                   ? buffers_[static_cast<std::size_t>(
                         selected_buffer_)]
                         .texture.Get()
                   : nullptr;
    }
    [[nodiscard]] bool initialized() const noexcept { return manager_ != nullptr; }
    [[nodiscard]] bool independent_flip_supported() const noexcept
    {
        return independent_flip_supported_;
    }
    [[nodiscard]] bool statistics_available() const noexcept
    {
        return statistics_event_ != nullptr;
    }
    [[nodiscard]] UINT preferred_duration() const noexcept
    {
        return preferred_duration_;
    }
    [[nodiscard]] UINT preferred_tolerance() const noexcept
    {
        return preferred_tolerance_;
    }
    [[nodiscard]] UINT_PTR content_tag() const noexcept { return content_tag_; }
    [[nodiscard]] HRESULT preferred_duration_result() const noexcept
    {
        return preferred_duration_result_;
    }
    [[nodiscard]] HRESULT present_status_statistics_result() const noexcept
    {
        return present_status_statistics_result_;
    }
    [[nodiscard]] HRESULT composition_statistics_result() const noexcept
    {
        return composition_statistics_result_;
    }
    [[nodiscard]] HRESULT independent_flip_statistics_result() const noexcept
    {
        return independent_flip_statistics_result_;
    }
    [[nodiscard]] HRESULT statistics_event_result() const noexcept
    {
        return statistics_event_result_;
    }
    [[nodiscard]] std::string_view last_error_operation() const noexcept
    {
        return last_error_operation_;
    }

private:
    struct Buffer {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target;
        Microsoft::WRL::ComPtr<IPresentationBuffer> presentation;
        HANDLE available_event = nullptr;

        void Reset() noexcept;
    };

    HRESULT RecordFailure(std::string_view operation, HRESULT result) noexcept;
    HRESULT CreateBuffers(ID3D11Device* device, UINT width, UINT height);
    void ResetBuffers() noexcept;
    void DrainStatistics() noexcept;

    Microsoft::WRL::ComPtr<IPresentationFactory> factory_;
    Microsoft::WRL::ComPtr<IPresentationManager> manager_;
    Microsoft::WRL::ComPtr<IPresentationSurface> presentation_surface_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> composition_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> composition_visual_;
    Microsoft::WRL::ComPtr<IUnknown> composition_content_;
    std::array<Buffer, 3> buffers_;
    HANDLE surface_handle_ = nullptr;
    HANDLE statistics_event_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    int selected_buffer_ = -1;
    UINT preferred_duration_ = 0;
    UINT preferred_tolerance_ = 0;
    UINT_PTR content_tag_ = 0;
    HRESULT preferred_duration_result_ = E_FAIL;
    HRESULT present_status_statistics_result_ = E_FAIL;
    HRESULT composition_statistics_result_ = E_FAIL;
    HRESULT independent_flip_statistics_result_ = E_FAIL;
    HRESULT statistics_event_result_ = E_FAIL;
    bool independent_flip_supported_ = false;
    bool independent_flip_reported_ = false;
    D3D11CompositionFeedback feedback_;
    std::string_view last_error_operation_;
};

}  // namespace specforge
