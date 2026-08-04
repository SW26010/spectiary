#pragma once

#include "app/local_user_state.h"
#include "domain/sample_navigation_direction.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_snapshot.h"
#include "ui/background_retirement.h"
#include "ui/sample_navigation_sequence.h"
#include "ui/sample_navigation_state_cache_io.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

struct NavigationTargetResolutionReport;
struct PreparedSampleWorkflowState;

enum class SampleNavigationRequestKind {
    Previous,
    Next,
    LabelAdvance,
    LocateRow,
    LocateSequencePosition,
    LocateSourceRowInSequence,
    LocateSampleName,
    LocateSampleNameMatch,
    RestoreLabelUndoPosition,
};

struct SampleNavigationRequest {
    SampleNavigationRequestKind kind = SampleNavigationRequestKind::LocateRow;
    std::size_t row_index = 0;
    std::size_t sequence_position = 0;
    std::string sample_name;
    std::vector<bool> eligible_samples;

    [[nodiscard]] static SampleNavigationRequest Previous();
    [[nodiscard]] static SampleNavigationRequest Next();
    [[nodiscard]] static SampleNavigationRequest LabelAdvance();
    [[nodiscard]] static SampleNavigationRequest LabelAdvanceToEligible(std::vector<bool> eligible_samples);
    [[nodiscard]] static SampleNavigationRequest LocateRow(std::size_t row_index);
    [[nodiscard]] static SampleNavigationRequest LocateSequencePosition(
        std::size_t sequence_position);
    [[nodiscard]] static SampleNavigationRequest LocateSourceRowInSequence(std::size_t row_index);
    [[nodiscard]] static SampleNavigationRequest LocateSampleName(std::string sample_name);
    [[nodiscard]] static SampleNavigationRequest LocateSampleNameMatch(
        std::size_t row_index,
        std::string sample_name);
    [[nodiscard]] static SampleNavigationRequest RestoreLabelUndoPosition(std::size_t row_index);
};

struct SampleNavigationResult {
    bool has_active_source = false;
    bool has_current_sample = false;
    bool target_found = false;
    bool moved = false;
    bool blocked_by_filter = false;
    bool current_sample_in_filter = true;
    std::size_t previous_index = 0;
    std::size_t current_index = 0;
    std::size_t filtered_sample_count = 0;
    bool sequence_active = false;
    bool sequence_empty = false;
    std::size_t sequence_count = 0;
    std::optional<std::size_t> current_sequence_position;
    std::optional<std::size_t> current_source_row;
    bool row_location_available = true;
};

struct SampleNavigationPrefetchPolicy {
    std::size_t ahead = 1;
    std::size_t behind = 0;
};

struct ExactSampleNameResolution {
    bool names_available = false;
    std::vector<std::size_t> matching_rows;
    bool first_match_in_active_sequence = false;
};

class SampleNavigationController {
public:
    SampleNavigationController();
    explicit SampleNavigationController(std::filesystem::path state_cache_path);

