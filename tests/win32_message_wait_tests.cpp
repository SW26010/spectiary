#include "app/render_wake_scheduler.h"
#include "profile/presentation_trace.h"
#include "platform/win32_native_size_trace.h"
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
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kWindowClassName[] = L"SpectiaryWin32MessageWaitTests";
constexpr UINT kSentMessage = WM_APP + 1U;
constexpr UINT kUnusedQueuedMessage = WM_APP + 2U;
constexpr UINT kIgnoredClockMessage = WM_APP + 3U;
constexpr UINT kNestedTraceMessage = WM_APP + 4U;
constexpr UINT kTraceLeafMessage = WM_APP + 5U;
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
    if (message == kNestedTraceMessage) {
        return SendMessageW(hwnd, kTraceLeafMessage, 0, 0);
    }
    if (message == kTraceLeafMessage) { Sleep(3); return 123; }
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
    static_cast<spectiary::RenderWakeScheduler*>(context)->RequestFrame();
}

struct ObservedMessageState {
    int count = 0;
    spectiary::Win32ObservedMessage message;
};

void RecordObservedMessage(
    void* context,
    const spectiary::Win32ObservedMessage& message) noexcept
{
    if (message.message != WM_KEYDOWN) {
        return;
    }
    auto* state = static_cast<ObservedMessageState*>(context);
    ++state->count;
    state->message = message;
}

void SettleScheduler(spectiary::RenderWakeScheduler& scheduler)
{
    const spectiary::RenderWakeScheduler::TimePoint start{};
    const auto follow_up = start + spectiary::RenderWakeScheduler::kInteractiveFrameInterval;
    Require(
        scheduler.TakeAction(start, true) ==
            spectiary::RenderWakeAction::RenderFrame,
        "render scheduler should begin its initial frame");
    scheduler.CompleteFrame(
        start,
        {},
        spectiary::RenderFrameOutcome::Presented);
    Require(
        scheduler.TakeAction(follow_up, true) ==
            spectiary::RenderWakeAction::RenderFrame,
        "render scheduler should run its settling frame");
    scheduler.CompleteFrame(
        follow_up,
        {},
        spectiary::RenderFrameOutcome::Presented);
    Require(
        scheduler.TakeAction(follow_up, true) ==
            spectiary::RenderWakeAction::Wait,
        "render should leave the scheduler idle");
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
    const auto wake_reason = spectiary::WaitForWin32MessageOrDeadline(
        std::chrono::steady_clock::now() + 1s);
    Require(
        wake_reason == spectiary::Win32MessageWaitWake::MessageInput,
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

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
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
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            (spectiary::RenderWakeScheduler::TimePoint::min)(),
        "retrieving queued input for a secondary window should request a frame");

    observer.Stop();
}

