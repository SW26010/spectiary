#pragma once

#include "app/local_user_state.h"
#include "domain/spectrum_snapshot.h"
#include "domain/source_collection_manifest.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_folder_listing_generation.h"
#include "ui/source_collection_resident_snapshot.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

class SampleWorkflowCoordinator;
struct SampleWorkflowCommandResult;
class SourceCollectionRoster;
struct SourceCollectionRosterOpenResult;
class SourceCollectionSessionStatePersistence;

enum class SourceCollectionSessionRestoreMode {
    Immediate,
    Deferred,
};

struct SourceCollectionLoadHint {
    SourceCollectionIdentity identity;
    std::size_t spectrum_index = 0;
    std::uint64_t live_workflow_revision = 0;
    SourceCollectionFolderListingGenerationHandle folder_listing_generation_hint;
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof;
    std::optional<SourceCollectionResidentSnapshot> resident_snapshot;
};

struct SourceCollectionSnapshotPrefetchPlan {
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::vector<std::filesystem::path> annotation_paths;
    SourceCollectionLoadHint load_hint;
};

struct SourceCollectionSnapshotPrefetchStoreResult {
    bool stored = false;
    std::vector<BackgroundRetirementHandle> background_retirement;
};

enum class SourceCollectionSessionIntentKind {
    SourceCollection,
    SampleNavigation,
    ActiveSampleWorkflow,
    SampleFiltering,
    SampleSorting,
};

enum class SourceCollectionIntentKind {
    OpenSynchronously,
    SwitchActive,
    Remove,
    AddReadOnlyAnnotationResult,
    RemoveReadOnlyAnnotationResult,
    RenameAnnotationResultDisplayName,
};

enum class SampleNavigationIntentKind {
    Move,
    SetSampleNameQuery,
    CommitSampleNameSelection,
};

enum class ActiveSampleWorkflowIntentKind {
    StartOrResumeTemporaryLabelingTask,
    ActivateLabelingTaskFromAnnotation,
    DeleteActiveLabelingTask,
    UpsertActiveLabel,
    UpdateActiveLabel,
    RemoveActiveLabel,
    SetActiveLabelingAutoAdvance,
    SetActiveLabelingSkipLabeledOnAdvance,
    SetActiveLabelingOutputPath,
    DeactivateActiveLabelingTask,
    AssignActiveLabelToCurrentSample,
    ClearActiveLabelForCurrentSample,
    UndoLastLabelWrite,
};

enum class SampleFilteringIntentKind {
    ClearFilters,
    AddFilterSource,
    RemoveFilterSource,
    SetFilterValueSelected,
};

enum class SampleSortingIntentKind {
    ClearSorting,
    AddSortSource,
    RemoveSortSource,
    SetSortSource,
    SetSortDirection,
};

struct SourceCollectionIntent {
    // Compatibility path for headless/session callers. Interactive UI code
    // must enqueue a PreparedSourceCollection instead.
    [[nodiscard]] static SourceCollectionIntent OpenSynchronously(
        std::filesystem::path path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] static SourceCollectionIntent SwitchActive(std::size_t source_index);
    [[nodiscard]] static SourceCollectionIntent Remove(std::size_t source_index);
    [[nodiscard]] static SourceCollectionIntent AddReadOnlyAnnotationResult(std::filesystem::path path);
    [[nodiscard]] static SourceCollectionIntent RemoveReadOnlyAnnotationResult(std::filesystem::path path);
    [[nodiscard]] static SourceCollectionIntent RenameAnnotationResultDisplayName(
        std::filesystem::path path,
        std::string display_name);

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    SourceCollectionIntent() = default;

    SourceCollectionIntentKind kind = SourceCollectionIntentKind::OpenSynchronously;
    std::filesystem::path path;
    std::string display_name;
    std::size_t spectrum_index = 0;
    std::size_t source_index = 0;
};

struct SampleNavigationIntent {
    [[nodiscard]] static SampleNavigationIntent Move(SampleNavigationRequest request);
    [[nodiscard]] static SampleNavigationIntent SetSampleNameQuery(std::string query);
    [[nodiscard]] static SampleNavigationIntent CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name);

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    SampleNavigationIntent() = default;

    SampleNavigationIntentKind kind = SampleNavigationIntentKind::Move;
    SampleNavigationRequest request;
    std::string query;
    std::size_t target_row = 0;
    std::string matched_name;
};

struct ActiveSampleWorkflowIntent {
    [[nodiscard]] static ActiveSampleWorkflowIntent StartOrResumeTemporaryLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeleteActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] static ActiveSampleWorkflowIntent UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change = false);
    [[nodiscard]] static ActiveSampleWorkflowIntent RemoveActiveLabel(int code);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeactivateActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] static ActiveSampleWorkflowIntent ClearActiveLabelForCurrentSample();
    [[nodiscard]] static ActiveSampleWorkflowIntent UndoLastLabelWrite();

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    ActiveSampleWorkflowIntent() = default;

    ActiveSampleWorkflowIntentKind kind = ActiveSampleWorkflowIntentKind::StartOrResumeTemporaryLabelingTask;
    std::filesystem::path path;
    SampleLabelDefinition label;
    bool enabled = false;
    int label_code = kUnlabeledSampleLabelCode;
    bool allow_used_label_code_change = false;
};

