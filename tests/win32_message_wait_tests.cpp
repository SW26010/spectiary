#include "app/render_wake_scheduler.h"
#include "platform/win32_message_render_observer.h"
#include "platform/win32_message_wait.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kWindowClassName[] = L"SpecForgeWin32MessageWaitTests";
constexpr UINT kSentMessage = WM_APP + 1U;
constexpr UINT kUnusedQueuedMessage = WM_APP + 2U;
std::atomic_bool g_sent_message_handled = false;
std::atomic_bool g_hit_test_handled = false;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

LRESULT CALLBACK TestWindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == kSentMessage) {
        g_sent_message_handled = true;
        return 0;
    }
    if (message == WM_NCHITTEST) {
        g_hit_test_handled = true;
        return HTCLIENT;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

class TestWindow {
public:
    TestWindow() : instance_(GetModuleHandleW(nullptr))
    {
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = TestWindowProcedure;
        window_class.hInstance = instance_;
        window_class.lpszClassName = kWindowClassName;
        Require(RegisterClassW(&window_class) != 0, "message-only window class should register");

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
        Require(hwnd_ != nullptr, "message-only window should be created");
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

void RequestFrame(void* context) noexcept
{
    static_cast<specforge::RenderWakeScheduler*>(context)->RequestFrame();
}

void SettleScheduler(specforge::RenderWakeScheduler& scheduler)
{
    const specforge::RenderWakeScheduler::TimePoint start{};
    const auto follow_up = start + specforge::RenderWakeScheduler::kInteractiveFrameInterval;
    scheduler.BeginFrame(start);
    scheduler.EndFrame(start, {});
    scheduler.BeginFrame(follow_up);
    scheduler.EndFrame(follow_up, {});
    Require(!scheduler.ShouldRender(follow_up), "render should leave the scheduler idle");
}

void DrainQueuedMessages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
        DispatchMessageW(&message);
    }
}

void SendNonqueuedMessage(HWND window, UINT message, std::string_view failure_message)
{
    std::atomic_bool message_sent = false;
    std::thread sender([window, message, &message_sent]() {
        message_sent = SendNotifyMessageW(window, message, 0, 0) != FALSE;
    });
    sender.join();
    Require(message_sent, failure_message);
}

BOOL WaitAndDispatchSentMessage()
{
    const auto wake_reason = specforge::WaitForWin32MessageOrDeadline(
        std::chrono::steady_clock::now() + 1s);
    Require(
        wake_reason == specforge::Win32MessageWaitWake::MessageInput,
        "tail wait should report the pending sent message");

    MSG queued_message = {};
    return PeekMessageW(
        &queued_message,
        nullptr,
        kUnusedQueuedMessage,
        kUnusedQueuedMessage,
        PM_REMOVE);
}

void TestSentMessageWakeRemainsRenderableWhenPeekReturnsFalse()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    SendNonqueuedMessage(
        window.hwnd(),
        kSentMessage,
        "nonqueued message should be sent to the window thread");
    const BOOL queued = WaitAndDispatchSentMessage();

    Require(g_sent_message_handled, "PeekMessage should dispatch the nonqueued sent message");
    Require(queued == FALSE, "a dispatched sent message should not appear as a queued message");
    Require(
        scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "dispatching the sent message should request a frame even when PeekMessage returns false");

    observer.Stop();
}

void TestHitTestSentMessageDoesNotCreateRenderFeedback()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    SendNonqueuedMessage(
        window.hwnd(),
        WM_NCHITTEST,
        "hit-test message should be sent to the window thread");
    const BOOL queued = WaitAndDispatchSentMessage();

    Require(g_hit_test_handled, "PeekMessage should dispatch the hit-test message");
    Require(queued == FALSE, "a dispatched hit-test message should not appear queued");
    Require(
        !scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "a render-produced hit-test query must not request another frame");

    observer.Stop();
}

}  // namespace

int main()
{
    TestSentMessageWakeRemainsRenderableWhenPeekReturnsFalse();
    TestHitTestSentMessageDoesNotCreateRenderFeedback();
    return 0;
}
