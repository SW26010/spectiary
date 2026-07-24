#include "app/render_wake_scheduler.h"

#include <imgui.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using Scheduler = specforge::RenderWakeScheduler;
using Action = specforge::RenderWakeAction;
using FrameOutcome = specforge::RenderFrameOutcome;
using TickOutcome = specforge::CompositorClockTickOutcome;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void SettleInitialFrame(Scheduler& scheduler, Scheduler::TimePoint start)
{
    Require(
        scheduler.TakeAction(start, true) == Action::RenderFrame,
        "scheduler should request the initial frame");
    scheduler.CompleteFrame(start, {}, FrameOutcome::Presented);

    const auto follow_up = start + Scheduler::kInteractiveFrameInterval;
    Require(
        scheduler.TakeAction(follow_up, true) == Action::RenderFrame,
        "initial frame should receive one state-settling follow-up");
    scheduler.CompleteFrame(follow_up, {}, FrameOutcome::Presented);
    Require(
        scheduler.TakeAction(follow_up, true) == Action::Wait,
        "settled scheduler should have no immediate action");
    Require(!scheduler.NextWakeDeadline(true, std::nullopt), "settled scheduler should become fully idle");
}

void TestWindowInvalidationPersistsUntilRendered()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame();
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            Scheduler::TimePoint::min(),
        "unconsumed window invalidation should remain pending");

    Require(
        scheduler.TakeAction(start + 2s, true) == Action::RenderFrame,
        "window invalidation should select a render action");
    scheduler.CompleteFrame(
        start + 2s,
        {},
        FrameOutcome::Presented);
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            start + 2s + Scheduler::kInteractiveFrameInterval,
        "an externally requested frame should schedule one follow-up frame");
}

void TestClockPacingDefersInvalidationUntilPermitted()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.SetCompositorClockPaced(true);
    scheduler.RequestFrame();
    const auto input_time = start + 1s;
    Require(
        scheduler.TakeAction(input_time, true) == Action::Wait,
        "input invalidation should wait for compositor-clock permission");
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a deferred invalidation should not create a zero-time busy-loop deadline");

    const auto maintenance = input_time + 5s;
    Require(
        scheduler.NextWakeDeadline(true, maintenance) == maintenance,
        "clock pacing should preserve independent maintenance deadlines");
    Require(
        scheduler.OnCompositorClockTick(true, true) ==
            TickOutcome::PermissionGranted,
        "an active compositor tick should grant scheduler permission");
    Require(
        scheduler.TakeAction(input_time, true) == Action::RenderFrame,
        "the pending invalidation should render as soon as a compositor tick permits it");

    scheduler.CompleteFrame(
        input_time,
        {},
        FrameOutcome::Presented);
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a compositor-paced frame should not schedule a competing timer frame");
}

void TestBusyAcquireWaitsForTheNextCompositorTick()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto busy_frame = start + 1s;
    scheduler.SetCompositorClockPaced(true);
    scheduler.RequestFrame();
    (void)scheduler.OnCompositorClockTick(true, true);
    Require(
        scheduler.TakeAction(busy_frame, true) ==
            Action::RenderFrame,
        "the first tick should start the requested frame");
    scheduler.CompleteFrame(
        busy_frame,
        {},
        FrameOutcome::AcquireRetry);

    Require(
        scheduler.TakeAction(busy_frame, true) ==
            Action::Wait,
        "a busy acquire should retain work without reusing the consumed tick");
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a clock-paced retry without a tick must not create a zero-time busy-loop deadline");
    Require(
        scheduler.OnCompositorClockTick(true, true) ==
            TickOutcome::PermissionGranted,
        "the next compositor tick should grant retry permission");
    Require(
        scheduler.TakeAction(busy_frame, true) ==
            Action::RenderFrame,
        "the retained acquire retry should run on the next compositor tick");
    scheduler.CompleteFrame(
        busy_frame,
        {},
        FrameOutcome::Presented);
}

