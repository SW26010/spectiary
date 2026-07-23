#include "domain/source_collection_manifest.h"
#include "ui/source_collection_load_queue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

specforge::SourceCollectionLoadDependencies Dependencies(
    specforge::SourceCollectionLoadDependencies::SnapshotLoader loader,
    std::atomic_int* cache_loads = nullptr)
{
    specforge::SourceCollectionLoadDependencies dependencies;
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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

void TestMatchingIdentitySkipsWorkflowCacheLoad()
{
    const std::filesystem::path path = UniqueTempPath("_reuse.csv");
    WriteFixture(path);
    const auto snapshot = MakeSnapshot(path, 2);
    const specforge::SourceCollectionSingleFileState state =
        specforge::CaptureSourceCollectionSingleFileState(path);
    const specforge::SourceCollectionIdentity identity =
        specforge::BuildSourceCollectionIdentity(*snapshot, state);
    std::atomic_int cache_loads = 0;

    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; },
        &cache_loads));
    (void)queue.Enqueue({
        .path = path,
        .spectrum_index = 2,
        .reuse_identity = identity,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "reuse load should succeed");
    Require(
        std::holds_alternative<specforge::PreparedSourceCollectionReuse>(
            completions.front().prepared->payload),
        "matching source generation should use snapshot-only reuse");
    Require(cache_loads.load() == 0, "snapshot-only reuse should not read workflow caches");
    std::filesystem::remove(path);
}

void TestChangedContextFullPlanCarriesLiveWorkflowRevision()
{
    const std::filesystem::path path = UniqueTempPath("_revision.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path, 1);
    const specforge::SourceCollectionSingleFileState state =
        specforge::CaptureSourceCollectionSingleFileState(path);
    specforge::SourceCollectionIdentity previous_identity =
        specforge::BuildSourceCollectionIdentity(*snapshot, state);
    previous_identity.context_fingerprint = "previous-context";

    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; }));
    (void)queue.Enqueue({
        .path = path,
        .spectrum_index = 1,
        .reuse_identity = previous_identity,
        .base_live_workflow_revision = 42,
    });
    std::vector<specforge::SourceCollectionLoadCompletion> completions =
        WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed context load should succeed");
    const auto* plan = std::get_if<specforge::PreparedSourceCollectionPlan>(
        &completions.front().prepared->payload);
    Require(plan != nullptr, "changed context should produce a full prepared plan");
    Require(
        plan->base_live_workflow_revision == 42,
        "the full plan must retain the live workflow revision captured at enqueue time");
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
    specforge::SourceCollectionLoadQueue queue(Dependencies(
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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
    Require(!queue.NeedsService(), "a canceled buffered completion should leave no queued result");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestFolderUsesOnePreparedListing()
{
    const std::filesystem::path folder = UniqueTempPath("_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    std::atomic_int folder_loader_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_snapshot_loader =
        [&](const auto& path, std::size_t index, const auto& listing, const auto&) {
            ++folder_loader_calls;
            Require(listing.spectra.size() == 1, "folder loader should receive the prepared listing");
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({.path = folder});
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "folder task should prepare successfully");
    Require(folder_loader_calls.load() == 1, "folder decoder should consume one prepared listing");
    std::filesystem::remove_all(folder);
}

void TestStableFolderListingGenerationSurvivesLoadWorkerExit()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_generation_worker_exit");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    std::atomic_int folder_scan_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    dependencies.folder_snapshot_loader =
        [](const auto& path, std::size_t index, const auto& listing, const auto&) {
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));

    (void)queue.Enqueue({.path = folder});
    auto first_completions = WaitForCompletions(queue, 1);
    Require(
        first_completions.front().prepared.has_value(),
        "the initial folder task should prepare successfully");
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation =
        first_completions.front().prepared->folder_listing_generation;
    Require(
        listing_generation != nullptr,
        "the initial folder task should publish its listing generation");

    // TakeCompleted reaps and joins the finished request worker before it
    // returns, so this reuses the hint only after its creator worker exited.
    (void)queue.Enqueue({
        .path = folder,
        .folder_listing_generation_hint = listing_generation,
    });
    auto second_completions = WaitForCompletions(queue, 1);
    Require(
        second_completions.front().prepared.has_value(),
        "the stable follow-up folder task should prepare successfully");
    Require(
        folder_scan_calls.load() == 1,
        "a stable folder should reuse its listing after the request worker exits");
    std::filesystem::remove_all(folder);
}

void TestFolderListingGenerationInvalidatesSafelyAfterQueueDestruction()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_generation_queue_exit");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    specforge::SourceCollectionFolderListingGenerationHandle listing_generation;
    {
        specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
            [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
                throw std::runtime_error("folder task must not use the file loader");
            });
        dependencies.folder_snapshot_loader =
            [](const auto& path, std::size_t index, const auto& listing, const auto&) {
                return MakeSnapshot(path, index, listing.spectra.size());
            };
        specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
        (void)queue.Enqueue({.path = folder});
        auto completions = WaitForCompletions(queue, 1);
        Require(
            completions.front().prepared.has_value(),
            "the monitored folder task should prepare successfully");
        listing_generation =
            completions.front().prepared->folder_listing_generation;
    }

    Require(
        listing_generation && !listing_generation->IsCurrent(),
        "monitor shutdown should safely invalidate a published generation lease");
    listing_generation.reset();
    std::filesystem::remove_all(folder);
}

