#pragma once

#include "profile/navigation_latency_trace.h"
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

struct SourceCollectionPreparationAdapters;
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
    NavigationLatencyTimePoint worker_terminal_at;
    NavigationLatencyAttemptHandle latency_attempt;
};

class SourceCollectionLoadQueue {
public:
    using CompletionReadyCallback = std::function<void()>;

    SourceCollectionLoadQueue();
    ~SourceCollectionLoadQueue();

    SourceCollectionLoadQueue(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue& operator=(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue(const SourceCollectionLoadQueue&) = delete;
    SourceCollectionLoadQueue& operator=(const SourceCollectionLoadQueue&) = delete;

    // Every request runs on its own jthread.
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

    // The callback runs on the publishing thread after the queue mutex has
    // been released. It is invoked only when the published completion queue
    // transitions from empty to non-empty. Unregister waits for an already
    // running callback so its captured notification target can be destroyed
    // safely after this method returns.
    void RegisterCompletionReadyCallback(CompletionReadyCallback callback);
    void UnregisterCompletionReadyCallback();

    // Large immutable graphs replaced or rejected by the UI are released by
    // a dedicated background reclaimer, never by the UI caller.
    void RetirePrepared(PreparedSourceCollection prepared);
    void RetireResource(BackgroundRetirementHandle resource);

private:
    explicit SourceCollectionLoadQueue(
        SourceCollectionPreparationAdapters adapters);

    friend struct SourceCollectionLoadQueueTestAccess;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