void TestSecondaryWindowKeyMessagePreservesCorrelationPayload()
{
    TestWindow window;
    DrainQueuedMessages();

    ObservedMessageState observed;
    spectiary::Win32MessageRenderObserver observer;
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
    observer.ObserveQueuedMessage(spectiary::Win32ObservedMessage{
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

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
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
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a queued hit-test message must not create render feedback");

    observer.Stop();
}

void TestDirectManipulationUpdateMessageIsOnlyNeutralForAttachedWindow()
{
    constexpr std::uint32_t kObservedDirectManipulationUpdateMessage = 0x0096U;
    Require(
        spectiary::ClassifyWin32TouchpadQueuedMessage(
            kObservedDirectManipulationUpdateMessage,
            true) == spectiary::Win32TouchpadQueuedMessageAction::PumpUpdates,
        "Direct Manipulation internal updates must advance input without rendering");
    Require(
        spectiary::ClassifyWin32TouchpadQueuedMessage(
            kObservedDirectManipulationUpdateMessage,
            false) == spectiary::Win32TouchpadQueuedMessageAction::InvalidateRender,
        "the undocumented message must not be ignored outside an attached viewport");
    Require(
        spectiary::ClassifyWin32TouchpadQueuedMessage(WM_MOUSEMOVE, true) ==
            spectiary::Win32TouchpadQueuedMessageAction::InvalidateRender,
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
        spectiary::TryPostWin32TouchpadWake(1U, WM_APP, wake_pending, FailTouchpadWakePost) ==
            spectiary::Win32TouchpadWakePostResult::Failed,
        "a failed PostMessage call should be reported");
    Require(!wake_pending, "a failed PostMessage call must release the coalescing latch");

    g_touchpad_wake_post_count = 0;
    Require(
        spectiary::TryPostWin32TouchpadWake(
            1U,
            WM_APP,
            wake_pending,
            CountSuccessfulTouchpadWakePost) == spectiary::Win32TouchpadWakePostResult::Posted,
        "the next gesture should retry the wake after a failed post");
    Require(wake_pending, "a successful wake post should retain the coalescing latch until Poll");
    Require(
        spectiary::TryPostWin32TouchpadWake(
            1U,
            WM_APP,
            wake_pending,
            CountSuccessfulTouchpadWakePost) ==
            spectiary::Win32TouchpadWakePostResult::AlreadyPending,
        "a pending wake should coalesce later gestures");
    Require(g_touchpad_wake_post_count == 1, "coalescing should issue only one successful PostMessage");
}

void TestIgnoredQueuedClockMessageOnlyGrantsPermission()
{
    TestWindow window;
    DrainQueuedMessages();

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
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
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a clock permission message must not invalidate static content");
    observer.Stop();
}

void TestSentMessageWakeRemainsRenderableWhenPeekReturnsFalse()
{
    TestWindow window;
    DrainQueuedMessages();

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    SendNonqueuedMessage(
        window.hwnd(),
        kSentMessage,
        "nonqueued message should be sent to the window thread");
    const BOOL queued = WaitAndDispatchSentMessage();

    Require(g_sent_message_handled, "PeekMessage should dispatch the nonqueued sent message");
    Require(queued == FALSE, "a dispatched sent message should not appear as a queued message");
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            (spectiary::RenderWakeScheduler::TimePoint::min)(),
        "dispatching the sent message should request a frame even when PeekMessage returns false");

    observer.Stop();
}

void TestHitTestSentMessageDoesNotCreateRenderFeedback()
{
    TestWindow window;
    DrainQueuedMessages();

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
    Require(observer.Start(RequestFrame, &scheduler), "message observer should start");

    SendNonqueuedMessage(
        window.hwnd(),
        WM_NCHITTEST,
        "hit-test message should be sent to the window thread");
    const BOOL queued = WaitAndDispatchSentMessage();

    Require(g_hit_test_handled, "PeekMessage should dispatch the hit-test message");
    Require(queued == FALSE, "a dispatched hit-test message should not appear queued");
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a render-produced hit-test query must not request another frame");

    observer.Stop();
}

void TestIgnoredSentClockMessageDoesNotCreateRenderFeedback()
{
    TestWindow window;
    DrainQueuedMessages();

    spectiary::RenderWakeScheduler scheduler;
    SettleScheduler(scheduler);
    spectiary::Win32MessageRenderObserver observer;
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
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a sent clock permission message must not invalidate static content");
    observer.Stop();
}

}  // namespace

