#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_folder_listing_generation.h"
#include "ui/source_collection_resident_snapshot.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionRosterRemoveResult {
    SourceCollectionSessionAction action;
    bool removed = false;
    bool removed_current = false;
    std::filesystem::path removed_path;
    std::string removed_source_key;
    std::vector<SpectrumSnapshotHandle> retired_snapshots;
    SourceCollectionFolderListingGenerationHandle retired_folder_listing_generation;
};

struct SourceCollectionRosterOpenResult {
    SourceCollectionSessionAction action;
    std::vector<SpectrumSnapshotHandle> retired_snapshots;
    SourceCollectionFolderListingGenerationHandle replaced_folder_listing_generation;
};

struct SourceCollectionRosterResidentRetainResult {
    bool retained = false;
    std::vector<SpectrumSnapshotHandle> retired_snapshots;
};

class SourceCollectionRoster {
public:
    SourceCollectionRoster();

    [[nodiscard]] const SpectrumSnapshotHandle& snapshot() const;
    [[nodiscard]] std::optional<std::size_t> current_source_index() const;
    [[nodiscard]] bool has_source(std::size_t source_index) const;
    [[nodiscard]] std::optional<std::string> current_source_key() const;
    [[nodiscard]] bool has_active_source() const;
    [[nodiscard]] std::vector<SourceCollectionSourceView> SourceViews() const;
    [[nodiscard]] std::vector<SourceCollectionSavedSource> SavedSources() const;
    [[nodiscard]] std::vector<std::string> SavedSourceKeys() const;
    [[nodiscard]] SourceCollectionFolderListingGenerationHandle FolderListingGeneration(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<SourceCollectionContextReuseProof>
        ContextReuseProof(const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<SourceCollectionResidentSnapshot>
        ResidentSnapshot(
            const std::filesystem::path& path,
            std::size_t spectrum_index,
            const SourceCollectionIdentity& identity);
    [[nodiscard]] SourceCollectionRosterResidentRetainResult
        RetainPrefetchedSnapshot(
            const std::filesystem::path& path,
            SourceCollectionResidentSnapshot resident);

    [[nodiscard]] SourceCollectionRosterOpenResult OpenPreparedSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        SpectrumSnapshotHandle snapshot,
        SourceCollectionFolderListingGenerationHandle folder_listing_generation = {},
        std::optional<SourceCollectionContextReuseProof> context_reuse_proof =
            std::nullopt);
    [[nodiscard]] SourceCollectionSessionAction ActivateSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionRosterRemoveResult RemoveSource(std::size_t source_index);
    void RememberActiveSourceIndex(std::size_t spectrum_index);

private:
    struct AddOrUpdateSourceResult {
        std::size_t source_index = 0;
        std::vector<SpectrumSnapshotHandle> retired_snapshots;
        SourceCollectionFolderListingGenerationHandle replaced_folder_listing_generation;
    };

    struct ResidentSnapshotEntry {
        std::size_t spectrum_index = 0;
        SpectrumSnapshotHandle snapshot;
        SourceCollectionContextReuseProof context_reuse_proof;
        SourceCollectionFolderListingGenerationHandle folder_listing_generation;
        SourceCollectionResidentSnapshotOrigin origin =
            SourceCollectionResidentSnapshotOrigin::History;
        std::uint64_t prefetch_id = 0;
        std::uint64_t prefetch_task_id = 0;
        std::int64_t prefetch_scheduled_ns = 0;
        SampleNavigationDirection prefetch_direction =
            SampleNavigationDirection::Next;
        std::size_t estimated_payload_bytes = 0;
        std::uint64_t access_epoch = 0;
    };

    struct SourceListEntry {
        std::filesystem::path path;
        std::string key;
        std::string display_name;
        std::optional<std::string> type;
        SourceCollectionSourceState state =
            SourceCollectionSourceState::Unavailable;
        // Stores the last domain snapshot for this source so reactivation can use
        // an explicit cache instead of reloading. Do not remove as a summary-only
        // optimization without retesting CSV/folder error snapshots: that change
        // reproduced 0xc0000005 shared_ptr refcount crashes.
        SpectrumSnapshotHandle cached_snapshot;
        // Retained with the source entry so the next row load can reuse the
        // listing while its invalidation boundary remains current.
        SourceCollectionFolderListingGenerationHandle folder_listing_generation;
        // Paired with the snapshot and folder generation accepted by the same
        // prepared-open transaction.
        std::optional<SourceCollectionContextReuseProof> context_reuse_proof;
        std::size_t last_spectrum_index = 0;
        std::vector<ResidentSnapshotEntry> resident_snapshots;
    };

    [[nodiscard]] const SourceListEntry* current_source() const;
    [[nodiscard]] AddOrUpdateSourceResult AddOrUpdateSource(
        const std::filesystem::path& path,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index,
        SourceCollectionFolderListingGenerationHandle folder_listing_generation = {},
        std::optional<SourceCollectionContextReuseProof> context_reuse_proof =
            std::nullopt);
    void RetainPreviousSnapshot(
        SourceListEntry& source,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index,
        const std::optional<SourceCollectionContextReuseProof>& context_reuse_proof,
        const SourceCollectionFolderListingGenerationHandle& folder_listing_generation,
        SourceCollectionResidentSnapshotOrigin origin,
        std::uint64_t prefetch_id,
        std::uint64_t prefetch_task_id,
        std::int64_t prefetch_scheduled_ns,
        SampleNavigationDirection prefetch_direction,
        std::vector<SpectrumSnapshotHandle>& retired_snapshots);
    void InvalidateResidentSnapshots(
        SourceListEntry& source,
        std::vector<SpectrumSnapshotHandle>& retired_snapshots);
    void EvictResidentSnapshots(std::vector<SpectrumSnapshotHandle>& retired_snapshots);
    void SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action);

    SpectrumSnapshotHandle snapshot_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
    std::size_t resident_snapshot_count_ = 0;
    std::size_t resident_snapshot_payload_bytes_ = 0;
    std::uint64_t resident_access_epoch_ = 0;
};

}  // namespace specforge
