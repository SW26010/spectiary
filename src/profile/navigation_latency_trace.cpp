#include "profile/navigation_latency_trace.h"

#include "profile/profile_sink.h"

#include <charconv>
#include <string>
#include <utility>

namespace spectiary {
namespace {

std::string NumberOrNull(std::int64_t value)
{
    return value > 0 ? std::to_string(value) : "null";
}

std::string Milliseconds(double value)
{
    char buffer[64] = {};
    const auto result = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::fixed,
        4);
    return result.ec == std::errc() ? std::string(buffer, result.ptr) : "null";
}

std::string DurationMilliseconds(std::int64_t begin_ns, std::int64_t end_ns)
{
    if (begin_ns <= 0 || end_ns < begin_ns) {
        return "null";
    }
    return Milliseconds(static_cast<double>(end_ns - begin_ns) / 1'000'000.0);
}

std::string NanosecondsAsMilliseconds(std::int64_t duration_ns)
{
    return duration_ns >= 0
        ? Milliseconds(static_cast<double>(duration_ns) / 1'000'000.0)
        : "null";
}

template <typename TimestampGetter>
std::string AttemptDurationSum(
    const std::vector<NavigationLatencyAttemptReport>& attempts,
    TimestampGetter&& timestamps)
{
    if (attempts.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const NavigationLatencyAttemptReport& attempt : attempts) {
        const auto [begin_ns, end_ns] = timestamps(attempt);
        if (begin_ns <= 0 || end_ns < begin_ns) {
            return "null";
        }
        total_ns += end_ns - begin_ns;
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

std::string RetargetGapMilliseconds(
    const std::vector<NavigationLatencyAttemptReport>& attempts)
{
    std::int64_t total_ns = 0;
    for (std::size_t index = 1; index < attempts.size(); ++index) {
        const std::int64_t previous_end = attempts[index - 1].completion_drained_ns;
        const std::int64_t next_begin = attempts[index].load_enqueued_ns;
        if (previous_end <= 0 || next_begin < previous_end) {
            return "null";
        }
        total_ns += next_begin - previous_end;
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

template <typename TimestampGetter>
std::string PreparationDurationSum(
    const NavigationLatencyAttemptReport& attempt,
    TimestampGetter&& timestamps)
{
    if (attempt.preparation_rounds.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const NavigationLatencyPreparationRoundReport& round : attempt.preparation_rounds) {
        const auto [begin_ns, end_ns] = timestamps(round);
        if (begin_ns <= 0 || end_ns < begin_ns) {
            return "null";
        }
        total_ns += end_ns - begin_ns;
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

template <typename TimestampGetter>
std::string AttemptPreparationDurationSum(
    const std::vector<NavigationLatencyAttemptReport>& attempts,
    TimestampGetter&& timestamps)
{
    if (attempts.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const NavigationLatencyAttemptReport& attempt : attempts) {
        if (attempt.preparation_rounds.empty()) {
            return "null";
        }
        for (const NavigationLatencyPreparationRoundReport& round : attempt.preparation_rounds) {
            const auto [begin_ns, end_ns] = timestamps(round);
            if (begin_ns <= 0 || end_ns < begin_ns) {
                return "null";
            }
            total_ns += end_ns - begin_ns;
        }
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

bool WritePreparationRoundEvent(
    ProfileSink& sink,
    std::uint64_t navigation_id,
    const NavigationLatencyAttemptReport& attempt,
    const NavigationLatencyPreparationRoundReport& round)
{
    return sink.WriteEvent("navigation_latency_preparation_round", {
        ProfileSink::Field::Number("navigation_id", std::to_string(navigation_id)),
        ProfileSink::Field::Number("attempt_index", std::to_string(attempt.attempt_index)),
        ProfileSink::Field::Number("preparation_round_index", std::to_string(round.round_index)),
        ProfileSink::Field::Number("target_index", std::to_string(attempt.target_index)),
        ProfileSink::Field::Number("source_task_id", std::to_string(attempt.source_task_id)),
        ProfileSink::Field::String("source_kind", round.source_is_folder ? "folder" : "file"),
        ProfileSink::Field::Bool("hint_present", round.hint_present),
        ProfileSink::Field::Bool(
            "generation_current_at_start",
            round.generation_current_at_start),
        ProfileSink::Field::Bool(
            "listing_scan_performed",
            round.listing_scan_performed),
        ProfileSink::Field::Bool("context_reused", round.context_reused),
        ProfileSink::Field::Bool("revalidation_succeeded", round.revalidation_succeeded),
        ProfileSink::Field::Number("preparation_started_steady_ns", NumberOrNull(round.preparation_started_ns)),
        ProfileSink::Field::Number("snapshot_load_started_steady_ns", NumberOrNull(round.snapshot_load_started_ns)),
        ProfileSink::Field::Number("snapshot_load_finished_steady_ns", NumberOrNull(round.snapshot_load_finished_ns)),
        ProfileSink::Field::Number("context_prepared_steady_ns", NumberOrNull(round.context_prepared_ns)),
        ProfileSink::Field::Number("source_revalidated_steady_ns", NumberOrNull(round.source_revalidated_ns)),
        ProfileSink::Field::Number("source_inspection_ms", DurationMilliseconds(round.preparation_started_ns, round.snapshot_load_started_ns)),
        ProfileSink::Field::Number("decode_ms", DurationMilliseconds(round.snapshot_load_started_ns, round.snapshot_load_finished_ns)),
        ProfileSink::Field::Number("context_prepare_ms", DurationMilliseconds(round.snapshot_load_finished_ns, round.context_prepared_ns)),
        ProfileSink::Field::Number("source_revalidation_ms", DurationMilliseconds(round.context_prepared_ns, round.source_revalidated_ns)),
        ProfileSink::Field::Number("round_total_ms", DurationMilliseconds(round.preparation_started_ns, round.source_revalidated_ns)),
    });
}

bool WriteAttemptEvent(
    ProfileSink& sink,
    std::uint64_t navigation_id,
    const NavigationLatencyAttemptReport& attempt)
{
    bool accepted = true;
    for (const NavigationLatencyPreparationRoundReport& round : attempt.preparation_rounds) {
        accepted = WritePreparationRoundEvent(sink, navigation_id, attempt, round) && accepted;
    }
    const bool attempt_accepted = sink.WriteEvent("navigation_latency_attempt", {
        ProfileSink::Field::Number("navigation_id", std::to_string(navigation_id)),
        ProfileSink::Field::Number("attempt_index", std::to_string(attempt.attempt_index)),
        ProfileSink::Field::Number("target_index", std::to_string(attempt.target_index)),
        ProfileSink::Field::Number("source_task_id", std::to_string(attempt.source_task_id)),
        ProfileSink::Field::String("source_kind", attempt.source_is_folder ? "folder" : "file"),
        ProfileSink::Field::Bool("workflow_reused", attempt.workflow_reused),
        ProfileSink::Field::Bool("context_reused", attempt.context_reused),
        ProfileSink::Field::Number("preparation_round_count", std::to_string(attempt.preparation_rounds.size())),
        ProfileSink::Field::Number("load_enqueued_steady_ns", NumberOrNull(attempt.load_enqueued_ns)),
        ProfileSink::Field::Number("worker_started_steady_ns", NumberOrNull(attempt.worker_started_ns)),
        ProfileSink::Field::Number("snapshot_load_started_steady_ns", NumberOrNull(attempt.snapshot_load_started_ns)),
        ProfileSink::Field::Number("snapshot_load_finished_steady_ns", NumberOrNull(attempt.snapshot_load_finished_ns)),
        ProfileSink::Field::Number("context_prepared_steady_ns", NumberOrNull(attempt.context_prepared_ns)),
        ProfileSink::Field::Number("source_revalidated_steady_ns", NumberOrNull(attempt.source_revalidated_ns)),
        ProfileSink::Field::Number("worker_prepared_steady_ns", NumberOrNull(attempt.worker_prepared_ns)),
        ProfileSink::Field::Number("completion_ready_steady_ns", NumberOrNull(attempt.completion_ready_ns)),
        ProfileSink::Field::Number("completion_published_steady_ns", NumberOrNull(attempt.completion_published_ns)),
        ProfileSink::Field::Number("completion_drained_steady_ns", NumberOrNull(attempt.completion_drained_ns)),
        ProfileSink::Field::Number("queue_wait_ms", DurationMilliseconds(attempt.load_enqueued_ns, attempt.worker_started_ns)),
        ProfileSink::Field::Number("source_inspection_ms", PreparationDurationSum(
            attempt,
            [](const auto& round) { return std::pair{round.preparation_started_ns, round.snapshot_load_started_ns}; })),
        ProfileSink::Field::Number("decode_ms", PreparationDurationSum(
            attempt,
            [](const auto& round) { return std::pair{round.snapshot_load_started_ns, round.snapshot_load_finished_ns}; })),
        ProfileSink::Field::Number("context_prepare_ms", PreparationDurationSum(
            attempt,
            [](const auto& round) { return std::pair{round.snapshot_load_finished_ns, round.context_prepared_ns}; })),
        ProfileSink::Field::Number("source_revalidation_ms", PreparationDurationSum(
            attempt,
            [](const auto& round) { return std::pair{round.context_prepared_ns, round.source_revalidated_ns}; })),
        ProfileSink::Field::Number("workflow_prepare_ms", DurationMilliseconds(attempt.source_revalidated_ns, attempt.worker_prepared_ns)),
        ProfileSink::Field::Number("completion_ready_ms", DurationMilliseconds(attempt.worker_prepared_ns, attempt.completion_ready_ns)),
        ProfileSink::Field::Number("ordered_publish_wait_ms", DurationMilliseconds(attempt.completion_ready_ns, attempt.completion_published_ns)),
        ProfileSink::Field::Number("completion_service_wait_ms", DurationMilliseconds(attempt.completion_published_ns, attempt.completion_drained_ns)),
        ProfileSink::Field::Number("attempt_total_ms", DurationMilliseconds(attempt.load_enqueued_ns, attempt.completion_drained_ns)),
    });
    return accepted && attempt_accepted;
}

}  // namespace

NavigationLatencyTrace::NavigationLatencyTrace(
    std::uint64_t navigation_id,
    std::size_t from_index,
    std::size_t target_index,
    NavigationLatencyInputKind input_kind,
    NavigationLatencyTimePoint input_at,
    NavigationLatencyTimePoint requested_at,
    NavigationLatencyTimePoint target_resolved_at,
    NavigationTargetResolutionReport target_resolution)
    : navigation_id_(navigation_id),
      from_index_(from_index),
      input_kind_(input_kind),
      input_ns_(LoadLatencyNanoseconds(input_at)),
      requested_ns_(LoadLatencyNanoseconds(requested_at)),
      target_resolved_ns_(LoadLatencyNanoseconds(target_resolved_at)),
      target_resolution_(std::move(target_resolution)),
      terminal_presentation_(target_index)
{
    const std::int64_t measured_ns = target_resolved_ns_ - requested_ns_;
    const std::int64_t attributed_ns =
        target_resolution_.effective_index_ns +
        target_resolution_.pending_activation_supersede_ns +
        target_resolution_.base_sequence_ns +
        target_resolution_.target_lookup_ns +
        target_resolution_.target_sequence_ns;
    target_resolution_.navigation_state_result_ns =
        measured_ns >= attributed_ns ? measured_ns - attributed_ns : 0;
}

NavigationLatencyTimePoint NavigationLatencyTrace::Now() noexcept
{
    return NavigationLatencyClock::now();
}

void NavigationLatencyTrace::SetTargetIndex(std::size_t target_index) noexcept
{
    terminal_presentation_.SetTargetIndex(target_index);
}

void NavigationLatencyTrace::SetCacheHit(bool cache_hit) noexcept
{
    SetCacheKind(
        cache_hit ? NavigationSnapshotCacheKind::History
                  : NavigationSnapshotCacheKind::None);
}

void NavigationLatencyTrace::SetCacheKind(
    NavigationSnapshotCacheKind cache_kind) noexcept
{
    cache_kind_.store(cache_kind, std::memory_order_relaxed);
}

NavigationLatencyAttemptHandle NavigationLatencyTrace::BeginLoadAttempt(
    std::size_t target_index,
    NavigationLatencyTimePoint at)
{
    return load_attempts_.Begin(target_index, at);
}

void NavigationLatencyTrace::MarkSnapshotActivated(
    std::uint64_t frame_index,
    NavigationLatencyTimePoint at) noexcept
{
    terminal_presentation_.MarkSnapshotActivated(frame_index, at);
}

void NavigationLatencyTrace::MarkUiUpdated(NavigationLatencyTimePoint at) noexcept
{
    terminal_presentation_.MarkUiUpdated(at);
}

bool NavigationLatencyTrace::MarkPresentedForViewport(
    std::uint64_t frame_index,
    unsigned int viewport_id,
    NavigationLatencyTimePoint at) noexcept
{
    return terminal_presentation_.MarkPresentedForViewport(
        frame_index,
        viewport_id,
        at);
}

bool NavigationLatencyTrace::MarkTerminal(
    NavigationLatencyOutcome outcome,
    NavigationLatencyTimePoint at) noexcept
{
    return terminal_presentation_.MarkTerminal(outcome, at);
}

std::optional<NavigationLatencyReport> NavigationLatencyTrace::TerminalReport() const noexcept
{
    const auto lifecycle = terminal_presentation_.TerminalSnapshot();
    if (!lifecycle) {
        return std::nullopt;
    }

    NavigationLatencyReport report;
    report.navigation_id = navigation_id_;
    report.activation_frame = lifecycle->activation_frame;
    report.presentation_viewport_id =
        lifecycle->presentation_viewport_id;
    report.from_index = from_index_;
    report.target_index = lifecycle->target_index;
    report.input_kind = input_kind_;
    report.outcome = lifecycle->outcome;
    report.cache_kind =
        cache_kind_.load(std::memory_order_relaxed);
    report.cache_hit =
        report.cache_kind != NavigationSnapshotCacheKind::None;
    report.target_resolution = target_resolution_;
    report.input_ns = input_ns_;
    report.requested_ns = requested_ns_;
    report.target_resolved_ns = target_resolved_ns_;
    report.snapshot_activated_ns = lifecycle->snapshot_activated_ns;
    report.ui_updated_ns = lifecycle->ui_updated_ns;
    report.first_present_ns = lifecycle->first_present_ns;
    report.terminal_ns = lifecycle->terminal_ns;
    report.attempts = load_attempts_.Reports();
    return report;
}

const char* NavigationLatencyInputKindName(NavigationLatencyInputKind kind) noexcept
{
    switch (kind) {
    case NavigationLatencyInputKind::KeyboardPrevious:
        return "keyboard_previous";
    case NavigationLatencyInputKind::KeyboardNext:
        return "keyboard_next";
    case NavigationLatencyInputKind::UiPrevious:
        return "ui_previous";
    case NavigationLatencyInputKind::UiNext:
        return "ui_next";
    case NavigationLatencyInputKind::AutoAdvance:
        return "auto_advance";
    }
    return "unknown";
}

const char* NavigationLatencyOutcomeName(NavigationLatencyOutcome outcome) noexcept
{
    switch (outcome) {
    case NavigationLatencyOutcome::Pending:
        return "pending";
    case NavigationLatencyOutcome::Presented:
        return "presented";
    case NavigationLatencyOutcome::Failed:
        return "failed";
    case NavigationLatencyOutcome::Rejected:
        return "rejected";
    case NavigationLatencyOutcome::Superseded:
        return "superseded";
    case NavigationLatencyOutcome::Coalesced:
        return "coalesced";
    }
    return "unknown";
}

const char* NavigationSnapshotCacheKindName(
    NavigationSnapshotCacheKind kind) noexcept
{
    switch (kind) {
    case NavigationSnapshotCacheKind::None:
        return "none";
    case NavigationSnapshotCacheKind::History:
        return "history";
    case NavigationSnapshotCacheKind::Prefetch:
        return "prefetch";
    }
    return "none";
}

bool WriteNavigationLatencyProfileEvent(ProfileSink& sink, const NavigationLatencyReport& report)
{
    bool accepted = true;
    for (const NavigationLatencyAttemptReport& attempt : report.attempts) {
        accepted = WriteAttemptEvent(sink, report.navigation_id, attempt) && accepted;
    }

    const std::int64_t total_end_ns = report.first_present_ns > 0
        ? report.first_present_ns
        : report.terminal_ns;
    const std::int64_t first_enqueued_ns = report.attempts.empty()
        ? 0
        : report.attempts.front().load_enqueued_ns;
    const std::int64_t final_drained_ns = report.attempts.empty()
        ? 0
        : report.attempts.back().completion_drained_ns;

    const auto sum = [&report](auto begin, auto end) {
        return AttemptDurationSum(
            report.attempts,
            [begin, end](const NavigationLatencyAttemptReport& attempt) {
                return std::pair{begin(attempt), end(attempt)};
            });
    };

    const bool summary_accepted = sink.WriteEvent("navigation_latency", {
        ProfileSink::Field::Number("navigation_id", std::to_string(report.navigation_id)),
        ProfileSink::Field::Number("activation_frame", std::to_string(report.activation_frame)),
        ProfileSink::Field::Number("presentation_viewport_id", std::to_string(report.presentation_viewport_id)),
        ProfileSink::Field::String("outcome", NavigationLatencyOutcomeName(report.outcome)),
        ProfileSink::Field::String("input_kind", NavigationLatencyInputKindName(report.input_kind)),
        ProfileSink::Field::Number("from_index", std::to_string(report.from_index)),
        ProfileSink::Field::Number("target_index", std::to_string(report.target_index)),
        ProfileSink::Field::Bool("cache_hit", report.cache_hit),
        ProfileSink::Field::String(
            "cache_kind",
            NavigationSnapshotCacheKindName(report.cache_kind)),
        ProfileSink::Field::Number(
            "row_count",
            std::to_string(report.target_resolution.row_count)),
        ProfileSink::Field::Bool(
            "filter_active",
            report.target_resolution.filter_active),
        ProfileSink::Field::Bool(
            "sort_active",
            report.target_resolution.sort_active),
        ProfileSink::Field::Bool(
            "query_active",
            report.target_resolution.query_active),
        ProfileSink::Field::Bool(
            "pending_present",
            report.target_resolution.pending_present),
        ProfileSink::Field::Bool(
            "sequence_cache_hit",
            report.target_resolution.sequence_cache_hit),
        ProfileSink::Field::Number(
            "sequence_build_count",
            std::to_string(report.target_resolution.sequence_build_count)),
        ProfileSink::Field::Number("attempt_count", std::to_string(report.attempts.size())),
        ProfileSink::Field::Number("input_steady_ns", NumberOrNull(report.input_ns)),
        ProfileSink::Field::Number("requested_steady_ns", NumberOrNull(report.requested_ns)),
        ProfileSink::Field::Number("target_resolved_steady_ns", NumberOrNull(report.target_resolved_ns)),
        ProfileSink::Field::Number("snapshot_activated_steady_ns", NumberOrNull(report.snapshot_activated_ns)),
        ProfileSink::Field::Number("ui_updated_steady_ns", NumberOrNull(report.ui_updated_ns)),
        ProfileSink::Field::Number("first_present_steady_ns", NumberOrNull(report.first_present_ns)),
        ProfileSink::Field::Number("input_to_request_ms", DurationMilliseconds(report.input_ns, report.requested_ns)),
        ProfileSink::Field::Number("target_resolution_ms", DurationMilliseconds(report.requested_ns, report.target_resolved_ns)),
        ProfileSink::Field::Number(
            "effective_index_ms",
            NanosecondsAsMilliseconds(report.target_resolution.effective_index_ns)),
        ProfileSink::Field::Number(
            "pending_activation_supersede_ms",
            NanosecondsAsMilliseconds(
                report.target_resolution.pending_activation_supersede_ns)),
        ProfileSink::Field::Number(
            "base_sequence_ms",
            NanosecondsAsMilliseconds(report.target_resolution.base_sequence_ns)),
        ProfileSink::Field::Number(
            "target_lookup_ms",
            NanosecondsAsMilliseconds(report.target_resolution.target_lookup_ns)),
        ProfileSink::Field::Number(
            "target_sequence_ms",
            NanosecondsAsMilliseconds(report.target_resolution.target_sequence_ns)),
        ProfileSink::Field::Number(
            "navigation_state_result_ms",
            NanosecondsAsMilliseconds(
                report.target_resolution.navigation_state_result_ns)),
        ProfileSink::Field::Number("enqueue_ms", DurationMilliseconds(report.target_resolved_ns, first_enqueued_ns)),
        ProfileSink::Field::Number("queue_wait_ms", sum(
            [](const auto& attempt) { return attempt.load_enqueued_ns; },
            [](const auto& attempt) { return attempt.worker_started_ns; })),
        ProfileSink::Field::Number("source_inspection_ms", AttemptPreparationDurationSum(
            report.attempts,
            [](const auto& round) { return std::pair{round.preparation_started_ns, round.snapshot_load_started_ns}; })),
        ProfileSink::Field::Number("decode_ms", AttemptPreparationDurationSum(
            report.attempts,
            [](const auto& round) { return std::pair{round.snapshot_load_started_ns, round.snapshot_load_finished_ns}; })),
        ProfileSink::Field::Number("context_prepare_ms", AttemptPreparationDurationSum(
            report.attempts,
            [](const auto& round) { return std::pair{round.snapshot_load_finished_ns, round.context_prepared_ns}; })),
        ProfileSink::Field::Number("source_revalidation_ms", AttemptPreparationDurationSum(
            report.attempts,
            [](const auto& round) { return std::pair{round.context_prepared_ns, round.source_revalidated_ns}; })),
        ProfileSink::Field::Number("workflow_prepare_ms", sum(
            [](const auto& attempt) { return attempt.source_revalidated_ns; },
            [](const auto& attempt) { return attempt.worker_prepared_ns; })),
        ProfileSink::Field::Number("completion_ready_ms", sum(
            [](const auto& attempt) { return attempt.worker_prepared_ns; },
            [](const auto& attempt) { return attempt.completion_ready_ns; })),
        ProfileSink::Field::Number("ordered_publish_wait_ms", sum(
            [](const auto& attempt) { return attempt.completion_ready_ns; },
            [](const auto& attempt) { return attempt.completion_published_ns; })),
        ProfileSink::Field::Number("completion_service_wait_ms", sum(
            [](const auto& attempt) { return attempt.completion_published_ns; },
            [](const auto& attempt) { return attempt.completion_drained_ns; })),
        ProfileSink::Field::Number("retarget_gap_ms", RetargetGapMilliseconds(report.attempts)),
        ProfileSink::Field::Number("activation_ms", DurationMilliseconds(final_drained_ns, report.snapshot_activated_ns)),
        ProfileSink::Field::Number("ui_update_ms", DurationMilliseconds(report.snapshot_activated_ns, report.ui_updated_ns)),
        ProfileSink::Field::Number("ui_to_present_ms", DurationMilliseconds(report.ui_updated_ns, report.first_present_ns)),
        ProfileSink::Field::Number("total_ms", DurationMilliseconds(report.input_ns, total_end_ns)),
    });
    return accepted && summary_accepted;
}

}  // namespace spectiary
