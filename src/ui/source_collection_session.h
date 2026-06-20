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

namespace specforge {

class SampleWorkflowCoordinator;
struct SampleWorkflowCommandResult;
class SourceCollectionRoster;

enum class SourceCollectionSessionCommandKind {
    OpenSource,
    ActivateSource,
    RemoveSource,
    NavigateSample,
    AddReadOnlyAnnotation,
    SetSampleNameQuery,
    CommitSampleNameSelection,
    CreateDefaultLabelingTask,
    UpsertActiveLabel,
    SetActiveLabelingAutoAdvance,
    SetActiveLabelingSkipLabeledOnAdvance,
    SetActiveLabelingOutputPath,
    AssignActiveLabelToCurrentSample,
    ClearActiveLabelForCurrentSample,
    ClearFilters,
    SetFilterValueSelected,
    SetActiveLabelingFilterSourceSelected,
};

struct SourceCollectionSessionCommand {
    [[nodiscard]] static SourceCollectionSessionCommand OpenSource(
        std::filesystem::path path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] static SourceCollectionSessionCommand ActivateSource(std::size_t source_index);
    [[nodiscard]] static SourceCollectionSessionCommand RemoveSource(std::size_t source_index);
    [[nodiscard]] static SourceCollectionSessionCommand NavigateSample(SampleNavigationRequest request);
    [[nodiscard]] static SourceCollectionSessionCommand AddReadOnlyAnnotation(std::filesystem::path path);
    [[nodiscard]] static SourceCollectionSessionCommand SetSampleNameQuery(std::string query);
    [[nodiscard]] static SourceCollectionSessionCommand CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name);
    [[nodiscard]] static SourceCollectionSessionCommand CreateDefaultLabelingTask();
    [[nodiscard]] static SourceCollectionSessionCommand UpsertActiveLabel(SampleLabelDefinition label);
    [[nodiscard]] static SourceCollectionSessionCommand SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] static SourceCollectionSessionCommand SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] static SourceCollectionSessionCommand SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] static SourceCollectionSessionCommand AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] static SourceCollectionSessionCommand ClearActiveLabelForCurrentSample();
    [[nodiscard]] static SourceCollectionSessionCommand ClearFilters();
    [[nodiscard]] static SourceCollectionSessionCommand SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] static SourceCollectionSessionCommand SetActiveLabelingFilterSourceSelected(bool selected);

private:
    friend class SourceCollectionSession;

    SourceCollectionSessionCommand() = default;

    SourceCollectionSessionCommandKind kind = SourceCollectionSessionCommandKind::SetSampleNameQuery;
    std::filesystem::path path;
    std::size_t spectrum_index = 0;
    std::size_t source_index = 0;
    SampleNavigationRequest navigation_request;
    std::string query;
    std::size_t target_row = 0;
    std::string matched_name;
    SampleLabelDefinition label;
    bool enabled = false;
    int label_code = kUnlabeledSampleLabelCode;
    std::string filter_source_id;
    std::string filter_value_key;
    bool selected = false;
};

struct SourceCollectionSessionResult {
    SourceCollectionSessionAction action;
    SourceCollectionSessionView view;
    SampleNavigationResult navigation;
    bool changed = false;
    bool loaded = false;
    std::string message;
};

using SourceCollectionCommandSubmitter =
    std::function<SourceCollectionSessionResult(SourceCollectionSessionCommand)>;

class SourceCollectionSession {
public:
    using SnapshotLoader = std::function<SpectrumSnapshotHandle(const std::filesystem::path&, std::size_t)>;

    explicit SourceCollectionSession(SnapshotLoader snapshot_loader);
    SourceCollectionSession(
        SnapshotLoader snapshot_loader,
        std::filesystem::path navigation_state_cache_path,
        std::filesystem::path labeling_state_cache_path);
    ~SourceCollectionSession();

    SourceCollectionSession(SourceCollectionSession&&) noexcept;
    SourceCollectionSession& operator=(SourceCollectionSession&&) noexcept;
    SourceCollectionSession(const SourceCollectionSession&) = delete;
    SourceCollectionSession& operator=(const SourceCollectionSession&) = delete;

    [[nodiscard]] SourceCollectionSessionResult Submit(SourceCollectionSessionCommand command);
    [[nodiscard]] SourceCollectionSessionView View() const;

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

    [[nodiscard]] SourceCollectionSessionAction SetSampleNameQuery(std::string query);
    [[nodiscard]] SourceCollectionSessionAction CommitSampleNameSelection(
        std::size_t target_row,
        std::string matched_name,
        SampleNavigationResult* navigation_result = nullptr);
    [[nodiscard]] SourceCollectionSessionAction CreateDefaultLabelingTask();
    [[nodiscard]] SourceCollectionSessionAction UpsertActiveLabel(
        SampleLabelDefinition label,
        bool* changed = nullptr);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingAutoAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingSkipLabeledOnAdvance(bool enabled);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingOutputPath(std::filesystem::path output_path);
    [[nodiscard]] SourceCollectionSessionAction AssignActiveLabelToCurrentSample(int code);
    [[nodiscard]] SourceCollectionSessionAction ClearActiveLabelForCurrentSample();
    [[nodiscard]] SourceCollectionSessionAction ClearFilters();
    [[nodiscard]] SourceCollectionSessionAction SetFilterValueSelected(
        std::string source_id,
        std::string value_key,
        bool selected);
    [[nodiscard]] SourceCollectionSessionAction SetActiveLabelingFilterSourceSelected(bool selected);

    [[nodiscard]] SourceCollectionSessionAction EnsureSnapshotMatchesNavigation();
    [[nodiscard]] SourceCollectionSessionAction LoadActiveSourceAt(std::size_t spectrum_index);
    void ApplyWorkflowCommandResult(
        SourceCollectionSessionAction& action,
        const SampleWorkflowCommandResult& command_result,
        SampleNavigationResult* navigation_result = nullptr);

    std::unique_ptr<SourceCollectionRoster> roster_;
    std::unique_ptr<SampleWorkflowCoordinator> workflow_;
};

}  // namespace specforge