void TestCanceledBlockedRegistrationStopsBeforeQueueDestruction()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_generation_cancel");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    std::promise<void> registration_entered_promise;
    std::shared_future<void> registration_entered =
        registration_entered_promise.get_future().share();
    std::promise<void> registration_stopped_promise;
    std::future<void> registration_stopped =
        registration_stopped_promise.get_future();
    std::mutex registration_mutex;
    std::condition_variable_any registration_condition;

    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_change_generation_registration_factory =
        [&](const std::filesystem::path&, std::stop_token stop_token) {
            registration_entered_promise.set_value();
            std::unique_lock lock(registration_mutex);
            const bool released = registration_condition.wait_for(
                lock,
                stop_token,
                2s,
                []() {
                    return false;
                });
            Require(
                !released && stop_token.stop_requested(),
                "blocked registration should be canceled by monitor shutdown");
            registration_stopped_promise.set_value();
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };
    dependencies.folder_snapshot_loader =
        [](const auto& path, std::size_t index, const auto& listing, const auto&) {
            return MakeSnapshot(path, index, listing.spectra.size());
        };

    {
        specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
        const std::uint64_t task_id = queue.Enqueue({.path = folder});
        Require(
            registration_entered.wait_for(2s) == std::future_status::ready,
            "folder registration should enter its blocking operation");
        queue.Cancel(task_id);
        Require(
            WaitUntil([&queue]() { return !queue.NeedsService(); }),
            "canceling the folder task should release its registration caller");
        Require(
            queue.TakeCompleted().empty(),
            "a canceled registration must not publish a source completion");
    }

    Require(
        registration_stopped.wait_for(2s) == std::future_status::ready,
        "queue destruction should stop and join the blocked monitor registration");
    std::filesystem::remove_all(folder);
}