void TestBusyAcquireRetriesImmediatelyUnderOrdinaryPacing()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto busy_frame = start + 1s;
    scheduler.RequestFrame();
    Require(
        scheduler.TakeAction(busy_frame, true) ==
            Action::RenderFrame,
        "ordinary pacing should begin requested work immediately");
    scheduler.CompleteFrame(
        busy_frame,
        {},
        FrameOutcome::AcquireRetry);
    Require(
        scheduler.TakeAction(busy_frame, true) ==
            Action::RenderFrame,
        "ordinary pacing should retain a busy acquire as immediate retry work");
    scheduler.CompleteFrame(
        busy_frame,
        {},
        FrameOutcome::Presented);
}

void TestBoostTransitionsPreservePendingRenderDemand()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto transition_time = start + 1s;
    scheduler.RequestFrame();
    scheduler.SetCompositorClockPaced(true);
    Require(
        scheduler.TakeAction(transition_time, true) ==
            Action::Wait,
        "entering compositor pacing should revoke ordinary render permission");
    (void)scheduler.OnCompositorClockTick(true, true);
    Require(
        scheduler.TakeAction(transition_time, true) ==
            Action::RenderFrame,
        "a paced render should begin after its tick");
    scheduler.CompleteFrame(
        transition_time,
        {},
        FrameOutcome::Presented);

    scheduler.RequestFrame();
    scheduler.SetCompositorClockPaced(false);
    Require(
        scheduler.TakeAction(transition_time, true) ==
            Action::RenderFrame,
        "leaving compositor pacing should release pending work without another tick");
    scheduler.CompleteFrame(
        transition_time,
        {},
        FrameOutcome::Presented);
}

void TestFailedCompositorClockTickEstablishesFallbackTouchpadCadence()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto failure_time = start + 1s;
    scheduler.SetCompositorClockPaced(true);
    Require(
        scheduler.OnCompositorClockTick(true, false) ==
            TickOutcome::FallbackFrameRequested,
        "a final tick from an inactive waiter should request fallback work");
    Require(
        scheduler.TakeAction(failure_time, true) ==
            Action::RenderFrame,
        "waiter failure should request one transition frame");
    scheduler.CompleteFrame(
        failure_time,
        {.touchpad_active = true},
        FrameOutcome::Presented);
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            failure_time + Scheduler::kTouchpadFrameInterval,
        "the transition frame should establish bounded touchpad fallback pacing");
    Require(
        scheduler.OnCompositorClockTick(false, false) ==
            TickOutcome::Ignored,
        "a stale tick message must not create fallback work");
}

void TestTouchpadUpdateCanRunWithoutRendering()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.SetCompositorClockPaced(true);
    scheduler.RequestTouchpadUpdate();
    (void)scheduler.OnCompositorClockTick(true, true);
    Require(
        scheduler.TakeAction(start + 1s, true) ==
            Action::PumpTouchpadUpdates,
        "a paced touchpad tick should pump updates without inventing render demand");
    Require(
        scheduler.TakeAction(start + 1s, true) ==
            Action::Wait,
        "one tick should authorize at most one touchpad update pump");
}

void TestSettingsSaveWakeIsDebounced()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame(5s);
    Require(
        scheduler.TakeAction(start + 1s, true) == Action::RenderFrame,
        "the first settings invalidation should render");
    scheduler.CompleteFrame(
        start + 1s,
        {},
        FrameOutcome::Presented);
    scheduler.RequestFrame(5s);
    Require(
        scheduler.TakeAction(start + 2s, true) == Action::RenderFrame,
        "the second settings invalidation should render");
    scheduler.CompleteFrame(
        start + 2s,
        {},
        FrameOutcome::Presented);

    const auto follow_up =
        start + 2s + Scheduler::kInteractiveFrameInterval;
    Require(
        scheduler.TakeAction(follow_up, true) ==
            Action::RenderFrame,
        "settings invalidation should retain its settling frame");
    scheduler.CompleteFrame(
        follow_up,
        {},
        FrameOutcome::Presented);
    Require(
        scheduler.TakeAction(start + 6999ms, true) ==
            Action::Wait,
        "settings save wake should wait for the last invalidation");
    Require(
        scheduler.TakeAction(start + 7s, true) ==
            Action::RenderFrame,
        "settings save wake should fire after the debounce");

    scheduler.CompleteFrame(
        start + 7s,
        {},
        FrameOutcome::Presented);
    Require(!scheduler.NextWakeDeadline(true, std::nullopt), "settings save wake should be consumed by a frame");
}

