#include "ui/sample_workflow_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kLabelingWindow = "Labeling###SpecForgeLabelingV1";
constexpr const char* kFiltersWindow = "Sample Filters###SpecForgeFiltersV1";
constexpr const char* kSortingWindow = "Sample Sorting###SpecForgeSampleSortingV1";
constexpr const char* kSampleAnnotationDragPayload = "SPECFORGE_SAMPLE_ANNOTATION_PATH";
constexpr const char* kAnnotationToLabelingPopup =
    "Use annotation as labeling task?###SpecForgeAnnotationToLabelingPopup";
constexpr const char* kDeleteLabelingTaskPopup =
    "Delete labeling task?###SpecForgeDeleteLabelingTaskPopup";

SourceCollectionSessionIntent UpdateSampleNavigation(SampleNavigationIntent intent)
{
    return SourceCollectionSessionIntent::UpdateSampleNavigation(std::move(intent));
}

SourceCollectionSessionIntent ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent intent)
{
    return SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(std::move(intent));
}

SourceCollectionSessionIntent ApplySampleFiltering(SampleFilteringIntent intent)
{
    return SourceCollectionSessionIntent::ApplySampleFiltering(std::move(intent));
}

SourceCollectionSessionIntent ApplySampleSorting(SampleSortingIntent intent)
{
    return SourceCollectionSessionIntent::ApplySampleSorting(std::move(intent));
}

