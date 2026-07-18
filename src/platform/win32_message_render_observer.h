#pragma once

#include <cstdint>
#include <memory>

namespace specforge {

[[nodiscard]] bool Win32MessageCanInvalidateRender(std::uint32_t message) noexcept;

class Win32MessageRenderObserver {
public:
    using InvalidateCallback = void (*)(void* context) noexcept;

    Win32MessageRenderObserver();
    ~Win32MessageRenderObserver();

    Win32MessageRenderObserver(const Win32MessageRenderObserver&) = delete;
    Win32MessageRenderObserver& operator=(const Win32MessageRenderObserver&) = delete;

    [[nodiscard]] bool Start(InvalidateCallback callback, void* context) noexcept;
    void ObserveQueuedMessage(std::uint32_t message) noexcept;
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
