#pragma once

#include "app/runtime_paths.h"

#include "domain/sample_annotation_io.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spectiary {

struct SampleLabelingSourceState {
    std::size_t sample_count = 0;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::vector<SampleLabelingTask> tasks;
    std::optional<std::string> active_task_id;
};

struct SampleLabelingStateCache {
    std::unordered_map<std::string, SampleLabelingSourceState> sources;
};

enum class SampleLabelingStateCacheLoadIssueKind {
    None,
    ReadFailed,
    InvalidDocument,
    UnsupportedFormatOrSchema,
};

enum class SampleLabelingPersistenceCompletion {
    NotAttempted,
    Succeeded,
    Failed,
};

struct SampleLabelingStateCacheOwnerLoadOutcome {
    SampleLabelingPersistenceCompletion completion =
        SampleLabelingPersistenceCompletion::NotAttempted;
    SampleLabelingStateCacheLoadIssueKind issue_kind =
        SampleLabelingStateCacheLoadIssueKind::None;
    std::string diagnostic_detail;
};

struct SampleLabelingStateCacheLoadResult {
    SampleLabelingStateCache cache;
    SampleLabelingStateCacheOwnerLoadOutcome ordinary_state;
    SampleLabelingStateCacheOwnerLoadOutcome draft_checkpoint;
    // The bounded legacy reader does not read either split owner.
    SampleLabelingStateCacheOwnerLoadOutcome legacy_seed;
    std::string warning;
    SampleLabelingStateCacheLoadIssueKind issue_kind =
        SampleLabelingStateCacheLoadIssueKind::None;
    std::string diagnostic_detail;
};

struct SampleLabelingStateCacheOwnerSaveOutcome {
    // Describes the owner save operation, including its validation. Failed does
    // not imply that atomic replacement reached the filesystem.
    SampleLabelingPersistenceCompletion completion =
        SampleLabelingPersistenceCompletion::NotAttempted;
    std::string diagnostic_detail;
};

struct SampleLabelingStateCacheSaveResult {
    SampleLabelingStateCacheOwnerSaveOutcome ordinary_state;
    SampleLabelingStateCacheOwnerSaveOutcome draft_checkpoint;
    // Includes preflight failures even when neither replacement was attempted.
    std::string diagnostic_detail;

    [[nodiscard]] bool Succeeded() const
    {
        return RegistrationSucceeded() &&
            draft_checkpoint.completion ==
                SampleLabelingPersistenceCompletion::Succeeded;
    }

    [[nodiscard]] bool RegistrationSucceeded() const
    {
        return ordinary_state.completion ==
            SampleLabelingPersistenceCompletion::Succeeded;
    }

    [[nodiscard]] bool CheckpointCleanupPending() const
    {
        return RegistrationSucceeded() &&
            draft_checkpoint.completion !=
                SampleLabelingPersistenceCompletion::Succeeded;
    }

    explicit operator bool() const { return Succeeded(); }
};

struct SampleLabelingSourceMetadataPatch {
    std::size_t sample_count = 0;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
};

struct SampleLabelingSourceStatePatch {
    std::optional<SampleLabelingSourceMetadataPatch> metadata;
    std::vector<SampleLabelingTask> task_upserts;
    // Creation retries must never replace a task that appeared after the
    // candidate was prepared. These IDs are checked while the commit lock is
    // held and remain attached to later edits of the same pending task.
    std::unordered_set<std::string> task_ids_expected_absent;
    std::vector<std::string> task_tombstones;
    bool active_task_selection_changed = false;
    std::optional<std::string> active_task_id;
};

struct SampleLabelingStateCachePatch {
    std::unordered_map<std::string, SampleLabelingSourceStatePatch>
        sources;
};

enum class SampleLabelingStateCacheLoadPolicy {
    AllowPersistentOutputs,
    AllowPersistentOutputsWithoutResultHydration,
    // Output conflict checks must not depend on best-effort draft contents.
    OrdinaryRegistrationsOnly,
    InternalDraftsOnly,
};

[[nodiscard]] std::filesystem::path DefaultSampleLabelingStateCachePath(const RuntimePaths& runtime_paths);
[[nodiscard]] std::filesystem::path SampleLabelingDraftCheckpointPath(
    const RuntimePaths& runtime_paths, const std::filesystem::path& state_path);

[[nodiscard]] std::filesystem::path
SampleLabelingStateCoordinationDirectory(
    const std::filesystem::path& state_cache_path);

// Returns the mandatory normalized-path coordination directory followed by
// any optional physical-identity alias directories. The order is stable and
// callers that acquire more than one lock must use it as returned.
[[nodiscard]] std::vector<std::filesystem::path>
SampleLabelingStateCoordinationDirectories(
    const std::filesystem::path& state_cache_path);

[[nodiscard]] SampleLabelingStateCacheLoadResult LoadSampleLabelingStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint = {},
    SampleLabelingStateCacheLoadPolicy policy =
        SampleLabelingStateCacheLoadPolicy::
            AllowPersistentOutputs);

[[nodiscard]] SampleLabelingStateCacheSaveResult SaveSampleLabelingStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SampleLabelingStateCache& cache,
    std::string* error_message = nullptr);

// Explicit, read-only schema-4 import for pinned automation fixtures.
[[nodiscard]] SampleLabelingStateCacheLoadResult LoadLegacySampleLabelingDraftSeed(
    const std::filesystem::path& path);

[[nodiscard]] SampleLabelingStateCacheSaveResult CommitSampleLabelingStateCachePatch(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SampleLabelingStateCachePatch& patch,
    std::string* error_message = nullptr,
    std::chrono::milliseconds commit_lock_wait =
        std::chrono::milliseconds::zero());

[[nodiscard]] bool HasSampleLabelingOutputPathConflict(
    const SampleLabelingStateCache& cache,
    const SampleLabelingTask& candidate,
    std::string_view source_identity);

}  // namespace spectiary
