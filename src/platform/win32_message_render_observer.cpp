#include "platform/win32_message_render_observer.h"
#include "profile/presentation_trace.h"
#include "platform/win32_native_size_trace.h"

#include <Windows.h>

#include <utility>

namespace spectiary {

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
            if (message != nullptr) {
                const DWORD saved_error = GetLastError();
                native_size_trace::Enter(message->hwnd, message->message);
                SetLastError(saved_error);
            }
            if (message != nullptr && (message->message == WM_ENTERSIZEMOVE ||
                                       message->message == WM_EXITSIZEMOVE)) {
                presentation_trace::SizeMove(
                    reinterpret_cast<std::uintptr_t>(message->hwnd),
                    message->message == WM_ENTERSIZEMOVE);
            }
            if (message != nullptr && Win32MessageCanInvalidateRender(
                                          message->message,
                                          observer->permission_only_message)) {
                observer->callback(observer->context);
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    HHOOK hook = nullptr;
    HHOOK return_hook = nullptr;
    static LRESULT CALLBACK ReturnHookProcedure(int code, WPARAM wparam, LPARAM lparam)
    {
        if (code >= 0 && active != nullptr) {
            const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
            const DWORD saved_error = GetLastError();
            if (message != nullptr) native_size_trace::Leave(message->hwnd, message->message);
            SetLastError(saved_error);
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    InvalidateCallback callback = nullptr;
    MessageCallback message_callback = nullptr;
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
    std::optional<std::uint32_t> permission_only_message,
    MessageCallback message_callback) noexcept
{
    if (callback == nullptr || impl_->hook != nullptr || Impl::active != nullptr) {
        return false;
    }

    impl_->callback = callback;
    impl_->message_callback = message_callback;
    impl_->context = context;
    impl_->permission_only_message = permission_only_message;
    Impl::active = impl_.get();
    impl_->hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        &Impl::HookProcedure,
        nullptr,
        GetCurrentThreadId());
    if (impl_->hook != nullptr) {
        impl_->return_hook = SetWindowsHookExW(
            WH_CALLWNDPROCRET, &Impl::ReturnHookProcedure, nullptr, GetCurrentThreadId());
        native_size_trace::hook_available = impl_->return_hook != nullptr;
        return true;
    }

    Impl::active = nullptr;
    impl_->callback = nullptr;
    impl_->message_callback = nullptr;
    impl_->context = nullptr;
    impl_->permission_only_message.reset();
    return false;
}

void Win32MessageRenderObserver::ObserveQueuedMessage(std::uint32_t message) noexcept
{
    ObserveQueuedMessage(Win32ObservedMessage{.message = message});
}

void Win32MessageRenderObserver::ObserveQueuedMessage(
    const Win32ObservedMessage& message) noexcept
{
    if (impl_->message_callback != nullptr) {
        impl_->message_callback(impl_->context, message);
    }
    if (impl_->callback != nullptr &&
        Win32MessageCanInvalidateRender(message.message, impl_->permission_only_message)) {
        impl_->callback(impl_->context);
    }
}

void Win32MessageRenderObserver::Stop() noexcept
{
    if (impl_->return_hook != nullptr) {
        (void)UnhookWindowsHookEx(std::exchange(impl_->return_hook, nullptr));
    }
    if (impl_->hook != nullptr) {
        (void)UnhookWindowsHookEx(std::exchange(impl_->hook, nullptr));
    }
    if (Impl::active == impl_.get()) {
        native_size_trace::hook_available = false;
        Impl::active = nullptr;
    }
    impl_->callback = nullptr;
    impl_->message_callback = nullptr;
    impl_->context = nullptr;
    impl_->permission_only_message.reset();
}

}  // namespace spectiary
