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

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void SettleInitialFrame(Scheduler& scheduler, Scheduler::TimePoint start)
{
    Require(scheduler.ShouldRender(start), "scheduler should request the initial frame");
    scheduler.BeginFrame(start);
    scheduler.EndFrame(start, {});

    const auto follow_up = start + Scheduler::kInteractiveFrameInterval;
    Require(scheduler.ShouldRender(follow_up), "initial frame should receive one state-settling follow-up");
    scheduler.BeginFrame(follow_up);
    scheduler.EndFrame(follow_up, {});
    Require(!scheduler.NextWakeDeadline(true, std::nullopt), "settled scheduler should become fully idle");
}

void TestWindowInvalidationPersistsUntilRendered()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame();
    Require(scheduler.ShouldRender(start + 1s), "window invalidation should request a frame");
    Require(scheduler.ShouldRender(start + 2s), "unconsumed window invalidation should remain pending");

    scheduler.BeginFrame(start + 2s);
    scheduler.EndFrame(start + 2s, {});
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

    scheduler.RequestFrame();
    const auto input_time = start + 1s;
    Require(
        !scheduler.ShouldRender(input_time, false),
        "input invalidation should wait for compositor-clock permission");
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt, false),
        "a deferred invalidation should not create a zero-time busy-loop deadline");

    const auto maintenance = input_time + 5s;
    Require(
        scheduler.NextWakeDeadline(true, maintenance, false) == maintenance,
        "clock pacing should preserve independent maintenance deadlines");
    Require(
        scheduler.ShouldRender(input_time, true),
        "the pending invalidation should render as soon as a compositor tick permits it");

    scheduler.BeginFrame(input_time);
    scheduler.EndFrame(input_time, {.compositor_clock_paced = true});
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt),
        "a compositor-paced frame should not schedule a competing timer frame");
}

void TestRetryRequestedDuringFrameSurvivesClockPacedEndFrame()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto busy_frame = start + 1s;
    scheduler.RequestFrame();
    scheduler.BeginFrame(busy_frame);
    scheduler.RequestFrame();
    scheduler.EndFrame(
        busy_frame,
        {.compositor_clock_paced = true});

    Require(
        !scheduler.ShouldRender(busy_frame, false),
        "a busy-frame retry should continue waiting without compositor permission");
    Require(
        !scheduler.NextWakeDeadline(true, std::nullopt, false),
        "a clock-paced retry without a tick must not create a zero-time busy-loop deadline");
    Require(
        scheduler.ShouldRender(busy_frame, true),
        "the retry requested during rendering must survive EndFrame and the next compositor tick");
}

void TestFailedCompositorClockTickEstablishesFallbackTouchpadCadence()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    const auto failure_time = start + 1s;
    const auto action = specforge::ClassifyCompositorClockTick(true, false);
    Require(
        action == specforge::CompositorClockTickAction::RequestFallbackFrame,
        "a consumed tick from an inactive clock must select fallback transition work");
    scheduler.RequestFrame();

    Require(
        scheduler.ShouldRender(failure_time),
        "the final tick from a failed compositor waiter must request a transition frame");
    scheduler.BeginFrame(failure_time);
    scheduler.EndFrame(failure_time, {.touchpad_active = true});
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            failure_time + Scheduler::kTouchpadFrameInterval,
        "the transition frame must establish bounded touchpad fallback pacing");

    Require(
        specforge::ClassifyCompositorClockTick(false, false) ==
            specforge::CompositorClockTickAction::None,
        "a stale tick message must not create a fallback transition frame");
    Require(
        specforge::ClassifyCompositorClockTick(true, true) ==
            specforge::CompositorClockTickAction::GrantFramePermission,
        "a normal active-clock tick must remain permission-only");
}

