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

namespace specforge {
namespace {

constexpr const char* kLabelingWindow = "Labeling###SpecForgeLabelingV1";
constexpr const char* kFiltersWindow = "Filters###SpecForgeFiltersV1";

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

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
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

void SampleWorkflowPanelUi::ResetForSampleWorkflow()
{
    std::fill(new_label_name_buffer_.begin(), new_label_name_buffer_.end(), '\0');
    std::snprintf(new_label_name_buffer_.data(), new_label_name_buffer_.size(), "%s", "bad");
    std::snprintf(new_label_code_buffer_.data(), new_label_code_buffer_.size(), "%d", 0);
    new_label_shortcut_buffer_.fill('\0');
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

    if (!labeling_view.has_active_task) {
        if (ImGui::Button("Create task")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::CreateDefaultLabelingTask())).action);
        }
        ImGui::TextDisabled("No active task");
        ImGui::End();
        return action;
    }

    const int current_code = labeling_view.current_code;
    ImGui::TextUnformatted(labeling_view.task_name.c_str());
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
        if (ImGui::Button("Resume")) {
            MergeSourceCollectionSessionAction(
                action,
                submit(UpdateSampleNavigation(SampleNavigationIntent::Move(
                           SampleNavigationRequest::LocateRow(*labeling_view.remembered_position))))
                    .action);
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

}  // namespace specforge
