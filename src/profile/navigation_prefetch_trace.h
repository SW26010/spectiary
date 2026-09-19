#pragma once

#include "domain/sample_navigation_direction.h"
#include "profile/navigation_latency_trace.h"

#include <cstddef>
#include <cstdint>

namespace spectiary {

class ProfileSink;

enum class NavigationPrefetchOutcome : std::uint8_t {
    Completed,
    Canceled,
    Stale,
    Failed,
    Consumed,
};

struct NavigationPrefetchReport {
    std::uint64_t prefetch_id = 0;
    std::uint64_t source_task_id = 0;
    std::size_t target_index = 0;
    SampleNavigationDirection direction =
        SampleNavigationDirection::Next;
    NavigationPrefetchOutcome outcome =
        NavigationPrefetchOutcome::Completed;
    NavigationLatencyTimePoint scheduled_at;
    NavigationLatencyTimePoint cancel_requested_at;
    NavigationLatencyTimePoint terminal_at;
};

[[nodiscard]] const char* NavigationPrefetchOutcomeName(
    NavigationPrefetchOutcome outcome) noexcept;
[[nodiscard]] const char* SampleNavigationDirectionName(
    SampleNavigationDirection direction) noexcept;
bool WriteNavigationPrefetchProfileEvent(
    ProfileSink& sink,
    const NavigationPrefetchReport& report);

}  // namespace spectiary
