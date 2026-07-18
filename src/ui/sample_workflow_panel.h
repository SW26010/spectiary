#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_session.h"

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

    [[nodiscard]] SourceCollectionSessionAction RenderLabeling(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool plot_shortcut_context_active,
        bool* open,
        const std::function<std::optional<std::filesystem::path>()>& choose_output_path);

    [[nodiscard]] SourceCollectionSessionAction RenderFilters(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        const SourceCollectionSessionViewReader& read_view,
        bool* open);

    [[nodiscard]] SourceCollectionSessionAction RenderSorting(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        const SourceCollectionSessionViewReader& read_view,
        bool* open);

private:
    std::array<char, 128> new_task_name_buffer_ = {};
    std::array<char, 128> active_task_name_buffer_ = {};
    std::string active_task_name_buffer_task_id_;
    std::optional<int> editing_label_code_;
    std::string label_name_edit_buffer_;
    std::array<char, 16> label_code_edit_buffer_ = {};
    std::array<char, 2> label_shortcut_edit_buffer_ = {};
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