int main()
{
    {
        namespace trace = spectiary::presentation_trace;
        namespace native = spectiary::native_size_trace;
        TestWindow window;
        spectiary::RenderWakeScheduler scheduler;
        spectiary::Win32MessageRenderObserver observer;
        Require(observer.Start(&RequestFrame, &scheduler), "native trace hook must start");
        Require(native::hook_available, "return hook must be available in integration test");
        const auto hwnd = reinterpret_cast<std::uintptr_t>(window.hwnd());
        trace::Register(hwnd, 320, 240);
        std::vector<trace::Event> events;
        events.reserve(64);
        trace::context = &events;
        trace::enabled = [](void*) { return true; };
        trace::callback = [](void* context, const trace::Event& event) {
            static_cast<std::vector<trace::Event>*>(context)->push_back(event);
        };
        {
            trace::Span outer({.name = "platform_window_size", .window = trace::Lookup(hwnd)});
            {
                native::Scope scope;
                Require(SendMessageW(window.hwnd(), kNestedTraceMessage, 0, 0) == 123,
                    "message tracing must preserve the original result");
                SetLastError(4321);
            }
            Require(GetLastError() == 4321, "native scope must preserve last error");
        }
        const trace::Event* parent = nullptr;
        const trace::Event* child = nullptr;
        bool observation = false, cpu = false;
        for (const auto& event : events) {
            if (event.phase != "end") continue;
            if (event.name == "native_size_message" && event.message_id == kNestedTraceMessage) parent = &event;
            if (event.name == "native_size_message" && event.message_id == kTraceLeafMessage) child = &event;
            if (event.name == "native_size_observation") observation = event.result == 0 && event.count == 0;
            if (event.name == "native_size_thread_cpu") cpu = event.result_valid && event.duration_ms >= 0;
        }
        Require(parent && child && child->parent == parent->operation && child->message_hwnd == hwnd &&
            child->duration_ms >= 1 && parent->duration_ms >= child->duration_ms,
            "real nested sent messages must be paired and timed under their parent");
        Require(observation && cpu && native::depth == 0 && trace::current.operation == 0,
            "scope must unwind and report complete observation and CPU availability");
        events.clear();
        native::Enter(window.hwnd(), kTraceLeafMessage);
        native::Leave(window.hwnd(), kTraceLeafMessage);
        Require(events.empty(), "messages outside a size scope must not be traced");
        {
            native::Scope scope;
            for (unsigned i = 0; i < 40; ++i) native::Enter(window.hwnd(), kTraceLeafMessage);
            Require(native::depth == 32, "message storage must remain bounded");
            for (unsigned i = 0; i < 40; ++i) native::Leave(window.hwnd(), kTraceLeafMessage);
        }
        bool overflow = false;
        for (const auto& event : events) {
            if (event.name == "native_size_observation") overflow = event.count == 8;
        }
        Require(overflow && native::depth == 0 && native::suppressed_depth == 0,
            "overflow must be explicit and fully unwind");
        events.clear();
        {
            native::Scope scope;
            native::Enter(window.hwnd(), kTraceLeafMessage);
            native::Leave(window.hwnd(), kNestedTraceMessage);
        }
        bool mismatch = false;
        for (const auto& event : events) {
            if (event.name == "native_size_observation") mismatch = event.count > 0;
        }
        Require(mismatch && native::depth == 0 && trace::current.operation == 0,
            "mismatch must report incomplete observation and restore the parent");
        events.clear();
        trace::enabled = [](void*) { return false; };
        {
            native::Scope scope;
            Require(SendMessageW(window.hwnd(), kNestedTraceMessage, 0, 0) == 123,
                "disabled telemetry must preserve dispatch");
        }
        Require(events.empty() && native::scopes == 0, "disabled telemetry must emit nothing");
        trace::callback = nullptr;
        trace::enabled = nullptr;
        trace::context = nullptr;
        trace::Unregister(hwnd);
        observer.Stop();
    }
    {
        namespace trace = spectiary::presentation_trace;
        TestWindow window;
        spectiary::RenderWakeScheduler scheduler;
        spectiary::Win32MessageRenderObserver observer;
        Require(observer.Start(&RequestFrame, &scheduler), "size-move hook must start");
        const auto hwnd = reinterpret_cast<std::uintptr_t>(window.hwnd());
        trace::Register(hwnd, 320, 240);
        SendMessageW(window.hwnd(), WM_ENTERSIZEMOVE, 0, 0);
        const auto entered = trace::Lookup(hwnd);
        Require(entered.in_size_move && entered.size_move != 0, "real enter message must be observed");
        SendMessageW(window.hwnd(), WM_EXITSIZEMOVE, 0, 0);
        Require(!trace::Lookup(hwnd).in_size_move && trace::Lookup(hwnd).size_move == entered.size_move,
            "real exit message must close the same size-move session");
        observer.Stop();
        trace::Unregister(hwnd);
    }
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
