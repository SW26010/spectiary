#pragma once

#include "ui/source_collection_preparation.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

namespace specforge {

class SourceCollectionPreparationCanceled : public std::runtime_error {
public:
    SourceCollectionPreparationCanceled();
};

class SourceCollectionPreparationStale : public std::runtime_error {
public:
    SourceCollectionPreparationStale();
};

struct SourceCollectionPreparationAdapters {
    // A queue may invoke these adapters concurrently from independent source
    // loads. Test adapters must therefore be safe for concurrent calls.
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
    using SourceOpenProbe = std::function<SourceOpenFilesystemProbe(
        const SourceOpenRequest&,
        const SourceCollectionCancellationCheckpoint&)>;
    using FolderChangeGenerationFactory =
        std::function<DirectoryChangeGenerationHandle(
            const std::filesystem::path&,
            const SourceCollectionCancellationCheckpoint&)>;
    using WorkflowCacheLoader =
        std::function<SampleWorkflowPreparationCacheBundle(
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
    SourceOpenProbe source_open_probe;
    FolderChangeGenerationFactory folder_change_generation_factory;
    // Lower-level lifecycle seam for the default monitor. Ignored when a
    // complete folder_change_generation_factory is injected.
    DirectoryChangeGenerationMonitor::RegistrationFactory
        folder_change_generation_registration_factory;
    WorkflowCacheLoader workflow_cache_loader;
    FileContextBuilder file_context_builder;
    FolderContextBuilder folder_context_builder;
    SampleWorkflowPreparationPaths workflow_cache_paths;
};

using SourceCollectionWorkflowCacheProvider =
    std::function<
        std::shared_ptr<const SampleWorkflowPreparationCacheBundle>(
            const SourceCollectionCancellationCheckpoint&)>;

class SourceCollectionPreparation {
public:
    SourceCollectionPreparation();
    explicit SourceCollectionPreparation(
        SourceCollectionPreparationAdapters adapters);
    ~SourceCollectionPreparation();

    SourceCollectionPreparation(
        SourceCollectionPreparation&&) noexcept;
    SourceCollectionPreparation& operator=(
        SourceCollectionPreparation&&) noexcept;
    SourceCollectionPreparation(
        const SourceCollectionPreparation&) = delete;
    SourceCollectionPreparation& operator=(
        const SourceCollectionPreparation&) = delete;

    [[nodiscard]] std::shared_ptr<
        const SampleWorkflowPreparationCacheBundle>
    LoadWorkflowCache(
        const SourceCollectionCancellationCheckpoint& checkpoint);
    [[nodiscard]] PreparedSourceCollection Prepare(
        std::uint64_t task_id,
        const SourceCollectionLoadRequest& request,
        const SourceCollectionCancellationCheckpoint& checkpoint,
        const SourceCollectionWorkflowCacheProvider&
            workflow_cache_provider = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