    void ActivateSource(std::string source_key, const SpectrumSnapshotHandle& snapshot);
    void ActivateSource(
        std::string source_key,
        const SpectrumSnapshotHandle& snapshot,
        const SourceCollectionIdentity& identity,
        SourceCollectionManifest manifest,
        std::optional<std::size_t> prepared_index = std::nullopt);
    [[nodiscard]] BackgroundRetirementHandle ActivatePreparedSource(
        std::string source_key,
        const SpectrumSnapshotHandle& snapshot,
        const SourceCollectionIdentity& identity,
        SourceCollectionManifest manifest,
        PreparedSampleWorkflowState prepared);
    [[nodiscard]] BackgroundRetirementHandle AdoptPreparedStateCache(
        std::shared_ptr<const SampleNavigationStateCacheLoadResult>
            cache_snapshot);
    [[nodiscard]] BackgroundRetirementHandle
        ReleaseBackgroundResourcesForShutdown();
    [[nodiscard]] std::optional<SourceCollectionIdentity> ActivateKnownSource(
        std::string_view source_key);
    [[nodiscard]] std::optional<SourceCollectionIdentity> KnownSourceIdentity(
        std::string_view source_key) const;
    [[nodiscard]] std::optional<std::size_t> KnownSourceCurrentIndex(
        std::string_view source_key) const;
    [[nodiscard]] std::optional<SourceCollectionIdentity> active_source_identity() const;
    [[nodiscard]] BackgroundRetirementHandle RemoveSource(std::string_view source_key);
    void ClearActiveSource();
    [[nodiscard]] bool AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        std::string* message = nullptr);
    [[nodiscard]] bool RemoveReadOnlyAnnotationFromActiveSource(const std::filesystem::path& path);
    [[nodiscard]] bool RestoreReadOnlyAnnotationsForActiveSource(
        const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSourceKey(
        std::string_view source_key) const;
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::filesystem::path>>
        AnnotationPathsBySourceKey() const;

    [[nodiscard]] SampleNavigationResult Navigate(const SampleNavigationRequest& request);
    [[nodiscard]] SampleNavigationResult NavigateDeferred(
        const SampleNavigationRequest& request,
        bool remember_labeling_position,
        std::optional<std::size_t> base_index = std::nullopt,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] bool RetargetDeferredNavigation(std::size_t spectrum_index);
    [[nodiscard]] bool CommitDeferredNavigation(std::size_t spectrum_index);
    void CancelDeferredNavigation();
    [[nodiscard]] std::optional<std::size_t> current_index() const;
    [[nodiscard]] std::optional<std::size_t> pending_index() const;
    [[nodiscard]] bool pending_navigation_remembers_labeling_position() const;
    [[nodiscard]] std::optional<std::size_t> spectrum_count() const;
    [[nodiscard]] bool can_move_previous() const;
    [[nodiscard]] bool can_move_next() const;
    std::optional<std::size_t> SetSampleFilter(
        std::vector<bool> included_samples,
        bool defer_navigation = false);
    std::optional<std::size_t> ClearSampleFilter(bool defer_navigation = false);
    [[nodiscard]] bool filter_active() const;
    [[nodiscard]] std::size_t filtered_sample_count() const;
    [[nodiscard]] bool current_sample_in_filter() const;
    std::optional<std::size_t> SetSampleSorting(
        SampleNavigationSortChoice sort_choice,
        bool defer_navigation = false);
    std::optional<std::size_t> ClearSampleSorting(bool defer_navigation = false);
    [[nodiscard]] bool sorting_active() const;
    [[nodiscard]] const SampleNavigationSequence& current_sequence() const;
    [[nodiscard]] std::vector<std::size_t> AdjacentRows(
        SampleNavigationDirection direction,
        SampleNavigationPrefetchPolicy policy = {}) const;
    [[nodiscard]] bool SetSampleNameQuery(std::string query);
    [[nodiscard]] std::string_view sample_name_query() const;
    [[nodiscard]] const std::vector<std::size_t>& sample_name_matches() const;
    [[nodiscard]] const SourceCollectionManifest* active_context() const;
    [[nodiscard]] ExactSampleNameResolution
    ResolveExactSampleName(std::string_view name) const;
    [[nodiscard]] std::uint64_t active_context_generation() const;
    [[nodiscard]] std::uint64_t sequence_topology_revision() const;
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCache();
    [[nodiscard]] LocalUserStatePersistenceStatus PersistenceStatus() const;

private:
    struct PublishedSequenceTopology {
        std::string source_collection_identity;
        std::string source_fingerprint;
        std::string context_fingerprint;
        bool active = false;
        std::size_t source_row_count = 0;
        std::vector<std::size_t> ordered_rows;
    };

    struct SourceSession {
        std::string source_collection_identity;
        std::string source_name;
        std::string source_fingerprint;
        std::string context_fingerprint;
        std::size_t spectrum_count = 0;
        // current_index owns the committed UI presentation. pending_index is
        // only a navigation intent until its prepared snapshot arrives.
        std::optional<std::size_t> current_index;
        std::optional<std::size_t> pending_index;
        bool pending_navigation_remembers_labeling_position = false;
        SourceCollectionManifest manifest;
        std::string sample_name_query;
        std::vector<std::size_t> sample_name_matches;
        bool filter_active = false;
        std::vector<bool> filter_included_samples;
        std::size_t filtered_sample_count = 0;
        std::optional<std::size_t> index_before_active_filter;
        SampleNavigationSortChoice sort_choice;
        // Cached topology never owns search-query or committed/pending cursor state.
        mutable bool sequence_state_cache_valid = false;
        mutable SampleNavigationSequence sequence_state_cache;
    };

    [[nodiscard]] SourceSession* ActiveSession();
    [[nodiscard]] const SourceSession* ActiveSession() const;
    [[nodiscard]] static SampleNavigationSequence BuildSequenceState(
        const SourceSession& session);
    [[nodiscard]] static const SampleNavigationSequence& CachedSequenceState(
        const SourceSession& session);
    [[nodiscard]] static PublishedSequenceTopology
        CapturePublishedSequenceTopology(
            const SourceSession& session,
            const SampleNavigationSequence& sequence);
    [[nodiscard]] static bool MatchesPublishedSequenceTopology(
        const PublishedSequenceTopology& published,
        const SourceSession& session,
        const SampleNavigationSequence& sequence);
    [[nodiscard]] static const SampleNavigationSequence& CachedSequence(const SourceSession& session);
    void InvalidateSequenceState(SourceSession& session);
    void RefreshSequenceTopologyRevision();
    static std::optional<std::size_t> ReconcileCurrentWithSequence(SourceSession& session);
    static std::optional<std::size_t> ReconcileDeferredWithSequence(
        SourceSession& session,
        std::optional<std::size_t> preferred_index = std::nullopt);
    [[nodiscard]] static bool IsSampleInFilter(const SourceSession& session, std::size_t sample_index);
    static void PopulateResultFromSequence(
        SampleNavigationResult& result,
        const SourceSession& session,
        const SampleNavigationSequence& sequence,
        const SampleNavigationSequenceProjection& projection);
    [[nodiscard]] static bool LoadReadOnlyAnnotationIntoSession(
        SourceSession& session,
        const std::filesystem::path& path,
        std::string* message = nullptr);
    void EnsureStateCacheLoaded();
    [[nodiscard]] std::optional<std::size_t>
        CachedIndex(std::string_view source_identity) const;
    void PersistActiveIndex();
    [[nodiscard]] LocalUserStatePersistenceLifecycle::SaveResult
        SaveStateCache();
    static void RecomputeMatches(SourceSession& session);

    std::unordered_map<std::string, SourceSession> sessions_;
    std::unordered_map<std::string, std::string> source_key_to_session_key_;
    SampleNavigationStateCache state_cache_;
    std::shared_ptr<const SampleNavigationStateCacheLoadResult>
        state_cache_snapshot_;
    std::filesystem::path state_cache_path_;
    LocalUserStatePersistenceLifecycle state_cache_persistence_;
    std::optional<std::string> active_source_key_;
    std::uint64_t active_context_generation_ = 0;
    // Cursor movement does not change the ordered-row topology.
    std::uint64_t sequence_topology_revision_ = 0;
    std::optional<PublishedSequenceTopology>
        published_sequence_topology_;
    bool state_cache_loaded_ = false;
};

}  // namespace specforge
