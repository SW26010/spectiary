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

void TestStableKnownFileSkipsFullContextBuilder()
{
    const std::filesystem::path path = UniqueTempPath("_stable_reuse.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path, 2);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*snapshot);
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; });
    dependencies.file_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::LoadSourceCollectionContextCancelable(
                decoded,
                state,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = path,
        .spectrum_index = 2,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "stable known file should prepare");
    Require(
        context_builds.load() == 0,
        "stable known file navigation must not call the full context builder");
    Require(
        std::holds_alternative<specforge::PreparedSourceCollectionReuse>(
            completions.front().prepared->payload),
        "stable known file should reuse its existing workflow");
    Require(
        completions.front().prepared->context_reuse_proof.has_value(),
        "stable known file should republish its verified reuse proof");
    std::filesystem::remove(path);
}

void TestStableKnownFolderSkipsFullContextBuilder()
{
    const std::filesystem::path folder = UniqueTempPath("_stable_reuse_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const auto generation = std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation =
        MakeFolderListingGeneration(folder, generation);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(folder, 0, listing_generation->listing.spectra.size());
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFolderReuseProof(*snapshot, listing_generation->listing);
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_snapshot_loader =
        [snapshot](const auto&, std::size_t, const auto&, const auto&) {
            return snapshot;
        };
    dependencies.folder_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& listing,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::BuildFolderSourceCollectionContextCancelable(
                decoded,
                listing,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint = listing_generation,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "stable known folder should prepare");
    Require(
        context_builds.load() == 0,
        "stable known folder navigation must not call the full context builder");
    Require(
        std::holds_alternative<specforge::PreparedSourceCollectionReuse>(
            completions.front().prepared->payload),
        "stable known folder should reuse its existing workflow");
    std::filesystem::remove_all(folder);
}

void TestVerifiedResidentFileSkipsDecoder()
{
    const std::filesystem::path path =
        UniqueTempPath("_resident_hit.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle resident_snapshot =
        MakeSnapshot(path, 1);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*resident_snapshot);
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&decoder_calls](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            ++decoder_calls;
            return MakeSnapshot(source, index);
        }));

    (void)queue.Enqueue({
        .path = path,
        .spectrum_index = 1,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .resident_snapshot =
            specforge::SourceCollectionResidentSnapshot{
                1,
                resident_snapshot,
                proof,
                {},
            },
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(
        completions.front().prepared.has_value(),
        "verified resident file should prepare");
    Require(
        completions.front().prepared->snapshot_cache_hit,
        "verified resident file should report a true cache hit");
    Require(
        completions.front().prepared->snapshot == resident_snapshot,
        "resident hit should publish the already-decoded snapshot");
    Require(
        decoder_calls.load() == 0,
        "resident hit must not invoke the file decoder");
    std::filesystem::remove(path);
}

void TestBufferedResidentHitCanBeCanceled()
{
    const std::filesystem::path blocking_path =
        UniqueTempPath("_resident_cancel_blocking.csv");
    const std::filesystem::path resident_path =
        UniqueTempPath("_resident_cancel_hit.csv");
    WriteFixture(blocking_path);
    WriteFixture(resident_path);
    const specforge::SpectrumSnapshotHandle resident_snapshot =
        MakeSnapshot(resident_path, 1);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*resident_snapshot);
    std::promise<void> blocking_decoder_entered_promise;
    std::future<void> blocking_decoder_entered =
        blocking_decoder_entered_promise.get_future();
    std::promise<void> release_blocking_promise;
    std::shared_future<void> release_blocking =
        release_blocking_promise.get_future().share();
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&](const auto& source, std::size_t index, const auto& canceled) {
            ++decoder_calls;
            Require(
                source == blocking_path,
                "resident cancellation must not decode the hit");
            blocking_decoder_entered_promise.set_value();
            WaitForRelease(
                release_blocking,
                canceled,
                "timed out waiting to release the resident-cancel blocker");
            return MakeSnapshot(source, index);
        }));

    const std::vector<std::uint64_t> ids =
        queue.EnqueueBatch({
            {.path = blocking_path},
            {
                .path = resident_path,
                .spectrum_index = 1,
                .reuse_identity = proof.identity,
                .context_reuse_proof = proof,
                .resident_snapshot =
                    specforge::SourceCollectionResidentSnapshot{
                        1,
                        resident_snapshot,
                        proof,
                        {},
                    },
            },
        });
    Require(
        blocking_decoder_entered.wait_for(2s) ==
            std::future_status::ready,
        "resident cancellation blocker should start");
    std::this_thread::sleep_for(100ms);
    queue.Cancel(ids[1]);
    release_blocking_promise.set_value();

    const auto completions = WaitForCompletions(queue, 1);
    Require(
        completions.size() == 1 &&
            completions.front().task_id == ids[0],
        "a canceled resident hit must not publish a stale completion");
    Require(
        decoder_calls.load() == 1,
        "resident hit cancellation must still skip its decoder");
    Require(
        !queue.NeedsService(),
        "canceled resident hit should leave no activatable completion");
    std::filesystem::remove(blocking_path);
    std::filesystem::remove(resident_path);
}