bool CanResumeRememberedRow(
    const SourceCollectionLabelingView& labeling,
    std::size_t remembered_row,
    std::size_t sample_count)
{
    if (remembered_row >= sample_count) {
        return false;
    }
    return labeling.remembered_position_resumable;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path Utf8ToPath(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

template <std::size_t Size>
void CopyToBuffer(std::array<char, Size>& buffer, std::string_view text)
{
    static_assert(Size > 0);
    std::fill(buffer.begin(), buffer.end(), '\0');
    const std::size_t copy_size = std::min(text.size(), Size - 1);
    std::copy_n(text.begin(), copy_size, buffer.begin());
}

std::string PayloadString(const ImGuiPayload& payload)
{
    const char* data = static_cast<const char*>(payload.Data);
    if (data == nullptr || payload.DataSize <= 0) {
        return {};
    }
    std::string text(data, data + payload.DataSize);
    if (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

const SourceCollectionAnnotationValueView* FindAnnotationViewByPath(
    const SourceCollectionSessionView& session_view,
    const std::filesystem::path& path)
{
    const std::string path_text = PathToUtf8(path);
    const auto match = std::find_if(
        session_view.navigation.current_annotations.begin(),
        session_view.navigation.current_annotations.end(),
        [&path_text](const SourceCollectionAnnotationValueView& annotation) {
            return PathToUtf8(annotation.path) == path_text;
        });
    return match == session_view.navigation.current_annotations.end() ? nullptr : &*match;
}

std::string TrimAscii(std::string_view value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::optional<int> ParseInt(std::string_view text)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    std::size_t offset = 0;
    if (trimmed[0] == '-') {
        negative = true;
        offset = 1;
    }
    if (offset == trimmed.size()) {
        return std::nullopt;
    }

    int value = 0;
    for (std::size_t index = offset; index < trimmed.size(); ++index) {
        const char character = trimmed[index];
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const int digit = character - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

std::string_view SaveStateLabel(SampleLabelSaveStateKind kind)
{
    switch (kind) {
    case SampleLabelSaveStateKind::InternalDraftOnly:
        return "internal autosave draft";
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return "autosaved to output";
    case SampleLabelSaveStateKind::Pending:
        return "pending";
    case SampleLabelSaveStateKind::Failed:
        return "save failed";
    default:
        return "unknown";
    }
}

std::string SaveStateReminder(const SourceCollectionLabelingView& labeling_view)
{
    switch (labeling_view.save_state.kind) {
    case SampleLabelSaveStateKind::InternalDraftOnly:
        return "State: local draft only; choose an output path to create a reusable annotation result.";
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return "State: output file and metadata sidecar are saved.";
    case SampleLabelSaveStateKind::Pending:
        return "State: output/metadata autosave is pending; close is disabled until it finishes.";
    case SampleLabelSaveStateKind::Failed:
        return "State: output/metadata autosave failed; close is disabled until the save succeeds.";
    default:
        return "State: unknown save state.";
    }
}

bool AnnotationActivationNeedsConfirmation(SampleAnnotationWorkflowRelationship relationship)
{
    return relationship != SampleAnnotationWorkflowRelationship::LocalLabelingTask;
}

bool IsLabelShortcutPressed(char shortcut, bool context_active)
{
    if (!context_active) {
        return false;
    }
    const char normalized = NormalizeSampleLabelShortcut(shortcut);
    const ImGuiIO& io = ImGui::GetIO();
    if (normalized == '\0' || io.WantTextInput || io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper) {
        return false;
    }

    ImGuiKey key = ImGuiKey_None;
    if (normalized >= 'a' && normalized <= 'z') {
        key = static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_A) + (normalized - 'a'));
    } else if (normalized >= '0' && normalized <= '9') {
        key = static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_0) + (normalized - '0'));
    }
    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
}

std::string LabelButtonText(const SampleLabelDefinition& label)
{
    std::string text = label.name;
    text += " (";
    text += std::to_string(label.code);
    text += ")";
    if (label.shortcut != '\0') {
        text += " [";
        text.push_back(label.shortcut);
        text += "]";
    }
    return text;
}

}  // namespace

const char* SampleWorkflowPanelUi::LabelingWindowName()
{
    return kLabelingWindow;
}

const char* SampleWorkflowPanelUi::FiltersWindowName()
{
    return kFiltersWindow;
}

const char* SampleWorkflowPanelUi::SortingWindowName()
{
    return kSortingWindow;
}

void SampleWorkflowPanelUi::ResetForSampleWorkflow()
{
    std::fill(new_task_name_buffer_.begin(), new_task_name_buffer_.end(), '\0');
    std::snprintf(new_task_name_buffer_.data(), new_task_name_buffer_.size(), "%s", "Manual labeling");
    active_task_name_buffer_.fill('\0');
    active_task_name_buffer_task_id_.clear();
    std::fill(new_label_name_buffer_.begin(), new_label_name_buffer_.end(), '\0');
    std::snprintf(new_label_name_buffer_.data(), new_label_name_buffer_.size(), "%s", "bad");
    std::snprintf(new_label_code_buffer_.data(), new_label_code_buffer_.size(), "%d", 0);
    new_label_shortcut_buffer_.fill('\0');
    pending_annotation_activation_path_.clear();
    pending_annotation_activation_name_.clear();
    pending_delete_task_name_.clear();
    pending_annotation_activation_relationship_ = SampleAnnotationWorkflowRelationship::PlainAnnotation;
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderLabeling(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    bool plot_shortcut_context_active,
    bool* open,
    const std::function<std::optional<std::filesystem::path>()>& choose_output_path)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kLabelingWindow, open)) {
        ImGui::End();
        return action;
    }
    const bool labeling_context_active =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    const SourceCollectionLabelingView labeling_view = session_view.labeling;
    const std::optional<std::size_t> current_index = labeling_view.current_index;
    if (!labeling_view.has_active_source || !current_index) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    const float drop_target_width = std::max(160.0f, ImGui::GetContentRegionAvail().x);
    const float drop_target_height = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 drop_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("annotation_drop_target", ImVec2(drop_target_width, drop_target_height));
    const ImVec2 drop_max(drop_min.x + drop_target_width, drop_min.y + drop_target_height);
    const bool drop_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRect(
        drop_min,
        drop_max,
        ImGui::GetColorU32(drop_hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Border),
        3.0f);
    const char* drop_text =
        labeling_view.has_active_task ? "Close task to use annotation" : "Annotation target: start task";
    draw_list->AddText(
        ImVec2(drop_min.x + 8.0f, drop_min.y + 0.5f * (drop_target_height - ImGui::GetTextLineHeight())),
        ImGui::GetColorU32(ImGuiCol_TextDisabled),
        drop_text);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSampleAnnotationDragPayload)) {
            const std::filesystem::path annotation_path = Utf8ToPath(PayloadString(*payload));
            if (const SourceCollectionAnnotationValueView* annotation =
                    FindAnnotationViewByPath(session_view, annotation_path);
                annotation != nullptr && annotation->can_activate_labeling && !labeling_view.has_active_task) {
                if (AnnotationActivationNeedsConfirmation(annotation->relationship)) {
                    pending_annotation_activation_path_ = annotation->path;
                    pending_annotation_activation_name_ = annotation->name;
                    pending_annotation_activation_relationship_ = annotation->relationship;
                    ImGui::OpenPopup(kAnnotationToLabelingPopup);
                } else {
                    MergeSourceCollectionSessionAction(
                        action,
                        submit(ChangeActiveSampleWorkflow(
                                   ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(annotation->path)))
                            .action);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupModal(kAnnotationToLabelingPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Make \"%s\" editable in Labeling. Future autosaves will write to this annotation result and its "
            "metadata sidecar.",
            pending_annotation_activation_name_.c_str());
        ImGui::TextWrapped(
            "This edits the selected annotation result in place. Back up the file first if you need to preserve "
            "the original labels.");
        if (pending_annotation_activation_relationship_ ==
            SampleAnnotationWorkflowRelationship::PlainAnnotation) {
            ImGui::TextDisabled("No metadata sidecar is present; one will be created on save.");
        } else {
            ImGui::TextDisabled("Existing label metadata will be reused.");
        }
        if (ImGui::Button("Use annotation")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(
                           ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(
                               pending_annotation_activation_path_)))
                    .action);
            pending_annotation_activation_path_.clear();
            pending_annotation_activation_name_.clear();
            pending_annotation_activation_relationship_ =
                SampleAnnotationWorkflowRelationship::PlainAnnotation;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_annotation_activation_path_.clear();
            pending_annotation_activation_name_.clear();
            pending_annotation_activation_relationship_ =
                SampleAnnotationWorkflowRelationship::PlainAnnotation;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!labeling_view.has_active_task) {
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Task name", new_task_name_buffer_.data(), new_task_name_buffer_.size());
        if (ImGui::Button("Create task")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(
                           ActiveSampleWorkflowIntent::CreateLabelingTask(new_task_name_buffer_.data())))
                    .action);
        }
        ImGui::TextDisabled("No active task");
        ImGui::End();
        return action;
    }

    const int current_code = labeling_view.current_code;
    if (active_task_name_buffer_task_id_ != labeling_view.task_id) {
        active_task_name_buffer_task_id_ = labeling_view.task_id;
        CopyToBuffer(active_task_name_buffer_, labeling_view.task_name);
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Task name##ActiveLabelingTaskName", active_task_name_buffer_.data(), active_task_name_buffer_.size());
    ImGui::SameLine();
    const std::string requested_task_name = TrimAscii(active_task_name_buffer_.data());
    const bool rename_disabled = requested_task_name.empty() || requested_task_name == labeling_view.task_name;
    if (rename_disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Rename")) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(
                       ActiveSampleWorkflowIntent::RenameActiveLabelingTask(requested_task_name)))
                .action);
    }
    if (rename_disabled) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!labeling_view.can_deactivate_task) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Close task")) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::DeactivateActiveLabelingTask()))
                .action);
        ImGui::End();
        return action;
    }
    if (!labeling_view.can_deactivate_task) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Output autosave must finish before this task can be closed.");
        }
    }
    ImGui::SameLine();
    if (!labeling_view.can_delete_task) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Delete")) {
        pending_delete_task_name_ = labeling_view.task_name;
        ImGui::OpenPopup(kDeleteLabelingTaskPopup);
    }
    if (!labeling_view.can_delete_task) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Output autosave must finish before this task can be deleted.");
        }
    }
    if (ImGui::BeginPopupModal(kDeleteLabelingTaskPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Delete local task \"%s\". Output files are not deleted.",
            pending_delete_task_name_.c_str());
        if (ImGui::Button("Delete task")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::DeleteActiveLabelingTask()))
                    .action);
            pending_delete_task_name_.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::End();
            return action;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_delete_task_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Text(
        "Progress: %llu labeled / %llu",
        static_cast<unsigned long long>(labeling_view.labeled_count),
        static_cast<unsigned long long>(labeling_view.sample_count));
    const std::string current_value = FormatSampleLabelValue(labeling_view.label_set, current_code);
    ImGui::Text("Current: %s", current_value.c_str());
    if (labeling_view.remembered_position && *labeling_view.remembered_position < labeling_view.sample_count &&
        *labeling_view.remembered_position != *current_index) {
        ImGui::Text(
            "Remembered row: %llu",
            static_cast<unsigned long long>(*labeling_view.remembered_position));
        ImGui::SameLine();
        const bool resume_available = CanResumeRememberedRow(
            labeling_view,
            *labeling_view.remembered_position,
            labeling_view.sample_count);
        if (!resume_available) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Resume")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(UpdateSampleNavigation(SampleNavigationIntent::Move(
                           SampleNavigationRequest::LocateSourceRowInSequence(*labeling_view.remembered_position))))
                    .action);
        }
        if (!resume_available) {
            ImGui::EndDisabled();
        }
    }

    const std::string_view save_state = SaveStateLabel(labeling_view.save_state.kind);
    if (labeling_view.save_state.kind == SampleLabelSaveStateKind::Pending ||
        labeling_view.save_state.kind == SampleLabelSaveStateKind::Failed) {
        ImGui::Text(
            "Save: %.*s (%llu)",
            static_cast<int>(save_state.size()),
            save_state.data(),
            static_cast<unsigned long long>(labeling_view.save_state.pending_count));
    } else {
        ImGui::Text("Save: %.*s", static_cast<int>(save_state.size()), save_state.data());
    }
    if (!labeling_view.save_state.message.empty()) {
        ImGui::TextDisabled("%s", labeling_view.save_state.message.c_str());
    }
    const std::string save_state_reminder = SaveStateReminder(labeling_view);
    ImGui::TextDisabled("%s", save_state_reminder.c_str());
    if (labeling_view.state_save_failed) {
        const std::string_view error = labeling_view.state_save_error;
        ImGui::TextDisabled(
            "Local task record: %.*s",
            static_cast<int>(error.size()),
            error.data());
    }
    if (!labeling_view.state_load_warning.empty()) {
        const std::string_view warning = labeling_view.state_load_warning;
        ImGui::TextDisabled(
            "Local task record: %.*s",
            static_cast<int>(warning.size()),
            warning.data());
    }
    if (labeling_view.output_path) {
        const std::string path = PathToUtf8(*labeling_view.output_path);
        ImGui::TextDisabled("%s", path.c_str());
    }

    ImGui::Spacing();
    bool auto_advance = labeling_view.auto_advance;
    if (ImGui::Checkbox("Auto-advance", &auto_advance)) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(
                       ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(auto_advance)))
                .action);
    }
    ImGui::SameLine();
    if (!auto_advance) {
        ImGui::BeginDisabled();
    }
    bool skip_labeled_on_advance = labeling_view.skip_labeled_on_advance;
    if (ImGui::Checkbox("Skip labeled", &skip_labeled_on_advance)) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(
                       ActiveSampleWorkflowIntent::SetActiveLabelingSkipLabeledOnAdvance(skip_labeled_on_advance)))
                .action);
    }
    if (!auto_advance) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose output...")) {
        if (std::optional<std::filesystem::path> path = choose_output_path()) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::SetActiveLabelingOutputPath(*path)))
                    .action);
        }
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(72.0f);
    ImGui::InputText("Code", new_label_code_buffer_.data(), new_label_code_buffer_.size());
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("Name", new_label_name_buffer_.data(), new_label_name_buffer_.size());
    ImGui::SetNextItemWidth(72.0f);
    ImGui::InputText("Shortcut", new_label_shortcut_buffer_.data(), new_label_shortcut_buffer_.size());
    const std::optional<int> new_code = ParseInt(new_label_code_buffer_.data());
    if (ImGui::Button("Add label") && new_code) {
        const char shortcut = new_label_shortcut_buffer_[0];
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UpsertActiveLabel(
                SampleLabelDefinition{*new_code, new_label_name_buffer_.data(), shortcut})));
        MergeSourceCollectionSessionAction(action, result.action);
        if (result.changed) {
            const SourceCollectionLabelingView& refreshed_view = result.view.labeling;
            const SampleLabelSet& label_set =
                refreshed_view.has_active_task ? refreshed_view.label_set : labeling_view.label_set;
            std::snprintf(
                new_label_code_buffer_.data(),
                new_label_code_buffer_.size(),
                "%d",
                NextAvailableSampleLabelCode(label_set));
            new_label_shortcut_buffer_.fill('\0');
        }
    }

    ImGui::Separator();
    std::optional<int> label_code_to_assign;
    bool clear_label_requested = false;
    for (const SampleLabelDefinition& label : labeling_view.label_set.labels) {
        if (IsLabelShortcutPressed(label.shortcut, labeling_context_active || plot_shortcut_context_active)) {
            label_code_to_assign = label.code;
            clear_label_requested = false;
        }
    }

    for (const SampleLabelDefinition& label : labeling_view.label_set.labels) {
        const bool selected = current_code == label.code;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        }
        const std::string text = LabelButtonText(label);
        if (ImGui::Button(text.c_str())) {
            label_code_to_assign = label.code;
            clear_label_requested = false;
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    }

    const bool clear_disabled = current_code == kUnlabeledSampleLabelCode;
    if (clear_disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Clear")) {
        label_code_to_assign.reset();
        clear_label_requested = true;
    }
    if (clear_disabled) {
        ImGui::EndDisabled();
    }

    if (label_code_to_assign) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(
                       ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(*label_code_to_assign)))
                .action);
    } else if (clear_label_requested) {
        MergeSourceCollectionSessionAction(
            action,
            submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample()))
                .action);
    }

    ImGui::End();
    return action;
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderFilters(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    bool* open)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kFiltersWindow, open)) {
        ImGui::End();
        return action;
    }

    SourceCollectionFilterView filter_view = session_view.filter;
    if (!filter_view.has_active_source) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    if (filter_view.has_active_labeling_task) {
        bool use_labeling_source = filter_view.active_labeling_filter_source_selected;
        if (ImGui::Checkbox("Use active labeling task", &use_labeling_source)) {
            SourceCollectionSessionResult result =
                submit(ApplySampleFiltering(
                    SampleFilteringIntent::SetActiveLabelingSourceSelected(use_labeling_source)));
            MergeSourceCollectionSessionAction(action, result.action);
            filter_view = result.view.filter;
        }
        if (!use_labeling_source) {
            ImGui::TextDisabled("Labeling task filters are not selected.");
        }
    }

    ImGui::Text(
        "Visible: %llu / %llu",
        static_cast<unsigned long long>(filter_view.evaluation.included_count),
        static_cast<unsigned long long>(filter_view.sample_count));
    if (filter_view.navigation_filter_active && !filter_view.current_sample_in_filter) {
        ImGui::TextDisabled("Current sample is outside the active filter");
    }
    if (ImGui::Button("Clear filters")) {
        SourceCollectionSessionResult result = submit(ApplySampleFiltering(SampleFilteringIntent::Clear()));
        MergeSourceCollectionSessionAction(action, result.action);
        filter_view = result.view.filter;
    }

    for (const std::string& message : filter_view.evaluation.messages) {
        ImGui::TextDisabled("%s", message.c_str());
    }

    ImGui::Separator();
    bool has_filterable_source = false;
    for (const SourceCollectionFilterSourceView& source_view : filter_view.sources) {
        if (!source_view.filterable) {
            continue;
        }
        has_filterable_source = true;

        ImGui::PushID(source_view.id.c_str());
        if (ImGui::TreeNodeEx(source_view.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const SampleFilterValueOption& option : source_view.options) {
                bool selected = source_view.selected_value_keys.find(option.key) !=
                                source_view.selected_value_keys.end();
                std::string label = option.display_text;
                label += "  ";
                label += std::to_string(option.sample_count);
                ImGui::PushID(option.key.c_str());
                if (ImGui::Checkbox(label.c_str(), &selected)) {
                    MergeSourceCollectionSessionAction(
                        action,
                        submit(ApplySampleFiltering(SampleFilteringIntent::SetFilterValueSelected(
                                   source_view.id,
                                   option.key,
                                   selected)))
                            .action);
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (!has_filterable_source) {
        ImGui::TextDisabled("No filterable annotations");
    }

    ImGui::End();
    return action;
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderSorting(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    bool* open)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kSortingWindow, open)) {
        ImGui::End();
        return action;
    }

    SourceCollectionSampleSortingView sorting_view = session_view.sorting;
    SourceCollectionNavigationView navigation = session_view.navigation;
    if (!sorting_view.has_active_source) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    if (navigation.sequence_active) {
        if (navigation.current_sequence_position) {
            ImGui::Text(
                "Position: %llu / %llu",
                static_cast<unsigned long long>(*navigation.current_sequence_position + 1),
                static_cast<unsigned long long>(navigation.sequence_count));
        } else {
            ImGui::Text("Position: - / %llu", static_cast<unsigned long long>(navigation.sequence_count));
        }
        ImGui::Text(
            "Source rows: %llu / %llu",
            static_cast<unsigned long long>(navigation.sequence_count),
            static_cast<unsigned long long>(navigation.sample_count));
    } else {
        ImGui::TextDisabled("Source order");
    }
    ImGui::Separator();

    if (ImGui::RadioButton("Source order", !sorting_view.active)) {
        SourceCollectionSessionResult result =
            submit(ApplySampleSorting(SampleSortingIntent::Clear()));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = result.view.sorting;
        navigation = result.view.navigation;
    }

    bool has_sort_source = false;
    const std::vector<SourceCollectionSampleSortSourceView> sort_sources = sorting_view.sources;
    for (const SourceCollectionSampleSortSourceView& source_view : sort_sources) {
        has_sort_source = true;
        ImGui::PushID(source_view.id.c_str());
        if (ImGui::RadioButton(source_view.name.c_str(), source_view.selected)) {
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(SampleSortingIntent::SetSortSource(source_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = result.view.sorting;
            navigation = result.view.navigation;
        }
        ImGui::PopID();
    }
    if (!has_sort_source) {
        ImGui::TextDisabled("No comparable sort sources");
    }

    ImGui::Separator();
    const bool ascending = sorting_view.direction == SampleNavigationSortDirection::Ascending;
    if (ImGui::RadioButton("Ascending", ascending)) {
        SourceCollectionSessionResult result = submit(ApplySampleSorting(
            SampleSortingIntent::SetSortDirection(SampleNavigationSortDirection::Ascending)));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = result.view.sorting;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Descending", !ascending)) {
        SourceCollectionSessionResult result = submit(ApplySampleSorting(
            SampleSortingIntent::SetSortDirection(SampleNavigationSortDirection::Descending)));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = result.view.sorting;
    }

    ImGui::End();
    return action;
}

}  // namespace specforge
