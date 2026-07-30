#include "domain/source_collection_manifest.h"
#include "ui/source_collection_load_queue_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("specforge_source_queue_" + std::to_string(next_id.fetch_add(1)) + std::string(suffix));
}

void WriteFixture(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "could not create source queue fixture");
    stream << "fixture";
}

class MutableDirectoryChangeGeneration final
    : public specforge::DirectoryChangeGeneration {
public:
    [[nodiscard]] bool IsCurrent() const noexcept override
    {
        return current_.load(std::memory_order_relaxed);
    }

    void Invalidate() noexcept
    {
        current_.store(false, std::memory_order_relaxed);
    }

private:
    void Close() noexcept override
    {
        Invalidate();
    }

    std::atomic_bool current_ = true;
};

specforge::SourceCollectionFolderListingGenerationHandle MakeFolderListingGeneration(
    const std::filesystem::path& folder,
    specforge::DirectoryChangeGenerationHandle change_generation)
{
    return std::make_shared<const specforge::SourceCollectionFolderListingGeneration>(
        specforge::SourceCollectionFolderListingGeneration{
            specforge::ScanSourceCollectionFolder(folder),
            std::move(change_generation),
        });
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0,
    std::size_t spectrum_count = 3)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.id = "fixture";
    snapshot->source.display_name = "fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = spectrum_count;
    snapshot->collection.current_index = spectrum_index;
    snapshot->collection.can_move_previous = spectrum_index > 0;
    snapshot->collection.can_move_next = spectrum_index + 1 < spectrum_count;
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = true;
    return snapshot;
}

specforge::SourceCollectionContextReuseProof MakeFileReuseProof(
    const specforge::SpectrumSnapshot& snapshot,
    const std::vector<std::filesystem::path>& annotation_paths = {})
{
    const specforge::SourceCollectionSingleFileState dependency_state =
        specforge::CaptureSourceCollectionSingleFileState(
            snapshot.source.path,
            annotation_paths);
    specforge::SourceCollectionContext context =
        specforge::LoadSourceCollectionContextCancelable(
            snapshot,
            dependency_state,
            []() {});
    specforge::FinalizeSourceCollectionAnnotationContextFingerprint(
        context,
        annotation_paths);
    return {
        std::move(context.identity),
        dependency_state,
    };
}

specforge::SourceCollectionContextReuseProof MakeFolderReuseProof(
    const specforge::SpectrumSnapshot& snapshot,
    const specforge::SourceCollectionFolderListing& listing,
    const std::vector<std::filesystem::path>& annotation_paths = {})
{
    const specforge::SourceCollectionSingleFileState dependency_state =
        specforge::CaptureSourceCollectionSingleFileState(
            snapshot.source.path,
            annotation_paths);
    specforge::SourceCollectionContext context =
        specforge::BuildFolderSourceCollectionContext(snapshot, listing);
    specforge::FinalizeSourceCollectionAnnotationContextFingerprint(
        context,
        annotation_paths);
    return {
        std::move(context.identity),
        dependency_state,
    };
}

specforge::SourceCollectionPreparationAdapters Dependencies(
    specforge::SourceCollectionPreparationAdapters::SnapshotLoader loader,
    std::atomic_int* cache_loads = nullptr)
{
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = std::move(loader);
    dependencies.workflow_cache_loader =
        [cache_loads](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            if (cache_loads != nullptr) {
                ++*cache_loads;
            }
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    return dependencies;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

template <typename Future, typename CancellationCheck>
void WaitForRelease(
    const Future& release,
    const CancellationCheck& canceled,
    std::string_view timeout_message)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (release.wait_for(2ms) != std::future_status::ready) {
        if (canceled()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(timeout_message));
        }
    }
}

std::vector<specforge::SourceCollectionLoadCompletion> WaitForCompletions(
    specforge::SourceCollectionLoadQueue& queue,
    std::size_t expected_count)
{
    std::vector<specforge::SourceCollectionLoadCompletion> completions;
    Require(
        WaitUntil([&]() {
            std::vector<specforge::SourceCollectionLoadCompletion> ready = queue.TakeCompleted();
            completions.insert(
                completions.end(),
                std::make_move_iterator(ready.begin()),
                std::make_move_iterator(ready.end()));
            return completions.size() >= expected_count;
        }),
        "timed out waiting for background source completion");
    return completions;
}

