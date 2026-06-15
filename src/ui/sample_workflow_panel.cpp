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

SampleNavigationRequest BuildAutoAdvanceRequest(const SampleLabelingTask& task)
{
    if (!task.skip_labeled_on_advance) {
        return SampleNavigationRequest::LabelAdvance();
    }

    std::vector<bool> eligible_samples;
    eligible_samples.reserve(task.values.size());
    for (int value : task.values) {
        eligible_samples.push_back(value == kUnlabeledSampleLabelCode);
    }
    return SampleNavigationRequest::LabelAdvanceToEligible(std::move(eligible_samples));
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
    selected_labeling_filter_source_id_.reset();
}

std::vector<SampleFilterSource> SampleWorkflowPanelUi::BuildFilterSources(
    const SampleNavigationController& navigation,
    const SampleLabelingController& labeling) const
{
    std::vector<SampleFilterSource> sources;
    const SampleCollectionContext* context = navigation.active_context();
    if (context != nullptr) {
        sources.reserve(context->annotations.size() + 1);
        for (const SampleAnnotationResult& annotation : context->annotations) {
            sources.push_back(BuildAnnotationFilterSource(annotation));
        }
    }

    if (const SampleLabelingTask* task = labeling.active_task()) {
        SampleFilterSource labeling_source = BuildLabelingFilterSource(*task);
        if (selected_labeling_filter_source_id_ &&
            *selected_labeling_filter_source_id_ == labeling_source.id) {
            sources.push_back(std::move(labeling_source));
        }
    }
    return sources;
}

void SampleWorkflowPanelUi::RenderLabeling(
    const SpectrumSnapshotHandle& snapshot,
    SampleNavigationController& navigation,
    SampleLabelingController& labeling,
    bool plot_shortcut_context_active,
    const std::function<SampleNavigationResult(const SampleNavigationRequest&)>& request_navigation,
    const std::function<void()>& apply_filters,
    const std::function<std::optional<std::filesystem::path>()>& choose_output_path)
{
    ImGui::Begin(kLabelingWindow);
    const bool labeling_context_active =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    const std::optional<std::size_t> current_index = navigation.current_index();
    if (!snapshot || snapshot->source.path.empty() || !current_index) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return;
    }

    SampleLabelingTask* task = labeling.active_task();
    if (task == nullptr) {
        if (ImGui::Button("Create task")) {
            task = labeling.CreateTask("manual-labeling", "Manual labeling");
        }
        if (task == nullptr) {
            ImGui::TextDisabled("No active task");
            ImGui::End();
            return;
        }
    }

    const int current_code =
        *current_index < task->values.size() ? task->values[*current_index] : kUnlabeledSampleLabelCode;
    ImGui::TextUnformatted(task->task_name.c_str());
    ImGui::Text(
        "Progress: %llu labeled / %llu",
        static_cast<unsigned long long>(CountLabeledSamples(*task)),
        static_cast<unsigned long long>(task->values.size()));
    const std::string current_value = FormatSampleLabelValue(task->label_set, current_code);
    ImGui::Text("Current: %s", current_value.c_str());
    if (task->remembered_position && *task->remembered_position < task->values.size() &&
        *task->remembered_position != *current_index) {
        ImGui::Text(
            "Remembered row: %llu",
            static_cast<unsigned long long>(*task->remembered_position));
        ImGui::SameLine();
        if (ImGui::Button("Resume")) {
            (void)request_navigation(SampleNavigationRequest::LocateRow(*task->remembered_position));
        }
    }

    const std::string_view save_state = SaveStateLabel(task->save_state.kind);
    if (task->save_state.kind == SampleLabelSaveStateKind::Pending ||
        task->save_state.kind == SampleLabelSaveStateKind::Failed) {
        ImGui::Text(
            "Save: %.*s (%llu)",
            static_cast<int>(save_state.size()),
            save_state.data(),
            static_cast<unsigned long long>(task->save_state.pending_count));
    } else {
        ImGui::Text("Save: %.*s", static_cast<int>(save_state.size()), save_state.data());
    }
    if (!task->save_state.message.empty()) {
        ImGui::TextDisabled("%s", task->save_state.message.c_str());
    }
    if (labeling.state_save_failed()) {
        const std::string_view error = labeling.state_save_error();
        ImGui::TextDisabled(
            "Local task record: %.*s",
            static_cast<int>(error.size()),
            error.data());
    }
    if (!labeling.state_load_warning().empty()) {
        const std::string_view warning = labeling.state_load_warning();
        ImGui::TextDisabled(
            "Local task record: %.*s",
            static_cast<int>(warning.size()),
            warning.data());
    }
    if (task->output_path) {
        const std::string path = PathToUtf8(*task->output_path);
        ImGui::TextDisabled("%s", path.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Auto-advance", &task->auto_advance)) {
        (void)labeling.PersistActiveTaskRecord();
    }
    ImGui::SameLine();
    if (!task->auto_advance) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Skip labeled", &task->skip_labeled_on_advance)) {
        (void)labeling.PersistActiveTaskRecord();
    }
    if (!task->auto_advance) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose output...")) {
        if (std::optional<std::filesystem::path> path = choose_output_path()) {
            (void)labeling.SetActiveTaskOutputPath(*path);
            (void)labeling.PersistActiveTask();
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
        if (labeling.UpsertActiveLabel(SampleLabelDefinition{*new_code, new_label_name_buffer_.data(), shortcut})) {
            std::snprintf(
                new_label_code_buffer_.data(),
                new_label_code_buffer_.size(),
                "%d",
                NextAvailableSampleLabelCode(task->label_set));
            new_label_shortcut_buffer_.fill('\0');
            apply_filters();
        }
    }

    ImGui::Separator();
    bool wrote_label = false;
    SampleLabelWriteResult write_result;
    for (const SampleLabelDefinition& label : task->label_set.labels) {
        if (IsLabelShortcutPressed(label.shortcut, labeling_context_active || plot_shortcut_context_active)) {
            write_result = labeling.AssignLabel(*current_index, label.code);
            wrote_label = write_result.changed;
        }
    }

    for (const SampleLabelDefinition& label : task->label_set.labels) {
        const bool selected = current_code == label.code;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        }
        const std::string text = LabelButtonText(label);
        if (ImGui::Button(text.c_str())) {
            write_result = labeling.AssignLabel(*current_index, label.code);
            wrote_label = write_result.changed;
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
        write_result = labeling.ClearLabel(*current_index);
        wrote_label = write_result.changed;
    }
    if (clear_disabled) {
        ImGui::EndDisabled();
    }

    if (wrote_label) {
        const std::optional<SampleNavigationRequest> advance_request =
            write_result.advance_requested ? std::optional<SampleNavigationRequest>(BuildAutoAdvanceRequest(*task))
                                           : std::nullopt;
        if (task->output_path) {
            (void)labeling.PersistActiveTask();
        }
        if (advance_request) {
            const SampleNavigationResult advance_result = request_navigation(*advance_request);
            if (advance_result.has_active_source) {
                (void)labeling.RememberActivePosition(advance_result.current_index);
            }
        }
        apply_filters();
    }

    ImGui::End();
}

