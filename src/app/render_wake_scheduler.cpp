#include "app/render_wake_scheduler.h"
#include "profile/presentation_trace.h"

#include <algorithm>

namespace spectiary {
namespace {

void ConsiderEarlier(
    std::optional<RenderWakeScheduler::TimePoint>& deadline,
    RenderWakeScheduler::TimePoint candidate)
{
    if (!deadline || candidate < *deadline) {
        deadline = candidate;
    }
}

}  // namespace

void RenderWakeScheduler::RequestFrame(std::optional<Duration> settings_save_delay)
{
    presentation_trace::Invalidate();
    render_requested_ = true;
    if (!settings_save_delay || *settings_save_delay <= Duration::zero()) {
        return;
    }

    if (!pending_settings_save_delay_ || *settings_save_delay > *pending_settings_save_delay_) {
        pending_settings_save_delay_ = settings_save_delay;
    }
}

void RenderWakeScheduler::SetCompositorClockPaced(bool paced) noexcept
{
    if (paced == compositor_clock_paced_) {
        return;
    }
    compositor_clock_paced_ = paced;
    compositor_frame_permitted_ = false;
    touchpad_update_permitted_ = false;
}

void RenderWakeScheduler::SetContinuousRendering(bool active) noexcept
{
    continuous_rendering_ = active;
}

void RenderWakeScheduler::RequestTouchpadUpdate() noexcept
{
    touchpad_update_pending_ = true;
}

void RenderWakeScheduler::CancelTouchpadUpdate() noexcept
{
    touchpad_update_pending_ = false;
}

CompositorClockTickOutcome RenderWakeScheduler::OnCompositorClockTick(
    bool tick_consumed,
    bool compositor_clock_active)
{
    if (!tick_consumed) {
        return CompositorClockTickOutcome::Ignored;
    }

    SetCompositorClockPaced(compositor_clock_active);
    if (compositor_clock_active) {
        compositor_frame_permitted_ = true;
        touchpad_update_permitted_ = true;
        return CompositorClockTickOutcome::PermissionGranted;
    }

    RequestFrame();
    return CompositorClockTickOutcome::FallbackFrameRequested;
}

bool RenderWakeScheduler::RenderPermitted() const noexcept
{
    return !compositor_clock_paced_ || compositor_frame_permitted_;
}

bool RenderWakeScheduler::HasRenderWork(TimePoint now) const noexcept
{
    return continuous_rendering_ ||
           render_requested_ ||
           (next_frame_deadline_ && now >= *next_frame_deadline_) ||
           (settings_save_deadline_ && now >= *settings_save_deadline_);
}

RenderWakeAction RenderWakeScheduler::TakeAction(
    TimePoint now,
    bool window_renderable)
{
    if (!window_renderable) {
        return RenderWakeAction::Wait;
    }
    if (RenderPermitted() && HasRenderWork(now)) {
        if (compositor_clock_paced_) {
            compositor_frame_permitted_ = false;
            touchpad_update_permitted_ = false;
        }
        touchpad_update_pending_ = false;
        BeginFrame(now);
        return RenderWakeAction::RenderFrame;
    }
    if (compositor_clock_paced_ &&
        touchpad_update_pending_ &&
        touchpad_update_permitted_) {
        touchpad_update_pending_ = false;
        touchpad_update_permitted_ = false;
        return RenderWakeAction::PumpTouchpadUpdates;
    }
    return RenderWakeAction::Wait;
}

void RenderWakeScheduler::BeginFrame(TimePoint now)
{
    schedule_follow_up_ = render_requested_;
    render_requested_ = false;
    frame_settings_save_delay_ = pending_settings_save_delay_;
    pending_settings_save_delay_.reset();

    if (next_frame_deadline_ && now >= *next_frame_deadline_) {
        next_frame_deadline_.reset();
    }
    if (settings_save_deadline_ && now >= *settings_save_deadline_) {
        settings_save_deadline_.reset();
    }
}

void RenderWakeScheduler::CompleteFrame(
    TimePoint now,
    const RenderFrameActivity& activity,
    RenderFrameOutcome outcome)
{
    if (outcome != RenderFrameOutcome::Presented) {
        RequestFrame();
    }

    if (frame_settings_save_delay_) {
        const TimePoint candidate = now + *frame_settings_save_delay_;
        if (!settings_save_deadline_ || candidate > *settings_save_deadline_) {
            settings_save_deadline_ = candidate;
        }
        frame_settings_save_delay_.reset();
    }

    std::optional<TimePoint> next_frame;
    if (schedule_follow_up_ && !compositor_clock_paced_) {
        ConsiderEarlier(next_frame, now + kInteractiveFrameInterval);
    }
    schedule_follow_up_ = false;

    if (activity.popup_open != popup_open_) {
        popup_open_ = activity.popup_open;
        popup_animation_end_ = now + kPopupAnimationDuration;
    }
    if (popup_animation_end_) {
        if (now < *popup_animation_end_) {
            ConsiderEarlier(
                next_frame,
                std::min(now + kInteractiveFrameInterval, *popup_animation_end_));
        } else {
            popup_animation_end_.reset();
        }
    }

    if (activity.text_input_active) {
        ConsiderEarlier(next_frame, now + kTextCursorFrameInterval);
    }
    if (activity.touchpad_active && !compositor_clock_paced_) {
        ConsiderEarlier(next_frame, now + kTouchpadFrameInterval);
    }
    next_frame_deadline_ = next_frame;
}

std::optional<RenderWakeScheduler::TimePoint> RenderWakeScheduler::NextWakeDeadline(
    bool window_renderable,
    std::optional<TimePoint> maintenance_deadline) const
{
    std::optional<TimePoint> deadline = maintenance_deadline;
    if (!window_renderable) {
        return deadline;
    }

    if (RenderPermitted()) {
        if (continuous_rendering_ || render_requested_) {
            ConsiderEarlier(deadline, TimePoint::min());
        }
        if (next_frame_deadline_) {
            ConsiderEarlier(deadline, *next_frame_deadline_);
        }
        if (settings_save_deadline_) {
            ConsiderEarlier(deadline, *settings_save_deadline_);
        }
    }
    if (compositor_clock_paced_ &&
        touchpad_update_pending_ &&
        touchpad_update_permitted_) {
        ConsiderEarlier(deadline, TimePoint::min());
    }
    return deadline;
}

}  // namespace spectiary