void RenderImGuiSettingsFrame(float delta_time, bool move_window)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = delta_time;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();
    ImGui::Begin("Render wake scheduler integration");
    if (move_window) {
        ImGui::SetWindowPos(ImVec2(240.0f, 180.0f));
    }
    ImGui::End();
    ImGui::EndFrame();
}

void TestSettingsSaveWakeStartsAfterTheDirtyFrame()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.IniSavingRate = 1.0f;
    unsigned char* font_pixels = nullptr;
    int font_width = 0;
    int font_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    Require(font_pixels != nullptr && font_width > 0 && font_height > 0, "ImGui test font atlas should build");

    RenderImGuiSettingsFrame(1.0f / 60.0f, false);
    (void)ImGui::SaveIniSettingsToMemory();
    io.WantSaveIniSettings = false;

    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto message_time = start + 1s;
    const auto dirty_frame_start = message_time + 1ms;
    const auto dirty_frame_end = message_time + 10ms;
    scheduler.RequestFrame(1s);
    Require(
        scheduler.TakeAction(dirty_frame_start, true) ==
            Action::RenderFrame,
        "dirty settings frame should be scheduled");
    RenderImGuiSettingsFrame(1.0f / 60.0f, true);
    scheduler.CompleteFrame(
        dirty_frame_end,
        {},
        FrameOutcome::Presented);

    const auto follow_up = dirty_frame_end + Scheduler::kInteractiveFrameInterval;
    Require(
        scheduler.TakeAction(follow_up, true) ==
            Action::RenderFrame,
        "dirty settings frame should receive a settling frame");
    RenderImGuiSettingsFrame(0.025f, false);
    scheduler.CompleteFrame(
        follow_up,
        {},
        FrameOutcome::Presented);

    const auto save_wake = scheduler.NextWakeDeadline(true, std::nullopt);
    Require(save_wake.has_value(), "dirty ImGui settings should retain a save wake");
    Require(
        scheduler.TakeAction(*save_wake, true) ==
            Action::RenderFrame,
        "the ImGui save deadline should select a render action");
    const float final_delta = std::chrono::duration<float>(*save_wake - follow_up).count();
    RenderImGuiSettingsFrame(final_delta, false);
    scheduler.CompleteFrame(
        *save_wake,
        {},
        FrameOutcome::Presented);

    Require(
        io.WantSaveIniSettings,
        "the scheduled wake frame should advance the real ImGui dirty timer through its saving rate");
    ImGui::DestroyContext();
}

void TestPopupTransitionAnimatesForABoundedInterval()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame();
    Require(
        scheduler.TakeAction(start + 1s, true) ==
            Action::RenderFrame,
        "popup transition should render its invalidation");
    scheduler.CompleteFrame(
        start + 1s,
        {.popup_open = true},
        FrameOutcome::Presented);
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            start + 1s + Scheduler::kInteractiveFrameInterval,
        "opening a popup should schedule animation frames");

    const auto animation_end = start + 1s + Scheduler::kPopupAnimationDuration;
    Require(
        scheduler.TakeAction(animation_end, true) ==
            Action::RenderFrame,
        "popup animation deadline should select a render action");
    scheduler.CompleteFrame(
        animation_end,
        {.popup_open = true},
        FrameOutcome::Presented);
    Require(!scheduler.NextWakeDeadline(true, std::nullopt), "popup animation should stop after settling");
}

