#include "profile/presentation_trace.h"
#include "renderer/d3d11_composition_swap_chain.h"

#include <atomic>
#include <utility>

namespace spectiary {
namespace {

constexpr std::uint64_t kFeedbackReportIntervalFrames = 120;
constexpr DXGI_COLOR_SPACE_TYPE kCompositionColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
std::atomic<UINT_PTR> g_next_content_tag{0x5350'0000U};

}  // namespace

bool D3D11CompositionFeedback::empty() const noexcept
{
    return present_submissions == 0 && status_queued == 0 &&
           status_skipped == 0 && status_canceled == 0 &&
           composition_frames == 0 && independent_flip_frames == 0 &&
           buffer_acquire_skipped == 0 &&
           SUCCEEDED(statistics_error);
}

void D3D11CompositionSwapChain::Buffer::Reset() noexcept
{
    const auto release = [](std::string_view kind, auto&& operation) {
        auto event = presentation_trace::current;
        event.name = "presentation_resource_release";
        event.resource_kind = kind;
        presentation_trace::Span span(event);
        operation();
    };
    if (available_event != nullptr) {
        release("available_event", [&] { CloseHandle(std::exchange(available_event, nullptr)); });
    }
    if (presentation) release("presentation_buffer", [&] { presentation.Reset(); });
    if (render_target) release("render_target_view", [&] { render_target.Reset(); });
    if (texture) release("texture", [&] { texture.Reset(); });
}

D3D11CompositionSwapChain::~D3D11CompositionSwapChain()
{
    Shutdown();
}

HRESULT D3D11CompositionSwapChain::Initialize(
    ID3D11Device* device,
    HWND hwnd,
    UINT width,
    UINT height,
    const Win32DisplayRefreshState& refresh_state,
    bool incremental_buffers)
{
    Shutdown();
    incremental_buffers_ = incremental_buffers;
    if (incremental_buffers_ && (!BufferPixelBytes(width, height) ||
        BufferPixelBytes(width, height) > kIncrementalBufferBudget / 3)) {
        return RecordFailure("incremental buffers initial budget", E_OUTOFMEMORY);
    }
    last_error_operation_ = {};
    if (device == nullptr || hwnd == nullptr || width == 0 || height == 0 ||
        refresh_state.preferred_duration == 0 ||
        refresh_state.preferred_tolerance == 0) {
        return RecordFailure(
            "D3D11CompositionSwapChain::Initialize arguments",
            E_INVALIDARG);
    }

    HRESULT result = CreatePresentationFactory(
        device,
        IID_PPV_ARGS(factory_.GetAddressOf()));
    if (FAILED(result)) {
        return RecordFailure("CreatePresentationFactory", result);
    }
    if (!factory_->IsPresentationSupported()) {
        Shutdown();
        return RecordFailure(
            "IPresentationFactory::IsPresentationSupported",
            DXGI_ERROR_UNSUPPORTED);
    }

    D3D11_FEATURE_DATA_DISPLAYABLE displayable = {};
    result = device->CheckFeatureSupport(
        D3D11_FEATURE_DISPLAYABLE,
        &displayable,
        sizeof(displayable));
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure(
            "ID3D11Device::CheckFeatureSupport(D3D11_FEATURE_DISPLAYABLE)",
            result);
    }
    if (!displayable.DisplayableTexture) {
        Shutdown();
        return RecordFailure(
            "D3D11_FEATURE_DATA_DISPLAYABLE::DisplayableTexture",
            DXGI_ERROR_UNSUPPORTED);
    }

    independent_flip_supported_ =
        factory_->IsPresentationSupportedWithIndependentFlip();

    result = factory_->CreatePresentationManager(manager_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("CreatePresentationManager", result);
    }

    result = UpdatePreferredDuration(refresh_state);
    if (FAILED(result)) {
        Shutdown();
        return result;
    }

    present_status_statistics_result_ = manager_->EnablePresentStatisticsKind(
        PresentStatisticsKind_PresentStatus,
        true);
    composition_statistics_result_ = manager_->EnablePresentStatisticsKind(
        PresentStatisticsKind_CompositionFrame,
        true);
    independent_flip_statistics_result_ =
        manager_->EnablePresentStatisticsKind(
            PresentStatisticsKind_IndependentFlipFrame,
            true);
    statistics_event_result_ =
        manager_->GetPresentStatisticsAvailableEvent(&statistics_event_);
    if (FAILED(statistics_event_result_)) {
        statistics_event_ = nullptr;
    }

