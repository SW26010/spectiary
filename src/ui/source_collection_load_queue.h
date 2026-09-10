#pragma once

#include "profile/load_latency_trace_lifecycle.h"
#include "ui/background_retirement.h"
#include "ui/source_collection_preparation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionLoadDependencies;
struct SourceCollectionLoadQueueExecutionOptions;
struct SourceCollectionLoadQueueTestAccess;

struct SourceCollectionLoadCompletion {
    std::uint64_t task_id = 0;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::optional<PreparedSourceCollection> prepared;
    std::string error_message;
    bool stale = false;
    // Only speculative requests publish a cancellation marker. Foreground
    // cancellation continues to suppress completion entirely.
    bool canceled = false;
    LoadLatencyTimePoint worker_terminal_at;
    LoadLatencyAttemptHandle latency_attempt;
    std::shared_ptr<SourceLoadLatencyTrace> source_load_trace;
};

struct SourceCollectionLoadActivitySnapshot {
    std::size_t active_task_count = 0;
    std::size_t completed_count = 0;
    std::size_t worker_count = 0;
    std::size_t retirement_queued_count = 0;
    std::size_t retirement_in_flight_count = 0;
    std::uint64_t cancellation_request_count = 0;
    std::uint64_t successful_cancellation_count = 0;
    std::uint64_t retired_prepared_count = 0;
    std::uint64_t retired_resource_count = 0;
    bool runtime_resource_cancellation_checkpoint_waiting = false;

    [[nodiscard]] bool load_idle() const noexcept
    {
        return active_task_count == 0 && completed_count == 0;
    }

    [[nodiscard]] bool retirement_idle() const noexcept
    {
        return retirement_queued_count == 0 &&
               retirement_in_flight_count == 0;
    }
};

class SourceCollectionLoadQueue {
public:
    using CompletionReadyCallback = std::function<void()>;

    SourceCollectionLoadQueue();
    explicit SourceCollectionLoadQueue(
        SampleWorkflowPreparationPaths workflow_cache_paths);
    ~SourceCollectionLoadQueue();

    SourceCollectionLoadQueue(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue& operator=(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue(const SourceCollectionLoadQueue&) = delete;
    SourceCollectionLoadQueue& operator=(const SourceCollectionLoadQueue&) = delete;

    // Foreground preparation uses at most four workers, each draining queued
    // requests before exiting. Completion ordering is independent of execution.
    [[nodiscard]] std::uint64_t Enqueue(SourceCollectionLoadRequest request);
    // A single speculative request may use this unordered, below-normal
    // priority lane. It can never hold a later foreground completion behind
    // the queue's ordered publication boundary. Returns zero while an earlier
    // prefetch worker is still unwinding after cancellation.
    [[nodiscard]] std::uint64_t EnqueuePrefetch(
        SourceCollectionLoadRequest request);
    [[nodiscard]] std::vector<std::uint64_t> EnqueueBatch(
        std::vector<SourceCollectionLoadRequest> requests);
    bool Cancel(std::uint64_t task_id);
    [[nodiscard]] std::vector<SourceCollectionLoadCompletion> TakeCompleted();
    [[nodiscard]] bool NeedsService() const;
    [[nodiscard]] SourceCollectionLoadActivitySnapshot ActivitySnapshot() const;
    // One-shot test-workload handshake. The next foreground worker stops
    // before preparation until that task is canceled or the queue shuts down.
    // Arming is accepted only while the load queue is idle.
    [[nodiscard]] bool
    ArmRuntimeResourceCancellationCheckpoint();

    // The callback runs off the UI thread after the queue mutex has been
    // released. It is invoked when the published completion queue transitions
    // from empty to non-empty, when the runtime-resource cancellation
    // checkpoint becomes observable, and after a batch of background
    // resources has finished retiring. Unregister waits for an already running
    // callback so its captured notification target can be destroyed safely
    // after this method returns.
    void RegisterCompletionReadyCallback(CompletionReadyCallback callback);
    void UnregisterCompletionReadyCallback();

    // Large immutable graphs replaced or rejected by the UI are released by
    // a dedicated background reclaimer, never by the UI caller.
    void RetirePrepared(PreparedSourceCollection prepared);
    void RetireResource(BackgroundRetirementHandle resource);

private:
    explicit SourceCollectionLoadQueue(
        SourceCollectionLoadDependencies adapters,
        SourceCollectionLoadQueueExecutionOptions options);

    friend struct SourceCollectionLoadQueueTestAccess;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