void TestCurrentFolderListingGenerationAvoidsFullRescan()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_listing_hint");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const auto change_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation_hint =
        MakeFolderListingGeneration(folder, change_generation);
    std::atomic_int folder_scan_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    dependencies.folder_snapshot_loader =
        [](const auto& path, std::size_t index, const auto& listing, const auto&) {
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    specforge::NavigationLatencyTrace trace(
        1,
        0,
        0,
        specforge::NavigationLatencyInputKind::UiNext,
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now());
    const specforge::NavigationLatencyAttemptHandle navigation_attempt =
        trace.BeginLoadAttempt(0);
    bool prepared = false;
    bool published_listing = false;
    {
        specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
        (void)queue.Enqueue({
            .path = folder,
            .folder_listing_generation_hint = listing_generation_hint,
            .navigation_attempt = navigation_attempt,
        });
        auto completions = WaitForCompletions(queue, 1);
        prepared = completions.front().prepared.has_value();
        published_listing =
            prepared &&
            completions.front().prepared->folder_listing_generation != nullptr;
    }
    Require(prepared, "hinted folder task should prepare successfully");
    Require(
        folder_scan_calls.load() == 0,
        "a current folder listing generation should not rescan a stable folder");
    Require(
        published_listing,
        "a successful folder load should publish its post-decode verified listing");
    const specforge::NavigationLatencyAttemptReport report =
        navigation_attempt->Report();
    Require(
        report.preparation_rounds.size() == 1 &&
            report.preparation_rounds[0].hint_present &&
            report.preparation_rounds[0].generation_current_at_start &&
            !report.preparation_rounds[0].listing_scan_performed,
        "a stable warm folder round should report a current hint and no full scan");
    std::filesystem::remove_all(folder);
}

void TestDirectoryChangeGenerationInvalidatesAfterMutation()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_change_generation");
    std::filesystem::create_directory(folder);
    const std::filesystem::path sample = folder / "sample.csv";
    const std::filesystem::path added = folder / "added.csv";
    const std::filesystem::path renamed = folder / "renamed.csv";
    WriteFixture(sample);
    specforge::DirectoryChangeGenerationMonitor monitor;

    const auto require_invalidation =
        [&folder, &monitor](
            const std::function<void()>& mutate,
            std::string_view message) {
            const specforge::DirectoryChangeGenerationHandle generation =
                monitor.Begin(folder, []() {});
            Require(generation != nullptr, "Windows folder change generation should be available");
            Require(generation->IsCurrent(), "a new folder change generation should start current");
            mutate();
            Require(
                WaitUntil([&generation]() { return !generation->IsCurrent(); }),
                message);
        };
    require_invalidation(
        [&added]() { WriteFixture(added); },
        "adding a first-level file should invalidate the folder change generation");
    require_invalidation(
        [&sample]() {
            std::ofstream stream(sample, std::ios::binary | std::ios::app);
            stream << "changed";
        },
        "modifying a member file should invalidate the folder change generation");
    require_invalidation(
        [&added, &renamed]() { std::filesystem::rename(added, renamed); },
        "renaming a first-level file should invalidate the folder change generation");
    require_invalidation(
        [&renamed]() { std::filesystem::remove(renamed); },
        "removing a first-level file should invalidate the folder change generation");
    std::filesystem::remove_all(folder);
}

void TestUnavailableFolderGenerationKeepsFullRevalidationFallback()
{
    const std::filesystem::path folder = UniqueTempPath("_folder_generation_fallback");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation_hint =
        MakeFolderListingGeneration(folder, {});
    std::atomic_int folder_scan_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    dependencies.folder_change_generation_factory =
        [](const auto&, const auto&) -> specforge::DirectoryChangeGenerationHandle {
            return {};
        };
    dependencies.folder_snapshot_loader =
        [](const auto& path, std::size_t index, const auto& listing, const auto&) {
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .folder_listing_generation_hint = listing_generation_hint,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "fallback folder task should prepare");
    Require(
        folder_scan_calls.load() == 1,
        "an unavailable change generation should preserve one full post-decode scan");
    std::filesystem::remove_all(folder);
}

