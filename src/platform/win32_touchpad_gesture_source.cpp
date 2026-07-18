#include "platform/win32_touchpad_gesture_source.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <directmanipulation.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Implements;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::RuntimeClassType;

constexpr UINT kTouchpadGestureWakeMessage = WM_APP + 0x53;
constexpr float kTransformEpsilon = 1.0e-5f;

enum class GesturePhase {
    None,
    Pan,
    Pinch,
};

struct SharedGestureState {
    std::mutex mutex;
    PlotTouchpadTarget target;
    PlotGestureAxes axes = PlotGestureAxes::Both;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float last_scale = 1.0f;
    float last_x_offset = 0.0f;
    float last_y_offset = 0.0f;
    GesturePhase phase = GesturePhase::None;
    bool has_contact = false;
    bool active = false;
    bool inertia = false;
    bool wake_pending = false;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    std::vector<PlotTouchpadGestureDelta> deltas;
};

bool NearlyEqual(float left, float right)
{
    const float base = std::max({std::abs(left), std::abs(right), kTransformEpsilon});
    return std::abs(left - right) < kTransformEpsilon * base;
}

void PostWake(HWND hwnd, const std::shared_ptr<SharedGestureState>& state)
{
    const auto post_message = [](std::uintptr_t native_window, std::uint32_t message) noexcept {
        return PostMessageW(
                   reinterpret_cast<HWND>(native_window),
                   static_cast<UINT>(message),
                   0,
                   0) != FALSE;
    };
    std::lock_guard lock(state->mutex);
    (void)TryPostWin32TouchpadWake(
        reinterpret_cast<std::uintptr_t>(hwnd),
        kTouchpadGestureWakeMessage,
        state->wake_pending,
        post_message);
}

