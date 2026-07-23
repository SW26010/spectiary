#include "profile/navigation_prefetch_trace.h"

#include "profile/profile_sink.h"

#include <chrono>
#include <string>

namespace specforge {
namespace {

std::int64_t ToNanoseconds(NavigationLatencyTimePoint at) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               at.time_since_epoch())
        .count();
}

std::string DurationMilliseconds(
    NavigationLatencyTimePoint begin,
    NavigationLatencyTimePoint end)
{
    const double milliseconds =
        std::chrono::duration<double, std::milli>(end - begin).count();
    return std::to_string(milliseconds);
}

}  // namespace

const char* NavigationPrefetchOutcomeName(
    NavigationPrefetchOutcome outcome) noexcept
{
    switch (outcome) {
    case NavigationPrefetchOutcome::Completed:
        return "completed";
    case NavigationPrefetchOutcome::Canceled:
        return "canceled";
    case NavigationPrefetchOutcome::Stale:
        return "stale";
    case NavigationPrefetchOutcome::Failed:
        return "failed";
    case NavigationPrefetchOutcome::Consumed:
        return "consumed";
    }
    return "failed";
}

const char* SampleNavigationDirectionName(
    SampleNavigationDirection direction) noexcept
{
    switch (direction) {
    case SampleNavigationDirection::Previous:
        return "previous";
    case SampleNavigationDirection::Next:
        return "next";
    }
    return "next";
}

bool WriteNavigationPrefetchProfileEvent(
    ProfileSink& sink,
    const NavigationPrefetchReport& report)
{
    return sink.WriteEvent("navigation_prefetch", {
        ProfileSink::Field::Number(
            "prefetch_id",
            std::to_string(report.prefetch_id)),
        ProfileSink::Field::Number(
            "source_task_id",
            std::to_string(report.source_task_id)),
        ProfileSink::Field::Number(
            "target_index",
            std::to_string(report.target_index)),
        ProfileSink::Field::String(
            "direction",
            SampleNavigationDirectionName(report.direction)),
        ProfileSink::Field::String(
            "outcome",
            NavigationPrefetchOutcomeName(report.outcome)),
        ProfileSink::Field::Number(
            "scheduled_steady_ns",
            std::to_string(ToNanoseconds(report.scheduled_at))),
        ProfileSink::Field::Number(
            "cancel_requested_steady_ns",
            std::to_string(
                ToNanoseconds(
                    report.cancel_requested_at))),
        ProfileSink::Field::Number(
            "terminal_steady_ns",
            std::to_string(ToNanoseconds(report.terminal_at))),
        ProfileSink::Field::Number(
            "duration_ms",
            DurationMilliseconds(
                report.scheduled_at,
                report.terminal_at)),
    });
}

}  // namespace specforge
