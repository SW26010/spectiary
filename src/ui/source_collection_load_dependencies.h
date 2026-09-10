#pragma once

#include "ui/source_collection_preparation.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>

namespace specforge {

class SourceCollectionPreparationCanceled : public std::runtime_error {
public:
    SourceCollectionPreparationCanceled();
};

class SourceCollectionPreparationStale : public std::runtime_error {
public:
    SourceCollectionPreparationStale();
};

struct SourceCollectionLoadDependencies {
    // Only decoding and OS operations vary here. Scanning, context construction,
    // cache loading and workflow preparation belong to the implementation.
    // Dependencies may be called concurrently by independent source loads.
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
    using SourceOpenProbe = std::function<SourceOpenFilesystemProbe(
        const SourceOpenRequest&,
        const SourceCollectionCancellationCheckpoint&)>;

    // Existing format-decoding boundaries; also support controlled slow/error
    // decoders to verify queue scheduling and publication. Not a plugin ABI (#40).
    SnapshotLoader snapshot_loader;
    FolderSnapshotLoader folder_snapshot_loader;
    // Filesystem metadata can stall before decoding. Retain this narrow OS
    // boundary to verify external-open work stays off the UI thread and cancels.
    SourceOpenProbe source_open_probe;
    // OS registration boundary: deterministic cancellation, timeout and
    // late-result lifetime tests (ADR 0008).
    DirectoryChangeGenerationMonitor::RegistrationFactory
        folder_change_generation_registration_factory;
    SampleWorkflowPreparationPaths workflow_cache_paths;
};

}  // namespace specforge