class DirectManipulationEventHandler final
    : public RuntimeClass<
          RuntimeClassFlags<RuntimeClassType::ClassicCom>,
          Implements<
              RuntimeClassFlags<RuntimeClassType::ClassicCom>,
              FtmBase,
              IDirectManipulationViewportEventHandler>> {
public:
    DirectManipulationEventHandler(HWND hwnd, std::shared_ptr<SharedGestureState> state)
        : hwnd_(hwnd), state_(std::move(state))
    {
    }

    HRESULT STDMETHODCALLTYPE OnViewportStatusChanged(
        IDirectManipulationViewport* viewport,
        DIRECTMANIPULATION_STATUS current,
        DIRECTMANIPULATION_STATUS previous) override
    {
        if (viewport == nullptr) {
            return E_POINTER;
        }

        bool reset_transform = false;
        float reset_width = 0.0f;
        float reset_height = 0.0f;
        {
            std::lock_guard lock(state_->mutex);
            if (current == DIRECTMANIPULATION_RUNNING) {
                state_->active = true;
                state_->inertia = false;
                if (previous == DIRECTMANIPULATION_INERTIA) {
                    state_->phase = GesturePhase::None;
                }
            } else if (current == DIRECTMANIPULATION_INERTIA) {
                state_->active = state_->phase == GesturePhase::Pan;
                state_->inertia = state_->active;
            } else if (current == DIRECTMANIPULATION_READY) {
                state_->active = false;
                state_->inertia = false;
                state_->has_contact = false;
                reset_transform = !NearlyEqual(state_->last_scale, 1.0f) ||
                                  !NearlyEqual(state_->last_x_offset, 0.0f) ||
                                  !NearlyEqual(state_->last_y_offset, 0.0f);
                reset_width = state_->viewport_width;
                reset_height = state_->viewport_height;
            } else if (
                current == DIRECTMANIPULATION_DISABLED ||
                current == DIRECTMANIPULATION_SUSPENDED) {
                state_->active = false;
                state_->inertia = false;
                state_->has_contact = false;
            }
        }

        if (reset_transform && reset_width > 0.0f && reset_height > 0.0f) {
            (void)viewport->ZoomToRect(0.0f, 0.0f, reset_width, reset_height, FALSE);
        }

        if (current == DIRECTMANIPULATION_READY) {
            std::lock_guard lock(state_->mutex);
            state_->last_scale = 1.0f;
            state_->last_x_offset = 0.0f;
            state_->last_y_offset = 0.0f;
            state_->phase = GesturePhase::None;
        }

        PostWake(hwnd_, state_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnViewportUpdated(IDirectManipulationViewport*) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnContentUpdated(
        IDirectManipulationViewport* viewport,
        IDirectManipulationContent* content) override
    {
        if (viewport == nullptr || content == nullptr) {
            return E_POINTER;
        }

        float transform[6] = {};
        const HRESULT result = content->GetContentTransform(transform, ARRAYSIZE(transform));
        if (FAILED(result)) {
            return result;
        }

        const float scale = transform[0];
        const float x_offset = transform[4];
        const float y_offset = transform[5];
        if (!std::isfinite(scale) || scale <= 0.0f ||
            !std::isfinite(x_offset) || !std::isfinite(y_offset)) {
            return S_OK;
        }

        bool queued = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->has_contact && !state_->active) {
                return S_OK;
            }

            PlotTouchpadGestureDelta delta;
            delta.axes = state_->axes;
            delta.plot_rect = state_->target.plot_rect;
            delta.anchor_x = state_->anchor_x;
            delta.anchor_y = state_->anchor_y;
            delta.inertia = state_->inertia;
            delta.input_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();

            if (state_->phase == GesturePhase::Pinch || !NearlyEqual(scale, 1.0f)) {
                const double factor = static_cast<double>(scale) /
                                      static_cast<double>(state_->last_scale);
                if (std::isfinite(factor) && factor > 0.0 && !NearlyEqual(scale, state_->last_scale)) {
                    delta.kind = PlotTouchpadGestureKind::Zoom;
                    delta.zoom_factor = factor;
                    state_->deltas.push_back(delta);
                    queued = true;
                }
                state_->phase = GesturePhase::Pinch;
            } else {
                const float pan_x = x_offset - state_->last_x_offset;
                const float pan_y = y_offset - state_->last_y_offset;
                if (!NearlyEqual(pan_x, 0.0f) || !NearlyEqual(pan_y, 0.0f)) {
                    delta.kind = PlotTouchpadGestureKind::Pan;
                    delta.pan_x = pan_x;
                    delta.pan_y = pan_y;
                    state_->deltas.push_back(delta);
                    queued = true;
                }
                if (queued && state_->phase == GesturePhase::None) {
                    state_->phase = GesturePhase::Pan;
                }
            }

            state_->last_scale = scale;
            state_->last_x_offset = x_offset;
            state_->last_y_offset = y_offset;
        }

        if (queued) {
            PostWake(hwnd_, state_);
        }
        return S_OK;
    }

private:
    HWND hwnd_ = nullptr;
    std::shared_ptr<SharedGestureState> state_;
};

PlotPixelRect TargetBounds(const PlotTouchpadTarget& target)
{
    PlotPixelRect bounds;
    bool initialized = false;
    for (const PlotPixelRect& rect : {target.plot_rect, target.x_axis_rect, target.y_axis_rect}) {
        if (!rect.IsValid()) {
            continue;
        }
        if (!initialized) {
            bounds = rect;
            initialized = true;
            continue;
        }
        bounds.left = std::min(bounds.left, rect.left);
        bounds.top = std::min(bounds.top, rect.top);
        bounds.right = std::max(bounds.right, rect.right);
        bounds.bottom = std::max(bounds.bottom, rect.bottom);
    }
    return bounds;
}