void SampleWorkflowPanelUi::RenderFilters(
    const SpectrumSnapshotHandle& snapshot,
    SampleNavigationController& navigation,
    SampleLabelingController& labeling,
    SampleFilterController& filters,
    const std::function<void()>& apply_filters)
{
    ImGui::Begin(kFiltersWindow);

    const std::size_t sample_count =
        navigation.spectrum_count().value_or(snapshot ? snapshot->collection.spectrum_count : 0);
    if (!snapshot || snapshot->source.path.empty() || sample_count == 0) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return;
    }

    if (const SampleLabelingTask* task = labeling.active_task()) {
        const SampleFilterSource active_labeling_source = BuildLabelingFilterSource(*task);
        bool use_labeling_source = selected_labeling_filter_source_id_ &&
                                   *selected_labeling_filter_source_id_ == active_labeling_source.id;
        if (ImGui::Checkbox("Use active labeling task", &use_labeling_source)) {
            if (selected_labeling_filter_source_id_ &&
                *selected_labeling_filter_source_id_ != active_labeling_source.id) {
                filters.ClearCondition(*selected_labeling_filter_source_id_);
            }
            if (use_labeling_source) {
                selected_labeling_filter_source_id_ = active_labeling_source.id;
            } else {
                filters.ClearCondition(active_labeling_source.id);
                selected_labeling_filter_source_id_.reset();
            }
            apply_filters();
        }
        if (!use_labeling_source) {
            ImGui::TextDisabled("Labeling task filters are not selected.");
        }
    }

    const std::vector<SampleFilterSource> sources = BuildFilterSources(navigation, labeling);
    const SampleFilterEvaluation evaluation = filters.Evaluate(sources, sample_count);
    ImGui::Text(
        "Visible: %llu / %llu",
        static_cast<unsigned long long>(evaluation.included_count),
        static_cast<unsigned long long>(sample_count));
    if (navigation.filter_active() && !navigation.current_sample_in_filter()) {
        ImGui::TextDisabled("Current sample is outside the active filter");
    }
    if (ImGui::Button("Clear filters")) {
        filters.Clear();
        apply_filters();
    }

    for (const std::string& message : evaluation.messages) {
        ImGui::TextDisabled("%s", message.c_str());
    }

    ImGui::Separator();
    bool has_filterable_source = false;
    for (const SampleFilterSource& source : sources) {
        if (!source.filterable) {
            continue;
        }
        has_filterable_source = true;
        const SampleFilterCondition* condition = filters.FindCondition(source.id);
        std::unordered_set<std::string> selected_values =
            condition == nullptr ? std::unordered_set<std::string>{} : condition->allowed_value_keys;

        ImGui::PushID(source.id.c_str());
        if (ImGui::TreeNodeEx(source.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const SampleFilterValueOption& option : source.options) {
                bool selected = selected_values.find(option.key) != selected_values.end();
                std::string label = option.display_text;
                label += "  ";
                label += std::to_string(option.sample_count);
                ImGui::PushID(option.key.c_str());
                if (ImGui::Checkbox(label.c_str(), &selected)) {
                    if (selected) {
                        selected_values.insert(option.key);
                    } else {
                        selected_values.erase(option.key);
                    }
                    filters.SetCondition(source.id, selected_values);
                    apply_filters();
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
}

}  // namespace specforge
