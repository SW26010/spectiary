#include "platform/win32_compositor_clock.h"

#include <atomic>
#include <thread>
#include <utility>

namespace specforge {
namespace {

constexpr wchar_t kDirectCompositionLibrary[] = L"dcomp.dll";
constexpr char kBoostExport[] = "DCompositionBoostCompositorClock";
constexpr char kWaitExport[] = "DCompositionWaitForCompositorClock";

template <typename Function>
Function LoadFunction(HMODULE module, const char* name) noexcept
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

}  // namespace

struct Win32CompositorClock::Impl {
    explicit Impl(Win32CompositorClockApi configured_api = {})
        : configured_api(configured_api)
    {
    }

    [[nodiscard]] bool Initialize(HWND new_tick_window, std::uint32_t new_tick_message) noexcept
    {
        Shutdown();
        last_wait_result.store(WAIT_TIMEOUT, std::memory_order_relaxed);
        tick_count.store(0, std::memory_order_relaxed);
        if (new_tick_window == nullptr || new_tick_message < WM_APP || new_tick_message > 0xbfffU) {
            last_boost_result = E_INVALIDARG;
            return false;
        }

        api = configured_api;
        if (api.boost == nullptr && api.wait == nullptr) {
            module = LoadLibraryExW(
                kDirectCompositionLibrary,
                nullptr,
                LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module == nullptr) {
                last_boost_result = HRESULT_FROM_WIN32(GetLastError());
                return false;
            }
            api.boost = LoadFunction<Win32CompositorClockApi::BoostFunction>(module, kBoostExport);
            api.wait = LoadFunction<Win32CompositorClockApi::WaitFunction>(module, kWaitExport);
        }

        if (api.boost == nullptr || api.wait == nullptr) {
            last_boost_result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            ReleaseModule();
            api = {};
            return false;
        }

        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event == nullptr) {
            last_boost_result = HRESULT_FROM_WIN32(GetLastError());
            ReleaseModule();
            api = {};
            return false;
        }

        tick_window = new_tick_window;
        tick_message = new_tick_message;
        last_boost_result = S_OK;
        return true;
    }

    void Shutdown() noexcept
    {
        boost_requested = false;
        StopTicker();
        ReleaseBoost();

        if (stop_event != nullptr) {
            CloseHandle(std::exchange(stop_event, nullptr));
        }
        tick_window = nullptr;
        tick_message = 0;
        tick_pending.store(false, std::memory_order_relaxed);
        api = {};
        ReleaseModule();
    }

    [[nodiscard]] bool SetBoostRequested(bool requested) noexcept
    {
        if (!available()) {
            boost_requested = requested;
            return false;
        }
        if (requested == boost_requested) {
            return boost_active();
        }

        boost_requested = requested;
        if (!requested) {
            StopTicker();
            ReleaseBoost();
            return false;
        }

        last_boost_result = api.boost(TRUE);
        if (FAILED(last_boost_result)) {
            return false;
        }
        boost_api_enabled = true;
        ResetEvent(stop_event);
        ticker_running.store(true, std::memory_order_release);

        try {
            ticker = std::thread([this]() { RunTicker(); });
        } catch (...) {
            ticker_running.store(false, std::memory_order_release);
            ReleaseBoost();
            last_boost_result = E_FAIL;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ConsumeTick() noexcept
    {
        return tick_pending.exchange(false, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool available() const noexcept
    {
        return api.boost != nullptr && api.wait != nullptr && stop_event != nullptr;
    }

    [[nodiscard]] bool boost_active() const noexcept
    {
        return boost_api_enabled && ticker_running.load(std::memory_order_acquire);
    }

    void RunTicker() noexcept
    {
        const HANDLE wait_handle = stop_event;
        bool unexpected_exit = false;
        while (true) {
            const DWORD result = api.wait(1, &wait_handle, INFINITE);
            if (result == WAIT_OBJECT_0) {
                break;
            }
            last_wait_result.store(result, std::memory_order_relaxed);
            if (result != WAIT_OBJECT_0 + 1U) {
                unexpected_exit = true;
                break;
            }
            tick_count.fetch_add(1, std::memory_order_relaxed);

            if (!PostTick()) {
                break;
            }
        }
        ticker_running.store(false, std::memory_order_release);
        if (unexpected_exit) {
            (void)PostTick();
        }
    }

    [[nodiscard]] bool PostTick() noexcept
    {
        bool expected = false;
        if (!tick_pending.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
        if (!PostMessageW(tick_window, tick_message, 0, 0)) {
            tick_pending.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void StopTicker() noexcept
    {
        if (ticker.joinable()) {
            SetEvent(stop_event);
            ticker.join();
        }
        ticker_running.store(false, std::memory_order_release);
        tick_pending.store(false, std::memory_order_release);
    }

    void ReleaseBoost() noexcept
    {
        if (!boost_api_enabled || api.boost == nullptr) {
            return;
        }
        last_boost_result = api.boost(FALSE);
        boost_api_enabled = false;
    }

    void ReleaseModule() noexcept
    {
        if (module != nullptr) {
            FreeLibrary(std::exchange(module, nullptr));
        }
    }

    Win32CompositorClockApi configured_api;
    Win32CompositorClockApi api;
    HMODULE module = nullptr;
    HANDLE stop_event = nullptr;
    HWND tick_window = nullptr;
    std::uint32_t tick_message = 0;
    std::thread ticker;
    std::atomic_bool ticker_running = false;
    std::atomic_bool tick_pending = false;
    std::atomic<DWORD> last_wait_result = WAIT_TIMEOUT;
    std::atomic<std::uint64_t> tick_count = 0;
    HRESULT last_boost_result = S_OK;
    bool boost_requested = false;
    bool boost_api_enabled = false;
};

Win32CompositorClock::Win32CompositorClock()
    : impl_(std::make_unique<Impl>())
{
}

Win32CompositorClock::Win32CompositorClock(Win32CompositorClockApi api)
    : impl_(std::make_unique<Impl>(api))
{
}

Win32CompositorClock::~Win32CompositorClock()
{
    Shutdown();
}

bool Win32CompositorClock::Initialize(HWND tick_window, std::uint32_t tick_message) noexcept
{
    return impl_->Initialize(tick_window, tick_message);
}

void Win32CompositorClock::Shutdown() noexcept
{
    impl_->Shutdown();
}

bool Win32CompositorClock::SetBoostRequested(bool requested) noexcept
{
    return impl_->SetBoostRequested(requested);
}

bool Win32CompositorClock::ConsumeTick() noexcept
{
    return impl_->ConsumeTick();
}

bool Win32CompositorClock::available() const noexcept
{
    return impl_->available();
}

bool Win32CompositorClock::boost_requested() const noexcept
{
    return impl_->boost_requested;
}

bool Win32CompositorClock::boost_active() const noexcept
{
    return impl_->boost_active();
}

HRESULT Win32CompositorClock::last_boost_result() const noexcept
{
    return impl_->last_boost_result;
}

DWORD Win32CompositorClock::last_wait_result() const noexcept
{
    return impl_->last_wait_result.load(std::memory_order_relaxed);
}

std::uint64_t Win32CompositorClock::tick_count() const noexcept
{
    return impl_->tick_count.load(std::memory_order_relaxed);
}

}  // namespace specforge
