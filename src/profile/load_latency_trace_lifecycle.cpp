#include "profile/load_latency_trace_lifecycle.h"

namespace specforge {

std::int64_t LoadLatencyNanoseconds(LoadLatencyTimePoint at) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               at.time_since_epoch())
        .count();
}

LoadLatencyAttempt::LoadLatencyAttempt(
    std::size_t attempt_index,
    std::size_t target_index,
    LoadLatencyTimePoint load_enqueued_at)
    : attempt_index_(attempt_index),
      target_index_(target_index),
      load_enqueued_ns_(LoadLatencyNanoseconds(load_enqueued_at))
{
    preparation_rounds_.reserve(2);
}

void LoadLatencyAttempt::MarkSourceTaskId(std::uint64_t task_id) noexcept
{
    source_task_id_.store(task_id, std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkWorkerStarted(LoadLatencyTimePoint at) noexcept
{
    worker_started_ns_.store(
        LoadLatencyNanoseconds(at),
        std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkSnapshotLoadStarted(
    bool source_is_folder,
    LoadLatencyTimePoint at)
{
    MarkSnapshotLoadStarted(source_is_folder, {}, at);
}

void LoadLatencyAttempt::MarkFolderSnapshotLoadStarted(
    LoadLatencyFolderListingObservation observation,
    LoadLatencyTimePoint at)
{
    MarkSnapshotLoadStarted(true, observation, at);
}

void LoadLatencyAttempt::MarkSnapshotLoadStarted(
    bool source_is_folder,
    LoadLatencyFolderListingObservation observation,
    LoadLatencyTimePoint at)
{
    const std::int64_t started_ns = LoadLatencyNanoseconds(at);
    source_is_folder_.store(source_is_folder, std::memory_order_relaxed);
    std::int64_t missing = 0;
    (void)snapshot_load_started_ns_.compare_exchange_strong(
        missing,
        started_ns,
        std::memory_order_relaxed);
    std::lock_guard lock(preparation_rounds_mutex_);
    const std::int64_t preparation_started_ns = preparation_rounds_.empty()
        ? worker_started_ns_.load(std::memory_order_relaxed)
        : preparation_rounds_.back().source_revalidated_ns;
    preparation_rounds_.push_back({
        .round_index = preparation_rounds_.size(),
        .source_is_folder = source_is_folder,
        .hint_present = observation.hint_present,
        .generation_current_at_start =
            observation.generation_current_at_start,
        .listing_scan_performed = observation.listing_scan_performed,
        .preparation_started_ns = preparation_started_ns,
        .snapshot_load_started_ns = started_ns,
    });
}

void LoadLatencyAttempt::MarkFolderListingScanPerformed() noexcept
{
    std::lock_guard lock(preparation_rounds_mutex_);
    if (!preparation_rounds_.empty() &&
        preparation_rounds_.back().source_is_folder) {
        preparation_rounds_.back().listing_scan_performed = true;
    }
}

void LoadLatencyAttempt::MarkSnapshotLoadFinished(
    LoadLatencyTimePoint at) noexcept
{
    const std::int64_t finished_ns = LoadLatencyNanoseconds(at);
    snapshot_load_finished_ns_.store(
        finished_ns,
        std::memory_order_relaxed);
    std::lock_guard lock(preparation_rounds_mutex_);
    if (!preparation_rounds_.empty()) {
        preparation_rounds_.back().snapshot_load_finished_ns = finished_ns;
    }
}

void LoadLatencyAttempt::MarkContextPrepared(
    LoadLatencyTimePoint at) noexcept
{
    MarkContextPrepared(false, at);
}

void LoadLatencyAttempt::MarkContextPrepared(
    bool context_reused,
    LoadLatencyTimePoint at) noexcept
{
    const std::int64_t prepared_ns = LoadLatencyNanoseconds(at);
    context_reused_.store(context_reused, std::memory_order_relaxed);
    context_prepared_ns_.store(prepared_ns, std::memory_order_relaxed);
    std::lock_guard lock(preparation_rounds_mutex_);
    if (!preparation_rounds_.empty()) {
        preparation_rounds_.back().context_reused = context_reused;
        preparation_rounds_.back().context_prepared_ns = prepared_ns;
    }
}

void LoadLatencyAttempt::MarkSourceRevalidated(
    LoadLatencyTimePoint at) noexcept
{
    MarkSourceRevalidated(true, at);
}

void LoadLatencyAttempt::MarkSourceRevalidated(
    bool succeeded,
    LoadLatencyTimePoint at) noexcept
{
    const std::int64_t revalidated_ns = LoadLatencyNanoseconds(at);
    source_revalidated_ns_.store(
        revalidated_ns,
        std::memory_order_relaxed);
    std::lock_guard lock(preparation_rounds_mutex_);
    if (!preparation_rounds_.empty()) {
        preparation_rounds_.back().source_revalidated_ns = revalidated_ns;
        preparation_rounds_.back().revalidation_succeeded = succeeded;
    }
}

void LoadLatencyAttempt::MarkWorkflowReused(bool reused) noexcept
{
    workflow_reused_.store(reused, std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkWorkerPrepared(
    LoadLatencyTimePoint at) noexcept
{
    worker_prepared_ns_.store(
        LoadLatencyNanoseconds(at),
        std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkCompletionReady(
    LoadLatencyTimePoint at) noexcept
{
    completion_ready_ns_.store(
        LoadLatencyNanoseconds(at),
        std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkCompletionPublished(
    LoadLatencyTimePoint at) noexcept
{
    completion_published_ns_.store(
        LoadLatencyNanoseconds(at),
        std::memory_order_relaxed);
}

void LoadLatencyAttempt::MarkCompletionDrained(
    LoadLatencyTimePoint at) noexcept
{
    completion_drained_ns_.store(
        LoadLatencyNanoseconds(at),
        std::memory_order_relaxed);
}

LoadLatencyAttemptReport LoadLatencyAttempt::Report() const noexcept
{
    LoadLatencyAttemptReport report;
    report.attempt_index = attempt_index_;
    report.target_index = target_index_;
    report.source_task_id =
        source_task_id_.load(std::memory_order_relaxed);
    report.source_is_folder =
        source_is_folder_.load(std::memory_order_relaxed);
    report.workflow_reused =
        workflow_reused_.load(std::memory_order_relaxed);
    report.context_reused =
        context_reused_.load(std::memory_order_relaxed);
    report.load_enqueued_ns = load_enqueued_ns_;
    report.worker_started_ns =
        worker_started_ns_.load(std::memory_order_relaxed);
    report.snapshot_load_started_ns =
        snapshot_load_started_ns_.load(std::memory_order_relaxed);
    report.snapshot_load_finished_ns =
        snapshot_load_finished_ns_.load(std::memory_order_relaxed);
    report.context_prepared_ns =
        context_prepared_ns_.load(std::memory_order_relaxed);
    report.source_revalidated_ns =
        source_revalidated_ns_.load(std::memory_order_relaxed);
    report.worker_prepared_ns =
        worker_prepared_ns_.load(std::memory_order_relaxed);
    report.completion_ready_ns =
        completion_ready_ns_.load(std::memory_order_relaxed);
    report.completion_published_ns =
        completion_published_ns_.load(std::memory_order_relaxed);
    report.completion_drained_ns =
        completion_drained_ns_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(preparation_rounds_mutex_);
        report.preparation_rounds = preparation_rounds_;
    }
    return report;
}

LoadLatencyAttemptHandle LoadLatencyAttemptLifecycle::Begin(
    std::size_t target_index,
    LoadLatencyTimePoint at)
{
    std::lock_guard lock(mutex_);
    auto attempt = std::shared_ptr<LoadLatencyAttempt>(
        new LoadLatencyAttempt(attempts_.size(), target_index, at));
    attempts_.push_back(attempt);
    return attempt;
}

std::vector<LoadLatencyAttemptReport>
LoadLatencyAttemptLifecycle::Reports() const noexcept
{
    std::vector<LoadLatencyAttemptReport> reports;
    std::lock_guard lock(mutex_);
    reports.reserve(attempts_.size());
    for (const LoadLatencyAttemptHandle& attempt : attempts_) {
        reports.push_back(attempt->Report());
    }
    return reports;
}

}  // namespace specforge