bool TargetClientRect(HWND hwnd, const PlotTouchpadTarget& target, RECT& rect)
{
    const PlotPixelRect bounds = TargetBounds(target);
    if (!bounds.IsValid()) {
        return false;
    }

    POINT top_left{
        static_cast<LONG>(std::floor(bounds.left)),
        static_cast<LONG>(std::floor(bounds.top))};
    POINT bottom_right{
        static_cast<LONG>(std::ceil(bounds.right)),
        static_cast<LONG>(std::ceil(bounds.bottom))};
    if (!ScreenToClient(hwnd, &top_left) || !ScreenToClient(hwnd, &bottom_right)) {
        return false;
    }

    rect = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool SameRect(const RECT& left, const RECT& right)
{
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

}  // namespace

struct Win32TouchpadGestureSource::Impl {
    class Context {
    public:
        static std::unique_ptr<Context> Create(const PlotTouchpadTarget& target)
        {
            const HWND hwnd = reinterpret_cast<HWND>(target.native_window);
            if (hwnd == nullptr || !IsWindow(hwnd)) {
                return nullptr;
            }

            RECT viewport_rect = {};
            if (!TargetClientRect(hwnd, target, viewport_rect)) {
                return nullptr;
            }

            auto context = std::unique_ptr<Context>(new Context(hwnd));
            HRESULT result = CoCreateInstance(
                CLSID_DirectManipulationManager,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&context->manager_));
            if (FAILED(result)) {
                return nullptr;
            }

            result = context->manager_->GetUpdateManager(IID_PPV_ARGS(&context->update_manager_));
            if (FAILED(result)) {
                return nullptr;
            }

            result = context->manager_->CreateViewport(
                nullptr,
                hwnd,
                IID_PPV_ARGS(&context->viewport_));
            if (FAILED(result)) {
                return nullptr;
            }

            constexpr DIRECTMANIPULATION_CONFIGURATION configuration =
                DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
                DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
                DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y |
                DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_INERTIA |
                DIRECTMANIPULATION_CONFIGURATION_RAILS_X |
                DIRECTMANIPULATION_CONFIGURATION_RAILS_Y |
                DIRECTMANIPULATION_CONFIGURATION_SCALING;
            result = context->viewport_->ActivateConfiguration(configuration);
            if (FAILED(result)) {
                return nullptr;
            }

            result = context->viewport_->SetViewportOptions(
                DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE);
            if (FAILED(result)) {
                return nullptr;
            }

            result = context->viewport_->SetViewportRect(&viewport_rect);
            if (FAILED(result)) {
                return nullptr;
            }
            context->viewport_rect_ = viewport_rect;

            result = context->manager_->Activate(hwnd);
            if (FAILED(result)) {
                return nullptr;
            }
            context->manager_active_ = true;

            result = context->viewport_->Enable();
            if (FAILED(result)) {
                return nullptr;
            }

            context->state_->target = target;
            context->state_->viewport_width =
                static_cast<float>(viewport_rect.right - viewport_rect.left);
            context->state_->viewport_height =
                static_cast<float>(viewport_rect.bottom - viewport_rect.top);
            context->event_handler_ = Make<DirectManipulationEventHandler>(hwnd, context->state_);
            if (context->event_handler_ == nullptr) {
                return nullptr;
            }

            result = context->viewport_->AddEventHandler(
                hwnd,
                context->event_handler_.Get(),
                &context->event_handler_cookie_);
            if (FAILED(result)) {
                return nullptr;
            }
            context->event_handler_registered_ = true;

            if (!SetWindowSubclass(
                    hwnd,
                    &Context::SubclassProc,
                    reinterpret_cast<UINT_PTR>(context.get()),
                    reinterpret_cast<DWORD_PTR>(context.get()))) {
                return nullptr;
            }
            context->subclass_registered_ = true;

            (void)context->update_manager_->Update(nullptr);
            return context;
        }

        ~Context()
        {
            if (subclass_registered_ && IsWindow(hwnd_)) {
                (void)RemoveWindowSubclass(
                    hwnd_,
                    &Context::SubclassProc,
                    reinterpret_cast<UINT_PTR>(this));
            }
            if (viewport_ != nullptr) {
                (void)viewport_->Stop();
                if (event_handler_registered_) {
                    (void)viewport_->RemoveEventHandler(event_handler_cookie_);
                }
                (void)viewport_->Abandon();
            }
            if (manager_active_ && manager_ != nullptr && IsWindow(hwnd_)) {
                (void)manager_->Deactivate(hwnd_);
            }
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        [[nodiscard]] HWND hwnd() const
        {
            return hwnd_;
        }

        [[nodiscard]] bool NeedsContinuousUpdates() const
        {
            std::lock_guard lock(state_->mutex);
            return state_->active;
        }

        void SetTarget(const PlotTouchpadTarget& target)
        {
            RECT viewport_rect = {};
            if (!TargetClientRect(hwnd_, target, viewport_rect)) {
                return;
            }

            {
                std::lock_guard lock(state_->mutex);
                state_->target = target;
                state_->viewport_width =
                    static_cast<float>(viewport_rect.right - viewport_rect.left);
                state_->viewport_height =
                    static_cast<float>(viewport_rect.bottom - viewport_rect.top);
            }

            if (SameRect(viewport_rect_, viewport_rect)) {
                return;
            }

            (void)viewport_->Stop();
            if (SUCCEEDED(viewport_->SetViewportRect(&viewport_rect))) {
                viewport_rect_ = viewport_rect;
            }
        }

        [[nodiscard]] PlotTouchpadGestureBatch Poll()
        {
            (void)update_manager_->Update(nullptr);

            PlotTouchpadGestureBatch batch;
            std::lock_guard lock(state_->mutex);
            batch.deltas = std::move(state_->deltas);
            state_->deltas.clear();
            batch.active = state_->active;
            state_->wake_pending = false;
            return batch;
        }

        void PumpUpdates()
        {
            (void)update_manager_->Update(nullptr);
        }

    private:
        explicit Context(HWND hwnd)
            : hwnd_(hwnd), state_(std::make_shared<SharedGestureState>())
        {
        }

        static LRESULT CALLBACK SubclassProc(
            HWND hwnd,
            UINT message,
            WPARAM wparam,
            LPARAM lparam,
            UINT_PTR,
            DWORD_PTR reference)
        {
            auto* context = reinterpret_cast<Context*>(reference);
            if (context != nullptr && message == DM_POINTERHITTEST &&
                context->OnPointerHitTest(wparam)) {
                return 0;
            }
            return DefSubclassProc(hwnd, message, wparam, lparam);
        }

        bool OnPointerHitTest(WPARAM wparam)
        {
            const UINT32 pointer_id = GET_POINTERID_WPARAM(wparam);
            POINTER_INPUT_TYPE pointer_type = PT_POINTER;
            if (!GetPointerType(pointer_id, &pointer_type) || pointer_type != PT_TOUCHPAD) {
                return false;
            }

            POINT screen_point = {};
            POINTER_INFO pointer_info = {};
            if (GetPointerInfo(pointer_id, &pointer_info)) {
                screen_point = pointer_info.ptPixelLocation;
            } else if (!GetCursorPos(&screen_point)) {
                return false;
            }

            PlotTouchpadTarget target;
            {
                std::lock_guard lock(state_->mutex);
                target = state_->target;
            }

            bool hit = false;
            const PlotGestureAxes axes = HitTestPlotTouchpadTarget(
                target,
                static_cast<float>(screen_point.x),
                static_cast<float>(screen_point.y),
                &hit);
            if (!hit) {
                return false;
            }

            {
                std::lock_guard lock(state_->mutex);
                state_->axes = axes;
                state_->anchor_x = static_cast<float>(screen_point.x);
                state_->anchor_y = static_cast<float>(screen_point.y);
                state_->last_scale = 1.0f;
                state_->last_x_offset = 0.0f;
                state_->last_y_offset = 0.0f;
                state_->phase = GesturePhase::None;
                state_->has_contact = true;
                state_->active = false;
                state_->inertia = false;
            }

            const HRESULT result = viewport_->SetContact(pointer_id);
            {
                std::lock_guard lock(state_->mutex);
                if (SUCCEEDED(result)) {
                    state_->active = true;
                } else {
                    state_->has_contact = false;
                    state_->active = false;
                    state_->inertia = false;
                    state_->phase = GesturePhase::None;
                    state_->deltas.clear();
                }
            }
            return SUCCEEDED(result);
        }

        HWND hwnd_ = nullptr;
        RECT viewport_rect_ = {};
        bool manager_active_ = false;
        bool event_handler_registered_ = false;
        bool subclass_registered_ = false;
        DWORD event_handler_cookie_ = 0;
        std::shared_ptr<SharedGestureState> state_;
        ComPtr<IDirectManipulationManager> manager_;
        ComPtr<IDirectManipulationUpdateManager> update_manager_;
        ComPtr<IDirectManipulationViewport> viewport_;
        ComPtr<DirectManipulationEventHandler> event_handler_;
    };

    ~Impl()
    {
        context.reset();
        if (owns_com_apartment) {
            CoUninitialize();
        }
    }

    bool EnsureComApartment()
    {
        if (com_attempted) {
            return com_available;
        }

        com_attempted = true;
        const HRESULT result = CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(result)) {
            owns_com_apartment = true;
            com_available = true;
        } else if (result == RPC_E_CHANGED_MODE) {
            com_available = true;
        }
        return com_available;
    }

    std::unique_ptr<Context> context;
    HWND failed_window = nullptr;
    std::chrono::steady_clock::time_point retry_after = {};
    bool com_attempted = false;
    bool com_available = false;
    bool owns_com_apartment = false;
};

