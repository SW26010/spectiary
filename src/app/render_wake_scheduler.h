#pragma once

#include <chrono>
#include <optional>

namespace specforge {

struct RenderFrameActivity {
    bool touchpad_active = false;
    bool compositor_clock_paced = false;
    bool text_input_active = false;
    bool popup_open = false;
};

enum class CompositorClockTickAction {
    None,
    GrantFramePermission,
    RequestFallbackFrame,
};

[[nodiscard]] constexpr CompositorClockTickAction ClassifyCompositorClockTick(
    bool tick_consumed,
    bool compositor_clock_active) noexcept
{
    if (!tick_consumed) {
        return CompositorClockTickAction::None;
    }
    return compositor_clock_active
               ? CompositorClockTickAction::GrantFramePermission
               : CompositorClockTickAction::RequestFallbackFrame;
}

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
    [[nodiscard]] bool ShouldRender(TimePoint now, bool render_permitted = true) const;
    void BeginFrame(TimePoint now);
    void EndFrame(TimePoint now, const RenderFrameActivity& activity);

    [[nodiscard]] std::optional<TimePoint> NextWakeDeadline(
        bool window_renderable,
        std::optional<TimePoint> maintenance_deadline,
        bool render_permitted = true) const;

private:
    bool render_requested_ = true;
    bool schedule_follow_up_ = false;
    bool popup_open_ = false;
    std::optional<Duration> pending_settings_save_delay_;
    std::optional<Duration> frame_settings_save_delay_;
    std::optional<TimePoint> next_frame_deadline_;
    std::optional<TimePoint> popup_animation_end_;
    std::optional<TimePoint> settings_save_deadline_;
};

}  // namespace specforge
