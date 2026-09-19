#include "profile/source_load_latency_trace.h"

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

template <typename TimestampGetter>
std::string AttemptDurationSum(
    const std::vector<LoadLatencyAttemptReport>& attempts,
    TimestampGetter&& timestamps)
{
    if (attempts.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const LoadLatencyAttemptReport& attempt : attempts) {
        const auto [begin_ns, end_ns] = timestamps(attempt);
        if (begin_ns <= 0 || end_ns < begin_ns) {
            return "null";
        }
        total_ns += end_ns - begin_ns;
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

template <typename TimestampGetter>
std::string PreparationDurationSum(
    const LoadLatencyAttemptReport& attempt,
    TimestampGetter&& timestamps)
{
    if (attempt.preparation_rounds.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const LoadLatencyPreparationRoundReport& round :
         attempt.preparation_rounds) {
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
    const std::vector<LoadLatencyAttemptReport>& attempts,
    TimestampGetter&& timestamps)
{
    if (attempts.empty()) {
        return "null";
    }
    std::int64_t total_ns = 0;
    for (const LoadLatencyAttemptReport& attempt : attempts) {
        if (attempt.preparation_rounds.empty()) {
            return "null";
        }
        for (const LoadLatencyPreparationRoundReport& round :
             attempt.preparation_rounds) {
            const auto [begin_ns, end_ns] = timestamps(round);
            if (begin_ns <= 0 || end_ns < begin_ns) {
                return "null";
            }
            total_ns += end_ns - begin_ns;
        }
    }
    return Milliseconds(static_cast<double>(total_ns) / 1'000'000.0);
}

std::string RetargetGapMilliseconds(
    const std::vector<LoadLatencyAttemptReport>& attempts)
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

std::size_t PreparationRoundCount(
    const std::vector<LoadLatencyAttemptReport>& attempts)
{
    std::size_t count = 0;
    for (const LoadLatencyAttemptReport& attempt : attempts) {
        count += attempt.preparation_rounds.size();
    }
    return count;
}

std::size_t ListingScanCount(
    const std::vector<LoadLatencyAttemptReport>& attempts)
{
    std::size_t count = 0;
    for (const LoadLatencyAttemptReport& attempt : attempts) {
        for (const LoadLatencyPreparationRoundReport& round :
             attempt.preparation_rounds) {
            count += round.listing_scan_performed ? 1 : 0;
        }
    }
    return count;
}

bool WritePreparationRoundEvent(
    ProfileSink& sink,
    std::uint64_t source_load_id,
    const LoadLatencyAttemptReport& attempt,
    const LoadLatencyPreparationRoundReport& round)
{
    return sink.WriteEvent("source_load_latency_preparation_round", {
        ProfileSink::Field::Number(
            "source_load_id",
            std::to_string(source_load_id)),
        ProfileSink::Field::Number(
            "attempt_index",
            std::to_string(attempt.attempt_index)),
        ProfileSink::Field::Number(
            "preparation_round_index",
            std::to_string(round.round_index)),
        ProfileSink::Field::Number(
            "target_index",
            std::to_string(attempt.target_index)),
        ProfileSink::Field::Number(
            "source_task_id",
            std::to_string(attempt.source_task_id)),
        ProfileSink::Field::String(
            "source_kind",
            round.source_is_folder ? "folder" : "file"),
        ProfileSink::Field::Bool("hint_present", round.hint_present),
        ProfileSink::Field::Bool(
            "generation_current_at_start",
            round.generation_current_at_start),
        ProfileSink::Field::Bool(
            "listing_scan_performed",
            round.listing_scan_performed),
        ProfileSink::Field::Bool("context_reused", round.context_reused),
        ProfileSink::Field::Bool(
            "revalidation_succeeded",
            round.revalidation_succeeded),
        ProfileSink::Field::Number(
            "preparation_started_steady_ns",
            NumberOrNull(round.preparation_started_ns)),
        ProfileSink::Field::Number(
            "snapshot_load_started_steady_ns",
            NumberOrNull(round.snapshot_load_started_ns)),
        ProfileSink::Field::Number(
            "snapshot_load_finished_steady_ns",
            NumberOrNull(round.snapshot_load_finished_ns)),
        ProfileSink::Field::Number(
            "context_prepared_steady_ns",
            NumberOrNull(round.context_prepared_ns)),
        ProfileSink::Field::Number(
            "source_revalidated_steady_ns",
            NumberOrNull(round.source_revalidated_ns)),
        ProfileSink::Field::Number(
            "source_inspection_ms",
            DurationMilliseconds(
                round.preparation_started_ns,
                round.snapshot_load_started_ns)),
        ProfileSink::Field::Number(
            "decode_ms",
            DurationMilliseconds(
                round.snapshot_load_started_ns,
                round.snapshot_load_finished_ns)),
        ProfileSink::Field::Number(
            "context_prepare_ms",
            DurationMilliseconds(
                round.snapshot_load_finished_ns,
                round.context_prepared_ns)),
        ProfileSink::Field::Number(
            "source_revalidation_ms",
            DurationMilliseconds(
                round.context_prepared_ns,
                round.source_revalidated_ns)),
        ProfileSink::Field::Number(
            "round_total_ms",
            DurationMilliseconds(
                round.preparation_started_ns,
                round.source_revalidated_ns)),
    });
}

bool WriteAttemptEvent(
    ProfileSink& sink,
    std::uint64_t source_load_id,
    const LoadLatencyAttemptReport& attempt)
{
    bool accepted = true;
    for (const LoadLatencyPreparationRoundReport& round :
         attempt.preparation_rounds) {
        accepted =
            WritePreparationRoundEvent(sink, source_load_id, attempt, round) &&
            accepted;
    }
    const bool attempt_accepted = sink.WriteEvent("source_load_latency_attempt", {
        ProfileSink::Field::Number(
            "source_load_id",
            std::to_string(source_load_id)),
        ProfileSink::Field::Number(
            "attempt_index",
            std::to_string(attempt.attempt_index)),
        ProfileSink::Field::Number(
            "target_index",
            std::to_string(attempt.target_index)),
        ProfileSink::Field::Number(
            "source_task_id",
            std::to_string(attempt.source_task_id)),
        ProfileSink::Field::String(
            "source_kind",
            attempt.source_is_folder ? "folder" : "file"),
        ProfileSink::Field::Bool("workflow_reused", attempt.workflow_reused),
        ProfileSink::Field::Bool("context_reused", attempt.context_reused),
        ProfileSink::Field::Number(
            "preparation_round_count",
            std::to_string(attempt.preparation_rounds.size())),
        ProfileSink::Field::Number(
            "load_enqueued_steady_ns",
            NumberOrNull(attempt.load_enqueued_ns)),
        ProfileSink::Field::Number(
            "worker_started_steady_ns",
            NumberOrNull(attempt.worker_started_ns)),
        ProfileSink::Field::Number(
            "snapshot_load_started_steady_ns",
            NumberOrNull(attempt.snapshot_load_started_ns)),
        ProfileSink::Field::Number(
            "snapshot_load_finished_steady_ns",
            NumberOrNull(attempt.snapshot_load_finished_ns)),
        ProfileSink::Field::Number(
            "context_prepared_steady_ns",
            NumberOrNull(attempt.context_prepared_ns)),
        ProfileSink::Field::Number(
            "source_revalidated_steady_ns",
            NumberOrNull(attempt.source_revalidated_ns)),
        ProfileSink::Field::Number(
            "worker_prepared_steady_ns",
            NumberOrNull(attempt.worker_prepared_ns)),
        ProfileSink::Field::Number(
            "completion_ready_steady_ns",
            NumberOrNull(attempt.completion_ready_ns)),
        ProfileSink::Field::Number(
            "completion_published_steady_ns",
            NumberOrNull(attempt.completion_published_ns)),
        ProfileSink::Field::Number(
            "completion_drained_steady_ns",
            NumberOrNull(attempt.completion_drained_ns)),
        ProfileSink::Field::Number(
            "queue_wait_ms",
            DurationMilliseconds(
                attempt.load_enqueued_ns,
                attempt.worker_started_ns)),
        ProfileSink::Field::Number(
            "source_inspection_ms",
            PreparationDurationSum(
                attempt,
                [](const auto& round) {
                    return std::pair{
                        round.preparation_started_ns,
                        round.snapshot_load_started_ns};
                })),
        ProfileSink::Field::Number(
            "decode_ms",
            PreparationDurationSum(
                attempt,
                [](const auto& round) {
                    return std::pair{
                        round.snapshot_load_started_ns,
                        round.snapshot_load_finished_ns};
                })),
        ProfileSink::Field::Number(
            "context_prepare_ms",
            PreparationDurationSum(
                attempt,
                [](const auto& round) {
                    return std::pair{
                        round.snapshot_load_finished_ns,
                        round.context_prepared_ns};
                })),
        ProfileSink::Field::Number(
            "source_revalidation_ms",
            PreparationDurationSum(
                attempt,
                [](const auto& round) {
                    return std::pair{
                        round.context_prepared_ns,
                        round.source_revalidated_ns};
                })),
        ProfileSink::Field::Number(
            "workflow_prepare_ms",
            DurationMilliseconds(
                attempt.source_revalidated_ns,
                attempt.worker_prepared_ns)),
        ProfileSink::Field::Number(
            "completion_ready_ms",
            DurationMilliseconds(
                attempt.worker_prepared_ns,
                attempt.completion_ready_ns)),
        ProfileSink::Field::Number(
            "ordered_publish_wait_ms",
            DurationMilliseconds(
                attempt.completion_ready_ns,
                attempt.completion_published_ns)),
        ProfileSink::Field::Number(
            "completion_service_wait_ms",
            DurationMilliseconds(
                attempt.completion_published_ns,
                attempt.completion_drained_ns)),
        ProfileSink::Field::Number(
            "attempt_total_ms",
            DurationMilliseconds(
                attempt.load_enqueued_ns,
                attempt.completion_drained_ns)),
    });
    return accepted && attempt_accepted;
}

}  // namespace

SourceLoadLatencyTrace::SourceLoadLatencyTrace(
    std::uint64_t source_load_id,
    std::size_t target_index,
    SourceLoadLatencyRequestKind request_kind,
    LoadLatencyTimePoint accepted_at)
    : source_load_id_(source_load_id),
      request_kind_(request_kind),
      accepted_ns_(LoadLatencyNanoseconds(accepted_at)),
      terminal_presentation_(target_index)
{
}

void SourceLoadLatencyTrace::SetTargetIndex(std::size_t target_index) noexcept
{
    terminal_presentation_.SetTargetIndex(target_index);
}

LoadLatencyAttemptHandle SourceLoadLatencyTrace::BeginLoadAttempt(
    std::size_t target_index,
    LoadLatencyTimePoint at)
{
    return load_attempts_.Begin(target_index, at);
}

void SourceLoadLatencyTrace::MarkSnapshotActivated(
    std::uint64_t frame_index,
    LoadLatencyTimePoint at) noexcept
{
    terminal_presentation_.MarkSnapshotActivated(frame_index, at);
}

void SourceLoadLatencyTrace::MarkUiUpdated(
    LoadLatencyTimePoint at) noexcept
{
    terminal_presentation_.MarkUiUpdated(at);
}

bool SourceLoadLatencyTrace::MarkPresentedForViewport(
    std::uint64_t frame_index,
    unsigned int viewport_id,
    LoadLatencyTimePoint at) noexcept
{
    return terminal_presentation_.MarkPresentedForViewport(
        frame_index,
        viewport_id,
        at);
}

bool SourceLoadLatencyTrace::MarkTerminal(
    SourceLoadLatencyOutcome outcome,
    LoadLatencyTimePoint at) noexcept
{
    return terminal_presentation_.MarkTerminal(outcome, at);
}

std::optional<SourceLoadLatencyReport>
SourceLoadLatencyTrace::TerminalReport() const noexcept
{
    const auto lifecycle = terminal_presentation_.TerminalSnapshot();
    if (!lifecycle) {
        return std::nullopt;
    }

    SourceLoadLatencyReport report;
    report.source_load_id = source_load_id_;
    report.activation_frame = lifecycle->activation_frame;
    report.presentation_viewport_id =
        lifecycle->presentation_viewport_id;
    report.target_index = lifecycle->target_index;
    report.request_kind = request_kind_;
    report.outcome = lifecycle->outcome;
    report.accepted_ns = accepted_ns_;
    report.snapshot_activated_ns = lifecycle->snapshot_activated_ns;
    report.ui_updated_ns = lifecycle->ui_updated_ns;
    report.first_present_ns = lifecycle->first_present_ns;
    report.terminal_ns = lifecycle->terminal_ns;
    report.attempts = load_attempts_.Reports();
    return report;
}

const char* SourceLoadLatencyRequestKindName(
    SourceLoadLatencyRequestKind kind) noexcept
{
    switch (kind) {
    case SourceLoadLatencyRequestKind::ExplicitOpen:
        return "explicit_open";
    }
    return "unknown";
}

const char* SourceLoadLatencyOutcomeName(
    SourceLoadLatencyOutcome outcome) noexcept
{
    switch (outcome) {
    case SourceLoadLatencyOutcome::Pending:
        return "pending";
    case SourceLoadLatencyOutcome::Presented:
        return "presented";
    case SourceLoadLatencyOutcome::Failed:
        return "failed";
    case SourceLoadLatencyOutcome::Rejected:
        return "rejected";
    case SourceLoadLatencyOutcome::Superseded:
        return "superseded";
    }
    return "unknown";
}

bool WriteSourceLoadLatencyProfileEvent(
    ProfileSink& sink,
    const SourceLoadLatencyReport& report)
{
    bool accepted = true;
    for (const LoadLatencyAttemptReport& attempt : report.attempts) {
        accepted =
            WriteAttemptEvent(sink, report.source_load_id, attempt) && accepted;
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
    const std::uint64_t first_source_task_id = report.attempts.empty()
        ? 0
        : report.attempts.front().source_task_id;
    const std::uint64_t final_source_task_id = report.attempts.empty()
        ? 0
        : report.attempts.back().source_task_id;
    const bool source_is_folder =
        !report.attempts.empty() && report.attempts.back().source_is_folder;
    const bool context_reused =
        !report.attempts.empty() && report.attempts.back().context_reused;
    const bool workflow_reused =
        !report.attempts.empty() && report.attempts.back().workflow_reused;

    const auto sum = [&report](auto begin, auto end) {
        return AttemptDurationSum(
            report.attempts,
            [begin, end](const LoadLatencyAttemptReport& attempt) {
                return std::pair{begin(attempt), end(attempt)};
            });
    };

    const bool summary_accepted = sink.WriteEvent("source_load_latency", {
        ProfileSink::Field::Number(
            "source_load_id",
            std::to_string(report.source_load_id)),
        ProfileSink::Field::Number(
            "activation_frame",
            std::to_string(report.activation_frame)),
        ProfileSink::Field::Number(
            "presentation_viewport_id",
            std::to_string(report.presentation_viewport_id)),
        ProfileSink::Field::String(
            "outcome",
            SourceLoadLatencyOutcomeName(report.outcome)),
        ProfileSink::Field::String(
            "request_kind",
            SourceLoadLatencyRequestKindName(report.request_kind)),
        ProfileSink::Field::String(
            "source_kind",
            report.attempts.empty()
                ? "unknown"
                : (source_is_folder ? "folder" : "file")),
        ProfileSink::Field::Number(
            "target_index",
            std::to_string(report.target_index)),
        ProfileSink::Field::Number(
            "attempt_count",
            std::to_string(report.attempts.size())),
        ProfileSink::Field::Number(
            "preparation_round_count",
            std::to_string(PreparationRoundCount(report.attempts))),
        ProfileSink::Field::Number(
            "listing_scan_count",
            std::to_string(ListingScanCount(report.attempts))),
        ProfileSink::Field::Number(
            "first_source_task_id",
            std::to_string(first_source_task_id)),
        ProfileSink::Field::Number(
            "final_source_task_id",
            std::to_string(final_source_task_id)),
        ProfileSink::Field::Bool("context_reused", context_reused),
        ProfileSink::Field::Bool("workflow_reused", workflow_reused),
        ProfileSink::Field::Number(
            "accepted_steady_ns",
            NumberOrNull(report.accepted_ns)),
        ProfileSink::Field::Number(
            "first_enqueued_steady_ns",
            NumberOrNull(first_enqueued_ns)),
        ProfileSink::Field::Number(
            "final_completion_drained_steady_ns",
            NumberOrNull(final_drained_ns)),
        ProfileSink::Field::Number(
            "snapshot_activated_steady_ns",
            NumberOrNull(report.snapshot_activated_ns)),
        ProfileSink::Field::Number(
            "ui_updated_steady_ns",
            NumberOrNull(report.ui_updated_ns)),
        ProfileSink::Field::Number(
            "first_present_steady_ns",
            NumberOrNull(report.first_present_ns)),
        ProfileSink::Field::Number(
            "terminal_steady_ns",
            NumberOrNull(report.terminal_ns)),
        ProfileSink::Field::Number(
            "accept_to_enqueue_ms",
            DurationMilliseconds(report.accepted_ns, first_enqueued_ns)),
        ProfileSink::Field::Number("queue_wait_ms", sum(
            [](const auto& attempt) { return attempt.load_enqueued_ns; },
            [](const auto& attempt) { return attempt.worker_started_ns; })),
        ProfileSink::Field::Number(
            "source_inspection_ms",
            AttemptPreparationDurationSum(
                report.attempts,
                [](const auto& round) {
                    return std::pair{
                        round.preparation_started_ns,
                        round.snapshot_load_started_ns};
                })),
        ProfileSink::Field::Number(
            "decode_ms",
            AttemptPreparationDurationSum(
                report.attempts,
                [](const auto& round) {
                    return std::pair{
                        round.snapshot_load_started_ns,
                        round.snapshot_load_finished_ns};
                })),
        ProfileSink::Field::Number(
            "context_prepare_ms",
            AttemptPreparationDurationSum(
                report.attempts,
                [](const auto& round) {
                    return std::pair{
                        round.snapshot_load_finished_ns,
                        round.context_prepared_ns};
                })),
        ProfileSink::Field::Number(
            "source_revalidation_ms",
            AttemptPreparationDurationSum(
                report.attempts,
                [](const auto& round) {
                    return std::pair{
                        round.context_prepared_ns,
                        round.source_revalidated_ns};
                })),
        ProfileSink::Field::Number("workflow_prepare_ms", sum(
            [](const auto& attempt) { return attempt.source_revalidated_ns; },
            [](const auto& attempt) { return attempt.worker_prepared_ns; })),
        ProfileSink::Field::Number("completion_ready_ms", sum(
            [](const auto& attempt) { return attempt.worker_prepared_ns; },
            [](const auto& attempt) { return attempt.completion_ready_ns; })),
        ProfileSink::Field::Number("ordered_publish_wait_ms", sum(
            [](const auto& attempt) { return attempt.completion_ready_ns; },
            [](const auto& attempt) {
                return attempt.completion_published_ns;
            })),
        ProfileSink::Field::Number("completion_service_wait_ms", sum(
            [](const auto& attempt) {
                return attempt.completion_published_ns;
            },
            [](const auto& attempt) { return attempt.completion_drained_ns; })),
        ProfileSink::Field::Number(
            "retarget_gap_ms",
            RetargetGapMilliseconds(report.attempts)),
        ProfileSink::Field::Number(
            "activation_ms",
            DurationMilliseconds(final_drained_ns, report.snapshot_activated_ns)),
        ProfileSink::Field::Number(
            "ui_update_ms",
            DurationMilliseconds(
                report.snapshot_activated_ns,
                report.ui_updated_ns)),
        ProfileSink::Field::Number(
            "ui_to_present_ms",
            DurationMilliseconds(report.ui_updated_ns, report.first_present_ns)),
        ProfileSink::Field::Number(
            "total_ms",
            DurationMilliseconds(report.accepted_ns, total_end_ns)),
    });
    return accepted && summary_accepted;
}

}  // namespace spectiary
