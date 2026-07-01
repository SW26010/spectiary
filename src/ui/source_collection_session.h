#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/sample_navigation_controller.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

class SampleWorkflowCoordinator;
struct SampleWorkflowCommandResult;
class SourceCollectionRoster;
class SourceCollectionSessionStatePersistence;

enum class SourceCollectionSessionIntentKind {
    SourceCollection,
    SampleNavigation,
    ActiveSampleWorkflow,
    SampleFiltering,
    SampleSorting,
};

enum class SourceCollectionIntentKind {
    Open,
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
    CreateDefaultLabelingTask,
    CreateLabelingTask,
    ActivateLabelingTaskFromAnnotation,
    RenameActiveLabelingTask,
    DeleteActiveLabelingTask,
    UpsertActiveLabel,
    SetActiveLabelingAutoAdvance,
    SetActiveLabelingSkipLabeledOnAdvance,
    SetActiveLabelingOutputPath,
    DeactivateActiveLabelingTask,
    AssignActiveLabelToCurrentSample,
    ClearActiveLabelForCurrentSample,
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
    [[nodiscard]] static SourceCollectionIntent Open(
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

    SourceCollectionIntentKind kind = SourceCollectionIntentKind::Open;
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
    [[nodiscard]] static ActiveSampleWorkflowIntent CreateDefaultLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent CreateLabelingTask(std::string task_name);
    [[nodiscard]] static ActiveSampleWorkflowIntent ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent RenameActiveLabelingTask(std::string task_name);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeleteActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] static ActiveSampleWorkflowIntent SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] static ActiveSampleWorkflowIntent DeactivateActiveLabelingTask();
    [[nodiscard]] static ActiveSampleWorkflowIntent AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] static ActiveSampleWorkflowIntent ClearActiveLabelForCurrentSample();

private:
    friend class SourceCollectionSession;
    friend struct SourceCollectionSessionIntent;

    ActiveSampleWorkflowIntent() = default;

    ActiveSampleWorkflowIntentKind kind = ActiveSampleWorkflowIntentKind::CreateDefaultLabelingTask;
    std::filesystem::path path;
    std::string task_name;
    SampleLabelDefinition label;
    bool enabled = false;
    int label_code = kUnlabeledSampleLabelCode;
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
    SourceCollectionSessionView view;
    SampleNavigationResult navigation;
    bool changed = false;
    bool loaded = false;
    std::string message;
};

using SourceCollectionSessionIntentSubmitter =
    std::function<SourceCollectionSessionResult(SourceCollectionSessionIntent)>;

class SourceCollectionSession {
public:
    using SnapshotLoader = std::function<SpectrumSnapshotHandle(const std::filesystem::path&, std::size_t)>;

    explicit SourceCollectionSession(SnapshotLoader snapshot_loader);
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
    ~SourceCollectionSession();

    SourceCollectionSession(SourceCollectionSession&&) noexcept;
    SourceCollectionSession& operator=(SourceCollectionSession&&) noexcept;
    SourceCollectionSession(const SourceCollectionSession&) = delete;
    SourceCollectionSession& operator=(const SourceCollectionSession&) = delete;

    [[nodiscard]] SourceCollectionSessionResult Submit(SourceCollectionSessionIntent intent);
    [[nodiscard]] SourceCollectionSessionView View() const;
    [[nodiscard]] SpectrumSnapshotHandle CurrentSampleSnapshot() const;

    void MaybeSaveStateCaches(std::uint64_t frame_index);
    [[nodiscard]] bool FlushStateCaches();

private:
    [[nodiscard]] SourceCollectionSessionAction OpenSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] SourceCollectionSessionAction ActivateSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionSessionAction RemoveSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionSessionAction RequestSampleNavigation(
        const SampleNavigationRequest& request,
        SampleNavigationResult* navigation_result = nullptr);
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
    [[nodiscard]] SourceCollectionSessionAction CreateDefaultLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction CreateLabelingTask(std::string task_name);
    [[nodiscard]] SourceCollectionSessionAction ActivateLabelingTaskFromAnnotation(
        std::filesystem::path annotation_path);
    [[nodiscard]] SourceCollectionSessionAction RenameActiveLabelingTask(std::string task_name);
    [[nodiscard]] SourceCollectionSessionAction DeleteActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction DeactivateActiveLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveLabelForCurrentSample();
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

    [[nodiscard]] SourceCollectionSessionAction EnsureSnapshotMatchesNavigation();
    [[nodiscard]] SourceCollectionSessionAction LoadActiveSourceAt(std::size_t spectrum_index);
    [[nodiscard]] std::vector<SourceCollectionSavedSource> SavedSourcesWithAnnotations() const;
    void RestoreSourceSessionCache();
    void MarkSourceSessionCacheDirty();
    void ApplyWorkflowCommandResult(
        SourceCollectionSessionAction& action,
        const SampleWorkflowCommandResult& command_result,
        SampleNavigationResult* navigation_result = nullptr);

    std::unique_ptr<SourceCollectionRoster> roster_;
    std::unique_ptr<SampleWorkflowCoordinator> workflow_;
    std::unique_ptr<SourceCollectionSessionStatePersistence> source_session_state_;
};

}  // namespace specforge