void TestStaleFolderListingGenerationRefreshesBeforeDecode()
{
    const std::filesystem::path folder = UniqueTempPath("_stale_folder_listing_hint");
    std::filesystem::create_directory(folder);
    const std::filesystem::path sample = folder / "sample.csv";
    WriteFixture(sample);
    const auto change_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation_hint =
        MakeFolderListingGeneration(folder, change_generation);
    {
        std::ofstream stream(sample, std::ios::binary | std::ios::app);
        stream << "changed";
    }
    std::atomic_int folder_scan_calls = 0;
    std::atomic_int folder_loader_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    dependencies.folder_change_generation_factory =
        [](const auto&, const auto&) {
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };
    dependencies.folder_snapshot_loader =
        [&](const auto& path, std::size_t index, const auto& listing, const auto&) {
            ++folder_loader_calls;
            Require(
                listing.spectra.front().stat_fingerprint !=
                    listing_generation_hint->listing.spectra.front().stat_fingerprint,
                "the decoder should receive a refreshed listing after the target file changes");
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    specforge::NavigationLatencyTrace trace(
        1,
        0,
        0,
        specforge::NavigationLatencyInputKind::UiNext,
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now());
    const specforge::NavigationLatencyAttemptHandle navigation_attempt =
        trace.BeginLoadAttempt(0);
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .folder_listing_generation_hint = listing_generation_hint,
        .navigation_attempt = navigation_attempt,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "stale hinted folder task should refresh and succeed");
    Require(folder_loader_calls.load() == 1, "a stale target should not be decoded before its listing refresh");
    Require(
        folder_scan_calls.load() == 1,
        "a stale target should refresh once and validate the new generation without a second scan");
    const specforge::NavigationLatencyAttemptReport report =
        navigation_attempt->Report();
    Require(
        report.preparation_rounds.size() == 1 &&
            report.preparation_rounds[0].hint_present &&
            report.preparation_rounds[0].generation_current_at_start &&
            report.preparation_rounds[0].listing_scan_performed,
        "a stale target should report that its current generation still required a listing scan");
    std::filesystem::remove_all(folder);
}

void TestChangedFileRetriesOneStableGeneration()
{
    const std::filesystem::path path = UniqueTempPath("_retry.csv");
    WriteFixture(path);
    std::atomic_int loader_calls = 0;
    specforge::NavigationLatencyTrace trace(
        1,
        0,
        1,
        specforge::NavigationLatencyInputKind::UiNext,
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now());
    const specforge::NavigationLatencyAttemptHandle navigation_attempt =
        trace.BeginLoadAttempt(1);
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&](const auto& source, std::size_t index, const auto&) {
            if (++loader_calls == 1) {
                std::ofstream stream(source, std::ios::binary | std::ios::app);
                stream << "changed";
            }
            return MakeSnapshot(source, index);
        }));
    (void)queue.Enqueue({.path = path, .navigation_attempt = navigation_attempt});
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed file should settle on a stable retry");
    Require(loader_calls.load() == 2, "changed file should be decoded exactly one additional time");
    const specforge::NavigationLatencyAttemptReport report = navigation_attempt->Report();
    Require(
        report.preparation_rounds.size() == 2,
        "each TOCTOU preparation retry must retain its own timing round");
    Require(
        !report.preparation_rounds[0].revalidation_succeeded &&
            report.preparation_rounds[1].revalidation_succeeded,
        "the trace should distinguish the rejected generation from the accepted retry");
    std::filesystem::remove(path);
}

