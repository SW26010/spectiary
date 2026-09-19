#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace spectiary {

[[nodiscard]] bool Win32MessageCanInvalidateRender(
    std::uint32_t message,
    std::optional<std::uint32_t> permission_only_message = std::nullopt) noexcept;

struct Win32ObservedMessage {
    std::uintptr_t hwnd = 0;
    std::uint32_t message = 0;
    std::uintptr_t wparam = 0;
    std::intptr_t lparam = 0;
};

class Win32MessageRenderObserver {
public:
    using InvalidateCallback = void (*)(void* context) noexcept;
    using MessageCallback = void (*)(void* context, const Win32ObservedMessage& message) noexcept;

    Win32MessageRenderObserver();
    ~Win32MessageRenderObserver();

    Win32MessageRenderObserver(const Win32MessageRenderObserver&) = delete;
    Win32MessageRenderObserver& operator=(const Win32MessageRenderObserver&) = delete;

    [[nodiscard]] bool Start(
        InvalidateCallback callback,
        void* context,
        std::optional<std::uint32_t> permission_only_message = std::nullopt,
        MessageCallback message_callback = nullptr) noexcept;
    void ObserveQueuedMessage(std::uint32_t message) noexcept;
    void ObserveQueuedMessage(const Win32ObservedMessage& message) noexcept;
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spectiary