void TestEnqueueReturnsBeforeLoaderCompletes()
{
    const std::filesystem::path path = UniqueTempPath("_async.csv");
    WriteFixture(path);
    std::promise<void> entered_promise;
    std::shared_future<void> entered = entered_promise.get_future().share();
    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();
    std::atomic_bool entered_once = false;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const std::filesystem::path& source, std::size_t index, const auto& canceled) {
            if (!entered_once.exchange(true)) {
                entered_promise.set_value();
            }
            WaitForRelease(release, canceled, "timed out waiting to release the decoder");
            return MakeSnapshot(source, index);
        }));
    const std::uint64_t task_id = queue.Enqueue({.path = path, .spectrum_index = 1});
    Require(task_id != 0, "enqueue should return a task id");
    Require(entered.wait_for(2s) == std::future_status::ready, "worker should start the decoder");
    Require(queue.NeedsService(), "blocked worker should report active work");
    release_promise.set_value();

    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "successful decoder should publish a prepared result");
    Require(
        completions.front().prepared->spectrum_index == 1,
        "prepared result should preserve the requested row");
    std::filesystem::remove(path);
}

void TestBatchLoadsWorkflowCachesOnce()
{
    const std::filesystem::path first = UniqueTempPath("_batch_a.csv");
    const std::filesystem::path second = UniqueTempPath("_batch_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::atomic_int cache_loads = 0;
    std::atomic_int decoder_entries = 0;
    std::promise<void> both_decoders_entered_promise;
    std::shared_future<void> both_decoders_entered =
        both_decoders_entered_promise.get_future().share();
    std::promise<void> release_decoders_promise;
    std::shared_future<void> release_decoders = release_decoders_promise.get_future().share();
    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (decoder_entries.fetch_add(1) + 1 == 2) {
                both_decoders_entered_promise.set_value();
            }
            WaitForRelease(
                release_decoders,
                canceled,
                "timed out waiting to release the batch decoders");
            return MakeSnapshot(path, index);
        },
        &cache_loads));

    const auto ids = queue.EnqueueBatch({
        {.path = first},
        {.path = second},
    });
    Require(ids.size() == 2, "batch enqueue should return every task id");
    const bool both_started =
        both_decoders_entered.wait_for(2s) == std::future_status::ready;
    release_decoders_promise.set_value();
    auto completions = WaitForCompletions(queue, 2);
    Require(both_started, "batch sources should decode concurrently");
    Require(
        completions[0].prepared && completions[1].prepared,
        "every batch source should produce a prepared result");
    Require(cache_loads.load() == 1, "one restore batch should share one immutable cache snapshot");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestSourcesUseIndependentThreads()
{
    const std::filesystem::path first = UniqueTempPath("_parallel_a.csv");
    const std::filesystem::path second = UniqueTempPath("_parallel_b.csv");
    for (const auto& path : {first, second}) {
        WriteFixture(path);
    }

    std::atomic_int decoder_entries = 0;
    std::promise<void> both_decoders_entered_promise;
    std::shared_future<void> both_decoders_entered =
        both_decoders_entered_promise.get_future().share();
    std::promise<void> release_decoders_promise;
    std::shared_future<void> release_decoders = release_decoders_promise.get_future().share();
    std::mutex thread_ids_mutex;
    std::vector<std::thread::id> thread_ids;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            {
                std::lock_guard lock(thread_ids_mutex);
                thread_ids.push_back(std::this_thread::get_id());
            }
            if (decoder_entries.fetch_add(1) + 1 == 2) {
                both_decoders_entered_promise.set_value();
            }
            WaitForRelease(
                release_decoders,
                canceled,
                "timed out waiting to release the parallel decoders");
            return MakeSnapshot(path, index);
        }));

    (void)queue.Enqueue({.path = first});
    (void)queue.Enqueue({.path = second});
    const bool both_started =
        both_decoders_entered.wait_for(2s) == std::future_status::ready;
    release_decoders_promise.set_value();

    (void)WaitForCompletions(queue, 2);
    Require(both_started, "a source load must not wait for another source to finish");
    {
        std::lock_guard lock(thread_ids_mutex);
        Require(thread_ids.size() == 2, "parallel test should decode both sources");
        Require(thread_ids[0] != thread_ids[1], "each source should have an independent thread");
    }

    for (const auto& path : {first, second}) {
        std::filesystem::remove(path);
    }
}

