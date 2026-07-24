#pragma once

#include <chrono>
#include <optional>

namespace specforge {

struct RenderFrameActivity {
    bool touchpad_active = false;
    bool text_input_active = false;
    bool popup_open = false;
};

enum class RenderFrameOutcome {
    Presented,
    AcquireRetry,
    PresentRetry,
};

enum class RenderWakeAction {
    Wait,
    RenderFrame,
    PumpTouchpadUpdates,
};

enum class CompositorClockTickOutcome {
    Ignored,
    PermissionGranted,
    FallbackFrameRequested,
};

class RenderWakeScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    inline static constexpr Duration kInteractiveFrameInterval = std::chrono::milliseconds(16);
    inline static constexpr Duration kTouchpadFrameInterval = std::chrono::milliseconds(9);
    inline static constexpr Duration kPopupAnimationDuration = std::chrono::milliseconds(200);
    inline static constexpr Duration kTextCursorFrameInterval = std::chrono::milliseconds(400);

    void RequestFrame(std::optional<Duration> settings_save_delay = std::nullopt);
    void SetCompositorClockPaced(bool paced) noexcept;
    void RequestTouchpadUpdate() noexcept;
    void CancelTouchpadUpdate() noexcept;
    [[nodiscard]] CompositorClockTickOutcome OnCompositorClockTick(
        bool tick_consumed,
        bool compositor_clock_active);
    [[nodiscard]] RenderWakeAction TakeAction(
        TimePoint now,
        bool window_renderable);
    void CompleteFrame(
        TimePoint now,
        const RenderFrameActivity& activity,
        RenderFrameOutcome outcome);

    [[nodiscard]] std::optional<TimePoint> NextWakeDeadline(
        bool window_renderable,
        std::optional<TimePoint> maintenance_deadline) const;

private:
    [[nodiscard]] bool RenderPermitted() const noexcept;
    [[nodiscard]] bool HasRenderWork(TimePoint now) const noexcept;
    void BeginFrame(TimePoint now);

    bool render_requested_ = true;
    bool schedule_follow_up_ = false;
    bool compositor_clock_paced_ = false;
    bool compositor_frame_permitted_ = false;
    bool touchpad_update_pending_ = false;
    bool touchpad_update_permitted_ = false;
    bool popup_open_ = false;
    std::optional<Duration> pending_settings_save_delay_;
    std::optional<Duration> frame_settings_save_delay_;
    std::optional<TimePoint> next_frame_deadline_;
    std::optional<TimePoint> popup_animation_end_;
    std::optional<TimePoint> settings_save_deadline_;
};

}  // namespace specforge
