#include "profile/navigation_latency_trace.h"

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

specforge::NavigationLatencyTimePoint AtMilliseconds(std::int64_t milliseconds)
{
    return specforge::NavigationLatencyTimePoint(std::chrono::milliseconds(milliseconds));
}

std::filesystem::path UniqueTempPath()
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("specforge_navigation_latency_" + std::to_string(next_id.fetch_add(1)) + ".jsonl");
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void TestPresentedTraceCapturesCorrelatedPhases()
{
    specforge::NavigationLatencyTrace trace(
        7,
        10,
        11,
        specforge::NavigationLatencyInputKind::UiNext,
        AtMilliseconds(1),
        AtMilliseconds(2),
        AtMilliseconds(3),
        {
            .effective_index_ns = 100'000,
            .pending_activation_supersede_ns = 100'000,
            .base_sequence_ns = 200'000,
            .target_lookup_ns = 100'000,
            .target_sequence_ns = 200'000,
            .row_count = 3,
            .pending_present = true,
            .sequence_build_count = 2,
        });
    const specforge::NavigationLatencyAttemptHandle attempt =
        trace.BeginLoadAttempt(11, AtMilliseconds(4));
    attempt->MarkSourceTaskId(99);
    attempt->MarkWorkerStarted(AtMilliseconds(5));
    attempt->MarkFolderSnapshotLoadStarted(
        {
            .hint_present = true,
            .generation_current_at_start = false,
            .listing_scan_performed = true,
        },
        AtMilliseconds(6));
    attempt->MarkSnapshotLoadFinished(AtMilliseconds(8));
    attempt->MarkContextPrepared(true, AtMilliseconds(9));
    attempt->MarkSourceRevalidated(AtMilliseconds(10));
    attempt->MarkWorkflowReused(true);
    attempt->MarkWorkerPrepared(AtMilliseconds(11));
    attempt->MarkCompletionReady(AtMilliseconds(12));
    attempt->MarkCompletionPublished(AtMilliseconds(13));
    attempt->MarkCompletionDrained(AtMilliseconds(14));
    trace.SetCacheHit(true);
    trace.MarkSnapshotActivated(42, AtMilliseconds(15));
    trace.MarkUiUpdated(AtMilliseconds(16));

    Require(
        !trace.MarkPresentedForViewport(41, 7, AtMilliseconds(17)),
        "a frame before activation must not complete the trace");
    Require(
        trace.MarkPresentedForViewport(42, 7, AtMilliseconds(17)),
        "the activation frame Present should complete the trace");
    const std::optional<specforge::NavigationLatencyReport> report = trace.TerminalReport();
    Require(report.has_value(), "a presented trace should expose one terminal report");
    Require(report->navigation_id == 7, "navigation id should survive the trace");
    Require(report->attempts.size() == 1, "one load should remain one trace attempt");
    Require(report->attempts[0].source_task_id == 99, "source task id should survive the attempt");
    Require(report->from_index == 10 && report->target_index == 11, "sample indices should survive the trace");
    Require(report->activation_frame == 42, "activation frame should survive the trace");
    Require(report->presentation_viewport_id == 7, "presenting viewport should survive the trace");
    Require(report->outcome == specforge::NavigationLatencyOutcome::Presented, "outcome should be presented");
    Require(report->cache_hit, "verified decode reuse should survive the trace");
    Require(report->attempts[0].workflow_reused, "workflow reuse should be recorded");
    Require(report->attempts[0].context_reused, "context reuse should be recorded");
    Require(report->first_present_ns == 17'000'000, "first Present timestamp should be recorded");

    const std::filesystem::path path = UniqueTempPath();
    {
        specforge::ProfileSink sink(path);
        Require(sink.is_open(), "profile sink should open for the trace fixture");
        Require(
            specforge::WriteNavigationLatencyProfileEvent(sink, *report),
            "navigation latency event should be accepted");
        sink.Stop();
    }
    const std::string text = ReadText(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    Require(text.find("\"event\":\"navigation_latency\"") != std::string::npos, "event name should be stable");
    Require(text.find("\"outcome\":\"presented\"") != std::string::npos, "presented outcome should serialize");
    Require(text.find("\"event\":\"navigation_latency_attempt\"") != std::string::npos, "attempt event should serialize");
    Require(
        text.find("\"event\":\"navigation_latency_preparation_round\"") != std::string::npos,
        "each decode/revalidation round should serialize independently");
    Require(
        text.find("\"preparation_round_count\":1") != std::string::npos,
        "the attempt should declare its preparation round count");
    Require(
        text.find("\"hint_present\":true") != std::string::npos &&
            text.find("\"generation_current_at_start\":false") !=
                std::string::npos &&
            text.find("\"listing_scan_performed\":true") !=
                std::string::npos,
        "folder listing generation diagnostics should serialize");
    Require(
        text.find("\"context_reused\":true") != std::string::npos,
        "context reuse diagnostics should serialize");
    Require(text.find("\"input_kind\":\"ui_next\"") != std::string::npos, "input kind should serialize");
    Require(text.find("\"row_count\":3") != std::string::npos, "navigation row count should serialize");
    Require(text.find("\"pending_present\":true") != std::string::npos, "pending state should serialize");
    Require(
        text.find("\"sequence_build_count\":2") != std::string::npos,
        "sequence build count should serialize");
    Require(
        text.find("\"base_sequence_ms\":0.2000") != std::string::npos &&
            text.find("\"navigation_state_result_ms\":0.3000") !=
                std::string::npos,
        "target-resolution phases should serialize and preserve the aggregate");
    Require(text.find("\"presentation_viewport_id\":7") != std::string::npos, "viewport id should serialize");
    Require(text.find("\"cache_hit\":true") != std::string::npos, "decode reuse should serialize");
    Require(text.find("\"decode_ms\":2.0000") != std::string::npos, "decode duration should be calculated");
    Require(text.find("\"completion_service_wait_ms\":1.0000") != std::string::npos, "service wait should be calculated");
    Require(text.find("\"total_ms\":16.0000") != std::string::npos, "input-to-Present total should be calculated");
}

void TestTerminalOutcomeIsStable()
{
    specforge::NavigationLatencyTrace trace(
        8,
        2,
        3,
        specforge::NavigationLatencyInputKind::UiPrevious,
        AtMilliseconds(1),
        AtMilliseconds(1),
        AtMilliseconds(2));
    Require(
        trace.MarkTerminal(specforge::NavigationLatencyOutcome::Superseded, AtMilliseconds(4)),
        "the first terminal outcome should win");
    Require(
        !trace.MarkTerminal(specforge::NavigationLatencyOutcome::Failed, AtMilliseconds(5)),
        "a later terminal outcome must not overwrite the first");
    Require(
        !trace.MarkPresentedForViewport(100, 3, AtMilliseconds(6)),
        "a terminal trace must not later become presented");
    const std::optional<specforge::NavigationLatencyReport> report = trace.TerminalReport();
    Require(report.has_value(), "terminal trace should expose a report");
    Require(
        report->outcome == specforge::NavigationLatencyOutcome::Superseded,
        "the first terminal outcome should remain stable");
    Require(report->terminal_ns == 4'000'000, "terminal time should remain stable");
}

void TestSameFrameStopRetainsPresentedNavigationReport()
{
    specforge::NavigationLatencyTrace trace(
        10,
        4,
        5,
        specforge::NavigationLatencyInputKind::UiNext,
        AtMilliseconds(1),
        AtMilliseconds(2),
        AtMilliseconds(3));
    const specforge::NavigationLatencyAttemptHandle attempt =
        trace.BeginLoadAttempt(5, AtMilliseconds(4));
    attempt->MarkSourceTaskId(101);
    attempt->MarkWorkerStarted(AtMilliseconds(5));
    attempt->MarkSnapshotLoadStarted(false, AtMilliseconds(6));
    attempt->MarkSnapshotLoadFinished(AtMilliseconds(7));
    attempt->MarkContextPrepared(AtMilliseconds(8));
    attempt->MarkSourceRevalidated(AtMilliseconds(9));
    attempt->MarkWorkerPrepared(AtMilliseconds(10));
    attempt->MarkCompletionReady(AtMilliseconds(11));
    attempt->MarkCompletionPublished(AtMilliseconds(12));
    attempt->MarkCompletionDrained(AtMilliseconds(13));
    trace.MarkSnapshotActivated(55, AtMilliseconds(14));
    trace.MarkUiUpdated(AtMilliseconds(15));
    Require(
        trace.MarkPresentedForViewport(55, 9, AtMilliseconds(16)),
        "the fixture should complete on its successful Present");
    const auto report = trace.TerminalReport();
    Require(report.has_value(), "the presented fixture should produce a report");

    const std::filesystem::path path = UniqueTempPath();
    {
        specforge::ProfileSink sink(path);
        sink.BeginFrame();
        sink.RequestStopAfterFrame();
        Require(
            specforge::WriteNavigationLatencyProfileEvent(sink, *report),
            "a navigation report completed after Stop in the same frame must be retained");
        sink.CompleteFrameFinalization();
        sink.Stop();
    }
    const std::string text = ReadText(path);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    const std::size_t report_position = text.find("\"event\":\"navigation_latency\"");
    const std::size_t summary_position = text.find("\"event\":\"profile_recorder_summary\"");
    Require(
        report_position != std::string::npos &&
            summary_position != std::string::npos &&
            report_position < summary_position,
        "same-frame navigation evidence must precede the recorder summary");
}

void TestRetargetedLoadsKeepSeparateAttempts()
{
    specforge::NavigationLatencyTrace trace(
        9,
        1,
        2,
        specforge::NavigationLatencyInputKind::AutoAdvance,
        AtMilliseconds(1),
        AtMilliseconds(2),
        AtMilliseconds(3));
    const specforge::NavigationLatencyAttemptHandle first =
        trace.BeginLoadAttempt(2, AtMilliseconds(4));
    first->MarkSourceTaskId(10);
    first->MarkWorkerStarted(AtMilliseconds(5));
    first->MarkSnapshotLoadStarted(false, AtMilliseconds(6));
    first->MarkSnapshotLoadFinished(AtMilliseconds(8));
    first->MarkContextPrepared(AtMilliseconds(9));
    first->MarkSourceRevalidated(AtMilliseconds(10));
    first->MarkWorkerPrepared(AtMilliseconds(11));
    first->MarkCompletionReady(AtMilliseconds(12));
    first->MarkCompletionPublished(AtMilliseconds(13));
    first->MarkCompletionDrained(AtMilliseconds(14));

    trace.SetTargetIndex(3);
    const specforge::NavigationLatencyAttemptHandle second =
        trace.BeginLoadAttempt(3, AtMilliseconds(15));
    second->MarkSourceTaskId(11);
    second->MarkWorkerStarted(AtMilliseconds(16));
    second->MarkSnapshotLoadStarted(false, AtMilliseconds(17));
    second->MarkSnapshotLoadFinished(AtMilliseconds(20));
    second->MarkContextPrepared(AtMilliseconds(21));
    second->MarkSourceRevalidated(AtMilliseconds(22));
    second->MarkWorkerPrepared(AtMilliseconds(23));
    second->MarkCompletionReady(AtMilliseconds(24));
    second->MarkCompletionPublished(AtMilliseconds(25));
    second->MarkCompletionDrained(AtMilliseconds(26));
    Require(
        trace.MarkTerminal(specforge::NavigationLatencyOutcome::Superseded, AtMilliseconds(27)),
        "fixture should terminate after two attempts");

    const auto report = trace.TerminalReport();
    Require(report && report->attempts.size() == 2, "retargeting should retain both load attempts");
    Require(
        report->attempts[0].target_index == 2 && report->attempts[1].target_index == 3,
        "each attempt should retain its own target");
    Require(
        report->attempts[0].snapshot_load_finished_ns - report->attempts[0].snapshot_load_started_ns == 2'000'000 &&
            report->attempts[1].snapshot_load_finished_ns - report->attempts[1].snapshot_load_started_ns == 3'000'000,
        "retargeting must not overwrite earlier phase timings");
}

void TestRepeatedPreparationKeepsDistinctRounds()
{
    specforge::NavigationLatencyTrace trace(
        11,
        0,
        1,
        specforge::NavigationLatencyInputKind::UiNext,
        AtMilliseconds(1),
        AtMilliseconds(2),
        AtMilliseconds(3));
    const specforge::NavigationLatencyAttemptHandle attempt =
        trace.BeginLoadAttempt(1, AtMilliseconds(4));
    attempt->MarkWorkerStarted(AtMilliseconds(5));
    attempt->MarkSnapshotLoadStarted(false, AtMilliseconds(6));
    attempt->MarkSnapshotLoadFinished(AtMilliseconds(8));
    attempt->MarkContextPrepared(true, AtMilliseconds(9));
    attempt->MarkSourceRevalidated(AtMilliseconds(10));
    attempt->MarkSnapshotLoadStarted(false, AtMilliseconds(11));
    attempt->MarkSnapshotLoadFinished(AtMilliseconds(14));
    attempt->MarkContextPrepared(AtMilliseconds(15));
    attempt->MarkSourceRevalidated(AtMilliseconds(16));

    const specforge::NavigationLatencyAttemptReport report = attempt->Report();
    Require(
        report.preparation_rounds.size() == 2,
        "repeated preparation on one source task must retain two diagnostic rounds");
    Require(
        report.preparation_rounds[0].context_reused &&
            !report.preparation_rounds[1].context_reused &&
            !report.context_reused,
        "each round should retain context reuse and the attempt should report its final round");
}

}  // namespace

int main()
{
    try {
        TestPresentedTraceCapturesCorrelatedPhases();
        TestTerminalOutcomeIsStable();
        TestSameFrameStopRetainsPresentedNavigationReport();
        TestRetargetedLoadsKeepSeparateAttempts();
        TestRepeatedPreparationKeepsDistinctRounds();
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
