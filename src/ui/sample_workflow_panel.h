#pragma once

#include "app/runtime_paths.h"

#include "domain/sample_filter.h"
#include "domain/sample_label_export.h"
#include "domain/spectrum_snapshot.h"
#include "ui/panel_session_interaction.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/ui_text.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spectiary {

struct SampleWorkflowPanelUiTestAccess;
struct ShellUiTestAccess;
class ShellUi;

[[nodiscard]] std::string SampleWorkflowTemporaryDraftTaskName(
    UiLanguage language,
    std::string_view task_name);
[[nodiscard]] std::string SampleWorkflowSaveStateText(
    UiLanguage language,
    const SampleLabelSaveState& save_state);
[[nodiscard]] std::string SampleWorkflowSaveMessageText(
    UiLanguage language,
    const SampleLabelSaveState& save_state);

struct SampleWorkflowAnnotationActivationTextIds {
    UiTextId editable_message =
        UiTextId::UseAnnotationEditableMessage;
    std::optional<UiTextId> detail_message;
};

[[nodiscard]] SampleWorkflowAnnotationActivationTextIds
SampleWorkflowAnnotationActivationText(
    SampleAnnotationWorkflowRelationship relationship,
    SampleLabelingOutputArtifactFormat owner_format);

[[nodiscard]] std::optional<UiTextId>
SampleWorkflowCanonicalOutputActionTextId(
    const SourceCollectionLabelingView& labeling_view);

[[nodiscard]] SampleLabelExportFormat
RecommendedSampleLabelExportFormat(
    std::string_view source_kind) noexcept;

[[nodiscard]] std::filesystem::path
EnsureSampleLabelExportPathExtension(
    std::filesystem::path path,
    SampleLabelExportFormat format);

using SampleLabelExportPathChooser =
    std::function<std::optional<std::filesystem::path>(
        SampleLabelExportFormat,
        std::string_view)>;

using SampleLabelingOutputPathChooser =
    std::function<std::optional<std::filesystem::path>(
        std::string_view)>;

class SampleWorkflowPanelUi {
public:
    explicit SampleWorkflowPanelUi(const RuntimePaths& paths = {}) : runtime_paths_(paths) {}
    [[nodiscard]] static const char* LabelingWindowName();
    [[nodiscard]] static const char* FiltersWindowName();
    [[nodiscard]] static const char* SortingWindowName();

    void ResetForSampleWorkflow();

    void RenderLabeling(
        PanelSessionInteraction& interaction,
        bool* open,
        const SampleLabelingOutputPathChooser& choose_output_path,
        const SampleLabelExportPathChooser& choose_export_path,
        SampleWorkflowShortcut& shortcut);
    void RenderLabeling(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const SampleLabelingOutputPathChooser& choose_output_path,
        const SampleLabelExportPathChooser& choose_export_path,
        SampleWorkflowShortcut& shortcut);
    void FinalizeTaskNameEdit(
        PanelSessionInteraction& interaction,
        UiLanguage language);

    void RenderFilters(
        PanelSessionInteraction& interaction,
        bool* open);
    void RenderFilters(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open);

    void RenderSorting(
        PanelSessionInteraction& interaction,
        bool* open);
    void RenderSorting(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open);

private:
    RuntimePaths runtime_paths_;
    friend struct SampleWorkflowPanelUiTestAccess;
    friend struct ShellUiTestAccess;
    friend class ShellUi;

    enum class LabelShortcutNoticeKind {
        None,
        CaptureInstructions,
        UnboundOnSave,
        Unsupported,
        Selected,
        WillMove,
        Conflict,
    };

    void ResetLabelShortcutCapture();
    void ResetTaskNameEditState();
    void SyncTaskNameEditTarget(
        const SourceCollectionLabelingView& labeling_view);
    void RecordTaskNameEdit(
        bool item_active,
        bool enter_pressed,
        bool deactivated_after_edit,
        UiLanguage language);
    void RequestTaskNameEditFinalize(bool deferred);
    void SettlePendingTaskNameEdit(
        PanelSessionInteraction& interaction,
        UiLanguage language);
    void RenderTaskNameEditor(
        const SourceCollectionLabelingView& labeling_view,
        PanelSessionInteraction& interaction,
        UiLanguage language);
    [[nodiscard]] std::string LabelShortcutNotice(
        UiLanguage language) const;
    void CaptureLabelingOperationResult(
        const SourceCollectionSessionResult& result,
        UiLanguage language);
    void SetPendingAnnotationActivation(
        const SourceCollectionAnnotationValueView& annotation);
    void ClearLabelingOperationMessage();
    [[nodiscard]] static std::string ValidateTaskName(
        UiLanguage language,
        std::string_view task_name);
    [[nodiscard]] static std::string RecoveryDraftRowToken(
        const SourceCollectionLabelingView& labeling_view,
        std::size_t draft_index);

    std::string active_task_id_;
    std::string task_name_edit_task_id_;
    std::string task_name_edit_buffer_;
    std::string task_name_edit_baseline_;
    bool task_name_edit_focused_ = false;
    bool task_name_finalize_pending_ = false;
    bool task_name_finalize_deferred_ = false;
    std::string task_name_edit_validation_message_;
    std::string labeling_operation_message_;
    std::optional<UiTextId> labeling_operation_text_id_;
    std::optional<int> editing_label_code_;
    std::string label_name_edit_buffer_;
    std::array<char, 16> label_code_edit_buffer_ = {};
    std::array<char, 2> label_shortcut_edit_buffer_ = {};
    bool label_shortcut_capture_active_ = false;
    char pending_conflicting_shortcut_ = '\0';
    LabelShortcutNoticeKind label_shortcut_notice_kind_ =
        LabelShortcutNoticeKind::None;
    std::string label_shortcut_notice_shortcut_;
    std::string label_shortcut_notice_owner_;
    bool label_name_focus_pending_ = false;
    std::optional<int> pending_label_code_change_original_code_;
    SampleLabelDefinition pending_label_code_change_;
    std::size_t pending_label_code_change_usage_count_ = 0;
    std::optional<int> pending_delete_label_code_;
    std::string pending_delete_label_name_;
    std::size_t pending_delete_label_usage_count_ = 0;
    std::filesystem::path pending_annotation_activation_path_;
    std::string pending_annotation_activation_name_;
    std::string pending_delete_task_name_;
    bool pending_delete_task_is_temporary_ = false;
    bool pending_delete_task_is_recovery_ = false;
    std::string pending_delete_task_source_identity_;
    std::string pending_delete_task_id_;
    std::unordered_set<std::string> retained_recovery_drafts_;
    std::unordered_map<std::string, std::string>
        recovery_draft_fingerprints_;
    bool labeling_export_format_initialized_ = false;
    std::string labeling_export_source_identity_;
    SampleLabelExportFormat labeling_export_format_ =
        SampleLabelExportFormat::Npy;
    SampleAnnotationWorkflowRelationship pending_annotation_activation_relationship_ =
        SampleAnnotationWorkflowRelationship::PlainAnnotation;
    SampleLabelingOutputArtifactFormat
        pending_annotation_activation_owner_format_ =
            SampleLabelingOutputArtifactFormat::None;
};

}  // namespace spectiary