void TestIndividualLoadsPublishInRequestOrder()
{
    const std::filesystem::path first = UniqueTempPath("_individual_order_a.csv");
    const std::filesystem::path second = UniqueTempPath("_individual_order_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::promise<void> second_decoder_entered_promise;
    std::shared_future<void> second_decoder_entered =
        second_decoder_entered_promise.get_future().share();
    std::promise<void> release_first_promise;
    std::shared_future<void> release_first = release_first_promise.get_future().share();

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (path == first) {
                WaitForRelease(
                    release_first,
                    canceled,
                    "timed out waiting to release the first individual decoder");
                return MakeSnapshot(path, index);
            }
            second_decoder_entered_promise.set_value();
            throw std::runtime_error("second individual source finished first");
        }));
    const std::uint64_t first_id = queue.Enqueue({.path = first});
    const std::uint64_t second_id = queue.Enqueue({.path = second});
    const bool second_started =
        second_decoder_entered.wait_for(2s) == std::future_status::ready;
    bool published_early = false;
    std::vector<specforge::SourceCollectionLoadCompletion> completions;
    if (second_started) {
        published_early = WaitUntil(
            [&]() {
                std::vector<specforge::SourceCollectionLoadCompletion> ready =
                    queue.TakeCompleted();
                completions.insert(
                    completions.end(),
                    std::make_move_iterator(ready.begin()),
                    std::make_move_iterator(ready.end()));
                return !completions.empty();
            },
            100ms);
    }
    release_first_promise.set_value();
    if (completions.size() < 2) {
        std::vector<specforge::SourceCollectionLoadCompletion> remaining =
            WaitForCompletions(queue, 2 - completions.size());
        completions.insert(
            completions.end(),
            std::make_move_iterator(remaining.begin()),
            std::make_move_iterator(remaining.end()));
    }

    Require(second_started, "a later individual source should load before the first finishes");
    Require(!published_early, "individual source results should retain request order");
    Require(
        completions.size() == 2 && completions[0].task_id == first_id &&
            completions[1].task_id == second_id,
        "individual completion order should remain stable");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestPrefetchNeverBlocksForegroundPublication()
{
    const std::filesystem::path prefetch_path =
        UniqueTempPath("_prefetch_priority.csv");
    const std::filesystem::path foreground_path =
        UniqueTempPath("_prefetch_foreground.csv");
    WriteFixture(prefetch_path);
    WriteFixture(foreground_path);
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(prefetch_path, 0);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*initial_snapshot);
    std::promise<void> prefetch_entered_promise;
    std::shared_future<void> prefetch_entered =
        prefetch_entered_promise.get_future().share();
    std::promise<void> release_prefetch_promise;
    std::shared_future<void> release_prefetch =
        release_prefetch_promise.get_future().share();
    std::atomic_int prefetch_priority =
        THREAD_PRIORITY_ERROR_RETURN;
    std::atomic_int foreground_priority =
        THREAD_PRIORITY_ERROR_RETURN;
    std::atomic_bool prefetch_entered_once = false;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(
        Dependencies(
            [&](const auto& path,
                std::size_t index,
                const auto& canceled) {
                if (path == prefetch_path) {
                    prefetch_priority.store(
                        ::GetThreadPriority(
                            ::GetCurrentThread()),
                        std::memory_order_relaxed);
                    if (!prefetch_entered_once.exchange(
                            true,
                            std::memory_order_relaxed)) {
                        prefetch_entered_promise.set_value();
                    }
                    WaitForRelease(
                        release_prefetch,
                        canceled,
                        "timed out waiting to release prefetch");
                } else {
                    foreground_priority.store(
                        ::GetThreadPriority(
                            ::GetCurrentThread()),
                        std::memory_order_relaxed);
                }
                return MakeSnapshot(path, index);
            }));
    const std::uint64_t prefetch_id =
        queue.EnqueuePrefetch({
            .path = prefetch_path,
            .spectrum_index = 1,
            .reuse =
                specforge::SourceCollectionReuseCandidate::Verified(
                    proof,
                    0),
        });
    Require(
        prefetch_entered.wait_for(2s) ==
            std::future_status::ready,
        "prefetch worker should start");
    const std::uint64_t overlapping_prefetch_id =
        queue.EnqueuePrefetch({
            .path = prefetch_path,
            .spectrum_index = 1,
            .reuse =
                specforge::SourceCollectionReuseCandidate::Verified(
                    proof,
                    0),
        });
    const std::uint64_t foreground_id =
        queue.Enqueue({.path = foreground_path});

    std::vector<specforge::SourceCollectionLoadCompletion>
        foreground = WaitForCompletions(queue, 1);
    const bool cancel_requested =
        queue.Cancel(prefetch_id);
    release_prefetch_promise.set_value();
    std::vector<specforge::SourceCollectionLoadCompletion>
        canceled_prefetch;
    const bool canceled_terminal_published =
        WaitUntil([&]() {
            std::vector<specforge::SourceCollectionLoadCompletion>
                ready = queue.TakeCompleted();
            canceled_prefetch.insert(
                canceled_prefetch.end(),
                std::make_move_iterator(ready.begin()),
                std::make_move_iterator(ready.end()));
            return !canceled_prefetch.empty();
        });
    Require(
        WaitUntil([&queue]() {
            return !queue.NeedsService();
        }),
        "canceled prefetch should reach a terminal state");

    Require(
        foreground.size() == 1 &&
            foreground.front().task_id == foreground_id,
        "an unfinished prefetch must not hold a later foreground completion behind ordered publication");
    Require(
        cancel_requested,
        "the still-running prefetch should remain cancelable");
    Require(
        overlapping_prefetch_id == 0,
        "the queue should admit at most one live prefetch worker");
    Require(
        canceled_terminal_published &&
            canceled_prefetch.size() == 1 &&
            canceled_prefetch.front().task_id ==
                prefetch_id &&
            canceled_prefetch.front().canceled &&
            canceled_prefetch.front()
                    .worker_terminal_at !=
                specforge::
                    LoadLatencyTimePoint{},
        "a canceled prefetch should publish one worker-terminal marker");
    Require(
        prefetch_priority.load(
            std::memory_order_relaxed) ==
            THREAD_PRIORITY_BELOW_NORMAL,
        "prefetch worker should run below normal priority");
    Require(
        foreground_priority.load(
            std::memory_order_relaxed) ==
            THREAD_PRIORITY_NORMAL,
        "foreground worker should retain normal priority");
    const std::uint64_t retry_prefetch_id =
        queue.EnqueuePrefetch({
            .path = prefetch_path,
            .spectrum_index = 1,
            .reuse =
                specforge::SourceCollectionReuseCandidate::Verified(
                    proof,
                    0),
        });
    Require(
        retry_prefetch_id != 0,
        "a replacement prefetch should be admitted after the canceled worker exits");
    std::vector<specforge::SourceCollectionLoadCompletion> retried =
        WaitForCompletions(queue, 1);
    Require(
        retried.size() == 1 &&
            retried.front().task_id == retry_prefetch_id,
        "only the replacement prefetch should publish a completion");
    std::filesystem::remove(prefetch_path);
    std::filesystem::remove(foreground_path);
}

