#pragma once

#include <array>
#include <cstdint>

namespace spectiary {
inline constexpr std::uint64_t kIncrementalBufferBudget = 256ULL * 1024 * 1024;
struct IncrementalBufferSlot {
    unsigned width = 0, height = 0;
    std::uint64_t generation = 0;
};
enum class BufferPlanAction { Select, Replace, Skip, BudgetExceeded };
struct BufferPlan {
    BufferPlanAction action = BufferPlanAction::Skip;
    int slot = -1;
    std::uint64_t peak_bytes = 0;
};
// Division before multiplication bounds both the logical budget and integer arithmetic.
inline constexpr std::uint64_t BufferPixelBytes(unsigned width, unsigned height) noexcept
{
    if (!width || !height || width > 16384 || height > 16384 ||
        static_cast<std::uint64_t>(width) > kIncrementalBufferBudget / 4 / height) return 0;
    return static_cast<std::uint64_t>(width) * height * 4;
}
inline BufferPlan PlanIncrementalBuffer(const std::array<IncrementalBufferSlot, 3>& slots,
    const std::array<bool, 3>& available, int bound, unsigned width, unsigned height) noexcept
{
    const auto requested = BufferPixelBytes(width, height);
    if (!requested) return {BufferPlanAction::BudgetExceeded};
    std::uint64_t live = 0;
    for (const auto& slot : slots) {
        const auto bytes = BufferPixelBytes(slot.width, slot.height);
        if (!bytes || bytes > kIncrementalBufferBudget - live) return {BufferPlanAction::BudgetExceeded};
        live += bytes;
    }
    for (int i = 0; i < 3; ++i)
        if (available[i] && slots[i].width == width && slots[i].height == height)
            return {BufferPlanAction::Select, i, live};
    if (requested > kIncrementalBufferBudget - live) return {BufferPlanAction::BudgetExceeded};
    for (int i = 0; i < 3; ++i)
        if (available[i] && i != bound) return {BufferPlanAction::Replace, i, live + requested};
    return {BufferPlanAction::Skip, -1, live};
}
} // namespace spectiary