void TestChangedResidentFileFallsBackToDecoder()
{
    const std::filesystem::path path =
        UniqueTempPath("_resident_changed.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle resident_snapshot =
        MakeSnapshot(path, 1);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*resident_snapshot);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "changed";
    }
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionLoadQueue queue(Dependencies(
        [&decoder_calls](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            ++decoder_calls;
            return MakeSnapshot(source, index);
        }));

    (void)queue.Enqueue({
        .path = path,
        .spectrum_index = 1,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .resident_snapshot =
            specforge::SourceCollectionResidentSnapshot{
                1,
                resident_snapshot,
                proof,
                {},
            },
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(
        completions.front().prepared.has_value(),
        "changed resident file should fall back and prepare");
    Require(
        !completions.front().prepared->snapshot_cache_hit,
        "changed file state must be a cache miss");
    Require(
        decoder_calls.load() == 1,
        "changed file state must invoke the decoder");
    std::filesystem::remove(path);
}

void TestFolderGenerationControlsResidentDecodeReuse()
{
    const std::filesystem::path folder =
        UniqueTempPath("_resident_folder");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const auto generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle
        listing_generation =
            MakeFolderListingGeneration(folder, generation);
    const specforge::SpectrumSnapshotHandle resident_snapshot =
        MakeSnapshot(
            folder,
            0,
            listing_generation->listing.spectra.size());
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFolderReuseProof(
            *resident_snapshot,
            listing_generation->listing);
    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies =
        Dependencies(
            [](const auto&, std::size_t, const auto&)
                -> specforge::SpectrumSnapshotHandle {
                throw std::runtime_error(
                    "folder task must not use the file loader");
            });
    dependencies.folder_snapshot_loader =
        [&decoder_calls](
            const std::filesystem::path& source,
            std::size_t index,
            const specforge::SourceCollectionFolderListing& listing,
            const auto&) {
            ++decoder_calls;
            return MakeSnapshot(
                source,
                index,
                listing.spectra.size());
        };
    specforge::SourceCollectionLoadQueue queue(
        std::move(dependencies));
    const specforge::SourceCollectionResidentSnapshot resident{
        0,
        resident_snapshot,
        proof,
        listing_generation,
    };

    (void)queue.Enqueue({
        .path = folder,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint = listing_generation,
        .resident_snapshot = resident,
    });
    auto hit_completions = WaitForCompletions(queue, 1);
    Require(
        hit_completions.front().prepared &&
            hit_completions.front()
                .prepared->snapshot_cache_hit,
        "current folder generation should reuse the resident snapshot");
    Require(
        decoder_calls.load() == 0,
        "current folder generation should skip folder decode");

    generation->Invalidate();
    (void)queue.Enqueue({
        .path = folder,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint = listing_generation,
        .resident_snapshot = resident,
    });
    auto miss_completions = WaitForCompletions(queue, 1);
    Require(
        miss_completions.front().prepared &&
            !miss_completions.front()
                 .prepared->snapshot_cache_hit,
        "changed folder generation must be a cache miss");
    Require(
        decoder_calls.load() == 1,
        "changed folder generation must invoke folder decode");
    std::filesystem::remove_all(folder);
}

void TestKnownFileSourceChangeMaterializesContext()
{
    const std::filesystem::path path = UniqueTempPath("_changed_reuse_source.csv");
    WriteFixture(path);
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*snapshot);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "changed";
    }
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; });
    dependencies.file_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::LoadSourceCollectionContextCancelable(
                decoded,
                state,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = path,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed source should prepare");
    Require(
        context_builds.load() == 1,
        "changed source state must fall back to the full context builder");
    std::filesystem::remove(path);
}

