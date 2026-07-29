#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/panel_session_interaction.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/ui_text.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class SampleWorkflowPanelUi {
public:
    [[nodiscard]] static const char* LabelingWindowName();
    [[nodiscard]] static const char* FiltersWindowName();
    [[nodiscard]] static const char* SortingWindowName();

    void ResetForSampleWorkflow();

    void RenderLabeling(
        PanelSessionInteraction& interaction,
        bool* open,
        const std::function<std::optional<std::filesystem::path>()>& choose_output_path,
        SampleWorkflowShortcut& shortcut);
    void RenderLabeling(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const std::function<std::optional<std::filesystem::path>()>& choose_output_path,
        SampleWorkflowShortcut& shortcut);

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
    void ResetLabelShortcutCapture();

    std::string active_task_id_;
    std::optional<int> editing_label_code_;
    std::string label_name_edit_buffer_;
    std::array<char, 16> label_code_edit_buffer_ = {};
    std::array<char, 2> label_shortcut_edit_buffer_ = {};
    bool label_shortcut_capture_active_ = false;
    char pending_conflicting_shortcut_ = '\0';
    std::string label_shortcut_notice_;
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
    SampleAnnotationWorkflowRelationship pending_annotation_activation_relationship_ =
        SampleAnnotationWorkflowRelationship::PlainAnnotation;
};

}  // namespace specforge
