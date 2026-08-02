#pragma once

#include <cstdint>

namespace specforge {

class AutomationPanelMutationChain {
public:
    void RecordAppliedMutation(
        bool previous_visible,
        bool requested_visible,
        std::uint64_t generation,
        std::uint64_t accepted_frame) noexcept
    {
        if (!active_) {
            baseline_visible_ = previous_visible;
        }
        active_ = true;
        requested_visible_ = requested_visible;
        generation_ = generation;
        accepted_frame_ = accepted_frame;
    }

    void Clear() noexcept
    {
        active_ = false;
    }

    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

    [[nodiscard]] bool baseline_visible() const noexcept
    {
        return baseline_visible_;
    }

    [[nodiscard]] bool requested_visible() const noexcept
    {
        return requested_visible_;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] std::uint64_t accepted_frame() const noexcept
    {
        return accepted_frame_;
    }

private:
    bool active_ = false;
    bool baseline_visible_ = true;
    bool requested_visible_ = true;
    std::uint64_t generation_ = 0;
    std::uint64_t accepted_frame_ = 0;
};

}  // namespace specforge
