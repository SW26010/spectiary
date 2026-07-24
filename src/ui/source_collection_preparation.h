#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_folder_listing_generation.h"
#include "ui/source_collection_resident_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace specforge {

class SourceCollectionReuseCandidate {
public:
    [[nodiscard]] static SourceCollectionReuseCandidate Known(
        SourceCollectionIdentity identity,
        std::uint64_t live_workflow_revision,
        SourceCollectionFolderListingGenerationHandle
            folder_listing_generation = {});
    [[nodiscard]] static SourceCollectionReuseCandidate Verified(
        SourceCollectionContextReuseProof proof,
        std::uint64_t live_workflow_revision,
        SourceCollectionFolderListingGenerationHandle
            folder_listing_generation = {},
        std::optional<SourceCollectionResidentSnapshot>
            resident_snapshot = std::nullopt);

    [[nodiscard]] const SourceCollectionIdentity& identity() const noexcept;
    [[nodiscard]] std::uint64_t live_workflow_revision() const noexcept;
    [[nodiscard]] const std::optional<SourceCollectionContextReuseProof>&
    context_reuse_proof() const noexcept;
    [[nodiscard]] const SourceCollectionFolderListingGenerationHandle&
    folder_listing_generation() const noexcept;
    [[nodiscard]] const std::optional<SourceCollectionResidentSnapshot>&
    resident_snapshot() const noexcept;

private:
    SourceCollectionReuseCandidate(
        SourceCollectionIdentity identity,
        std::uint64_t live_workflow_revision,
        SourceCollectionFolderListingGenerationHandle
            folder_listing_generation,
        std::optional<SourceCollectionContextReuseProof>
            context_reuse_proof,
        std::optional<SourceCollectionResidentSnapshot>
            resident_snapshot);

    SourceCollectionIdentity identity_;
    std::uint64_t live_workflow_revision_ = 0;
    SourceCollectionFolderListingGenerationHandle
        folder_listing_generation_;
    std::optional<SourceCollectionContextReuseProof>
        context_reuse_proof_;
    std::optional<SourceCollectionResidentSnapshot>
        resident_snapshot_;
};

struct SourceCollectionLoadRequest {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
    std::optional<SourceCollectionReuseCandidate> reuse;
    // Prefetch requests may only publish a snapshot under an already-proven
    // context. They never materialize or publish workflow state.
    bool snapshot_only = false;
    NavigationLatencyAttemptHandle latency_attempt;
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
    // True only when preparation reused a resident snapshot and did not
    // invoke either snapshot decoder.
    bool snapshot_cache_hit = false;
    std::optional<SourceCollectionResidentSnapshotOrigin>
        snapshot_cache_origin;
    std::uint64_t snapshot_prefetch_id = 0;
    std::uint64_t snapshot_prefetch_task_id = 0;
    std::int64_t snapshot_prefetch_scheduled_ns = 0;
    SampleNavigationDirection snapshot_prefetch_direction =
        SampleNavigationDirection::Next;
};

}  // namespace specforge
