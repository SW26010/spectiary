#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "profile/navigation_latency_trace.h"
#include "ui/background_retirement.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_folder_listing_generation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionLoadRequest {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
    std::optional<SourceCollectionIdentity> reuse_identity;
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof;
    std::optional<std::uint64_t> base_live_workflow_revision;
    // The last stable listing generation observed for this loaded source. It
    // remains an optimization hint; the worker revalidates its generation and
    // the source/annotation dependencies before publishing a snapshot.
    SourceCollectionFolderListingGenerationHandle folder_listing_generation_hint;
    NavigationLatencyAttemptHandle navigation_attempt;
};

struct PreparedSourceCollection {
    std::uint64_t task_id = 0;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    SpectrumSnapshotHandle snapshot;
    PreparedSourceCollectionPayload payload;
    // Published only after the post-decode dependency check succeeds. The
    // source roster retains it alongside the accepted snapshot/generation.
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof;
    // Immutable listing cache and invalidation boundary accepted by the
    // successful post-decode revalidation pass.
    SourceCollectionFolderListingGenerationHandle folder_listing_generation;
};

struct SourceCollectionLoadCompletion {
    std::uint64_t task_id = 0;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::optional<PreparedSourceCollection> prepared;
    std::string error_message;
    NavigationLatencyAttemptHandle navigation_attempt;
};

struct SourceCollectionLoadDependencies {
    using CancellationCheck = std::function<bool()>;
    using SnapshotLoader = std::function<SpectrumSnapshotHandle(
        const std::filesystem::path&,
        std::size_t,
        const CancellationCheck&)>;
    using FolderSnapshotLoader = std::function<SpectrumSnapshotHandle(
        const std::filesystem::path&,
        std::size_t,
        const SourceCollectionFolderListing&,
        const CancellationCheck&)>;
    using FolderScanner = std::function<SourceCollectionFolderListing(
        const std::filesystem::path&,
        const SourceCollectionCancellationCheckpoint&)>;
    using FolderChangeGenerationFactory = std::function<DirectoryChangeGenerationHandle(
        const std::filesystem::path&,
        const SourceCollectionCancellationCheckpoint&)>;
    using WorkflowCacheLoader = std::function<SampleWorkflowPreparationCacheBundle(
        const SampleWorkflowPreparationPaths&,
        const std::function<void()>&)>;
    using FileContextBuilder = std::function<SourceCollectionContext(
        const SpectrumSnapshot&,
        const SourceCollectionSingleFileState&,
        const SourceCollectionCancellationCheckpoint&)>;
    using FolderContextBuilder = std::function<SourceCollectionContext(
        const SpectrumSnapshot&,
        const SourceCollectionFolderListing&,
        const SourceCollectionCancellationCheckpoint&)>;

    SnapshotLoader snapshot_loader;
    FolderSnapshotLoader folder_snapshot_loader;
    FolderScanner folder_scanner;
    FolderChangeGenerationFactory folder_change_generation_factory;
    // Lower-level lifecycle seam for the default monitor. Ignored when a
    // complete folder_change_generation_factory is injected.
    DirectoryChangeGenerationMonitor::RegistrationFactory
        folder_change_generation_registration_factory;
    WorkflowCacheLoader workflow_cache_loader;
    // Test/measurement seams for the full manifest-building path.
    FileContextBuilder file_context_builder;
    FolderContextBuilder folder_context_builder;
    SampleWorkflowPreparationPaths workflow_cache_paths;
};

class SourceCollectionLoadQueue {
public:
    using CompletionReadyCallback = std::function<void()>;

    SourceCollectionLoadQueue();
    explicit SourceCollectionLoadQueue(SourceCollectionLoadDependencies dependencies);
    ~SourceCollectionLoadQueue();

    SourceCollectionLoadQueue(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue& operator=(SourceCollectionLoadQueue&&) noexcept;
    SourceCollectionLoadQueue(const SourceCollectionLoadQueue&) = delete;
    SourceCollectionLoadQueue& operator=(const SourceCollectionLoadQueue&) = delete;

    // Every request runs on its own jthread. Injected dependencies therefore
    // need to support concurrent calls from independent source loads.
    [[nodiscard]] std::uint64_t Enqueue(SourceCollectionLoadRequest request);
    [[nodiscard]] std::vector<std::uint64_t> EnqueueBatch(
        std::vector<SourceCollectionLoadRequest> requests);
    void Cancel(std::uint64_t task_id);
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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