    result = DCompositionCreateSurfaceHandle(
        COMPOSITIONOBJECT_ALL_ACCESS,
        nullptr,
        &surface_handle_);
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("DCompositionCreateSurfaceHandle", result);
    }
    result = manager_->CreatePresentationSurface(
        surface_handle_,
        presentation_surface_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("CreatePresentationSurface", result);
    }

    content_tag_ = g_next_content_tag.fetch_add(1, std::memory_order_relaxed);
    presentation_surface_->SetTag(content_tag_);
    result = presentation_surface_->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE);
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IPresentationSurface::SetAlphaMode", result);
    }
    result = presentation_surface_->SetColorSpace(kCompositionColorSpace);
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IPresentationSurface::SetColorSpace", result);
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    result = device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("ID3D11Device::QueryInterface(IDXGIDevice)", result);
    }
    result = DCompositionCreateDevice(
        dxgi_device.Get(),
        IID_PPV_ARGS(composition_device_.GetAddressOf()));
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("DCompositionCreateDevice", result);
    }
    result = composition_device_->CreateTargetForHwnd(
        hwnd,
        TRUE,
        composition_target_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionDevice::CreateTargetForHwnd", result);
    }
    result = composition_device_->CreateVisual(
        composition_visual_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionDevice::CreateVisual", result);
    }
    result = composition_device_->CreateSurfaceFromHandle(
        surface_handle_,
        composition_content_.GetAddressOf());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionDevice::CreateSurfaceFromHandle", result);
    }
    result = composition_visual_->SetContent(composition_content_.Get());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionVisual::SetContent", result);
    }
    result = composition_target_->SetRoot(composition_visual_.Get());
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionTarget::SetRoot", result);
    }
    result = composition_device_->Commit();
    if (FAILED(result)) {
        Shutdown();
        return RecordFailure("IDCompositionDevice::Commit", result);
    }

    result = CreateBuffers(device, width, height);
    if (FAILED(result)) {
        Shutdown();
        return result;
    }
    feedback_ = {};
    last_error_operation_ = {};
    return S_OK;
}

void D3D11CompositionSwapChain::Shutdown() noexcept
{
    if (composition_target_ != nullptr) {
        (void)composition_target_->SetRoot(nullptr);
    }
    if (composition_device_ != nullptr) {
        (void)composition_device_->Commit();
    }
    ResetBuffers();
    composition_content_.Reset();
    composition_visual_.Reset();
    composition_target_.Reset();
    composition_device_.Reset();
    presentation_surface_.Reset();
    manager_.Reset();
    factory_.Reset();
    if (statistics_event_ != nullptr) {
        CloseHandle(std::exchange(statistics_event_, nullptr));
    }
    if (surface_handle_ != nullptr) {
        CloseHandle(std::exchange(surface_handle_, nullptr));
    }
    width_ = 0;
    height_ = 0;
    selected_buffer_ = -1;
    preferred_duration_ = 0;
    bound_buffer_ = -1;
    incremental_buffers_ = false;
    generation_ = resize_serial_ = last_replacement_frame_ = 0;
    preferred_tolerance_ = 0;
    content_tag_ = 0;
    preferred_duration_result_ = E_FAIL;
    present_status_statistics_result_ = E_FAIL;
    composition_statistics_result_ = E_FAIL;
    independent_flip_statistics_result_ = E_FAIL;
    statistics_event_result_ = E_FAIL;
    independent_flip_supported_ = false;
    independent_flip_reported_ = false;
    feedback_ = {};
}

HRESULT D3D11CompositionSwapChain::Resize(
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    UINT width,
    UINT height)
{
    if (device == nullptr || device_context == nullptr || manager_ == nullptr ||
        width == 0 || height == 0) {
        return RecordFailure(
            "D3D11CompositionSwapChain::Resize arguments",
            E_INVALIDARG);
    }
    device_context->OMSetRenderTargets(0, nullptr, nullptr);
    selected_buffer_ = -1;
    if (incremental_buffers_) {
        width_ = width;
        height_ = height;
        ++resize_serial_;
        auto event = presentation_trace::current;
        event.name = "presentation_resize_request";
        event.resize_request_serial = resize_serial_;
        event.buffer_action = "request";
        return presentation_trace::Measure(event, [&]() -> HRESULT {
            if (!BufferPixelBytes(width, height)) return RecordFailure("incremental buffers request budget", E_OUTOFMEMORY);
            last_error_operation_ = {};
            return S_OK;
        });
    }
    const HRESULT result = CreateBuffers(device, width, height);
    if (SUCCEEDED(result)) {
        last_error_operation_ = {};
    }
    return result;
}