struct SampleFilteringIntent {
    [[nodiscard]] static SampleFilteringIntent Clear();
    [[nodiscard]] static SampleFilteringIntent AddSource(std::string source_id);
    [[nodiscard]] static SampleFilteringIntent RemoveSource(std::string source_id);
    [[nodiscard]] static SampleFilteringIntent SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    SampleFilteringIntent() = default;

    SampleFilteringIntentKind kind = SampleFilteringIntentKind::ClearFilters;
    std::string source_id;
    std::string value_key;
    bool selected = false;
};

struct SampleSortingIntent {
    [[nodiscard]] static SampleSortingIntent Clear();
    [[nodiscard]] static SampleSortingIntent AddSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent RemoveSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent SetSortSource(std::string source_id);
    [[nodiscard]] static SampleSortingIntent SetSortDirection(SampleNavigationSortDirection direction);

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    SampleSortingIntent() = default;

    SampleSortingIntentKind kind = SampleSortingIntentKind::ClearSorting;
    std::string source_id;
    SampleNavigationSortDirection direction = SampleNavigationSortDirection::Ascending;
};

struct SourceCollectionSessionIntent {
    [[nodiscard]] static SourceCollectionSessionIntent EditSourceCollection(SourceCollectionIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent UpdateSampleNavigation(SampleNavigationIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ApplySampleFiltering(SampleFilteringIntent intent);
    [[nodiscard]] static SourceCollectionSessionIntent ApplySampleSorting(SampleSortingIntent intent);

private:
    friend class SourceCollectionSession;

    SourceCollectionSessionIntent() = default;

    SourceCollectionSessionIntentKind kind = SourceCollectionSessionIntentKind::SourceCollection;
    SourceCollectionIntent source_collection;
    SampleNavigationIntent sample_navigation;
    ActiveSampleWorkflowIntent active_sample_workflow;
    SampleFilteringIntent sample_filtering;
    SampleSortingIntent sample_sorting;
};

struct SourceCollectionSessionResult {
    SourceCollectionSessionAction action;
    SampleNavigationResult navigation;
    std::optional<std::size_t> follow_up_spectrum_index;
    std::optional<std::filesystem::path> canceled_source_follow_up_path;
    std::vector<BackgroundRetirementHandle> background_retirement;
    bool changed = false;
    bool loaded = false;
    std::string message;
};

using SourceCollectionSessionIntentSubmitter =
    std::function<SourceCollectionSessionResult(SourceCollectionSessionIntent)>;
using SourceCollectionSessionViewReader = std::function<const SourceCollectionSessionView&()>;

class SourceCollectionSession {
public:
    using SnapshotLoader = std::function<SpectrumSnapshotHandle(const std::filesystem::path&, std::size_t)>;

    explicit SourceCollectionSession(SnapshotLoader snapshot_loader);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        SourceCollectionSessionRestoreMode restore_mode);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path source_session_state_cache_path,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path,
        std::filesystem::path workflow_state_cache_path,
        SourceCollectionSessionRestoreMode restore_mode);
    ~SourceCollectionSession();

    SourceCollectionSession(SourceCollectionSession&&) noexcept;
    SourceCollectionSession& operator=(SourceCollectionSession&&) noexcept;
    SourceCollectionSession(const SourceCollectionSession&) = delete;
    SourceCollectionSession& operator=(const SourceCollectionSession&) = delete;