void TestBatchPublishesInRequestOrder()
{
    const std::filesystem::path first = UniqueTempPath("_ordered_a.csv");
    const std::filesystem::path second = UniqueTempPath("_ordered_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::promise<void> second_decoder_entered_promise;
    std::shared_future<void> second_decoder_entered =
        second_decoder_entered_promise.get_future().share();
    std::promise<void> release_first_promise;
    std::shared_future<void> release_first = release_first_promise.get_future().share();

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (path == first) {
                WaitForRelease(
                    release_first,
                    canceled,
                    "timed out waiting to release the first batch decoder");
                return MakeSnapshot(path, index);
            }
            second_decoder_entered_promise.set_value();
            throw std::runtime_error("second source finished first");
        }));
    const std::vector<std::uint64_t> ids = queue.EnqueueBatch({
        {.path = first},
        {.path = second},
    });
    const bool second_started =
        second_decoder_entered.wait_for(2s) == std::future_status::ready;
    bool published_early = false;
    std::vector<specforge::SourceCollectionLoadCompletion> completions;
    if (second_started) {
        published_early = WaitUntil(
            [&]() {
                std::vector<specforge::SourceCollectionLoadCompletion> ready =
                    queue.TakeCompleted();
                completions.insert(
                    completions.end(),
                    std::make_move_iterator(ready.begin()),
                    std::make_move_iterator(ready.end()));
                return !completions.empty();
            },
            100ms);
    }
    release_first_promise.set_value();
    if (completions.size() < 2) {
        std::vector<specforge::SourceCollectionLoadCompletion> remaining =
            WaitForCompletions(queue, 2 - completions.size());
        completions.insert(
            completions.end(),
            std::make_move_iterator(remaining.begin()),
            std::make_move_iterator(remaining.end()));
    }

    Require(second_started, "later batch sources should start before the first source finishes");
    Require(!published_early, "a batch should not publish later sources out of request order");
    Require(
        completions.size() == 2 && completions[0].task_id == ids[0] &&
            completions[1].task_id == ids[1],
        "batch completion order should remain stable");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestCompletionReadyNotificationCoalescesUntilDrain()
{
    const std::filesystem::path first = UniqueTempPath("_notify_batch_a.csv");
    const std::filesystem::path second = UniqueTempPath("_notify_batch_b.csv");
    const std::filesystem::path third = UniqueTempPath("_notify_after_drain.csv");
    WriteFixture(first);
    WriteFixture(second);
    WriteFixture(third);
    std::promise<void> second_decoder_entered_promise;
    std::shared_future<void> second_decoder_entered =
        second_decoder_entered_promise.get_future().share();
    std::promise<void> release_first_promise;
    std::shared_future<void> release_first = release_first_promise.get_future().share();
    std::atomic_int notifications = 0;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (path == first) {
                WaitForRelease(
                    release_first,
                    canceled,
                    "timed out waiting to release the first notification decoder");
            } else if (path == second) {
                second_decoder_entered_promise.set_value();
            }
            return MakeSnapshot(path, index);
        }));
    queue.RegisterCompletionReadyCallback([&notifications]() {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    const std::vector<std::uint64_t> batch_ids = queue.EnqueueBatch({
        {.path = first},
        {.path = second},
    });
    const bool second_started =
        second_decoder_entered.wait_for(2s) == std::future_status::ready;
    if (second_started) {
        std::this_thread::sleep_for(100ms);
    }
    release_first_promise.set_value();

    std::vector<specforge::SourceCollectionLoadCompletion> batch_completions =
        WaitForCompletions(queue, 2);
    Require(second_started, "notification test should finish the later batch task first");
    Require(
        batch_completions.size() == 2 &&
            batch_completions[0].task_id == batch_ids[0] &&
            batch_completions[1].task_id == batch_ids[1],
        "notification coalescing must preserve ordered completion publication");
    Require(
        WaitUntil([&notifications]() {
            return notifications.load(std::memory_order_relaxed) == 1;
        }),
        "multiple completions published into a non-empty queue should coalesce to one notification");

    const std::uint64_t third_id = queue.Enqueue({.path = third});
    std::vector<specforge::SourceCollectionLoadCompletion> next_completion =
        WaitForCompletions(queue, 1);
    Require(
        next_completion.size() == 1 && next_completion.front().task_id == third_id,
        "a completion published after drain should remain activatable");
    Require(
        WaitUntil([&notifications]() {
            return notifications.load(std::memory_order_relaxed) == 2;
        }),
        "draining the queue should allow the next publication to notify again");

    std::filesystem::remove(first);
    std::filesystem::remove(second);
    std::filesystem::remove(third);
}

void TestBufferedBatchCompletionCanBeCanceled()
{
    const std::filesystem::path first = UniqueTempPath("_buffered_cancel_a.csv");
    const std::filesystem::path second = UniqueTempPath("_buffered_cancel_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::promise<void> second_decoder_entered_promise;
    std::shared_future<void> second_decoder_entered =
        second_decoder_entered_promise.get_future().share();
    std::promise<void> release_first_promise;
    std::shared_future<void> release_first = release_first_promise.get_future().share();
    std::atomic_int notifications = 0;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (path == first) {
                WaitForRelease(
                    release_first,
                    canceled,
                    "timed out waiting to release the buffered first decoder");
                return MakeSnapshot(path, index);
            }
            second_decoder_entered_promise.set_value();
            throw std::runtime_error("buffered completion should be canceled");
        }));
    queue.RegisterCompletionReadyCallback([&notifications]() {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    const std::vector<std::uint64_t> ids = queue.EnqueueBatch({
        {.path = first},
        {.path = second},
    });
    const bool second_started =
        second_decoder_entered.wait_for(2s) == std::future_status::ready;
    if (second_started) {
        std::this_thread::sleep_for(100ms);
        queue.Cancel(ids[1]);
    }
    release_first_promise.set_value();

    std::vector<specforge::SourceCollectionLoadCompletion> completions =
        WaitForCompletions(queue, 1);
    Require(second_started, "buffered cancel test should run the later source concurrently");
    Require(
        completions.size() == 1 && completions.front().task_id == ids[0],
        "cancel should suppress a completion buffered behind an earlier batch source");
    Require(
        WaitUntil([&notifications]() {
            return notifications.load(std::memory_order_relaxed) == 1;
        }),
        "retiring a canceled buffered result must not add an activatable notification");
    Require(!queue.NeedsService(), "a canceled buffered completion should leave no queued result");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestCancelSuppressesCompletion()
{
    const std::filesystem::path path = UniqueTempPath("_cancel.csv");
    WriteFixture(path);
    std::promise<void> entered_promise;
    std::shared_future<void> entered = entered_promise.get_future().share();
    std::atomic_bool entered_once = false;
    std::atomic_int notifications = 0;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& source, std::size_t index, const auto& canceled) {
            if (!entered_once.exchange(true)) {
                entered_promise.set_value();
            }
            while (!canceled()) {
                std::this_thread::sleep_for(1ms);
            }
            return MakeSnapshot(source, index);
        }));
    queue.RegisterCompletionReadyCallback([&notifications]() {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    const std::uint64_t task_id = queue.Enqueue({.path = path});
    Require(entered.wait_for(2s) == std::future_status::ready, "cancel test loader should start");
    const specforge::SourceCollectionLoadActivitySnapshot active =
        queue.ActivitySnapshot();
    Require(
        active.active_task_count == 1 &&
            active.worker_count == 1,
        "activity snapshot should expose the live source worker");
    queue.Cancel(task_id);
    Require(
        WaitUntil([&]() { return !queue.NeedsService(); }),
        "canceled worker should reach a terminal state");
    Require(queue.TakeCompleted().empty(), "canceled task should not publish a stale result");
    Require(
        notifications.load(std::memory_order_relaxed) == 0,
        "a canceled task with no activatable completion must not notify");
    const specforge::SourceCollectionLoadActivitySnapshot canceled =
        queue.ActivitySnapshot();
    Require(
        canceled.cancellation_request_count == 1 &&
            canceled.successful_cancellation_count == 1,
        "activity snapshot should retain successful cancellation evidence");
    Require(
        canceled.load_idle(),
        "activity snapshot should become load-idle after cancellation");
    std::filesystem::remove(path);
}

void TestCanceledWorkerRemainsNonIdleUntilItRetires()
{
    const std::filesystem::path path =
        UniqueTempPath(
            "_cancel_retirement_barrier.csv");
    WriteFixture(path);
    std::promise<void> entered_promise;
    std::shared_future<void> entered =
        entered_promise.get_future().share();
    std::promise<void> cancellation_seen_promise;
    std::shared_future<void> cancellation_seen =
        cancellation_seen_promise.get_future().share();
    std::promise<void> release_promise;
    std::shared_future<void> release =
        release_promise.get_future().share();
    std::atomic_bool entered_once = false;
    std::atomic_bool cancellation_signaled = false;

    specforge::SourceCollectionLoadQueue queue =
        specforge::
            MakeSourceCollectionLoadQueueForTesting(
                Dependencies(
                    [&](const auto& source,
                        std::size_t index,
                        const auto& canceled) {
                        if (!entered_once.exchange(
                                true)) {
                            entered_promise.set_value();
                        }
                        while (!canceled()) {
                            std::this_thread::sleep_for(
                                1ms);
                        }
                        if (!cancellation_signaled
                                 .exchange(true)) {
                            cancellation_seen_promise
                                .set_value();
                        }
                        release.wait();
                        return MakeSnapshot(
                            source,
                            index);
                    }));
    const std::uint64_t task_id =
        queue.Enqueue({.path = path});
    Require(
        entered.wait_for(2s) ==
            std::future_status::ready,
        "retirement barrier fixture should start its source worker");
    Require(
        queue.Cancel(task_id),
        "retirement barrier fixture should cancel the active source task");
    Require(
        cancellation_seen.wait_for(2s) ==
            std::future_status::ready,
        "the source worker should observe cancellation before its controlled retirement");
    const auto before_retirement =
        queue.ActivitySnapshot();
    Require(
        queue.NeedsService() &&
            before_retirement.active_task_count ==
                1 &&
            !before_retirement.load_idle(),
        "a canceled worker must keep Shell loading non-idle until the worker actually retires");

    release_promise.set_value();
    Require(
        WaitUntil([&]() {
            return !queue.NeedsService();
        }),
        "the canceled source worker should become idle after controlled retirement");
    Require(
        queue.TakeCompleted().empty() &&
            queue.ActivitySnapshot()
                .load_idle(),
        "a retired canceled worker should leave no activatable completion");
    std::filesystem::remove(path);
}

void TestRuntimeResourceCancellationHandshakeControlsFastResidentReuse()
{
    const std::filesystem::path path =
        UniqueTempPath("_resident_cancel_race.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, 0);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*snapshot);
    specforge::SourceCollectionResidentSnapshot resident{
        .spectrum_index = 0,
        .snapshot = snapshot,
        .context_reuse_proof = proof,
    };
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(
            Dependencies(
                [&decoder_calls](
                    const auto& source,
                    std::size_t index,
                    const auto&) {
                    ++decoder_calls;
                    return MakeSnapshot(source, index);
                }));

    Require(
        queue.ArmRuntimeResourceCancellationCheckpoint(),
        "an idle queue should arm the runtime-resource cancellation checkpoint");
    const std::uint64_t task_id = queue.Enqueue({
        .path = path,
        .reuse =
            specforge::SourceCollectionReuseCandidate::Verified(
                proof,
                0,
                {},
                std::move(resident)),
    });
    Require(
        WaitUntil([&queue]() {
            return queue.ActivitySnapshot()
                .runtime_resource_cancellation_checkpoint_waiting;
        }),
        "the fast resident request should stop at the deterministic cancellation checkpoint");

    Require(
        queue.Cancel(task_id),
        "the checkpointed resident request should remain cancelable");
    Require(
        WaitUntil([&queue]() {
            (void)queue.TakeCompleted();
            return !queue.NeedsService();
        }),
        "the canceled checkpointed request should terminate");
    const specforge::SourceCollectionLoadActivitySnapshot
        activity = queue.ActivitySnapshot();
    Require(
        activity.successful_cancellation_count == 1 &&
            decoder_calls.load(std::memory_order_relaxed) == 0,
        "the cancellation race fixture should use the fast resident path");
    std::filesystem::remove(path);
}

void TestCompletionReadyCallbackIsReentrantAndUnregistersSafely()
{
    const std::filesystem::path first = UniqueTempPath("_notify_unregister_a.csv");
    const std::filesystem::path second = UniqueTempPath("_notify_unregister_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::promise<void> callback_entered_promise;
    std::future<void> callback_entered = callback_entered_promise.get_future();
    std::promise<void> release_callback_promise;
    std::shared_future<void> release_callback = release_callback_promise.get_future().share();
    std::atomic_bool callback_entered_once = false;
    std::atomic_bool reentrant_query_succeeded = false;
    std::atomic_bool notification_target_alive = true;
    std::atomic_int notifications = 0;
    std::atomic_int invalid_target_accesses = 0;

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [](const auto& path, std::size_t index, const auto&) {
            return MakeSnapshot(path, index);
        }));
    queue.RegisterCompletionReadyCallback([&]() {
        notifications.fetch_add(1, std::memory_order_relaxed);
        if (!notification_target_alive.load(std::memory_order_relaxed)) {
            invalid_target_accesses.fetch_add(1, std::memory_order_relaxed);
        }
        reentrant_query_succeeded.store(queue.NeedsService(), std::memory_order_relaxed);
        if (!callback_entered_once.exchange(true, std::memory_order_relaxed)) {
            callback_entered_promise.set_value();
        }
        release_callback.wait();
    });

    (void)queue.Enqueue({.path = first});
    const bool callback_started =
        callback_entered.wait_for(2s) == std::future_status::ready;
    std::promise<void> unregister_entered_promise;
    std::future<void> unregister_entered = unregister_entered_promise.get_future();
    std::future<void> unregister = std::async(std::launch::async, [&]() {
        unregister_entered_promise.set_value();
        queue.UnregisterCompletionReadyCallback();
    });
    const bool unregister_started =
        unregister_entered.wait_for(2s) == std::future_status::ready;
    const bool unregister_waited =
        callback_started && unregister_started &&
        unregister.wait_for(50ms) == std::future_status::timeout;
    release_callback_promise.set_value();
    const bool unregister_finished =
        unregister.wait_for(2s) == std::future_status::ready;

    Require(callback_started, "completion-ready callback should run for a published completion");
    Require(
        reentrant_query_succeeded.load(std::memory_order_relaxed),
        "completion-ready callback should query the queue without running under its mutex");
    Require(
        unregister_waited,
        "unregister should wait for an already running completion-ready callback");
    Require(unregister_finished, "completion-ready callback unregister should finish after callback exit");

    std::vector<specforge::SourceCollectionLoadCompletion> first_completion =
        WaitForCompletions(queue, 1);
    Require(first_completion.size() == 1, "the first notified completion should remain drainable");
    notification_target_alive.store(false, std::memory_order_relaxed);
    (void)queue.Enqueue({.path = second});
    std::vector<specforge::SourceCollectionLoadCompletion> second_completion =
        WaitForCompletions(queue, 1);
    Require(second_completion.size() == 1, "late completion should remain available after unregister");
    Require(
        notifications.load(std::memory_order_relaxed) == 1,
        "a completion published after unregister must not call the stale notification target");
    Require(
        invalid_target_accesses.load(std::memory_order_relaxed) == 0,
        "unregister should prevent access to an invalidated notification target");

    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestCancelStopsOnlyItsSourceThread()
{
    const std::filesystem::path canceled_path = UniqueTempPath("_cancel_one.csv");
    const std::filesystem::path surviving_path = UniqueTempPath("_cancel_survivor.csv");
    WriteFixture(canceled_path);
    WriteFixture(surviving_path);
    std::atomic_int decoder_entries = 0;
    std::atomic_bool canceled_decoder_stopped = false;
    std::promise<void> both_decoders_entered_promise;
    std::shared_future<void> both_decoders_entered =
        both_decoders_entered_promise.get_future().share();
    std::promise<void> release_survivor_promise;
    std::shared_future<void> release_survivor = release_survivor_promise.get_future().share();

    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [&](const auto& path, std::size_t index, const auto& canceled) {
            if (decoder_entries.fetch_add(1) + 1 == 2) {
                both_decoders_entered_promise.set_value();
            }
            if (path == canceled_path) {
                while (!canceled()) {
                    std::this_thread::sleep_for(1ms);
                }
                canceled_decoder_stopped.store(true);
            } else {
                while (release_survivor.wait_for(1ms) != std::future_status::ready &&
                       !canceled()) {
                }
            }
            return MakeSnapshot(path, index);
        }));
    const std::uint64_t canceled_id = queue.Enqueue({.path = canceled_path});
    const std::uint64_t surviving_id = queue.Enqueue({.path = surviving_path});
    Require(
        both_decoders_entered.wait_for(2s) == std::future_status::ready,
        "independent cancel test should start both source threads");
    queue.Cancel(canceled_id);
    Require(
        WaitUntil([&]() { return canceled_decoder_stopped.load(); }),
        "cancel should stop its selected source thread");
    Require(queue.NeedsService(), "canceling one source must not stop another active source");
    release_survivor_promise.set_value();

    std::vector<specforge::SourceCollectionLoadCompletion> completions =
        WaitForCompletions(queue, 1);
    Require(
        completions.size() == 1 && completions.front().task_id == surviving_id,
        "only the uncanceled source should publish a completion");
    std::filesystem::remove(canceled_path);
    std::filesystem::remove(surviving_path);
}

