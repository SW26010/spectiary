#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>

namespace spectiary {

struct Win32CompositorClockApi {
    using BoostFunction = HRESULT(WINAPI*)(BOOL enable);
    using WaitFunction = DWORD(WINAPI*)(UINT count, const HANDLE* handles, DWORD timeout_in_ms);

    BoostFunction boost = nullptr;
    WaitFunction wait = nullptr;
};

class Win32CompositorClock {
public:
    Win32CompositorClock();
    explicit Win32CompositorClock(Win32CompositorClockApi api);
    ~Win32CompositorClock();

    Win32CompositorClock(const Win32CompositorClock&) = delete;
    Win32CompositorClock& operator=(const Win32CompositorClock&) = delete;

    [[nodiscard]] bool Initialize(HWND tick_window, std::uint32_t tick_message) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool SetBoostRequested(bool requested) noexcept;
    [[nodiscard]] bool ConsumeTick() noexcept;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool boost_requested() const noexcept;
    [[nodiscard]] bool boost_active() const noexcept;
    [[nodiscard]] HRESULT last_boost_result() const noexcept;
    [[nodiscard]] DWORD last_wait_result() const noexcept;
    [[nodiscard]] std::uint64_t tick_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spectiary
