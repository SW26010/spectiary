#include "ui/sample_workflow_panel.h"

#include "app/local_user_state.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr const char* kAddSampleFilterSourcePopup =
    "Add sample filter source###SpecForgeAddSampleFilterSourcePopup";
constexpr const char* kAddSampleSortSourcePopup =
    "Add sample sort source###SpecForgeAddSampleSortSourcePopup";
constexpr const char* kDeleteLabelingTaskPopup =
    "Delete labeling task?###SpecForgeDeleteLabelingTaskPopup";
constexpr const char* kDeleteSampleLabelPopup = "Delete label?###SpecForgeDeleteSampleLabelPopup";
constexpr const char* kChangeSampleLabelCodePopup =
    "Change used label code?###SpecForgeChangeSampleLabelCodePopup";

enum class ActionIcon {
    Minus,
    Pencil,
    Trash,
    Check,
    Close,
};

struct SampleSortSourceRowAction {
    bool activate = false;
    bool toggle_direction = false;
    bool remove = false;
};

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

void DrawTableCellText(int column, std::string_view text)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::TableSetColumnIndex(column);
    const ImRect cell_rect = ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), column);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float text_y =
        cell_rect.Min.y + std::max(0.0f, (cell_rect.GetHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::PushClipRect(cell_rect.Min, cell_rect.Max, true);
    draw_list->AddText(
        ImVec2(cell_rect.Min.x + style.CellPadding.x, text_y),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.data(),
        text.data() + text.size());
    ImGui::PopClipRect();
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

const SourceCollectionFilterSourceView* FindAvailableFilterSourceByPath(
    const SourceCollectionFilterView& filter_view,
    const std::filesystem::path& path)
{
    const std::string path_text = PathToUtf8(path);
    const auto match = std::find_if(
        filter_view.available_sources.begin(),
        filter_view.available_sources.end(),
        [&path_text](const SourceCollectionFilterSourceView& source) {
            return PathToUtf8(source.annotation_path) == path_text;
        });
    return match == filter_view.available_sources.end() ? nullptr : &*match;
}

const SourceCollectionSampleSortSourceView* FindAvailableSortSourceByPath(
    const SourceCollectionSampleSortingView& sorting_view,
    const std::filesystem::path& path)
{
    const std::string path_text = PathToUtf8(path);
    const auto match = std::find_if(
        sorting_view.available_sources.begin(),
        sorting_view.available_sources.end(),
        [&path_text](const SourceCollectionSampleSortSourceView& source) {
            return PathToUtf8(source.annotation_path) == path_text;
        });
    return match == sorting_view.available_sources.end() ? nullptr : &*match;
}

float ActionIconButtonWidth()
{
    return ImGui::GetFrameHeight() * 0.5f;
}

void CollapseSampleFilterSourceTree(std::string_view source_id)
{
    ImGui::PushID(source_id.data(), source_id.data() + source_id.size());
    ImGui::GetStateStorage()->SetInt(ImGui::GetID("source"), 0);
    ImGui::PopID();
}

void DrawActionIcon(ImDrawList* draw_list, const ImRect& hit_rect, ActionIcon icon, ImU32 color)
{
    const float size = std::min(
        ActionIconButtonWidth(),
        std::max(1.0f, std::min(hit_rect.GetWidth(), hit_rect.GetHeight())));
    const float left = hit_rect.Min.x + (hit_rect.GetWidth() - size) * 0.5f;
    const float top = hit_rect.Min.y + (hit_rect.GetHeight() - size) * 0.5f;
    const float right = left + size;
    const float bottom = top + size;
    const float stroke = 1.35f;

    switch (icon) {
    case ActionIcon::Minus:
        draw_list->AddLine(
            ImVec2(left + size * 0.18f, top + size * 0.5f),
            ImVec2(right - size * 0.18f, top + size * 0.5f),
            color,
            stroke);
        break;
    case ActionIcon::Pencil: {
        const ImVec2 tip(left + size * 0.16f, bottom - size * 0.16f);
        const ImVec2 end(right - size * 0.14f, top + size * 0.14f);
        draw_list->AddLine(tip, end, color, 2.2f);
        draw_list->AddLine(
            ImVec2(end.x - size * 0.10f, end.y - size * 0.04f),
            ImVec2(end.x + size * 0.04f, end.y + size * 0.10f),
            color,
            stroke);
        draw_list->AddTriangleFilled(
            tip,
            ImVec2(tip.x + size * 0.05f, tip.y - size * 0.15f),
            ImVec2(tip.x + size * 0.15f, tip.y - size * 0.05f),
            color);
        break;
    }
    case ActionIcon::Trash:
        draw_list->AddLine(
            ImVec2(left + size * 0.38f, top + size * 0.25f),
            ImVec2(left + size * 0.62f, top + size * 0.25f),
            color,
            stroke);
        draw_list->AddLine(
            ImVec2(left + size * 0.14f, top + size * 0.34f),
            ImVec2(right - size * 0.14f, top + size * 0.34f),
            color,
            stroke);
        draw_list->AddRect(
            ImVec2(left + size * 0.19f, top + size * 0.43f),
            ImVec2(right - size * 0.19f, top + size * 0.73f),
            color,
            2.0f,
            0,
            stroke);
        draw_list->AddLine(
            ImVec2(left + size * 0.43f, top + size * 0.49f),
            ImVec2(left + size * 0.43f, top + size * 0.68f),
            color,
            1.0f);
        draw_list->AddLine(
            ImVec2(left + size * 0.57f, top + size * 0.49f),
            ImVec2(left + size * 0.57f, top + size * 0.68f),
            color,
            1.0f);
        break;
    case ActionIcon::Check:
        draw_list->AddLine(
            ImVec2(left + size * 0.16f, top + size * 0.53f),
            ImVec2(left + size * 0.40f, top + size * 0.76f),
            color,
            1.6f);
        draw_list->AddLine(
            ImVec2(left + size * 0.40f, top + size * 0.76f),
            ImVec2(right - size * 0.12f, top + size * 0.20f),
            color,
            1.6f);
        break;
    case ActionIcon::Close:
        draw_list->AddLine(
            ImVec2(left + size * 0.20f, top + size * 0.20f),
            ImVec2(right - size * 0.20f, bottom - size * 0.20f),
            color,
            1.5f);
        draw_list->AddLine(
            ImVec2(right - size * 0.20f, top + size * 0.20f),
            ImVec2(left + size * 0.20f, bottom - size * 0.20f),
            color,
            1.5f);
        break;
    }
}

bool HiddenActionIconButton(
    const char* id,
    const ImRect& hit_rect,
    ActionIcon icon,
    const char* tooltip,
    bool reveal_icon)
{
    const ImGuiID item_id = ImGui::GetID(id);
    const bool item_visible = ImGui::ItemAdd(hit_rect, item_id, &hit_rect, ImGuiItemFlags_AllowOverlap);
    bool hovered = false;
    bool held = false;
    const bool clicked = item_visible && ImGui::ButtonBehavior(hit_rect, item_id, &hovered, &held);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (hovered || held) {
        const ImU32 background = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
        draw_list->AddRectFilled(hit_rect.Min, hit_rect.Max, background, 3.0f);
    }

    const bool draw_icon = item_visible && (reveal_icon || hovered || held);
    const ImU32 icon_color = ImGui::GetColorU32(ImGuiCol_Text);
    if (draw_icon) {
        DrawActionIcon(draw_list, hit_rect, icon, icon_color);
    }

    if (item_visible && hovered && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool ActionIconButton(
    const char* id,
    const ImVec2& size,
    ActionIcon icon,
    const char* tooltip,
    bool reveal_icon)
{
    const ImVec2 button_min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const ImRect hit_rect(button_min, ImVec2(button_min.x + size.x, button_min.y + size.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (hovered || held) {
        const ImU32 background = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
        draw_list->AddRectFilled(hit_rect.Min, hit_rect.Max, background, 3.0f);
    }

    const bool draw_icon = reveal_icon || hovered || held;
    const ImU32 icon_color = ImGui::GetColorU32(ImGuiCol_Text);
    if (draw_icon) {
        DrawActionIcon(draw_list, hit_rect, icon, icon_color);
    }

    if (hovered && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool DirectionIconButton(
    const char* id,
    SampleNavigationSortDirection direction,
    bool active,
    const char* tooltip)
{
    const ImVec2 size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::PushID(id);
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        active ? style.Colors[ImGuiCol_HeaderActive] : style.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        active ? style.Colors[ImGuiCol_HeaderHovered] : style.Colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        active ? style.Colors[ImGuiCol_HeaderActive] : style.Colors[ImGuiCol_ButtonActive]);
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        active ? style.Colors[ImGuiCol_Text] : style.Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FrameBorderSize,
        active ? std::max(1.0f, style.FrameBorderSize) : style.FrameBorderSize);
    const bool clicked = ImGui::Button("##button", size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    const ImRect hit_rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const ImU32 icon_color = ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float center_x = hit_rect.Min.x + hit_rect.GetWidth() * 0.5f;
    const float top = hit_rect.Min.y + hit_rect.GetHeight() * 0.24f;
    const float bottom = hit_rect.Min.y + hit_rect.GetHeight() * 0.76f;
    const float head_width = hit_rect.GetWidth() * 0.20f;
    const float head_height = hit_rect.GetHeight() * 0.18f;
    const float stroke = active ? 1.8f : 1.35f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (direction == SampleNavigationSortDirection::Ascending) {
        draw_list->AddLine(ImVec2(center_x, bottom), ImVec2(center_x, top), icon_color, stroke);
        draw_list->AddLine(ImVec2(center_x, top), ImVec2(center_x - head_width, top + head_height), icon_color, stroke);
        draw_list->AddLine(ImVec2(center_x, top), ImVec2(center_x + head_width, top + head_height), icon_color, stroke);
    } else {
        draw_list->AddLine(ImVec2(center_x, top), ImVec2(center_x, bottom), icon_color, stroke);
        draw_list->AddLine(ImVec2(center_x, bottom), ImVec2(center_x - head_width, bottom - head_height), icon_color, stroke);
        draw_list->AddLine(ImVec2(center_x, bottom), ImVec2(center_x + head_width, bottom - head_height), icon_color, stroke);
    }

    if (ImGui::IsItemHovered() && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopID();
    return clicked;
}

std::optional<std::string> AcceptSampleFilterSourceDrop(
    const SourceCollectionFilterView& filter_view,
    bool& accepted)
{
    accepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSampleAnnotationDragPayload, flags)) {
        const std::filesystem::path annotation_path = Utf8ToPath(PayloadString(*payload));
        const SourceCollectionFilterSourceView* source =
            FindAvailableFilterSourceByPath(filter_view, annotation_path);
        if (source == nullptr) {
            return std::nullopt;
        }
        accepted = true;
        if (payload->IsDelivery()) {
            return source->id;
        }
    }
    return std::nullopt;
}

std::optional<std::string> RenderSampleFilterDropTarget(
    const SourceCollectionFilterView& filter_view,
    const ImRect& hit_rect)
{
    if (hit_rect.GetWidth() <= 0.0f || hit_rect.GetHeight() <= 0.0f) {
        return std::nullopt;
    }

    bool accepted = false;
    std::optional<std::string> dropped_source;
    if (ImGui::BeginDragDropTargetCustom(hit_rect, ImGui::GetID("sample_filter_panel_drop_target"))) {
        dropped_source = AcceptSampleFilterSourceDrop(filter_view, accepted);
        ImGui::EndDragDropTarget();
    }
    if (accepted) {
        ImGui::GetWindowDrawList()->AddRect(
            hit_rect.Min,
            hit_rect.Max,
            ImGui::GetColorU32(ImGuiCol_DragDropTarget),
            3.0f,
            0,
            2.0f);
    }
    return dropped_source;
}

std::optional<std::string> AcceptSampleSortSourceDrop(
    const SourceCollectionSampleSortingView& sorting_view,
    bool& accepted)
{
    accepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSampleAnnotationDragPayload, flags)) {
        const std::filesystem::path annotation_path = Utf8ToPath(PayloadString(*payload));
        const SourceCollectionSampleSortSourceView* source =
            FindAvailableSortSourceByPath(sorting_view, annotation_path);
        if (source == nullptr) {
            return std::nullopt;
        }
        accepted = true;
        if (payload->IsDelivery()) {
            return source->id;
        }
    }
    return std::nullopt;
}

std::optional<std::string> RenderSampleSortDropTarget(
    const SourceCollectionSampleSortingView& sorting_view,
    const ImRect& hit_rect)
{
    if (hit_rect.GetWidth() <= 0.0f || hit_rect.GetHeight() <= 0.0f) {
        return std::nullopt;
    }

    bool accepted = false;
    std::optional<std::string> dropped_source;
    if (ImGui::BeginDragDropTargetCustom(hit_rect, ImGui::GetID("sample_sort_panel_drop_target"))) {
        dropped_source = AcceptSampleSortSourceDrop(sorting_view, accepted);
        ImGui::EndDragDropTarget();
    }
    if (accepted) {
        ImGui::GetWindowDrawList()->AddRect(
            hit_rect.Min,
            hit_rect.Max,
            ImGui::GetColorU32(ImGuiCol_DragDropTarget),
            3.0f,
            0,
            2.0f);
    }
    return dropped_source;
}

SampleNavigationSortDirection OppositeSortDirection(SampleNavigationSortDirection direction)
{
    return direction == SampleNavigationSortDirection::Ascending
        ? SampleNavigationSortDirection::Descending
        : SampleNavigationSortDirection::Ascending;
}

const char* SortDirectionTooltip(SampleNavigationSortDirection direction)
{
    return direction == SampleNavigationSortDirection::Ascending
        ? "Ascending"
        : "Descending";
}

SampleSortSourceRowAction RenderSampleSortSourceRow(
    const SourceCollectionSampleSortSourceView& source_view,
    SampleNavigationSortDirection direction)
{
    SampleSortSourceRowAction action;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frame_height = ImGui::GetFrameHeight();
    const float remove_width = source_view.removable ? frame_height : 0.0f;
    const float inner_spacing = style.ItemInnerSpacing.x;
    const float available_width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float reserved_width =
        frame_height + inner_spacing +
        (source_view.removable ? remove_width + inner_spacing : 0.0f);
    const float label_width = std::max(1.0f, available_width - reserved_width);

    ImGui::PushID(source_view.id.c_str());
    if (DirectionIconButton("direction", direction, source_view.selected, SortDirectionTooltip(direction))) {
        if (source_view.selected) {
            action.toggle_direction = true;
        } else {
            action.activate = true;
        }
    }

    ImGui::SameLine(0.0f, inner_spacing);
    const ImVec2 label_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("label", ImVec2(label_width, frame_height));
    if (ImGui::IsItemClicked() && !source_view.selected) {
        action.activate = true;
    }
    if (ImGui::IsItemHovered()) {
        const std::string tooltip = source_view.annotation_path.empty()
            ? source_view.name
            : UserPathDisplayText(source_view.annotation_path);
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
    const ImRect label_rect(label_min, ImVec2(label_min.x + label_width, label_min.y + frame_height));
    ImGui::RenderTextClipped(
        label_rect.Min,
        label_rect.Max,
        source_view.name.c_str(),
        nullptr,
        nullptr,
        ImVec2(0.0f, 0.5f),
        &label_rect);

    if (source_view.removable) {
        ImGui::SameLine(0.0f, inner_spacing);
        if (ActionIconButton(
                "remove_sort_source",
                ImVec2(remove_width, frame_height),
                ActionIcon::Minus,
                "Remove sample sorting",
                true)) {
            action.remove = true;
        }
    }
    ImGui::PopID();
    return action;
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

std::optional<int> ParseLabelCode(std::string_view value)
{
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    int code = 0;
    const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), code);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return code;
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
        return "State: temporary local draft; use Save to... to create a labeling annotation.";
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return "State: output file and metadata sidecar are saved.";
    case SampleLabelSaveStateKind::Pending:
        return "State: output/metadata autosave is pending; close is disabled until it finishes.";
    case SampleLabelSaveStateKind::Failed:
        if (labeling_view.active_task_is_temporary) {
            return "State: Save to... failed; choose this or another output, or pause the recoverable draft.";
        }
        return "State: output/metadata autosave failed; close is disabled until the save succeeds.";
    default:
        return "State: unknown save state.";
    }
}

bool AnnotationActivationNeedsConfirmation(SampleAnnotationWorkflowRelationship relationship)
{
    return relationship != SampleAnnotationWorkflowRelationship::LocalLabelingTask;
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
    active_task_id_.clear();
    editing_label_code_.reset();
    label_name_edit_buffer_.clear();
    label_code_edit_buffer_.fill('\0');
    label_shortcut_edit_buffer_.fill('\0');
    ResetLabelShortcutCapture();
    label_name_focus_pending_ = false;
    pending_label_code_change_original_code_.reset();
    pending_label_code_change_ = {};
    pending_label_code_change_usage_count_ = 0;
    pending_delete_label_code_.reset();
    pending_delete_label_name_.clear();
    pending_delete_label_usage_count_ = 0;
    pending_annotation_activation_path_.clear();
    pending_annotation_activation_name_.clear();
    pending_delete_task_name_.clear();
    pending_annotation_activation_relationship_ = SampleAnnotationWorkflowRelationship::PlainAnnotation;
}

void SampleWorkflowPanelUi::ResetLabelShortcutCapture()
{
    label_shortcut_capture_active_ = false;
    pending_conflicting_shortcut_ = '\0';
    label_shortcut_notice_.clear();
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderLabeling(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    const SourceCollectionSessionViewReader& read_view,
    bool* open,
    const std::function<std::optional<std::filesystem::path>()>& choose_output_path,
    SampleWorkflowShortcut& shortcut)
{
    SourceCollectionSessionAction action;
    shortcut = {};
    if (!ImGui::Begin(kLabelingWindow, open)) {
        ResetLabelShortcutCapture();
        ImGui::End();
        return action;
    }
    const bool labeling_context_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool labeling_context_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const auto route_latest_labeling_shortcuts = [&](bool blocked) {
        const SourceCollectionLabelingView& latest_labeling = read_view().labeling;
        shortcut = RouteSampleWorkflowShortcut(
            {
                .focused = labeling_context_focused,
                .hovered = labeling_context_hovered,
                .labeling_enabled = latest_labeling.has_active_task,
                .blocked = blocked,
            },
            latest_labeling.label_set);
    };

    const SourceCollectionLabelingView labeling_view = session_view.labeling;
    const std::optional<std::size_t> current_index = labeling_view.current_index;
    if (!labeling_view.has_active_source || !current_index) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        ImGui::TextDisabled("No active source");
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return action;
    }

    const bool task_switch_locked = labeling_view.has_active_task && !labeling_view.can_deactivate_task;
    constexpr const char* pause_label = "Pause";
    const ImGuiStyle& style = ImGui::GetStyle();
    float reserved_button_width = 0.0f;
    if (labeling_view.has_active_task) {
        reserved_button_width = ImGui::CalcTextSize(pause_label).x + ImGui::CalcTextSize("Delete").x +
                                style.FramePadding.x * 4.0f + style.ItemSpacing.x * 2.0f;
    }
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - reserved_button_width));
    const char* selector_preview =
        labeling_view.has_active_task ? labeling_view.task_name.c_str() : "Select labeling task";
    const bool selector_open = ImGui::BeginCombo("##labeling_task_selector", selector_preview);
    const ImRect selector_rect = GImGui->LastItemData.Rect;
    const ImGuiID selector_id = GImGui->LastItemData.ID;
    if (selector_open) {
        const bool temporary_selected =
            labeling_view.has_active_task && labeling_view.active_task_is_temporary;
        const char* temporary_action = !labeling_view.has_temporary_task
            ? "New labeling task"
            : temporary_selected ? "Temporary labeling draft" : "Resume labeling draft";
        const bool temporary_action_disabled = task_switch_locked && !temporary_selected;
        if (temporary_action_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Selectable(temporary_action, temporary_selected) && !temporary_selected) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(
                           ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()))
                    .action);
        }
        if (temporary_action_disabled) {
            ImGui::EndDisabled();
        }

        bool separated_annotations = false;
        for (const SourceCollectionAnnotationValueView& annotation :
             session_view.navigation.current_annotations) {
            if (annotation.relationship != SampleAnnotationWorkflowRelationship::LocalLabelingTask) {
                continue;
            }
            if (!separated_annotations) {
                ImGui::Separator();
                separated_annotations = true;
            }
            const bool selected = labeling_view.output_path &&
                                  PathToUtf8(*labeling_view.output_path) == PathToUtf8(annotation.path);
            const bool disabled = !annotation.can_activate_labeling || (task_switch_locked && !selected);
            ImGui::PushID(PathToUtf8(annotation.path).c_str());
            if (disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(annotation.name.c_str(), selected) && !selected) {
                MergeSourceCollectionSessionAction(
                    action,
                    submit(ChangeActiveSampleWorkflow(
                               ActiveSampleWorkflowIntent::ActivateLabelingTaskFromAnnotation(annotation.path)))
                        .action);
            }
            if (disabled) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (ImGui::BeginDragDropTargetCustom(selector_rect, selector_id)) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSampleAnnotationDragPayload)) {
            const std::filesystem::path annotation_path = Utf8ToPath(PayloadString(*payload));
            if (const SourceCollectionAnnotationValueView* annotation =
                    FindAnnotationViewByPath(session_view, annotation_path);
                annotation != nullptr && annotation->can_activate_labeling && !task_switch_locked) {
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

    if (labeling_view.has_active_task) {
        ImGui::SameLine();
        if (!labeling_view.can_deactivate_task) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(pause_label)) {
            MergeSourceCollectionSessionAction(
                action,
                submit(ChangeActiveSampleWorkflow(ActiveSampleWorkflowIntent::DeactivateActiveLabelingTask()))
                    .action);
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            route_latest_labeling_shortcuts(false);
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
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            route_latest_labeling_shortcuts(false);
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
    if (action.workflow_changed) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return action;
    }
    if (!labeling_view.has_active_task) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return action;
    }

    const int current_code = labeling_view.current_code;
    if (active_task_id_ != labeling_view.task_id) {
        active_task_id_ = labeling_view.task_id;
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        label_name_focus_pending_ = false;
        pending_delete_label_code_.reset();
        pending_delete_label_name_.clear();
        pending_delete_label_usage_count_ = 0;
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
        const std::string path = UserPathDisplayText(*labeling_view.output_path);
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
    if (labeling_view.active_task_is_temporary) {
        ImGui::SameLine();
        if (ImGui::Button("Save to...")) {
            if (std::optional<std::filesystem::path> path = choose_output_path()) {
                MergeSourceCollectionSessionAction(
                    action,
                    submit(ChangeActiveSampleWorkflow(
                               ActiveSampleWorkflowIntent::SetActiveLabelingOutputPath(*path)))
                        .action);
            }
        }
    }

    bool block_shortcuts_this_frame = label_shortcut_capture_active_;
    std::optional<int> label_code_to_assign;
    bool clear_label_requested = false;

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Labels");
    ImGui::SameLine();
    const bool add_label_requested = ImGui::SmallButton("+##AddSampleLabel");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add label");
    }
    if (add_label_requested) {
        const int code = NextAvailableSampleLabelCode(labeling_view.label_set);
        const std::string name = "Label " + std::to_string(code);
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UpsertActiveLabel(SampleLabelDefinition{code, name, '\0'})));
        MergeSourceCollectionSessionAction(action, result.action);
        if (result.changed) {
            editing_label_code_ = code;
            label_name_edit_buffer_ = name;
            CopyToBuffer(label_code_edit_buffer_, std::to_string(code));
            label_shortcut_edit_buffer_.fill('\0');
            ResetLabelShortcutCapture();
            label_name_focus_pending_ = true;
        }
    }

    std::optional<SampleLabelDefinition> label_to_update;
    std::optional<int> label_to_update_original_code;
    std::optional<int> label_code_to_remove;
    bool open_change_label_code_popup = false;
    bool open_delete_label_popup = false;
    if (ImGui::BeginTable(
            "sample_labels",
            5,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 132.0f);
        ImGui::TableSetupColumn("##Edit", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableHeadersRow();

        for (const SampleLabelDefinition& label : labeling_view.label_set.labels) {
            const bool selected = current_code == label.code;
            const bool editing = editing_label_code_ && *editing_label_code_ == label.code;
            const auto usage = labeling_view.label_usage_counts.find(label.code);
            const std::size_t usage_count =
                usage == labeling_view.label_usage_counts.end() ? 0 : usage->second;

            ImGui::PushID(label.code);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
            if (editing) {
                ImGui::PushFocusScope(ImGui::GetID("label_edit_focus"));
                bool submit_edit = false;
                ImGui::TableSetColumnIndex(0);
                ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
                if (label_name_focus_pending_) {
                    ImGui::SetKeyboardFocusHere();
                    label_name_focus_pending_ = false;
                }
                submit_edit = ImGui::InputText(
                    "##label_name",
                    &label_name_edit_buffer_,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
                submit_edit = ImGui::InputText(
                                  "##label_code",
                                  label_code_edit_buffer_.data(),
                                  label_code_edit_buffer_.size(),
                                  ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_AutoSelectAll) ||
                              submit_edit;

                ImGui::TableSetColumnIndex(2);
                const bool has_shortcut = label_shortcut_edit_buffer_[0] != '\0';
                const float clear_button_width = has_shortcut
                    ? ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f
                    : 0.0f;
                const float shortcut_button_width = std::max(
                    1.0f,
                    ImGui::GetContentRegionAvail().x -
                        (has_shortcut ? clear_button_width + ImGui::GetStyle().ItemSpacing.x : 0.0f));
                const std::string shortcut_button_label =
                    (label_shortcut_capture_active_
                         ? std::string{"Press key..."}
                         : FormatSampleLabelShortcut(label_shortcut_edit_buffer_[0])) +
                    "###label_shortcut_capture";
                if (ImGui::Button(shortcut_button_label.c_str(), ImVec2(shortcut_button_width, 0.0f))) {
                    block_shortcuts_this_frame = true;
                    if (label_shortcut_capture_active_) {
                        ResetLabelShortcutCapture();
                    } else {
                        label_shortcut_capture_active_ = true;
                        pending_conflicting_shortcut_ = '\0';
                        label_shortcut_notice_ =
                            "Press A-Z or 0-9. Backspace clears the binding; Escape cancels.";
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        label_shortcut_capture_active_
                            ? "Waiting for an unmodified letter or digit"
                            : "Capture a label shortcut");
                }
                if (has_shortcut) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##label_shortcut")) {
                        block_shortcuts_this_frame = true;
                        label_shortcut_edit_buffer_.fill('\0');
                        ResetLabelShortcutCapture();
                        label_shortcut_notice_ = "The shortcut will be unbound when this label is saved.";
                    }
                }

                if (label_shortcut_capture_active_) {
                    block_shortcuts_this_frame = true;
                    const SampleLabelShortcutCapture capture = CaptureSampleLabelShortcut();
                    switch (capture.kind) {
                    case SampleLabelShortcutCaptureKind::None:
                        break;
                    case SampleLabelShortcutCaptureKind::Cancelled:
                        ResetLabelShortcutCapture();
                        break;
                    case SampleLabelShortcutCaptureKind::Cleared:
                        label_shortcut_edit_buffer_.fill('\0');
                        ResetLabelShortcutCapture();
                        label_shortcut_notice_ = "The shortcut will be unbound when this label is saved.";
                        break;
                    case SampleLabelShortcutCaptureKind::Unsupported:
                        pending_conflicting_shortcut_ = '\0';
                        label_shortcut_notice_ = "Only unmodified A-Z and 0-9 keys can be assigned.";
                        break;
                    case SampleLabelShortcutCaptureKind::Captured: {
                        const char captured_shortcut = NormalizeSampleLabelShortcut(capture.shortcut);
                        const SampleLabelShortcutSelection selection = ResolveSampleLabelShortcutSelection(
                            captured_shortcut,
                            label.code,
                            pending_conflicting_shortcut_,
                            labeling_view.label_set);
                        const SampleLabelDefinition* captured_owner = selection.conflicting_label_code ==
                                kUnlabeledSampleLabelCode
                            ? nullptr
                            : FindSampleLabel(labeling_view.label_set, selection.conflicting_label_code);
                        const std::string display_shortcut = FormatSampleLabelShortcut(captured_shortcut);
                        if (captured_owner == nullptr) {
                            label_shortcut_edit_buffer_.fill('\0');
                            label_shortcut_edit_buffer_[0] = captured_shortcut;
                            ResetLabelShortcutCapture();
                            label_shortcut_notice_ =
                                "Shortcut " + display_shortcut + " selected. Save the label to apply it.";
                        } else if (selection.kind == SampleLabelShortcutSelectionKind::Accepted) {
                            const std::string previous_owner_name = captured_owner->name;
                            label_shortcut_edit_buffer_.fill('\0');
                            label_shortcut_edit_buffer_[0] = captured_shortcut;
                            ResetLabelShortcutCapture();
                            label_shortcut_notice_ = "Shortcut " + display_shortcut + " will move from " +
                                                     previous_owner_name + " when this label is saved.";
                        } else {
                            pending_conflicting_shortcut_ = captured_shortcut;
                            label_shortcut_notice_ = display_shortcut + " is assigned to " + captured_owner->name +
                                                     ". Press " + display_shortcut + " again to move it.";
                        }
                        break;
                    }
                    }
                }

                const std::string requested_name = TrimAscii(label_name_edit_buffer_);
                const char requested_shortcut = label_shortcut_edit_buffer_[0];
                const bool valid_shortcut =
                    requested_shortcut == '\0' || IsValidSampleLabelShortcut(requested_shortcut);
                const std::optional<int> requested_code = ParseLabelCode(label_code_edit_buffer_.data());
                const bool code_reserved = requested_code && *requested_code == kUnlabeledSampleLabelCode;
                const bool code_conflicts =
                    requested_code && labeling_view.HasConflictingLabelCode(label.code, *requested_code);
                const bool valid_code = requested_code && !code_reserved && !code_conflicts;
                const bool valid_edit = !requested_name.empty() && valid_shortcut && valid_code;
                const char normalized_shortcut = NormalizeSampleLabelShortcut(requested_shortcut);
                const auto shortcut_owner = std::find_if(
                    labeling_view.label_set.labels.begin(),
                    labeling_view.label_set.labels.end(),
                    [label_code = label.code, normalized_shortcut](const SampleLabelDefinition& candidate) {
                        return normalized_shortcut != '\0' && candidate.code != label_code &&
                               candidate.shortcut == normalized_shortcut;
                    });
                if (!valid_edit) {
                    ImGui::BeginDisabled();
                }
                ImGui::TableSetColumnIndex(3);
                const ImRect save_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                const bool save_clicked = HiddenActionIconButton(
                    "save_label",
                    save_cell,
                    ActionIcon::Check,
                    nullptr,
                    true);
                const bool save_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                if (save_clicked || (submit_edit && valid_edit)) {
                    SampleLabelDefinition requested_label{*requested_code, requested_name, requested_shortcut};
                    if (*requested_code != label.code && usage_count > 0) {
                        pending_label_code_change_original_code_ = label.code;
                        pending_label_code_change_ = std::move(requested_label);
                        pending_label_code_change_usage_count_ = usage_count;
                        open_change_label_code_popup = true;
                    } else {
                        label_to_update_original_code = label.code;
                        label_to_update = std::move(requested_label);
                    }
                }
                if (!valid_edit) {
                    ImGui::EndDisabled();
                    if (save_hovered) {
                        if (requested_name.empty()) {
                            ImGui::SetTooltip("Name is required");
                        } else if (!requested_code) {
                            ImGui::SetTooltip("Code must be an integer");
                        } else if (code_reserved) {
                            ImGui::SetTooltip("Code -1 is reserved for unlabeled samples");
                        } else if (code_conflicts) {
                            ImGui::SetTooltip(
                                "Code %d is already used by a label or sample value",
                                *requested_code);
                        } else {
                            ImGui::SetTooltip("Shortcut must be one letter or digit");
                        }
                    }
                } else if (
                    save_hovered && requested_code && *requested_code != label.code && usage_count > 0) {
                    ImGui::SetTooltip(
                        "Changing this code rewrites %llu assigned sample value(s)",
                        static_cast<unsigned long long>(usage_count));
                } else if (save_hovered && shortcut_owner != labeling_view.label_set.labels.end()) {
                    ImGui::SetTooltip("Saving moves this shortcut from %s", shortcut_owner->name.c_str());
                } else if (save_hovered) {
                    ImGui::SetTooltip("Save label");
                }
                ImGui::TableSetColumnIndex(4);
                const ImRect cancel_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                const auto cancel_label_edit = [&]() {
                    editing_label_code_.reset();
                    ResetLabelShortcutCapture();
                    label_name_focus_pending_ = false;
                };
                if (HiddenActionIconButton(
                        "cancel_label_edit",
                        cancel_cell,
                        ActionIcon::Close,
                        "Cancel editing",
                        true) ||
                    (!block_shortcuts_this_frame &&
                        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                        ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                    cancel_label_edit();
                }

                const ImRect editing_row_rect(
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), 0).Min,
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), 4).Max);
                const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                           ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                                           ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
                if (mouse_clicked && !editing_row_rect.Contains(ImGui::GetMousePos())) {
                    cancel_label_edit();
                }
                ImGui::PopFocusScope();
            } else {
                ImGui::TableSetColumnIndex(0);
                const bool row_clicked = ImGui::Selectable(
                    "##label_row",
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0.0f, ImGui::GetFrameHeight()));
                const std::string code_text = std::to_string(label.code);
                const std::string shortcut_text = FormatSampleLabelShortcut(label.shortcut);
                DrawTableCellText(0, label.name);
                DrawTableCellText(1, code_text);
                DrawTableCellText(2, shortcut_text);

                ImGui::TableSetColumnIndex(3);
                const ImRect edit_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                const bool edit_requested = HiddenActionIconButton(
                    "edit_label",
                    edit_cell,
                    ActionIcon::Pencil,
                    "Edit label",
                    true);
                if (edit_requested) {
                    editing_label_code_ = label.code;
                    label_name_edit_buffer_ = label.name;
                    CopyToBuffer(label_code_edit_buffer_, std::to_string(label.code));
                    label_shortcut_edit_buffer_.fill('\0');
                    if (label.shortcut != '\0') {
                        label_shortcut_edit_buffer_[0] = label.shortcut;
                    }
                    ResetLabelShortcutCapture();
                    label_name_focus_pending_ = true;
                }

                const std::string delete_tooltip = usage_count == 0
                    ? "Delete label"
                    : "Delete label and clear " + std::to_string(usage_count) + " sample(s)";
                ImGui::TableSetColumnIndex(4);
                const ImRect delete_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                const bool delete_requested = HiddenActionIconButton(
                    "delete_label",
                    delete_cell,
                    ActionIcon::Trash,
                    delete_tooltip.c_str(),
                    true);

                if (row_clicked && !edit_requested && !delete_requested) {
                    label_code_to_assign = label.code;
                    clear_label_requested = false;
                }
                if (delete_requested) {
                    if (usage_count == 0) {
                        label_code_to_remove = label.code;
                    } else {
                        pending_delete_label_code_ = label.code;
                        pending_delete_label_name_ = label.name;
                        pending_delete_label_usage_count_ = usage_count;
                        open_delete_label_popup = true;
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (editing_label_code_ && !label_shortcut_notice_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::BeginChild(
                "##label_shortcut_notice",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::TextWrapped("%s", label_shortcut_notice_.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (open_change_label_code_popup) {
        ImGui::OpenPopup(kChangeSampleLabelCodePopup);
    }
    if (ImGui::BeginPopupModal(kChangeSampleLabelCodePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Label code %d is assigned to %llu sample(s).",
            pending_label_code_change_original_code_.value_or(kUnlabeledSampleLabelCode),
            static_cast<unsigned long long>(pending_label_code_change_usage_count_));
        ImGui::TextWrapped(
            "Changing it to %d will rewrite every assigned sample value.",
            pending_label_code_change_.code);
        if (ImGui::Button("Change code") && pending_label_code_change_original_code_) {
            SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::UpdateActiveLabel(
                    *pending_label_code_change_original_code_,
                    pending_label_code_change_,
                    true)));
            MergeSourceCollectionSessionAction(action, result.action);
            if (result.changed) {
                editing_label_code_.reset();
                ResetLabelShortcutCapture();
                label_name_focus_pending_ = false;
                pending_label_code_change_original_code_.reset();
                pending_label_code_change_ = {};
                pending_label_code_change_usage_count_ = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_label_code_change_original_code_.reset();
            pending_label_code_change_ = {};
            pending_label_code_change_usage_count_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (open_delete_label_popup) {
        ImGui::OpenPopup(kDeleteSampleLabelPopup);
    }
    if (ImGui::BeginPopupModal(kDeleteSampleLabelPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Label \"%s\" is assigned to %llu sample(s).",
            pending_delete_label_name_.c_str(),
            static_cast<unsigned long long>(pending_delete_label_usage_count_));
        ImGui::TextWrapped(
            "Deleting it will change those values to Unlabeled (-1) and remove the label definition.");
        ImGui::TextWrapped(
            "Its selected sample-filter value will also be removed, which may move the current sample.");
        if (ImGui::Button("Delete label") && pending_delete_label_code_) {
            label_code_to_remove = pending_delete_label_code_;
            pending_delete_label_code_.reset();
            pending_delete_label_name_.clear();
            pending_delete_label_usage_count_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_delete_label_code_.reset();
            pending_delete_label_name_.clear();
            pending_delete_label_usage_count_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (label_to_update && label_to_update_original_code) {
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UpdateActiveLabel(
                *label_to_update_original_code,
                std::move(*label_to_update),
                false)));
        MergeSourceCollectionSessionAction(action, result.action);
        if (result.changed) {
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            label_name_focus_pending_ = false;
        }
    }
    if (label_code_to_remove) {
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::RemoveActiveLabel(*label_code_to_remove)));
        MergeSourceCollectionSessionAction(action, result.action);
        if (result.changed && editing_label_code_ == label_code_to_remove) {
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            label_name_focus_pending_ = false;
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

    route_latest_labeling_shortcuts(block_shortcuts_this_frame);

    ImGui::End();
    return action;
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderFilters(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    const SourceCollectionSessionViewReader& read_view,
    bool* open)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kFiltersWindow, open)) {
        ImGui::End();
        return action;
    }

    const SourceCollectionFilterView* filter_view = &session_view.filter;
    if (!filter_view->has_active_source) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    if (ImGui::SmallButton("+##AddSampleFilterSource")) {
        ImGui::OpenPopup(kAddSampleFilterSourcePopup);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add annotation sample filter");
    }
    std::optional<std::string> source_to_collapse;
    if (ImGui::BeginPopup(kAddSampleFilterSourcePopup)) {
        if (filter_view->available_sources.empty()) {
            ImGui::TextDisabled("No available annotations");
        }
        std::optional<std::string> source_to_add;
        for (const SourceCollectionFilterSourceView& source_view : filter_view->available_sources) {
            ImGui::PushID(source_view.id.c_str());
            if (ImGui::Selectable(source_view.name.c_str())) {
                source_to_add = source_view.id;
            }
            if (ImGui::IsItemHovered() && !source_view.annotation_path.empty()) {
                const std::string path = UserPathDisplayText(source_view.annotation_path);
                ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::PopID();
            if (source_to_add) {
                break;
            }
        }
        if (source_to_add) {
            SourceCollectionSessionResult result =
                submit(ApplySampleFiltering(SampleFilteringIntent::AddSource(*source_to_add)));
            MergeSourceCollectionSessionAction(action, result.action);
            filter_view = &read_view().filter;
            source_to_collapse = *source_to_add;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (source_to_collapse) {
        CollapseSampleFilterSourceTree(*source_to_collapse);
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset sample filters")) {
        SourceCollectionSessionResult result = submit(ApplySampleFiltering(SampleFilteringIntent::Clear()));
        MergeSourceCollectionSessionAction(action, result.action);
        filter_view = &read_view().filter;
    }

    ImGui::Spacing();

    ImGui::Text(
        "Visible: %llu / %llu",
        static_cast<unsigned long long>(filter_view->evaluation.included_count),
        static_cast<unsigned long long>(filter_view->sample_count));
    if (filter_view->navigation_filter_active && !filter_view->current_sample_in_filter) {
        ImGui::TextDisabled("Current sample is outside the active sample filters");
    }

    for (const std::string& message : filter_view->evaluation.messages) {
        ImGui::TextDisabled("%s", message.c_str());
    }

    ImGui::Separator();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 filter_drop_min(window->WorkRect.Min.x, ImGui::GetCursorScreenPos().y);
    if (filter_view->sources.empty()) {
        ImGui::TextDisabled("No sample filters");
    }
    bool stop_rendering_sources = false;
    for (const SourceCollectionFilterSourceView& source_view : filter_view->sources) {
        if (!source_view.filterable) {
            continue;
        }

        ImGui::PushID(source_view.id.c_str());
        const ImGuiTreeNodeFlags source_flags =
            ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_AllowOverlap;
        const bool tree_open = ImGui::TreeNodeEx("source", source_flags, "%s", source_view.name.c_str());
        const ImVec2 source_item_min = ImGui::GetItemRectMin();
        const ImVec2 source_item_max = ImGui::GetItemRectMax();
        const bool source_row_hovered = ImGui::IsItemHovered();
        if (source_row_hovered && !source_view.annotation_path.empty()) {
            const std::string path = UserPathDisplayText(source_view.annotation_path);
            ImGui::SetTooltip("%s", path.c_str());
        }
        bool removed_source = false;
        const float action_width = ImGui::GetFrameHeight();
        const ImRect remove_rect(
            ImVec2(std::max(source_item_min.x, source_item_max.x - action_width), source_item_min.y),
            source_item_max);
        if (HiddenActionIconButton(
                "remove_source",
                remove_rect,
                ActionIcon::Minus,
                "Remove sample filter",
                source_row_hovered)) {
            SourceCollectionSessionResult result =
                submit(ApplySampleFiltering(SampleFilteringIntent::RemoveSource(source_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            filter_view = &read_view().filter;
            removed_source = true;
        }
        if (tree_open && !removed_source) {
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
        } else if (tree_open) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        if (removed_source) {
            stop_rendering_sources = true;
        }
        if (stop_rendering_sources) {
            break;
        }
    }

    const ImVec2 filter_drop_content_end = ImGui::GetCursorScreenPos();
    const ImRect filter_drop_rect(
        filter_drop_min,
        ImVec2(
            window->WorkRect.Max.x,
            std::max(filter_drop_content_end.y + ImGui::GetStyle().ItemSpacing.y, window->WorkRect.Max.y)));
    if (std::optional<std::string> dropped_source =
            RenderSampleFilterDropTarget(*filter_view, filter_drop_rect)) {
        SourceCollectionSessionResult result =
            submit(ApplySampleFiltering(SampleFilteringIntent::AddSource(*dropped_source)));
        MergeSourceCollectionSessionAction(action, result.action);
        CollapseSampleFilterSourceTree(*dropped_source);
    }

    ImGui::End();
    return action;
}

SourceCollectionSessionAction SampleWorkflowPanelUi::RenderSorting(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    const SourceCollectionSessionViewReader& read_view,
    bool* open)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kSortingWindow, open)) {
        ImGui::End();
        return action;
    }

    const SourceCollectionSampleSortingView* sorting_view = &session_view.sorting;
    if (!sorting_view->has_active_source) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    if (ImGui::SmallButton("+##AddSampleSortSource")) {
        ImGui::OpenPopup(kAddSampleSortSourcePopup);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add annotation sample sorting");
    }

    ImGui::SameLine();
    bool reset_sorting_checked =
        !sorting_view->active ||
        (sorting_view->active_source_id == "source-order" &&
            sorting_view->direction == SampleNavigationSortDirection::Ascending);
    if (ImGui::Checkbox("Reset sorting", &reset_sorting_checked) && reset_sorting_checked) {
        SourceCollectionSessionResult result =
            submit(ApplySampleSorting(SampleSortingIntent::Clear()));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = &read_view().sorting;
    }

    if (ImGui::BeginPopup(kAddSampleSortSourcePopup)) {
        if (sorting_view->available_sources.empty()) {
            ImGui::TextDisabled("No available annotations");
        }
        std::optional<std::string> source_to_add;
        for (const SourceCollectionSampleSortSourceView& source_view : sorting_view->available_sources) {
            ImGui::PushID(source_view.id.c_str());
            if (ImGui::Selectable(source_view.name.c_str())) {
                source_to_add = source_view.id;
            }
            if (ImGui::IsItemHovered() && !source_view.annotation_path.empty()) {
                const std::string path = UserPathDisplayText(source_view.annotation_path);
                ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::PopID();
            if (source_to_add) {
                break;
            }
        }
        if (source_to_add) {
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(SampleSortingIntent::AddSource(*source_to_add)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 sort_drop_min(window->WorkRect.Min.x, ImGui::GetCursorScreenPos().y);
    SourceCollectionSampleSortSourceView source_order_view;
    source_order_view.id = "source-order";
    source_order_view.name = "Source order";
    source_order_view.selected = !sorting_view->active || sorting_view->active_source_id == source_order_view.id;
    const SampleNavigationSortDirection source_order_direction =
        source_order_view.selected ? sorting_view->direction : sorting_view->source_order_direction;
    const SampleSortSourceRowAction source_order_action =
        RenderSampleSortSourceRow(source_order_view, source_order_direction);
    if (source_order_action.toggle_direction) {
        if (sorting_view->active && sorting_view->active_source_id == source_order_view.id) {
            SourceCollectionSessionResult result =
                sorting_view->direction == SampleNavigationSortDirection::Descending
                    ? submit(ApplySampleSorting(SampleSortingIntent::Clear()))
                    : submit(ApplySampleSorting(SampleSortingIntent::SetSortDirection(
                          SampleNavigationSortDirection::Descending)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
        } else if (source_order_view.selected) {
            SourceCollectionSessionResult result = submit(ApplySampleSorting(
                SampleSortingIntent::SetSortDirection(SampleNavigationSortDirection::Descending)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
            result = submit(ApplySampleSorting(SampleSortingIntent::SetSortSource(source_order_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
        } else {
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(SampleSortingIntent::Clear()));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
        }
    } else if (source_order_action.activate) {
        SourceCollectionSessionResult result = source_order_direction == SampleNavigationSortDirection::Descending
            ? submit(ApplySampleSorting(SampleSortingIntent::SetSortSource(source_order_view.id)))
            : submit(ApplySampleSorting(SampleSortingIntent::Clear()));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = &read_view().sorting;
    }

    bool has_sort_source = false;
    bool stop_rendering_sources = false;
    const std::vector<SourceCollectionSampleSortSourceView>& sort_sources = sorting_view->sources;
    for (const SourceCollectionSampleSortSourceView& source_view : sort_sources) {
        has_sort_source = true;
        const SampleSortSourceRowAction row_action =
            RenderSampleSortSourceRow(source_view, source_view.direction);
        if (row_action.remove) {
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(SampleSortingIntent::RemoveSource(source_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
            stop_rendering_sources = true;
        } else if (row_action.toggle_direction) {
            const SampleNavigationSortDirection next_direction = source_view.selected
                ? OppositeSortDirection(source_view.direction)
                : source_view.direction;
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(source_view.selected
                    ? SampleSortingIntent::SetSortDirection(next_direction)
                    : SampleSortingIntent::SetSortSource(source_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
            stop_rendering_sources = true;
        } else if (row_action.activate) {
            SourceCollectionSessionResult result =
                submit(ApplySampleSorting(SampleSortingIntent::SetSortSource(source_view.id)));
            MergeSourceCollectionSessionAction(action, result.action);
            sorting_view = &read_view().sorting;
            stop_rendering_sources = true;
        }
        if (stop_rendering_sources) {
            break;
        }
    }
    if (!has_sort_source) {
        ImGui::TextDisabled("No comparable sort sources");
    }

    const ImVec2 sort_drop_content_end = ImGui::GetCursorScreenPos();
    const ImRect sort_drop_rect(
        sort_drop_min,
        ImVec2(
            window->WorkRect.Max.x,
            std::max(sort_drop_content_end.y + ImGui::GetStyle().ItemSpacing.y, window->WorkRect.Max.y)));
    if (std::optional<std::string> dropped_source =
            RenderSampleSortDropTarget(*sorting_view, sort_drop_rect)) {
        SourceCollectionSessionResult result =
            submit(ApplySampleSorting(SampleSortingIntent::AddSource(*dropped_source)));
        MergeSourceCollectionSessionAction(action, result.action);
        sorting_view = &read_view().sorting;
    }

    ImGui::End();
    return action;
}

}  // namespace specforge
