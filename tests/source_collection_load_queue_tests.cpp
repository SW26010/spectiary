#include "domain/source_collection_manifest.h"
#include "ui/source_collection_load_queue.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        [&](const std::filesystem::path& source, std::size_t index, const auto&) {
            if (!entered_once.exchange(true)) {
                entered_promise.set_value();
            }
            release.wait();
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

void TestBatchLoadsWorkflowCachesOnce()
{
    const std::filesystem::path first = UniqueTempPath("_batch_a.csv");
    const std::filesystem::path second = UniqueTempPath("_batch_b.csv");
    WriteFixture(first);
    WriteFixture(second);
    std::atomic_int cache_loads = 0;
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [](const auto& path, std::size_t index, const auto&) {
            return MakeSnapshot(path, index);
        },
        &cache_loads));

    const auto ids = queue.EnqueueBatch({
        {.path = first},
        {.path = second},
    });
    Require(ids.size() == 2, "batch enqueue should return every task id");
    auto completions = WaitForCompletions(queue, 2);
    Require(
        completions[0].prepared && completions[1].prepared,
        "every batch source should produce a prepared result");
    Require(cache_loads.load() == 1, "one restore batch should share one immutable cache snapshot");
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void TestInteractiveLoadRunsBeforeRemainingRestoreBatch()
{
    const std::filesystem::path active = UniqueTempPath("_priority_active.csv");
    const std::filesystem::path restore_a = UniqueTempPath("_priority_restore_a.csv");
    const std::filesystem::path restore_b = UniqueTempPath("_priority_restore_b.csv");
    const std::filesystem::path interactive = UniqueTempPath("_priority_interactive.csv");
    for (const auto& path : {active, restore_a, restore_b, interactive}) {
        WriteFixture(path);
    }

    std::promise<void> active_entered_promise;
    std::shared_future<void> active_entered = active_entered_promise.get_future().share();
    std::promise<void> release_active_promise;
    std::shared_future<void> release_active = release_active_promise.get_future().share();
    std::atomic_bool active_entered_once = false;
    std::mutex order_mutex;
    std::vector<std::filesystem::path> order;

    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&](const auto& path, std::size_t index, const auto&) {
            if (path == active && !active_entered_once.exchange(true)) {
                active_entered_promise.set_value();
                release_active.wait();
            }
            {
                std::lock_guard lock(order_mutex);
                order.push_back(path);
            }
            return MakeSnapshot(path, index);
        }));

    (void)queue.Enqueue({.path = active});
    Require(
        active_entered.wait_for(2s) == std::future_status::ready,
        "priority test should start its active task");
    (void)queue.EnqueueBatch({
        {.path = restore_a},
        {.path = restore_b},
    });
    (void)queue.Enqueue({.path = interactive});
    release_active_promise.set_value();

    (void)WaitForCompletions(queue, 4);
    {
        std::lock_guard lock(order_mutex);
        Require(order.size() == 4, "priority test should decode every task");
        Require(order[0] == active, "the already-running task should finish first");
        Require(order[1] == interactive, "interactive work should precede queued restore work");
        Require(order[2] == restore_a && order[3] == restore_b, "restore order should remain stable");
    }

    for (const auto& path : {active, restore_a, restore_b, interactive}) {
        std::filesystem::remove(path);
    }
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

void TestChangedFileRetriesOneStableGeneration()
{
    const std::filesystem::path path = UniqueTempPath("_retry.csv");
    WriteFixture(path);
    std::atomic_int loader_calls = 0;
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&](const auto& source, std::size_t index, const auto&) {
            if (++loader_calls == 1) {
                std::ofstream stream(source, std::ios::binary | std::ios::app);
                stream << "changed";
            }
            return MakeSnapshot(source, index);
        }));
    (void)queue.Enqueue({.path = path});
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed file should settle on a stable retry");
    Require(loader_calls.load() == 2, "changed file should be decoded exactly one additional time");
    std::filesystem::remove(path);
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
    TestBatchLoadsWorkflowCachesOnce();
    TestInteractiveLoadRunsBeforeRemainingRestoreBatch();
    TestFolderUsesOnePreparedListing();
    TestChangedFileRetriesOneStableGeneration();
    TestCancelSuppressesCompletion();
    TestFailureIsReported();
    TestRetirementRunsOnWorker();
    return 0;
}
