#pragma once

#include "domain/source_collection_manifest.h"
#include "domain/source_open_resolution.h"
#include "domain/spectrum_snapshot.h"
#include "profile/load_latency_trace_lifecycle.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_folder_listing_generation.h"
#include "ui/source_collection_resident_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace spectiary {

class SourceLoadLatencyTrace;

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

struct SourceCollectionAnnotationRestore {
    std::optional<std::string> source_identity;
};

struct SourceCollectionLoadRequest {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
    // Presence marks persisted attachments, including legacy caches without
    // an identity. Explicit attachment requests do not use this contract.
    std::optional<SourceCollectionAnnotationRestore> annotation_restore;
    // External, in-app, and automation opens carry their logical request here
    // so filesystem probing stays inside the cancellable load worker. Restore,
    // follow-up, and prefetch requests already carry a resolved source path.
    std::optional<SourceOpenRequest> source_open_request;
    // Retain the requested member until its listing is validated. Together
    // with source_open_request this is an existing-collection reuse candidate;
    // a stale candidate falls back to that original request. External startup
    // folder interpretation also sets this internally after resolution.
    std::optional<std::filesystem::path> preferred_member_path;
    // Set internally after validating an existing collection's member mapping.
    bool explicit_member_reuse = false;
    std::optional<SourceCollectionReuseCandidate> reuse;
    // Prefetch requests may only publish a snapshot under an already-proven
    // context. They never materialize or publish workflow state.
    bool snapshot_only = false;
    LoadLatencyAttemptHandle latency_attempt;
    std::shared_ptr<SourceLoadLatencyTrace> source_load_trace;
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
    // Retained through completion admission for the session's final membership
    // check. A worker-validated listing may be invalidated before UI commit.
    std::optional<std::filesystem::path> explicit_member_path;
};

}  // namespace spectiary
