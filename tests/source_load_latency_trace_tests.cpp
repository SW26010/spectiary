#include "profile/source_load_latency_trace.h"

#include "profile/profile_sink.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

spectiary::LoadLatencyTimePoint AtMilliseconds(std::int64_t milliseconds)
{
    return spectiary::LoadLatencyTimePoint(
        std::chrono::milliseconds(milliseconds));
}

std::filesystem::path UniqueTempPath()
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("spectiary_source_load_latency_" +
            std::to_string(next_id.fetch_add(1)) + ".jsonl");
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TestPresentedSourceLoadCapturesAcceptedToPresentPhases()
{
    spectiary::SourceLoadLatencyTrace trace(
        17,
        2,
        spectiary::SourceLoadLatencyRequestKind::ExplicitOpen,
        AtMilliseconds(1));
    const spectiary::LoadLatencyAttemptHandle attempt =
        trace.BeginLoadAttempt(2, AtMilliseconds(2));
    attempt->MarkSourceTaskId(71);
    attempt->MarkWorkerStarted(AtMilliseconds(3));
    attempt->MarkFolderSnapshotLoadStarted(
        {
            .hint_present = true,
            .generation_current_at_start = false,
            .listing_scan_performed = true,
        },
        AtMilliseconds(4));
    attempt->MarkSnapshotLoadFinished(AtMilliseconds(6));
    attempt->MarkContextPrepared(true, AtMilliseconds(7));
    attempt->MarkSourceRevalidated(AtMilliseconds(8));
    attempt->MarkWorkflowReused(true);
    attempt->MarkWorkerPrepared(AtMilliseconds(9));
    attempt->MarkCompletionReady(AtMilliseconds(10));
    attempt->MarkCompletionPublished(AtMilliseconds(11));
    attempt->MarkCompletionDrained(AtMilliseconds(12));
    trace.MarkSnapshotActivated(42, AtMilliseconds(13));
    trace.MarkUiUpdated(AtMilliseconds(14));

    Require(
        trace.MarkPresentedForViewport(42, 7, AtMilliseconds(15)),
        "the first successful Present should complete the source load trace");
    const std::optional<spectiary::SourceLoadLatencyReport> report =
        trace.TerminalReport();
    Require(report.has_value(), "a presented source load should expose a report");
    Require(
        report->source_load_id == 17 && report->target_index == 2,
        "source load identity and final target should survive the trace");
    Require(
        report->outcome == spectiary::SourceLoadLatencyOutcome::Presented,
        "the source load outcome should be presented");
    Require(
        report->attempts.size() == 1 &&
            report->attempts.front().source_task_id == 71,
        "the correlated queue task should survive the trace");
    Require(
        report->first_present_ns == 15'000'000,
        "the first successful Present timestamp should be retained");

    const std::filesystem::path path = UniqueTempPath();
    {
        spectiary::ProfileSink sink(path);
        Require(sink.is_open(), "profile sink should open for the source load fixture");
        Require(
            spectiary::WriteSourceLoadLatencyProfileEvent(sink, *report),
            "source load latency event should be accepted");
        sink.Stop();
    }
    const std::string text = ReadText(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    Require(
        text.find("\"event\":\"source_load_latency\"") != std::string::npos,
        "source load event name should be stable");
    Require(
        text.find("\"event\":\"source_load_latency_attempt\"") !=
                std::string::npos &&
            text.find(
                "\"event\":\"source_load_latency_preparation_round\"") !=
                std::string::npos,
        "source load attempts and preparation rounds should serialize");
    Require(
        text.find("\"source_load_id\":17") != std::string::npos &&
            text.find("\"first_source_task_id\":71") != std::string::npos &&
            text.find("\"final_completion_drained_steady_ns\":12000000") !=
                std::string::npos &&
            text.find("\"terminal_steady_ns\":15000000") !=
                std::string::npos,
        "source load and queue task identities should serialize");
    Require(
        text.find("\"request_kind\":\"explicit_open\"") != std::string::npos &&
            text.find("\"source_kind\":\"folder\"") != std::string::npos,
        "request and source kinds should serialize");
    Require(
        text.find("\"preparation_round_count\":1") != std::string::npos &&
            text.find("\"listing_scan_count\":1") != std::string::npos,
        "preparation and folder scan counts should serialize");
    Require(
        text.find("\"accept_to_enqueue_ms\":1.0000") != std::string::npos &&
            text.find("\"decode_ms\":2.0000") != std::string::npos &&
            text.find("\"ui_to_present_ms\":1.0000") != std::string::npos &&
            text.find("\"total_ms\":14.0000") != std::string::npos,
        "accepted-to-Present stages should serialize with calculated durations");
}

void TestTerminalOutcomeIsStable()
{
    spectiary::SourceLoadLatencyTrace trace(
        18,
        0,
        spectiary::SourceLoadLatencyRequestKind::ExplicitOpen,
        AtMilliseconds(1));
    Require(
        trace.MarkTerminal(
            spectiary::SourceLoadLatencyOutcome::Superseded,
            AtMilliseconds(4)),
        "the first source load terminal outcome should win");
    Require(
        !trace.MarkTerminal(
            spectiary::SourceLoadLatencyOutcome::Failed,
            AtMilliseconds(5)),
        "a later terminal outcome must not replace superseded");
    Require(
        !trace.MarkPresentedForViewport(10, 7, AtMilliseconds(6)),
        "a terminal source load must not later become presented");
    const auto report = trace.TerminalReport();
    Require(
        report &&
            report->outcome ==
                spectiary::SourceLoadLatencyOutcome::Superseded &&
            report->terminal_ns == 4'000'000,
        "the first terminal source load state should remain stable");
}

void TestRetargetedSourceLoadAggregatesAttemptsAndRounds()
{
    spectiary::SourceLoadLatencyTrace trace(
        19,
        1,
        spectiary::SourceLoadLatencyRequestKind::ExplicitOpen,
        AtMilliseconds(1));
    const auto finish_attempt =
        [](const spectiary::LoadLatencyAttemptHandle& attempt,
           std::uint64_t task_id,
           std::int64_t base_ms,
           std::int64_t decode_end_ms) {
            attempt->MarkSourceTaskId(task_id);
            attempt->MarkWorkerStarted(AtMilliseconds(base_ms + 1));
            attempt->MarkSnapshotLoadStarted(
                false,
                AtMilliseconds(base_ms + 2));
            attempt->MarkSnapshotLoadFinished(
                AtMilliseconds(decode_end_ms));
            attempt->MarkContextPrepared(
                AtMilliseconds(decode_end_ms + 1));
            attempt->MarkSourceRevalidated(
                AtMilliseconds(decode_end_ms + 2));
            attempt->MarkWorkerPrepared(
                AtMilliseconds(decode_end_ms + 3));
            attempt->MarkCompletionReady(
                AtMilliseconds(decode_end_ms + 4));
            attempt->MarkCompletionPublished(
                AtMilliseconds(decode_end_ms + 5));
            attempt->MarkCompletionDrained(
                AtMilliseconds(decode_end_ms + 6));
        };

    const spectiary::LoadLatencyAttemptHandle first =
        trace.BeginLoadAttempt(1, AtMilliseconds(2));
    finish_attempt(first, 101, 2, 6);
    trace.SetTargetIndex(2);
    const spectiary::LoadLatencyAttemptHandle second =
        trace.BeginLoadAttempt(2, AtMilliseconds(13));
    finish_attempt(second, 102, 13, 18);
    trace.MarkSnapshotActivated(50, AtMilliseconds(25));
    trace.MarkUiUpdated(AtMilliseconds(26));
    Require(
        trace.MarkPresentedForViewport(50, 9, AtMilliseconds(27)),
        "the retargeted source load should complete on its final Present");

    const auto report = trace.TerminalReport();
    Require(
        report && report->target_index == 2 && report->attempts.size() == 2,
        "retargeting should preserve the final target and both attempts");
    Require(
        report->attempts[0].target_index == 1 &&
            report->attempts[1].target_index == 2 &&
            report->attempts[0].preparation_rounds.size() == 1 &&
            report->attempts[1].preparation_rounds.size() == 1,
        "each retarget attempt should retain its own target and round");

    const std::filesystem::path path = UniqueTempPath();
    {
        spectiary::ProfileSink sink(path);
        Require(
            spectiary::WriteSourceLoadLatencyProfileEvent(sink, *report),
            "the retargeted source load report should be accepted");
        sink.Stop();
    }
    const std::string text = ReadText(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    Require(
        text.find("\"attempt_count\":2") != std::string::npos &&
            text.find("\"preparation_round_count\":2") !=
                std::string::npos &&
            text.find("\"listing_scan_count\":0") != std::string::npos &&
            text.find("\"retarget_gap_ms\":1.0000") != std::string::npos &&
            text.find("\"decode_ms\":5.0000") != std::string::npos,
        "retargeted source load aggregates should sum both attempts");
}

}  // namespace

int main()
{
    try {
        TestPresentedSourceLoadCapturesAcceptedToPresentPhases();
        TestTerminalOutcomeIsStable();
        TestRetargetedSourceLoadAggregatesAttemptsAndRounds();
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