void TestKnownFileCompanionChangeMaterializesContext()
{
    const std::filesystem::path path = UniqueTempPath("_changed_reuse_companion.npy");
    WriteFixture(path);
    const std::optional<std::filesystem::path> companion =
        specforge::SourceCollectionCompanionNamePath(path);
    Require(companion.has_value(), "NPY fixture should have a companion-name path");
    WriteFixture(*companion);
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*snapshot);
    {
        std::ofstream stream(*companion, std::ios::binary | std::ios::app);
        stream << "changed";
    }
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; });
    dependencies.file_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::LoadSourceCollectionContextCancelable(
                decoded,
                state,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = path,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed companion should prepare");
    Require(
        context_builds.load() == 1,
        "changed companion state must fall back to the full context builder");
    std::filesystem::remove(*companion);
    std::filesystem::remove(path);
}

void TestKnownFileAnnotationChangeMaterializesContext()
{
    const std::filesystem::path path = UniqueTempPath("_changed_reuse_annotation.csv");
    const std::filesystem::path annotation =
        UniqueTempPath("_changed_reuse_annotation.npy");
    WriteFixture(path);
    WriteFixture(annotation);
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path);
    const std::vector<std::filesystem::path> annotation_paths = {annotation};
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*snapshot, annotation_paths);
    {
        std::ofstream stream(annotation, std::ios::binary | std::ios::app);
        stream << "changed";
    }
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [snapshot](const auto&, std::size_t, const auto&) { return snapshot; });
    dependencies.file_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::LoadSourceCollectionContextCancelable(
                decoded,
                state,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = path,
        .annotation_paths = annotation_paths,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed annotation should prepare");
    Require(
        context_builds.load() == 1,
        "changed annotation dependency must fall back to the full context builder");
    std::filesystem::remove(annotation);
    std::filesystem::remove(path);
}