HRESULT D3D11CompositionSwapChain::UpdatePreferredDuration(
    const Win32DisplayRefreshState& refresh_state) noexcept
{
    if (manager_ == nullptr || refresh_state.preferred_duration == 0 ||
        refresh_state.preferred_tolerance == 0) {
        return RecordFailure(
            "D3D11CompositionSwapChain::UpdatePreferredDuration arguments",
            E_INVALIDARG);
    }
    const SystemInterruptTime preferred_duration = {
        refresh_state.preferred_duration,
    };
    const SystemInterruptTime tolerance = {
        refresh_state.preferred_tolerance,
    };
    preferred_duration_result_ = manager_->SetPreferredPresentDuration(
        preferred_duration,
        tolerance);
    if (FAILED(preferred_duration_result_)) {
        return RecordFailure(
            "IPresentationManager::SetPreferredPresentDuration",
            preferred_duration_result_);
    }
    preferred_duration_ = refresh_state.preferred_duration;
    preferred_tolerance_ = refresh_state.preferred_tolerance;
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11CompositionSwapChain::BeginFrame(
    ID3D11DeviceContext* device_context,
    const float clear_color[4],
    bool clear,
    DWORD availability_timeout_ms,
    std::uint64_t frame_id)
{
    if (device_context == nullptr || (clear && clear_color == nullptr) || manager_ == nullptr ||
        selected_buffer_ >= 0) {
        return RecordFailure(
            "D3D11CompositionSwapChain::BeginFrame arguments",
            E_INVALIDARG);
    }

    DrainStatistics();
    if (incremental_buffers_) {
        const HRESULT result = SelectIncrementalBuffer(device_context, frame_id);
        if (FAILED(result)) return result;
        ID3D11RenderTargetView* target = buffers_[selected_buffer_].render_target.Get();
        device_context->OMSetRenderTargets(1, &target, nullptr);
        if (clear) device_context->ClearRenderTargetView(target, clear_color);
        last_error_operation_ = {};
        return S_OK;
    }
    std::array<HANDLE, 3> available_events = {};
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        available_events[index] = buffers_[index].available_event;
    }
    DWORD wait_error = ERROR_SUCCESS;
    auto wait_event = presentation_trace::current;
    wait_event.name = "presentation_available_wait";
    wait_event.timeout_ms = availability_timeout_ms;
    wait_event.count = available_events.size();
    const DWORD wait_result = presentation_trace::Measure(wait_event, [&]() {
        const DWORD result = WaitForMultipleObjects(
        static_cast<DWORD>(available_events.size()),
        available_events.data(),
        FALSE,
        availability_timeout_ms);
        if (result == WAIT_FAILED) wait_error = GetLastError();
        return result;
    });
    if (wait_result == WAIT_TIMEOUT) {
        ++feedback_.buffer_acquire_skipped;
        last_error_operation_ = {};
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    if (wait_result >= WAIT_OBJECT_0 + available_events.size()) {
        const DWORD error = wait_result == WAIT_FAILED
                                      ? wait_error
                                      : ERROR_GEN_FAILURE;
        const HRESULT result = HRESULT_FROM_WIN32(
            error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
        return RecordFailure("IPresentationBuffer::GetAvailableEvent wait", result);
    }

    selected_buffer_ = static_cast<int>(wait_result - WAIT_OBJECT_0);
    ID3D11RenderTargetView* render_target =
        buffers_[static_cast<std::size_t>(selected_buffer_)].render_target.Get();
    device_context->OMSetRenderTargets(1, &render_target, nullptr);
    if (clear) {
        device_context->ClearRenderTargetView(render_target, clear_color);
    }
    last_error_operation_ = {};
    return S_OK;
}

HRESULT D3D11CompositionSwapChain::Present(
    ID3D11DeviceContext* device_context)
{
    if (device_context == nullptr || manager_ == nullptr ||
        presentation_surface_ == nullptr || selected_buffer_ < 0) {
        return RecordFailure(
            "D3D11CompositionSwapChain::Present arguments",
            E_INVALIDARG);
    }

    device_context->Flush();
    Buffer& buffer = buffers_[static_cast<std::size_t>(selected_buffer_)];
    HRESULT result = presentation_surface_->SetBuffer(buffer.presentation.Get());
    if (FAILED(result)) {
        selected_buffer_ = -1;
        return RecordFailure("IPresentationSurface::SetBuffer", result);
    }
    const int submitted_slot = selected_buffer_;
    result = manager_->Present();
    selected_buffer_ = -1;
    if (FAILED(result)) {
        return RecordFailure("IPresentationManager::Present", result);
    }
    ++feedback_.present_submissions;
    if (incremental_buffers_) {
        bound_buffer_ = submitted_slot;
        auto event = presentation_trace::current;
        event.name = "presentation_buffer_submission";
        event.resize_request_serial = resize_serial_;
        event.buffer_slot = submitted_slot;
        event.bound_buffer_slot = bound_buffer_;
        event.allocation_generation = event.bound_generation = buffer.identity.generation;
        event.new_width = buffer.identity.width;
        event.new_height = buffer.identity.height;
        event.buffer_action = "submitted";
        presentation_trace::Span submitted(event);
        submitted.Result(result);
    }
    last_error_operation_ = {};
    return S_OK;
}

D3D11CompositionFeedback D3D11CompositionSwapChain::TakeFeedback(bool drain) noexcept
{
    if (drain) DrainStatistics();
    const bool first_independent_flip =
        !independent_flip_reported_ && feedback_.independent_flip_frames > 0;
    const bool anomaly = feedback_.status_skipped > 0 ||
                         feedback_.status_canceled > 0 ||
                         FAILED(feedback_.statistics_error);
    if (!first_independent_flip && !anomaly &&
        feedback_.present_submissions < kFeedbackReportIntervalFrames &&
        feedback_.buffer_acquire_skipped < kFeedbackReportIntervalFrames) {
        return {};
    }
    independent_flip_reported_ =
        independent_flip_reported_ || first_independent_flip;
    return std::exchange(feedback_, D3D11CompositionFeedback{});
}

HRESULT D3D11CompositionSwapChain::RecordFailure(
    std::string_view operation,
    HRESULT result) noexcept
{
    last_error_operation_ = operation;
    return result;
}

HRESULT D3D11CompositionSwapChain::CreateBuffers(
    ID3D11Device* device,
    UINT width,
    UINT height)
{
    return presentation_trace::Measure("presentation_buffer_rebuild", "composition", [&]() -> HRESULT {
        if (device == nullptr || manager_ == nullptr || presentation_surface_ == nullptr ||
            width == 0 || height == 0) {
            return RecordFailure(
                "D3D11CompositionSwapChain::CreateBuffers arguments",
                E_INVALIDARG);
        }

        {
            auto event = presentation_trace::current; event.name = "presentation_buffer_reset";
            presentation_trace::Span span(event);
            ResetBuffers();
        }
        width_ = width;
        height_ = height;
        const RECT source_rect = {
            0,
            0,
            static_cast<LONG>(width),
            static_cast<LONG>(height),
        };
        HRESULT result = presentation_trace::Measure("presentation_source_rect", [&] {
            return presentation_surface_->SetSourceRect(&source_rect);
        });
        if (FAILED(result)) {
            return RecordFailure("IPresentationSurface::SetSourceRect", result);
        }

        return presentation_trace::Measure("presentation_buffer_allocation", [&]() -> HRESULT {
            for (Buffer& buffer : buffers_) {
                result = CreateBuffer(device, width, height, buffer);
                if (FAILED(result)) return result;
            }
            return S_OK;
        });
    });
}

HRESULT D3D11CompositionSwapChain::CreateBuffer(ID3D11Device* device, UINT width, UINT height, Buffer& buffer)
{
    const auto checkpoint = [&](unsigned step) {
        const HRESULT result = allocation_checkpoint_ ? allocation_checkpoint_(step) : S_OK;
        return FAILED(result) ? RecordFailure("buffer allocation test checkpoint", result) : S_OK;
    };
    HRESULT injected = checkpoint(1);
    if (FAILED(injected)) return injected;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
    HRESULT result = device->CreateTexture2D(
        &description,
        nullptr,
        buffer.texture.GetAddressOf());
    if (FAILED(result)) {
        return RecordFailure("ID3D11Device::CreateTexture2D(displayable)", result);
    }
    injected = checkpoint(2);
    if (FAILED(injected)) return injected;
    result = device->CreateRenderTargetView(
        buffer.texture.Get(),
        nullptr,
        buffer.render_target.GetAddressOf());
    if (FAILED(result)) {
        return RecordFailure("ID3D11Device::CreateRenderTargetView", result);
    }
    injected = checkpoint(3);
    if (FAILED(injected)) return injected;
    result = manager_->AddBufferFromResource(
        buffer.texture.Get(),
        buffer.presentation.GetAddressOf());
    if (FAILED(result)) {
        return RecordFailure("IPresentationManager::AddBufferFromResource", result);
    }
    injected = checkpoint(4);
    if (FAILED(injected)) return injected;
    result = buffer.presentation->GetAvailableEvent(&buffer.available_event);
    if (FAILED(result)) {
        return RecordFailure("IPresentationBuffer::GetAvailableEvent", result);
    }
    buffer.identity = {width, height, ++generation_};
    return S_OK;
}

void D3D11CompositionSwapChain::ResetBuffers() noexcept
{
    selected_buffer_ = -1;
    bound_buffer_ = -1;
    for (Buffer& buffer : buffers_) {
        ReleaseBuffer(buffer, static_cast<int>(&buffer - buffers_.data()));
    }
}

void D3D11CompositionSwapChain::ReleaseBuffer(Buffer& buffer, int slot) noexcept
{
    auto event = presentation_trace::current;
    event.name = "presentation_buffer_release";
    event.backend = "composition";
    event.buffer_slot = slot;
    event.allocation_generation = buffer.identity.generation;
    presentation_trace::Span span(event);
    buffer.Reset();
    buffer.identity = {};
}

HRESULT D3D11CompositionSwapChain::SelectIncrementalBuffer(ID3D11DeviceContext* context, std::uint64_t frame_id)
{
    std::array<bool, 3> available{};
    std::array<IncrementalBufferSlot, 3> identities{};
    std::uint64_t mask = 0;
    for (int i = 0; i < 3; ++i) {
        identities[i] = buffers_[i].identity;
        auto wait = presentation_trace::current;
        wait.name = "presentation_available_wait";
        wait.buffer_slot = i;
        wait.timeout_ms = 0;
        wait.count = 1;
        const DWORD result = presentation_trace::Measure(wait, [&] {
            return WaitForSingleObject(buffers_[i].available_event, 0);
        });
        if (result == WAIT_FAILED) return RecordFailure("incremental buffers availability", E_FAIL);
        available[i] = result == WAIT_OBJECT_0;
        if (available[i]) mask |= 1ULL << i;
    }
    const BufferPlan plan = PlanIncrementalBuffer(identities, available, bound_buffer_, width_, height_);
    auto event = presentation_trace::current;
    event.name = "presentation_buffer_selection";
    event.buffer_slot = plan.slot;
    event.resize_request_serial = resize_serial_;
    event.new_width = width_;
    event.new_height = height_;
    event.logical_bytes = plan.peak_bytes;
    event.count = mask;
    event.bound_buffer_slot = bound_buffer_;
    event.bound_generation = bound_buffer_ >= 0 ? buffers_[bound_buffer_].identity.generation : 0;
    if (plan.action == BufferPlanAction::BudgetExceeded) {
        event.buffer_action = "budget_failure";
        return presentation_trace::Measure(event, [&] { return RecordFailure("incremental buffers replacement budget", E_OUTOFMEMORY); });
    }
    if (plan.action == BufferPlanAction::Skip ||
        (plan.action == BufferPlanAction::Replace && frame_id != 0 && frame_id == last_replacement_frame_)) {
        event.buffer_action = plan.action == BufferPlanAction::Skip ? "skip_unavailable" : "skip_frame_budget";
        return presentation_trace::Measure(event, [&] {
            ++feedback_.buffer_acquire_skipped;
            last_error_operation_ = {};
            return DXGI_ERROR_WAS_STILL_DRAWING;
        });
    }
    event.buffer_action = plan.action == BufferPlanAction::Replace ? "replace" : "select";
    event.allocation_generation = plan.action == BufferPlanAction::Replace ? generation_ + 1 : buffers_[plan.slot].identity.generation;
    return presentation_trace::Measure(event, [&]() -> HRESULT {
        if (plan.action == BufferPlanAction::Replace) {
            last_replacement_frame_ = frame_id;
            Buffer replacement;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(device.GetAddressOf());
            auto replace = event;
            replace.name = "presentation_buffer_replace";
            const HRESULT result = presentation_trace::Measure(replace, [&]() -> HRESULT {
                const HRESULT allocation = CreateBuffer(device.Get(), width_, height_, replacement);
                if (FAILED(allocation)) {
                    ReleaseBuffer(replacement, plan.slot);
                    return allocation;
                }
                // Only commit a fully constructed slot. The temporary then owns the old resources.
                std::swap(buffers_[plan.slot], replacement);
                ReleaseBuffer(replacement, plan.slot);
                return S_OK;
            });
            if (FAILED(result)) return result;
        }
        // A newly registered buffer must also be observed available before rendering.
        auto wait = event;
        wait.name = "presentation_available_wait";
        wait.timeout_ms = 0;
        wait.count = 1;
        const DWORD ready = presentation_trace::Measure(wait, [&] {
            return WaitForSingleObject(buffers_[plan.slot].available_event, 0);
        });
        if (ready == WAIT_TIMEOUT) {
            ++feedback_.buffer_acquire_skipped;
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        if (ready != WAIT_OBJECT_0) return RecordFailure("incremental buffers selected availability", E_FAIL);
        const RECT rect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        const HRESULT result = presentation_trace::Measure("presentation_source_rect", [&] {
            return presentation_surface_->SetSourceRect(&rect);
        });
        if (FAILED(result)) return RecordFailure("incremental buffers source rectangle", result);
        selected_buffer_ = plan.slot;
        return S_OK;
    });
}

void D3D11CompositionSwapChain::DrainStatistics() noexcept
{
    if (statistics_event_ == nullptr || manager_ == nullptr) {
        return;
    }
    auto drain_event = presentation_trace::current;
    drain_event.name = "presentation_statistics_drain";
    drain_event.backend = "composition";
    drain_event.timeout_ms = 0;
    presentation_trace::Span drain(drain_event);
    std::uint64_t items = 0;
    while (presentation_trace::Measure("presentation_statistics_poll", [&] {
        return WaitForSingleObject(statistics_event_, 0);
    }) == WAIT_OBJECT_0) {
        drain.Count(++items);
        auto item_event = presentation_trace::current;
        item_event.name = "presentation_statistics_item";
        presentation_trace::Span item(item_event);
        Microsoft::WRL::ComPtr<IPresentStatistics> statistics;
        const HRESULT result = presentation_trace::Measure("presentation_statistics_get_next", [&] {
            return manager_->GetNextPresentStatistics(statistics.GetAddressOf());
        });
        item.Result(result);
        if (FAILED(result)) {
            feedback_.statistics_error = result;
            CloseHandle(std::exchange(statistics_event_, nullptr));
            return;
        }
        switch (statistics->GetKind()) {
        case PresentStatisticsKind_PresentStatus: {
            Microsoft::WRL::ComPtr<IPresentStatusPresentStatistics> status;
            if (SUCCEEDED(statistics.As(&status))) {
                switch (status->GetPresentStatus()) {
                case PresentStatus_Queued:
                    ++feedback_.status_queued;
                    break;
                case PresentStatus_Skipped:
                    ++feedback_.status_skipped;
                    break;
                case PresentStatus_Canceled:
                    ++feedback_.status_canceled;
                    break;
                default:
                    break;
                }
            }
            break;
        }
        case PresentStatisticsKind_CompositionFrame:
            ++feedback_.composition_frames;
            break;
        case PresentStatisticsKind_IndependentFlipFrame: {
            Microsoft::WRL::ComPtr<IIndependentFlipFramePresentStatistics>
                independent_flip;
            if (SUCCEEDED(statistics.As(&independent_flip)) &&
                independent_flip->GetContentTag() == content_tag_) {
                ++feedback_.independent_flip_frames;
                feedback_.last_actual_duration =
                    independent_flip->GetPresentDuration().value;
                feedback_.last_displayed_time =
                    independent_flip->GetDisplayedTime().value;
            }
            break;
        }
        default:
            break;
        }
    }
}

}  // namespace spectiary
