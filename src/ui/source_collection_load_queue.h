#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/background_retirement.h"
#include "ui/sample_workflow_preparation.h"

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
};

struct PreparedSourceCollection {
    std::uint64_t task_id = 0;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    SpectrumSnapshotHandle snapshot;
    PreparedSourceCollectionPayload payload;
};

struct SourceCollectionLoadCompletion {
    std::uint64_t task_id = 0;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::optional<PreparedSourceCollection> prepared;
    std::string error_message;
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
    using WorkflowCacheLoader = std::function<SampleWorkflowPreparationCacheBundle(
        const SampleWorkflowPreparationPaths&,
        const std::function<void()>&)>;

    SnapshotLoader snapshot_loader;
    FolderSnapshotLoader folder_snapshot_loader;
    WorkflowCacheLoader workflow_cache_loader;
    SampleWorkflowPreparationPaths workflow_cache_paths;
};

class SourceCollectionLoadQueue {
public:
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

    // Large immutable graphs replaced or rejected by the UI are released by
    // a dedicated background reclaimer, never by the UI caller.
    void RetirePrepared(PreparedSourceCollection prepared);
    void RetireResource(BackgroundRetirementHandle resource);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