void TestKnownFolderGenerationChangeMaterializesContext()
{
    const std::filesystem::path folder =
        UniqueTempPath("_changed_reuse_generation");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample.csv");
    const auto generation = std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle listing_generation =
        MakeFolderListingGeneration(folder, generation);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(folder, 0, listing_generation->listing.spectra.size());
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFolderReuseProof(*snapshot, listing_generation->listing);
    generation->Invalidate();
    std::atomic_int context_builds = 0;
    specforge::SourceCollectionLoadDependencies dependencies = Dependencies(
        [](const auto&, std::size_t, const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("folder task must not use the file loader");
        });
    dependencies.folder_change_generation_factory =
        [](const auto&, const auto&) {
            return std::make_shared<MutableDirectoryChangeGeneration>();
        };
    dependencies.folder_snapshot_loader =
        [snapshot](const auto&, std::size_t, const auto&, const auto&) {
            return snapshot;
        };
    dependencies.folder_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& listing,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::BuildFolderSourceCollectionContextCancelable(
                decoded,
                listing,
                checkpoint);
        };

    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint = listing_generation,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed generation should prepare");
    Require(
        context_builds.load() == 1,
        "invalidated folder generation must fall back to the full context builder");
    std::filesystem::remove_all(folder);
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

    specforge::SourceCollectionLoadQueue queue(
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
            .reuse_identity = proof.identity,
            .context_reuse_proof = proof,
        });
    Require(
        prefetch_entered.wait_for(2s) ==
            std::future_status::ready,
        "prefetch worker should start");
    const std::uint64_t overlapping_prefetch_id =
        queue.EnqueuePrefetch({
            .path = prefetch_path,
            .spectrum_index = 1,
            .reuse_identity = proof.identity,
            .context_reuse_proof = proof,
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
                    NavigationLatencyTimePoint{},
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
            .reuse_identity = proof.identity,
            .context_reuse_proof = proof,
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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
            .latency_attempt = navigation_attempt,
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

void TestPrefetchRequiresCurrentFolderGenerationWithoutScanning()
{
    const std::filesystem::path folder =
        UniqueTempPath("_prefetch_folder_listing_hint");
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "sample-a.csv");
    WriteFixture(folder / "sample-b.csv");
    const auto change_generation =
        std::make_shared<MutableDirectoryChangeGeneration>();
    const specforge::SourceCollectionFolderListingGenerationHandle
        listing_generation_hint =
            MakeFolderListingGeneration(folder, change_generation);
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(
            folder,
            1,
            listing_generation_hint->listing.spectra.size());
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFolderReuseProof(
            *snapshot,
            listing_generation_hint->listing);
    std::atomic_int folder_scan_calls = 0;
    std::atomic_int folder_loader_calls = 0;
    specforge::SourceCollectionLoadDependencies dependencies =
        Dependencies(
            [](const auto&, std::size_t, const auto&)
                -> specforge::SpectrumSnapshotHandle {
                throw std::runtime_error(
                    "folder prefetch must not use the file loader");
            });
    dependencies.folder_scanner =
        [&](const auto& path, const auto& checkpoint) {
            ++folder_scan_calls;
            return specforge::ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    dependencies.folder_snapshot_loader =
        [&](const auto& path,
            std::size_t index,
            const auto& listing,
            const auto&) {
            ++folder_loader_calls;
            return MakeSnapshot(
                path,
                index,
                listing.spectra.size());
        };
    specforge::SourceCollectionLoadQueue queue(
        std::move(dependencies));

    (void)queue.EnqueuePrefetch({
        .path = folder,
        .spectrum_index = 1,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint =
            listing_generation_hint,
    });
    auto current = WaitForCompletions(queue, 1);
    Require(
        current.front().prepared.has_value() &&
            std::holds_alternative<
                specforge::PreparedSourceCollectionReuse>(
                current.front().prepared->payload),
        "a current folder generation should allow snapshot-only context reuse");
    Require(
        folder_scan_calls.load() == 0 &&
            folder_loader_calls.load() == 1,
        "a current folder prefetch should decode without a listing scan");

    change_generation->Invalidate();
    (void)queue.EnqueuePrefetch({
        .path = folder,
        .spectrum_index = 1,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint =
            listing_generation_hint,
    });
    auto stale = WaitForCompletions(queue, 1);
    Require(
        !stale.front().prepared &&
            stale.front().stale,
        "an invalidated folder generation should make prefetch stale");
    Require(
        folder_scan_calls.load() == 0 &&
            folder_loader_calls.load() == 1,
        "stale prefetch should not scan or decode a changed folder");
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
        .latency_attempt = navigation_attempt,
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
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(path, 1);
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFileReuseProof(*initial_snapshot);
    std::atomic_int loader_calls = 0;
    std::atomic_int context_builds = 0;
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
        [&](const auto& source, std::size_t index, const auto&) {
            if (++loader_calls == 1) {
                std::ofstream stream(source, std::ios::binary | std::ios::app);
                stream << "changed";
            }
            return MakeSnapshot(source, index);
        });
    dependencies.file_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& state,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::LoadSourceCollectionContextCancelable(
                decoded,
                state,
                checkpoint);
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = path,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .latency_attempt = navigation_attempt,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed file should settle on a stable retry");
    Require(loader_calls.load() == 2, "changed file should be decoded exactly one additional time");
    Require(
        context_builds.load() == 1,
        "a failed reuse-proof round should materialize only the stable retry");
    const specforge::NavigationLatencyAttemptReport report = navigation_attempt->Report();
    Require(
        report.preparation_rounds.size() == 2,
        "each TOCTOU preparation retry must retain its own timing round");
    Require(
        !report.preparation_rounds[0].revalidation_succeeded &&
            report.preparation_rounds[1].revalidation_succeeded,
        "the trace should distinguish the rejected generation from the accepted retry");
    Require(
        report.preparation_rounds[0].context_reused &&
            !report.preparation_rounds[1].context_reused,
        "TOCTOU retry should distinguish the rejected proof from the materialized retry");
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
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        MakeSnapshot(
            folder,
            0,
            listing_generation_hint->listing.spectra.size());
    const specforge::SourceCollectionContextReuseProof proof =
        MakeFolderReuseProof(
            *initial_snapshot,
            listing_generation_hint->listing);
    std::atomic_int loader_calls = 0;
    std::atomic_int folder_scan_calls = 0;
    std::atomic_int context_builds = 0;
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
    dependencies.folder_context_builder =
        [&context_builds](
            const auto& decoded,
            const auto& listing,
            const auto& checkpoint) {
            ++context_builds;
            return specforge::BuildFolderSourceCollectionContextCancelable(
                decoded,
                listing,
                checkpoint);
        };
    specforge::SourceCollectionLoadQueue queue(std::move(dependencies));
    (void)queue.Enqueue({
        .path = folder,
        .reuse_identity = proof.identity,
        .context_reuse_proof = proof,
        .folder_listing_generation_hint = listing_generation_hint,
        .latency_attempt = navigation_attempt,
    });
    auto completions = WaitForCompletions(queue, 1);
    Require(completions.front().prepared.has_value(), "changed folder should settle on a stable retry");
    Require(loader_calls.load() == 2, "changed folder should be decoded exactly one additional time");
    Require(
        context_builds.load() == 1,
        "an invalidated proof should materialize only the replacement generation");
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
        report.preparation_rounds[0].context_reused &&
            !report.preparation_rounds[1].context_reused,
        "folder retry should distinguish the rejected proof from the materialized generation");
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
    std::atomic_int notifications = 0;

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
    queue.RegisterCompletionReadyCallback([&notifications]() {
        notifications.fetch_add(1, std::memory_order_relaxed);
    });
    const std::uint64_t task_id = queue.Enqueue({.path = path});
    Require(entered.wait_for(2s) == std::future_status::ready, "cancel test loader should start");
    queue.Cancel(task_id);
    Require(
        WaitUntil([&]() { return !queue.NeedsService(); }),
        "canceled worker should reach a terminal state");
    Require(queue.TakeCompleted().empty(), "canceled task should not publish a stale result");
    Require(
        notifications.load(std::memory_order_relaxed) == 0,
        "a canceled task with no activatable completion must not notify");
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

    specforge::SourceCollectionLoadQueue queue(Dependencies(
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
    TestStableKnownFileSkipsFullContextBuilder();
    TestStableKnownFolderSkipsFullContextBuilder();
    TestVerifiedResidentFileSkipsDecoder();
    TestBufferedResidentHitCanBeCanceled();
    TestChangedResidentFileFallsBackToDecoder();
    TestFolderGenerationControlsResidentDecodeReuse();
    TestKnownFileSourceChangeMaterializesContext();
    TestKnownFileCompanionChangeMaterializesContext();
    TestKnownFileAnnotationChangeMaterializesContext();
    TestKnownFolderGenerationChangeMaterializesContext();
    TestChangedContextFullPlanCarriesLiveWorkflowRevision();
    TestBatchLoadsWorkflowCachesOnce();
    TestSourcesUseIndependentThreads();
    TestIndividualLoadsPublishInRequestOrder();
    TestPrefetchNeverBlocksForegroundPublication();
    TestBatchPublishesInRequestOrder();
    TestCompletionReadyNotificationCoalescesUntilDrain();
    TestBufferedBatchCompletionCanBeCanceled();
    TestFolderUsesOnePreparedListing();
    TestStableFolderListingGenerationSurvivesLoadWorkerExit();
    TestFolderListingGenerationInvalidatesSafelyAfterQueueDestruction();
    TestCanceledBlockedRegistrationStopsBeforeQueueDestruction();
    TestCurrentFolderListingGenerationAvoidsFullRescan();
    TestPrefetchRequiresCurrentFolderGenerationWithoutScanning();
    TestDirectoryChangeGenerationInvalidatesAfterMutation();
    TestUnavailableFolderGenerationKeepsFullRevalidationFallback();
    TestStaleFolderListingGenerationRefreshesBeforeDecode();
    TestChangedFileRetriesOneStableGeneration();
    TestChangedFolderRetriesOneStableGeneration();
    TestCancelSuppressesCompletion();
    TestCompletionReadyCallbackIsReentrantAndUnregistersSafely();
    TestCancelStopsOnlyItsSourceThread();
    TestFailureIsReported();
    TestDestructionStopsEverySourceThread();
    TestRetirementRunsOnWorker();
    return 0;
}