void TestFailureIsReported()
{
    const std::filesystem::path path = UniqueTempPath("_failure.csv");
    WriteFixture(path);
    specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("decoder failure");
        }));
    (void)queue.Enqueue({.path = path});
    auto completions = WaitForCompletions(queue, 1);
    Require(!completions.front().prepared, "failed decoder must not publish prepared data");
    Require(
        completions.front().error_message == "decoder failure",
        "failed decoder should preserve its diagnostic");
    std::filesystem::remove(path);
}

void TestDestructionStopsEverySourceThread()
{
    const std::filesystem::path first = UniqueTempPath("_shutdown_a.csv");
    const std::filesystem::path second = UniqueTempPath("_shutdown_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::atomic_int decoder_entries = 0;
    std::atomic_int canceled_decoders = 0;
    std::promise<void> both_decoders_entered_promise;
    std::shared_future<void> both_decoders_entered =
        both_decoders_entered_promise.get_future().share();

    {
        specforge::SourceCollectionLoadQueue queue =
        specforge::MakeSourceCollectionLoadQueueForTesting(Dependencies(
            [&](const auto& path, std::size_t index, const auto& canceled) {
                if (decoder_entries.fetch_add(1) + 1 == 2) {
                    both_decoders_entered_promise.set_value();
                }
                while (!canceled()) {
                    std::this_thread::sleep_for(1ms);
                }
                ++canceled_decoders;
                return MakeSnapshot(path, index);
            }));
        (void)queue.Enqueue({.path = first});
        (void)queue.Enqueue({.path = second});
        Require(
            both_decoders_entered.wait_for(2s) == std::future_status::ready,
            "shutdown test should start every source thread");
    }

    Require(canceled_decoders.load() == 2, "queue destruction should stop and join every source thread");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

struct DestructionProbe {
    std::promise<std::thread::id>* destroyed = nullptr;

    ~DestructionProbe()
    {
        destroyed->set_value(std::this_thread::get_id());
    }
};

void TestRetirementRunsOnWorker()
{
    std::promise<std::thread::id> destroyed_promise;
    std::future<std::thread::id> destroyed = destroyed_promise.get_future();
    const std::thread::id caller = std::this_thread::get_id();
    specforge::SourceCollectionLoadQueue queue;
    queue.RetireResource(std::make_shared<DestructionProbe>(&destroyed_promise));
    Require(
        destroyed.wait_for(2s) == std::future_status::ready,
        "retired resource should be released promptly");
    Require(destroyed.get() != caller, "retired resource should not be destroyed on the UI caller");
    Require(
        WaitUntil([&queue]() {
            const auto activity =
                queue.ActivitySnapshot();
            return activity.retirement_idle() &&
                   activity.retired_resource_count == 1;
        }),
        "activity snapshot should become retirement-idle after destruction");
}

}  // namespace

int main()
{
    try {
        TestEnqueueReturnsBeforeLoaderCompletes();
        TestBatchLoadsWorkflowCachesOnce();
        TestSourcesUseIndependentThreads();
        TestIndividualLoadsPublishInRequestOrder();
        TestPrefetchNeverBlocksForegroundPublication();
        TestBatchPublishesInRequestOrder();
        TestCompletionReadyNotificationCoalescesUntilDrain();
        TestBufferedBatchCompletionCanBeCanceled();
        TestCancelSuppressesCompletion();
        TestCanceledWorkerRemainsNonIdleUntilItRetires();
        TestRuntimeResourceCancellationHandshakeControlsFastResidentReuse();
        TestCompletionReadyCallbackIsReentrantAndUnregistersSafely();
        TestCancelStopsOnlyItsSourceThread();
        TestFailureIsReported();
        TestDestructionStopsEverySourceThread();
        TestRetirementRunsOnWorker();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
