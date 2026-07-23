#include "app/render_wake_scheduler.h"
#include "platform/win32_message_render_observer.h"
#include "platform/win32_message_wait.h"
#include "platform/win32_touchpad_gesture_source.h"

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
constexpr UINT kIgnoredClockMessage = WM_APP + 3U;
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

struct ObservedMessageState {
    int count = 0;
    specforge::Win32ObservedMessage message;
};

void RecordObservedMessage(
    void* context,
    const specforge::Win32ObservedMessage& message) noexcept
{
    if (message.message != WM_KEYDOWN) {
        return;
    }
    auto* state = static_cast<ObservedMessageState*>(context);
    ++state->count;
    state->message = message;
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

void TestQueuedWindowMessageRequestsFrame()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    Require(
        PostMessageW(window.hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(8, 13)) != FALSE,
        "secondary window mouse input should be queued");

    MSG queued_message = {};
    Require(
        PeekMessageW(
            &queued_message,
            window.hwnd(),
            WM_MOUSEMOVE,
            WM_MOUSEMOVE,
            PM_REMOVE) != FALSE,
        "secondary window mouse input should be retrieved from the queue");
    observer.ObserveQueuedMessage(queued_message.message);
    TranslateMessage(&queued_message);
    DispatchMessageW(&queued_message);

    Require(
        scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "retrieving queued input for a secondary window should request a frame");

    observer.Stop();
}

void TestSecondaryWindowKeyMessagePreservesCorrelationPayload()
{
    TestWindow window;
    DrainQueuedMessages();

    ObservedMessageState observed;
    specforge::Win32MessageRenderObserver observer;
    Require(
        observer.Start(
            [](void*) noexcept {},
            &observed,
            std::nullopt,
            RecordObservedMessage),
        "message observer should expose secondary-window payloads");
    Require(
        PostMessageW(window.hwnd(), WM_KEYDOWN, VK_RIGHT, 1) != FALSE,
        "secondary window key input should be queued");

    MSG queued = {};
    Require(
        GetMessageW(&queued, window.hwnd(), WM_KEYDOWN, WM_KEYDOWN) > 0,
        "secondary window key input should be retrieved");
    observer.ObserveQueuedMessage(specforge::Win32ObservedMessage{
        reinterpret_cast<std::uintptr_t>(queued.hwnd),
        queued.message,
        static_cast<std::uintptr_t>(queued.wParam),
        static_cast<std::intptr_t>(queued.lParam)});
    DispatchMessageW(&queued);

    Require(observed.count == 1, "one dispatched key message should produce one correlation payload");
    Require(
        observed.message.hwnd == reinterpret_cast<std::uintptr_t>(window.hwnd()) &&
            observed.message.message == WM_KEYDOWN && observed.message.wparam == VK_RIGHT &&
            observed.message.lparam == 1,
        "the observer should preserve detached HWND, key, and key-state payload");
    observer.Stop();
}

void TestQueuedHitTestDoesNotCreateRenderFeedback()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    Require(
        PostMessageW(window.hwnd(), WM_NCHITTEST, 0, MAKELPARAM(8, 13)) != FALSE,
        "hit-test message should be queued");

    MSG queued_message = {};
    Require(
        PeekMessageW(
            &queued_message,
            window.hwnd(),
            WM_NCHITTEST,
            WM_NCHITTEST,
            PM_REMOVE) != FALSE,
        "hit-test message should be retrieved from the queue");
    observer.ObserveQueuedMessage(queued_message.message);

    Require(
        !scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "a queued hit-test message must not create render feedback");

    observer.Stop();
}

void TestDirectManipulationUpdateMessageIsOnlyNeutralForAttachedWindow()
{
    constexpr std::uint32_t kObservedDirectManipulationUpdateMessage = 0x0096U;
    Require(
        specforge::ClassifyWin32TouchpadQueuedMessage(
            kObservedDirectManipulationUpdateMessage,
            true) == specforge::Win32TouchpadQueuedMessageAction::PumpUpdates,
        "Direct Manipulation internal updates must advance input without rendering");
    Require(
        specforge::ClassifyWin32TouchpadQueuedMessage(
            kObservedDirectManipulationUpdateMessage,
            false) == specforge::Win32TouchpadQueuedMessageAction::InvalidateRender,
        "the undocumented message must not be ignored outside an attached viewport");
    Require(
        specforge::ClassifyWin32TouchpadQueuedMessage(WM_MOUSEMOVE, true) ==
            specforge::Win32TouchpadQueuedMessageAction::InvalidateRender,
        "real queued pointer input must keep invalidating attached viewports");
}