    [[nodiscard]] SourceCollectionSessionResult Submit(
        SourceCollectionSessionIntent intent,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] bool SupersedesPendingSourceActivation(
        const SourceCollectionSessionIntent& intent) const;
    [[nodiscard]] SourceCollectionSessionView View() const;
    // A deferred pending target is the origin for a subsequent navigation command.
    [[nodiscard]] std::optional<std::size_t> EffectiveSampleNavigationIndex() const;
    [[nodiscard]] SpectrumSnapshotHandle CurrentSampleSnapshot() const;
    [[nodiscard]] SpectrumSnapshotHandle CurrentSourceSnapshot() const;
    [[nodiscard]] std::vector<std::filesystem::path> AnnotationPathsForSource(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<SourceCollectionLoadHint> LoadHintForSource(
        const std::filesystem::path& path,
        std::optional<std::size_t> spectrum_index = std::nullopt);
    [[nodiscard]] std::optional<SourceCollectionSnapshotPrefetchPlan>
        PlanSnapshotPrefetch(
            SampleNavigationDirection direction,
            SampleNavigationPrefetchPolicy policy = {});
    [[nodiscard]] SourceCollectionSnapshotPrefetchStoreResult
        StorePrefetchedSnapshot(
            const std::filesystem::path& path,
            SourceCollectionResidentSnapshot resident);
    [[nodiscard]] SourceCollectionSessionResult OpenPreparedSource(
        std::filesystem::path path,
        std::size_t spectrum_index,
        SpectrumSnapshotHandle snapshot,
        PreparedSourceCollectionPayload payload,
        SourceCollectionFolderListingGenerationHandle folder_listing_generation = {},
        std::optional<SourceCollectionContextReuseProof> context_reuse_proof =
            std::nullopt);
    [[nodiscard]] SourceCollectionSessionResult OpenPreparedSource(
        std::filesystem::path path,
        std::size_t spectrum_index,
        SpectrumSnapshotHandle snapshot,
        SourceCollectionContext context,
        PreparedSampleWorkflowState prepared_workflow);
    [[nodiscard]] std::optional<SourceCollectionDeferredRestorePlan> TakeDeferredRestorePlan();
    void FinishDeferredRestore();
    [[nodiscard]] bool HasUnresolvedSourceIntent(const std::filesystem::path& path) const;
    [[nodiscard]] bool ForgetUnresolvedSourceIntent(const std::filesystem::path& path);
    [[nodiscard]] bool CancelPendingSampleNavigation(
        const std::filesystem::path& path,
        std::size_t spectrum_index);
    [[nodiscard]] bool CancelActivePendingSampleNavigation();

    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] bool FlushStateCaches();
    [[nodiscard]] std::vector<BackgroundRetirementHandle> ReleaseBackgroundResourcesForShutdown();

private:
    [[nodiscard]] SourceCollectionSessionAction OpenSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] SourceCollectionSessionAction ActivateSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionSessionAction RemoveSource(
        std::size_t source_index,
        std::vector<BackgroundRetirementHandle>& background_retirement,
        std::optional<std::filesystem::path>* canceled_source_follow_up_path);
    [[nodiscard]] SourceCollectionSessionAction RequestSampleNavigation(
        const SampleNavigationRequest& request,
        SampleNavigationResult* navigation_result = nullptr,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SourceCollectionSessionAction AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        bool* loaded = nullptr,
        std::string* message = nullptr);
    [[nodiscard]] SourceCollectionSessionAction RemoveReadOnlyAnnotationFromActiveSource(
        const std::filesystem::path& path);
    [[nodiscard]] SourceCollectionSessionAction RenameAnnotationDisplayName(
        std::filesystem::path path,
        std::string display_name);

    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SourceCollectionSessionAction CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        SampleNavigationResult* navigation_result = nullptr);
    [[nodiscard]] SourceCollectionSessionAction StartOrResumeTemporaryLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] SourceCollectionSessionAction DeleteActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction UpdateActiveLabel(
        int original_code,
        SampleLabelDefinition label,
        bool allow_used_code_change,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction RemoveActiveLabel(
        int code,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction DeactivateActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction AssignActiveLabelToCurrentSample(
        int code,
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveLabelForCurrentSample(
        NavigationTargetResolutionReport* target_resolution = nullptr);
    [[nodiscard]] SourceCollectionSessionAction UndoLastLabelWrite();
    [[nodiscard]] SourceCollectionSessionAction ClearFilters();
    [[nodiscard]] SourceCollectionSessionAction AddFilterSource(std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction RemoveFilterSource(std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SourceCollectionSessionAction ClearSampleSorting();
    [[nodiscard]] SourceCollectionSessionAction AddSampleSortSource(std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction RemoveSampleSortSource(std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetSampleSortSource(std::string source_id);
    [[nodiscard]] SourceCollectionSessionAction SetSampleSortDirection(SampleNavigationSortDirection direction);

    [[nodiscard]] SourceCollectionSessionAction EnsureSnapshotMatchesNavigation(
        bool refresh_source_context = false);
    void PreserveRequiredBackgroundSnapshotLoad();
    [[nodiscard]] SourceCollectionSessionAction LoadActiveSourceAt(std::size_t spectrum_index);
    [[nodiscard]] std::vector<SourceCollectionSavedSource> SavedSourcesWithAnnotations() const;
    void RestoreSourceSessionCache();
    void PrepareDeferredSourceSessionRestore();
    void MarkSourceSessionCacheDirty();
    void ApplyWorkflowCommandResult(
        SourceCollectionSessionAction& action,
        const SampleWorkflowCommandResult& command_result,
        SampleNavigationResult* navigation_result = nullptr);
    [[nodiscard]] SourceCollectionSessionAction AdoptRosterOpenResult(
        SourceCollectionRosterOpenResult result);

    std::unique_ptr<SourceCollectionRoster> roster_;
    std::unique_ptr<SampleWorkflowCoordinator> workflow_;
    std::unique_ptr<SourceCollectionSessionStatePersistence> source_session_state_;
    std::optional<SourceCollectionDeferredRestorePlan> deferred_restore_plan_;
    std::vector<SourceCollectionSavedSource> unresolved_deferred_restore_sources_;
    bool deferred_restore_active_ = false;
    bool background_loads_required_ = false;
    std::optional<std::size_t> pending_background_spectrum_index_;
    std::vector<BackgroundRetirementHandle> pending_background_retirement_;
    std::unordered_map<std::string, std::uint64_t> live_workflow_revisions_;
};

}  // namespace specforge
