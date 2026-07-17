#include "platform/win32_compositor_clock.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kWindowClassName[] = L"SpecForgeCompositorClockTests";
constexpr UINT kTickMessage = WM_APP + 0x71U;
std::atomic_int g_enable_calls = 0;
std::atomic_int g_disable_calls = 0;
std::atomic_int g_wait_calls = 0;
std::atomic_bool g_fail_enable = false;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

HRESULT WINAPI FakeBoost(BOOL enable)
{
    if (enable) {
        ++g_enable_calls;
        return g_fail_enable ? E_FAIL : S_OK;
    }
    ++g_disable_calls;
    return S_OK;
}

DWORD WINAPI FakeWait(UINT count, const HANDLE* handles, DWORD)
{
    ++g_wait_calls;
    if (count != 1 || handles == nullptr || handles[0] == nullptr) {
        return WAIT_FAILED;
    }
    if (WaitForSingleObject(handles[0], 2) == WAIT_OBJECT_0) {
        return WAIT_OBJECT_0;
    }
    return WAIT_OBJECT_0 + count;
}

DWORD WINAPI FailingWait(UINT, const HANDLE*, DWORD)
{
    ++g_wait_calls;
    return WAIT_FAILED;
}

class TestWindow {
public:
    TestWindow() : instance_(GetModuleHandleW(nullptr))
    {
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance_;
        window_class.lpszClassName = kWindowClassName;
        Require(RegisterClassW(&window_class) != 0, "test window class should register");
        hwnd_ = CreateWindowExW(
            0,
            kWindowClassName,
            L"",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            instance_,
            nullptr);
        Require(hwnd_ != nullptr, "message-only test window should be created");
    }

    ~TestWindow()
    {
        if (hwnd_ != nullptr) {
            (void)DestroyWindow(hwnd_);
        }
        (void)UnregisterClassW(kWindowClassName, instance_);
    }

    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    [[nodiscard]] HWND hwnd() const { return hwnd_; }

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
};

void ResetFakeApi()
{
    g_enable_calls = 0;
    g_disable_calls = 0;
    g_wait_calls = 0;
    g_fail_enable = false;
}

bool WaitForTick(HWND window)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message = {};
        if (PeekMessageW(&message, window, kTickMessage, kTickMessage, PM_REMOVE)) {
            return true;
        }
        (void)MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return false;
}

specforge::Win32CompositorClock MakeFakeClock()
{
    return specforge::Win32CompositorClock({FakeBoost, FakeWait});
}

void TestBoostPostsCoalescedClockTicksAndReleasesCleanly()
{
    ResetFakeApi();
    TestWindow window;
    specforge::Win32CompositorClock clock = MakeFakeClock();

    Require(clock.Initialize(window.hwnd(), kTickMessage), "injected compositor API should initialize");
    Require(clock.available(), "initialized compositor clock should report available");
    Require(clock.SetBoostRequested(true), "successful boost should start compositor-clock pacing");
    Require(clock.boost_requested(), "boost request state should be retained");
    Require(WaitForTick(window.hwnd()), "compositor clock should post a render tick");
    Require(clock.ConsumeTick(), "posted tick should be consumable exactly once");
    Require(!clock.ConsumeTick(), "consumed tick should not remain pending");

    (void)clock.SetBoostRequested(false);
    Require(!clock.boost_requested(), "release should clear requested state");
    Require(!clock.boost_active(), "release should stop compositor-clock pacing");
    Require(g_enable_calls == 1, "one interaction should issue one enable request");
    Require(g_disable_calls == 1, "one interaction should issue one matching release");
    Require(g_wait_calls > 0, "ticker should wait on the compositor clock");
}

void TestFailedBoostDoesNotStartTicker()
{
    ResetFakeApi();
    g_fail_enable = true;
    TestWindow window;
    specforge::Win32CompositorClock clock = MakeFakeClock();

    Require(clock.Initialize(window.hwnd(), kTickMessage), "fake API should still initialize");
    Require(!clock.SetBoostRequested(true), "failed system boost should fall back without a ticker");
    Require(clock.boost_requested(), "failed request should not be retried every frame");
    Require(!clock.boost_active(), "failed boost should keep vsync presentation active");
    Require(clock.last_boost_result() == E_FAIL, "failed boost HRESULT should remain observable");
    Require(g_wait_calls == 0, "ticker should not start after a failed boost request");

    (void)clock.SetBoostRequested(false);
    Require(g_disable_calls == 0, "failed enable should not issue an unmatched release");
}

void TestShutdownReleasesAnOutstandingBoost()
{
    ResetFakeApi();
    TestWindow window;
    specforge::Win32CompositorClock clock = MakeFakeClock();

    Require(clock.Initialize(window.hwnd(), kTickMessage), "fake API should initialize");
    Require(clock.SetBoostRequested(true), "boost should start before shutdown");
    Require(WaitForTick(window.hwnd()), "ticker should be running before shutdown");
    clock.Shutdown();

    Require(!clock.available(), "shutdown should release runtime API state");
    Require(!clock.boost_requested(), "shutdown should clear requested state");
    Require(!clock.boost_active(), "shutdown should join the ticker");
    Require(g_disable_calls == 1, "shutdown should release the system boost request");
}

void TestUnexpectedWaitExitWakesUiForFallbackPacing()
{
    ResetFakeApi();
    TestWindow window;
    specforge::Win32CompositorClock clock({FakeBoost, FailingWait});

    Require(clock.Initialize(window.hwnd(), kTickMessage), "failing wait API should initialize");
    Require(clock.SetBoostRequested(true), "boost should start before the wait failure is observed");
    Require(WaitForTick(window.hwnd()), "unexpected wait exit should wake the UI to select fallback pacing");
    Require(!clock.boost_active(), "failed waiter should stop compositor-clock presentation mode");
    Require(clock.last_wait_result() == WAIT_FAILED, "unexpected wait status should remain observable");
    Require(clock.ConsumeTick(), "fallback wake should clear through the normal tick-message path");

    (void)clock.SetBoostRequested(false);
    Require(g_disable_calls == 1, "wait failure should still release the matching boost request");
}

void TestInvalidInitializationArgumentsFailClosed()
{
    ResetFakeApi();
    specforge::Win32CompositorClock clock = MakeFakeClock();

    Require(!clock.Initialize(nullptr, kTickMessage), "null tick window should be rejected");
    Require(clock.last_boost_result() == E_INVALIDARG, "invalid arguments should preserve E_INVALIDARG");
    Require(!clock.SetBoostRequested(true), "uninitialized compositor clock should remain a safe fallback");
}

}  // namespace

int main()
{
    TestBoostPostsCoalescedClockTicksAndReleasesCleanly();
    TestFailedBoostDoesNotStartTicker();
    TestShutdownReleasesAnOutstandingBoost();
    TestUnexpectedWaitExitWakesUiForFallbackPacing();
    TestInvalidInitializationArgumentsFailClosed();
    return 0;
}