void TestChangedFolderRetriesOneStableGeneration()
{
    const std::filesystem::path folder = UniqueTempPath("_retry_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const auto initial_change_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation_hint =
        MakeFolderListingGeneration(folder, initial_change_generation);
    std::atomic_int loader_calls = 0;
    std::atomic_int folder_scan_calls = 0;
    specforge::NavigationLatencyTrace trace(
        1,
        0,
        1,
        specforge::NavigationLatencyInputKind::UiNext,
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now(),
        specforge::NavigationLatencyTrace::Now());
    const specforge::NavigationLatencyAttemptHandle navigation_attempt =
        trace.BeginLoadAttempt(1);
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_snapshot_loader =
        [&](const auto& path, std::size_t index, const auto& listing, const auto&) {
            if (++loader_calls == 1) {
                WriteFixture(path / "added.csv");
                initial_change_generation->Invalidate();
            }
            return MakeSnapshot(path, index, listing.spectra.size());
        };
    dependencies.folder_change_generation_factory =
        [](const auto&, const auto&) {
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .folder_listing_generation_hint = listing_generation_hint,
        .navigation_attempt = navigation_attempt,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed folder should settle on a stable retry");
    Require(loader_calls.load() == 2, "changed folder should be decoded exactly one additional time");
    Require(
        folder_scan_calls.load() == 1,
        "a changed hinted folder should scan only the replacement generation");
    Require(
        completions.front().prepared->folder_listing_generation &&
            completions.front().prepared->folder_listing_generation
                    ->listing.spectra.size() == 2,
        "the stable retry should publish the newly verified folder generation");
    const specforge::NavigationLatencyAttemptReport report = navigation_attempt->Report();
    Require(
        report.preparation_rounds.size() == 2,
        "each folder TOCTOU retry must retain its own timing round");
    Require(
        report.preparation_rounds[0].source_is_folder &&
            report.preparation_rounds[1].source_is_folder &&
            !report.preparation_rounds[0].revalidation_succeeded &&
            report.preparation_rounds[1].revalidation_succeeded,
        "the folder trace should distinguish the rejected generation from the accepted retry");
    Require(
        report.preparation_rounds[0].hint_present &&
            report.preparation_rounds[0].generation_current_at_start &&
            !report.preparation_rounds[0].listing_scan_performed &&
            !report.preparation_rounds[1].hint_present &&
            !report.preparation_rounds[1].generation_current_at_start &&
            report.preparation_rounds[1].listing_scan_performed,
        "the folder trace should attribute the retry scan to the invalidated generation");
    std::filesystem::remove_all(folder);
}

void TestCancelSuppressesCompletion()
{
    const std::filesystem::path path = UniqueTempPath("_cancel.csv");
    WriteFixture(path);
    std::promise<void> entered_promise;
    std::shared_future<void> entered = entered_promise.get_future().share();
    std::atomic_bool entered_once = false;

    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&](const auto& source, std::size_t index, const auto& canceled) {
            if (!entered_once.exchange(true)) {
                entered_promise.set_value();
            }
            while (!canceled()) {
                std::this_thread::sleep_for(1ms);
            }
            return MakeSnapshot(source, index);
        }));
    const std::uint64_t task_id = queue.Enqueue({.path = path});
    Require(entered.wait_for(2s) == std::future_status::ready, "cancel test loader should start");
    queue.Cancel(task_id);
    Require(
        WaitUntil([&]() { return !queue.NeedsService(); }),
        "canceled worker should reach a terminal state");
    Require(queue.TakeCompleted().empty(), "canceled task should not publish a stale result");
    std::filesystem::remove(path);
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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
    specforge::SourceCollectionLoadQueue queue(Dependencies(
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
        specforge::SourceCollectionLoadQueue queue(Dependencies(
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
}

}  // namespace

int main()
{
    TestEnqueueReturnsBeforeLoaderCompletes();
    TestMatchingIdentitySkipsWorkflowCacheLoad();
    TestChangedContextFullPlanCarriesLiveWorkflowRevision();
    TestBatchLoadsWorkflowCachesOnce();
    TestSourcesUseIndependentThreads();
    TestIndividualLoadsPublishInRequestOrder();
    TestBatchPublishesInRequestOrder();
    TestBufferedBatchCompletionCanBeCanceled();
    TestFolderUsesOnePreparedListing();
    TestStableFolderListingGenerationSurvivesLoadWorkerExit();
    TestFolderListingGenerationInvalidatesSafelyAfterQueueDestruction();
    TestCanceledBlockedRegistrationStopsBeforeQueueDestruction();
    TestCurrentFolderListingGenerationAvoidsFullRescan();
    TestDirectoryChangeGenerationInvalidatesAfterMutation();
    TestUnavailableFolderGenerationKeepsFullRevalidationFallback();
    TestStaleFolderListingGenerationRefreshesBeforeDecode();
    TestChangedFileRetriesOneStableGeneration();
    TestChangedFolderRetriesOneStableGeneration();
    TestCancelSuppressesCompletion();
    TestCancelStopsOnlyItsSourceThread();
    TestFailureIsReported();
    TestDestructionStopsEverySourceThread();
    TestRetirementRunsOnWorker();
    return 0;
}