bool FailTouchpadWakePost(std::uintptr_t, std::uint32_t) noexcept
{
    return false;
}

int g_touchpad_wake_post_count = 0;

bool CountSuccessfulTouchpadWakePost(std::uintptr_t, std::uint32_t) noexcept
{
    ++g_touchpad_wake_post_count;
    return true;
}

void TestFailedTouchpadWakePostCanRetry()
{
    bool wake_pending = false;
    Require(
        specforge::TryPostWin32TouchpadWake(1U, WM_APP, wake_pending, FailTouchpadWakePost) ==
            specforge::Win32TouchpadWakePostResult::Failed,
        "a failed PostMessage call should be reported");
    Require(!wake_pending, "a failed PostMessage call must release the coalescing latch");

    g_touchpad_wake_post_count = 0;
    Require(
        specforge::TryPostWin32TouchpadWake(
            1U,
            WM_APP,
            wake_pending,
            CountSuccessfulTouchpadWakePost) == specforge::Win32TouchpadWakePostResult::Posted,
        "the next gesture should retry the wake after a failed post");
    Require(wake_pending, "a successful wake post should retain the coalescing latch until Poll");
    Require(
        specforge::TryPostWin32TouchpadWake(
            1U,
            WM_APP,
            wake_pending,
            CountSuccessfulTouchpadWakePost) ==
            specforge::Win32TouchpadWakePostResult::AlreadyPending,
        "a pending wake should coalesce later gestures");
    Require(g_touchpad_wake_post_count == 1, "coalescing should issue only one successful PostMessage");
}

void TestIgnoredQueuedClockMessageOnlyGrantsPermission()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(
        observer.Start(RequestFrame, &scheduler, kIgnoredClockMessage),
        "message observer should accept one permission-only message");

    Require(
        PostMessageW(window.hwnd(), kIgnoredClockMessage, 0, 0) != FALSE,
        "permission-only clock message should be queued");
    MSG queued_message = {};
    Require(
        PeekMessageW(
            &queued_message,
            window.hwnd(),
            kIgnoredClockMessage,
            kIgnoredClockMessage,
            PM_REMOVE) != FALSE,
        "permission-only clock message should be retrieved");
    observer.ObserveQueuedMessage(queued_message.message);
    DispatchMessageW(&queued_message);

    Require(
        !scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "a clock permission message must not invalidate static content");
    observer.Stop();
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

void TestIgnoredSentClockMessageDoesNotCreateRenderFeedback()
{
    TestWindow window;
    DrainQueuedMessages();

    specforge::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    specforge::Win32MessageRenderObserver observer;
    Require(
        observer.Start(RequestFrame, &scheduler, kIgnoredClockMessage),
        "message observer should start with a permission-only message");

    SendNonqueuedMessage(
        window.hwnd(),
        kIgnoredClockMessage,
        "permission-only clock message should be sent to the window thread");
    const BOOL queued = WaitAndDispatchSentMessage();

    Require(queued == FALSE, "a sent clock message should not appear queued");
    Require(
        !scheduler.ShouldRender(std::chrono::steady_clock::now()),
        "a sent clock permission message must not invalidate static content");
    observer.Stop();
}

}  // namespace

int main()
{
    TestQueuedWindowMessageRequestsFrame();
    TestSecondaryWindowKeyMessagePreservesCorrelationPayload();
    TestQueuedHitTestDoesNotCreateRenderFeedback();
    TestDirectManipulationUpdateMessageIsOnlyNeutralForAttachedWindow();
    TestFailedTouchpadWakePostCanRetry();
    TestIgnoredQueuedClockMessageOnlyGrantsPermission();
    TestSentMessageWakeRemainsRenderableWhenPeekReturnsFalse();
    TestHitTestSentMessageDoesNotCreateRenderFeedback();
    TestIgnoredSentClockMessageDoesNotCreateRenderFeedback();
    return 0;
}