void TestTextInputAndTouchpadExposeTimeDrivenDemand()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame();
    Require(
        scheduler.TakeAction(start + 1s, true) ==
            Action::RenderFrame,
        "text input invalidation should render");
    scheduler.CompleteFrame(
        start + 1s,
        {.text_input_active = true},
        FrameOutcome::Presented);
    const auto text_follow_up =
        start + 1s + Scheduler::kInteractiveFrameInterval;
    Require(
        scheduler.TakeAction(text_follow_up, true) ==
            Action::RenderFrame,
        "text input should receive a settling frame");
    scheduler.CompleteFrame(
        text_follow_up,
        {.text_input_active = true},
        FrameOutcome::Presented);
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            text_follow_up + Scheduler::kTextCursorFrameInterval,
        "active text input should keep the cursor clock moving at a bounded cadence");

    Scheduler clock_paced_touchpad;
    SettleInitialFrame(clock_paced_touchpad, start);
    const auto touchpad_frame = start + 2s;
    clock_paced_touchpad.SetCompositorClockPaced(true);
    clock_paced_touchpad.RequestFrame();
    (void)clock_paced_touchpad.OnCompositorClockTick(
        true,
        true);
    Require(
        clock_paced_touchpad.TakeAction(
            touchpad_frame,
            true) == Action::RenderFrame,
        "clock-paced touchpad activity should render on its tick");
    clock_paced_touchpad.CompleteFrame(
        touchpad_frame,
        {.touchpad_active = true},
        FrameOutcome::Presented);
    Require(
        !clock_paced_touchpad.NextWakeDeadline(true, std::nullopt),
        "a clock-paced touchpad should pump input without forcing an unchanged render");

    Scheduler fallback_touchpad;
    SettleInitialFrame(fallback_touchpad, start);
    const auto fallback_frame = start + 3s;
    fallback_touchpad.RequestFrame();
    Require(
        fallback_touchpad.TakeAction(
            fallback_frame,
            true) == Action::RenderFrame,
        "fallback touchpad activity should render immediately");
    fallback_touchpad.CompleteFrame(
        fallback_frame,
        {.touchpad_active = true},
        FrameOutcome::Presented);
    Require(
        fallback_touchpad.NextWakeDeadline(true, std::nullopt) ==
            fallback_frame + Scheduler::kTouchpadFrameInterval,
        "touchpad pacing should retain a bounded fallback deadline when compositor ticks are unavailable");
}

void TestHiddenWindowIgnoresRenderDeadlinesButKeepsMaintenance()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);
    scheduler.RequestFrame(5s);

    const auto maintenance = start + 3s;
    Require(
        scheduler.NextWakeDeadline(false, maintenance) == maintenance,
        "hidden windows should wait only for maintenance work");
    Require(
        scheduler.TakeAction(start + 10s, false) ==
            Action::Wait,
        "hidden windows should not consume render demand");
    Require(
        scheduler.TakeAction(start + 10s, true) ==
            Action::RenderFrame,
        "hidden-window invalidation should remain pending for restore");
}

}  // namespace

int main()
{
    TestWindowInvalidationPersistsUntilRendered();
    TestClockPacingDefersInvalidationUntilPermitted();
    TestBusyAcquireWaitsForTheNextCompositorTick();
    TestBusyAcquireRetriesImmediatelyUnderOrdinaryPacing();
    TestBoostTransitionsPreservePendingRenderDemand();
    TestFailedCompositorClockTickEstablishesFallbackTouchpadCadence();
    TestTouchpadUpdateCanRunWithoutRendering();
    TestSettingsSaveWakeIsDebounced();
    TestSettingsSaveWakeStartsAfterTheDirtyFrame();
    TestPopupTransitionAnimatesForABoundedInterval();
    TestTextInputAndTouchpadExposeTimeDrivenDemand();
    TestHiddenWindowIgnoresRenderDeadlinesButKeepsMaintenance();
    return 0;
}