Win32TouchpadGestureSource::Win32TouchpadGestureSource()
    : impl_(std::make_unique<Impl>())
{
}

Win32TouchpadGestureSource::~Win32TouchpadGestureSource() = default;

PlotTouchpadGestureBatch Win32TouchpadGestureSource::Poll(std::uintptr_t native_window)
{
    if (impl_->context == nullptr ||
        reinterpret_cast<std::uintptr_t>(impl_->context->hwnd()) != native_window) {
        return {};
    }
    return impl_->context->Poll();
}

void Win32TouchpadGestureSource::SetTarget(const PlotTouchpadTarget& target)
{
    const HWND hwnd = reinterpret_cast<HWND>(target.native_window);
    if (hwnd == nullptr || !target.plot_rect.IsValid()) {
        ClearTarget();
        return;
    }

    if (impl_->context != nullptr && impl_->context->hwnd() == hwnd) {
        impl_->context->SetTarget(target);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (impl_->failed_window == hwnd && now < impl_->retry_after) {
        return;
    }

    impl_->context.reset();
    if (!impl_->EnsureComApartment()) {
        return;
    }
    impl_->context = Impl::Context::Create(target);
    if (impl_->context == nullptr) {
        impl_->failed_window = hwnd;
        impl_->retry_after = now + std::chrono::seconds(2);
    } else {
        impl_->failed_window = nullptr;
        impl_->retry_after = {};
    }
}

void Win32TouchpadGestureSource::ClearTarget()
{
    impl_->context.reset();
    impl_->failed_window = nullptr;
    impl_->retry_after = {};
}

void Win32TouchpadGestureSource::PumpUpdates()
{
    if (impl_->context != nullptr) {
        impl_->context->PumpUpdates();
    }
}

bool Win32TouchpadGestureSource::OwnsWindow(std::uintptr_t native_window) const noexcept
{
    return impl_->context != nullptr &&
           reinterpret_cast<std::uintptr_t>(impl_->context->hwnd()) == native_window;
}

bool Win32TouchpadGestureSource::NeedsContinuousUpdates() const
{
    return impl_->context != nullptr && impl_->context->NeedsContinuousUpdates();
}

}  // namespace specforge
