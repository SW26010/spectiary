#pragma once

#include "plot/plot_touchpad_gesture.h"

#include <cstdint>
#include <memory>

namespace spectiary {

enum class Win32TouchpadQueuedMessageAction {
    InvalidateRender,
    PumpUpdates,
};

using Win32TouchpadMessagePoster = bool (*)(std::uintptr_t, std::uint32_t) noexcept;

enum class Win32TouchpadWakePostResult {
    AlreadyPending,
    Posted,
    Failed,
};

// The caller serializes access to wake_pending with the gesture-state mutex.
[[nodiscard]] inline Win32TouchpadWakePostResult TryPostWin32TouchpadWake(
    std::uintptr_t native_window,
    std::uint32_t message,
    bool& wake_pending,
    Win32TouchpadMessagePoster post_message) noexcept
{
    if (wake_pending) {
        return Win32TouchpadWakePostResult::AlreadyPending;
    }

    wake_pending = true;
    if (post_message == nullptr || !post_message(native_window, message)) {
        wake_pending = false;
        return Win32TouchpadWakePostResult::Failed;
    }
    return Win32TouchpadWakePostResult::Posted;
}

[[nodiscard]] constexpr Win32TouchpadQueuedMessageAction
ClassifyWin32TouchpadQueuedMessage(
    std::uint32_t message,
    bool direct_manipulation_attached) noexcept
{
    // Direct Manipulation posts this undocumented internal update message while a
    // manual-update viewport is attached. It must still be dispatched and followed
    // by Update(), but treating it as a visual invalidation creates a render loop.
    constexpr std::uint32_t kDirectManipulationInternalUpdateMessage = 0x0096U;
    if (direct_manipulation_attached &&
        message == kDirectManipulationInternalUpdateMessage) {
        return Win32TouchpadQueuedMessageAction::PumpUpdates;
    }
    return Win32TouchpadQueuedMessageAction::InvalidateRender;
}

class Win32TouchpadGestureSource final : public PlotTouchpadGestureSource {
public:
    Win32TouchpadGestureSource();
    ~Win32TouchpadGestureSource() override;

    Win32TouchpadGestureSource(const Win32TouchpadGestureSource&) = delete;
    Win32TouchpadGestureSource& operator=(const Win32TouchpadGestureSource&) = delete;

    [[nodiscard]] PlotTouchpadGestureBatch Poll(std::uintptr_t native_window) override;
    void SetTarget(const PlotTouchpadTarget& target) override;
    void ClearTarget() override;
    void PumpUpdates();
    [[nodiscard]] bool OwnsWindow(std::uintptr_t native_window) const noexcept;
    [[nodiscard]] bool NeedsContinuousUpdates() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spectiary
