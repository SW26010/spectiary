#include "platform/win32_message_render_observer.h"

#include <Windows.h>

#include <utility>

namespace specforge {

bool Win32MessageCanInvalidateRender(
    std::uint32_t message,
    std::optional<std::uint32_t> permission_only_message) noexcept
{
    return message != WM_NCHITTEST &&
           (!permission_only_message || message != *permission_only_message);
}

struct Win32MessageRenderObserver::Impl {
    static thread_local Impl* active;

    static LRESULT CALLBACK HookProcedure(int code, WPARAM wparam, LPARAM lparam)
    {
        Impl* observer = active;
        if (code >= 0 && observer != nullptr && observer->callback != nullptr) {
            const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
            if (message != nullptr && Win32MessageCanInvalidateRender(
                                          message->message,
                                          observer->permission_only_message)) {
                observer->callback(observer->context);
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    HHOOK hook = nullptr;
    InvalidateCallback callback = nullptr;
    void* context = nullptr;
    std::optional<std::uint32_t> permission_only_message;
};

thread_local Win32MessageRenderObserver::Impl* Win32MessageRenderObserver::Impl::active = nullptr;

Win32MessageRenderObserver::Win32MessageRenderObserver()
    : impl_(std::make_unique<Impl>())
{
}

Win32MessageRenderObserver::~Win32MessageRenderObserver()
{
    Stop();
}

bool Win32MessageRenderObserver::Start(
    InvalidateCallback callback,
    void* context,
    std::optional<std::uint32_t> permission_only_message) noexcept
{
    if (callback == nullptr || impl_->hook != nullptr || Impl::active != nullptr) {
        return false;
    }

    impl_->callback = callback;
    impl_->context = context;
    impl_->permission_only_message = permission_only_message;
    Impl::active = impl_.get();
    impl_->hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        &Impl::HookProcedure,
        nullptr,
        GetCurrentThreadId());
    if (impl_->hook != nullptr) {
        return true;
    }

    Impl::active = nullptr;
    impl_->callback = nullptr;
    impl_->context = nullptr;
    impl_->permission_only_message.reset();
    return false;
}

void Win32MessageRenderObserver::ObserveQueuedMessage(std::uint32_t message) noexcept
{
    if (impl_->callback != nullptr &&
        Win32MessageCanInvalidateRender(message, impl_->permission_only_message)) {
        impl_->callback(impl_->context);
    }
}

void Win32MessageRenderObserver::Stop() noexcept
{
    if (impl_->hook != nullptr) {
        (void)UnhookWindowsHookEx(std::exchange(impl_->hook, nullptr));
    }
    if (Impl::active == impl_.get()) {
        Impl::active = nullptr;
    }
    impl_->callback = nullptr;
    impl_->context = nullptr;
    impl_->permission_only_message.reset();
}

}  // namespace specforge