void TestSettingsSaveWakeIsDebounced()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame(5s);
    scheduler.BeginFrame(start + 1s);
    scheduler.EndFrame(start + 1s, {});
    scheduler.RequestFrame(5s);
    scheduler.BeginFrame(start + 2s);
    scheduler.EndFrame(start + 2s, {});

    scheduler.BeginFrame(start + 2s + Scheduler::kInteractiveFrameInterval);
    scheduler.EndFrame(start + 2s + Scheduler::kInteractiveFrameInterval, {});
    Require(!scheduler.ShouldRender(start + 6999ms), "settings save wake should wait for the last invalidation");
    Require(scheduler.ShouldRender(start + 7s), "settings save wake should fire after the debounce");

    scheduler.BeginFrame(start + 7s);
    scheduler.EndFrame(start + 7s, {});
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
    scheduler.BeginFrame(dirty_frame_start);
    RenderImGuiSettingsFrame(1.0f / 60.0f, true);
    scheduler.EndFrame(dirty_frame_end, {});

    const auto follow_up = dirty_frame_end + Scheduler::kInteractiveFrameInterval;
    scheduler.BeginFrame(follow_up);
    RenderImGuiSettingsFrame(0.025f, false);
    scheduler.EndFrame(follow_up, {});

    const auto save_wake = scheduler.NextWakeDeadline(true, std::nullopt);
    Require(save_wake.has_value(), "dirty ImGui settings should retain a save wake");
    scheduler.BeginFrame(*save_wake);
    const float final_delta = std::chrono::duration<float>(*save_wake - follow_up).count();
    RenderImGuiSettingsFrame(final_delta, false);
    scheduler.EndFrame(*save_wake, {});

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
    scheduler.BeginFrame(start + 1s);
    scheduler.EndFrame(start + 1s, {.popup_open = true});
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            start + 1s + Scheduler::kInteractiveFrameInterval,
        "opening a popup should schedule animation frames");

    const auto animation_end = start + 1s + Scheduler::kPopupAnimationDuration;
    scheduler.BeginFrame(animation_end);
    scheduler.EndFrame(animation_end, {.popup_open = true});
    Require(!scheduler.NextWakeDeadline(true, std::nullopt), "popup animation should stop after settling");
}

void TestTextInputAndTouchpadExposeTimeDrivenDemand()
{
    const Scheduler::TimePoint start{};
    Scheduler scheduler;
    SettleInitialFrame(scheduler, start);

    scheduler.RequestFrame();
    scheduler.BeginFrame(start + 1s);
    scheduler.EndFrame(start + 1s, {.text_input_active = true});
    scheduler.BeginFrame(start + 1s + Scheduler::kInteractiveFrameInterval);
    scheduler.EndFrame(start + 1s + Scheduler::kInteractiveFrameInterval, {.text_input_active = true});
    Require(
        scheduler.NextWakeDeadline(true, std::nullopt) ==
            start + 1s + Scheduler::kInteractiveFrameInterval + Scheduler::kTextCursorFrameInterval,
        "active text input should keep the cursor clock moving at a bounded cadence");

    Scheduler clock_paced_touchpad;
    SettleInitialFrame(clock_paced_touchpad, start);
    const auto touchpad_frame = start + 2s;
    clock_paced_touchpad.RequestFrame();
    clock_paced_touchpad.BeginFrame(touchpad_frame);
    clock_paced_touchpad.EndFrame(
        touchpad_frame,
        {.touchpad_active = true, .compositor_clock_paced = true});
    Require(
        !clock_paced_touchpad.NextWakeDeadline(true, std::nullopt),
        "a clock-paced touchpad should pump input without forcing an unchanged render");

    Scheduler fallback_touchpad;
    SettleInitialFrame(fallback_touchpad, start);
    const auto fallback_frame = start + 3s;
    fallback_touchpad.RequestFrame();
    fallback_touchpad.BeginFrame(fallback_frame);
    fallback_touchpad.EndFrame(fallback_frame, {.touchpad_active = true});
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
    Require(scheduler.ShouldRender(start + 10s), "hidden-window invalidation should remain pending for restore");
}

}  // namespace

int main()
{
    TestWindowInvalidationPersistsUntilRendered();
    TestClockPacingDefersInvalidationUntilPermitted();
    TestRetryRequestedDuringFrameSurvivesClockPacedEndFrame();
    TestFailedCompositorClockTickEstablishesFallbackTouchpadCadence();
    TestSettingsSaveWakeIsDebounced();
    TestSettingsSaveWakeStartsAfterTheDirtyFrame();
    TestPopupTransitionAnimatesForABoundedInterval();
    TestTextInputAndTouchpadExposeTimeDrivenDemand();
    TestHiddenWindowIgnoresRenderDeadlinesButKeepsMaintenance();
    return 0;
}
