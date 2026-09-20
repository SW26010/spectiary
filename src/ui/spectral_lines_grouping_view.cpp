#include "overlays/spectral_line_projection.h"
#include "plot/series_color.h"
#include "ui/spectral_lines_grouping_view.h"
#include "ui/spectral_lines_name_localization.h"
#include "ui/theme.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {
namespace {

constexpr const char* kMarkerReferenceDragPayload = "MarkerReference";
constexpr const char* kUserGroupDragPayload = "UserGroup";

struct MarkerReferenceDragPayload {
    std::string view_id;
    std::string source_group_id;
    std::string marker_id;
};

struct UserGroupDragPayload {
    std::string view_id;
    std::string group_id;
};

struct UserGroupReorderLine {
    float y = 0.0f;
    float x_min = 0.0f;
    float x_max = 0.0f;
};

struct UserGroupReorderGapResult {
    UserGroupReorderLine line;
    float min_y = 0.0f;
};

enum class ActionIcon {
    Minus,
    Trash,
};

bool HasNonWhitespace(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
}

template <typename... Args>
std::string FormatUiText(
    UiLanguage language,
    UiTextId text_id,
    Args... args)
{
    const std::string_view format = UiText(language, text_id);
    const int required =
        std::snprintf(nullptr, 0, format.data(), args...);
    if (required <= 0) {
        return std::string(format);
    }

    std::string result(
        static_cast<std::size_t>(required),
        '\0');
    (void)std::snprintf(
        result.data(),
        result.size() + 1,
        format.data(),
        args...);
    return result;
}

std::string LocalizedGroupName(
    UiLanguage language,
    const SpectralLineGroupView& group)
{
    if (group.is_unassigned) {
        return std::string(
            UiText(
                language,
                UiTextId::UnassignedGroup));
    }

    return LocalizedSpectralLineName(
        language,
        group.name,
        group.generated_name);
}

std::string EncodeMarkerReferenceDragPayload(
    std::string_view view_id,
    std::string_view source_group_id,
    std::string_view marker_id)
{
    std::string payload;
    payload.reserve(view_id.size() + source_group_id.size() + marker_id.size() + 2);
    payload.append(view_id);
    payload.push_back('\0');
    payload.append(source_group_id);
    payload.push_back('\0');
    payload.append(marker_id);
    return payload;
}

std::optional<MarkerReferenceDragPayload> DecodeMarkerReferenceDragPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0) {
        return std::nullopt;
    }

    const auto* bytes = static_cast<const char*>(payload.Data);
    const std::string_view data(bytes, static_cast<std::size_t>(payload.DataSize));
    const std::size_t first_separator = data.find('\0');
    if (first_separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second_separator = data.find('\0', first_separator + 1);
    if (second_separator == std::string_view::npos) {
        return std::nullopt;
    }

    MarkerReferenceDragPayload decoded;
    decoded.view_id = std::string(data.substr(0, first_separator));
    decoded.source_group_id =
        std::string(data.substr(first_separator + 1, second_separator - first_separator - 1));
    decoded.marker_id = std::string(data.substr(second_separator + 1));
    if (decoded.view_id.empty() || decoded.source_group_id.empty() || decoded.marker_id.empty()) {
        return std::nullopt;
    }
    return decoded;
}

void SubmitMarkerReferenceDragPayload(
    std::string_view view_id,
    std::string_view source_group_id,
    std::string_view marker_id,
    std::string_view label,
    UiLanguage language)
{
    const std::string drag_payload = EncodeMarkerReferenceDragPayload(view_id, source_group_id, marker_id);
    ImGui::SetDragDropPayload(
        kMarkerReferenceDragPayload,
        drag_payload.data(),
        static_cast<int>(drag_payload.size()));
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    const std::string_view instruction =
        UiText(
            language,
            ImGui::GetIO().KeyCtrl
                ? UiTextId::DragDropCopy
                : UiTextId::DragDropMoveOrCopy);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(instruction.size()),
        instruction.data());
}

bool BeginCtrlMarkerReferenceDragDropSource(const ImRect& hit_rect)
{
    if (!ImGui::GetIO().KeyCtrl || hit_rect.GetWidth() <= 0.0f || hit_rect.GetHeight() <= 0.0f) {
        return false;
    }

    const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(hit_rect.Min);
    ImGui::InvisibleButton("marker_ctrl_drag_source", hit_rect.GetSize(), ImGuiButtonFlags_MouseButtonLeft);
    ImGui::SetCursorScreenPos(saved_cursor);
    return ImGui::BeginDragDropSource(ImGuiDragDropFlags_None);
}

ImRect CurrentFullWidthFrameRect()
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    return ImRect(
        ImVec2(window->WorkRect.Min.x, cursor.y),
        ImVec2(window->WorkRect.Max.x, cursor.y + ImGui::GetFrameHeight()));
}

void RenderCtrlMarkerReferenceHover(const ImRect& hit_rect)
{
    if (!ImGui::GetIO().KeyCtrl || ImGui::GetDragDropPayload() != nullptr ||
        !ImGui::IsMouseHoveringRect(hit_rect.Min, hit_rect.Max, true)) {
        return;
    }

    ImGui::GetWindowDrawList()->AddRectFilled(
        hit_rect.Min,
        hit_rect.Max,
        ImGui::GetColorU32(ImGuiCol_HeaderHovered));
}

std::string EncodeUserGroupDragPayload(std::string_view view_id, std::string_view group_id)
{
    std::string payload;
    payload.reserve(view_id.size() + group_id.size() + 1);
    payload.append(view_id);
    payload.push_back('\0');
    payload.append(group_id);
    return payload;
}

std::optional<UserGroupDragPayload> DecodeUserGroupDragPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0) {
        return std::nullopt;
    }

    const auto* bytes = static_cast<const char*>(payload.Data);
    const std::string_view data(bytes, static_cast<std::size_t>(payload.DataSize));
    const std::size_t separator = data.find('\0');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    UserGroupDragPayload decoded;
    decoded.view_id = std::string(data.substr(0, separator));
    decoded.group_id = std::string(data.substr(separator + 1));
    if (decoded.view_id.empty() || decoded.group_id.empty()) {
        return std::nullopt;
    }
    return decoded;
}

std::optional<UserGroupDragPayload> CurrentUserGroupDragPayload()
{
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(kUserGroupDragPayload)) {
        return std::nullopt;
    }
    return DecodeUserGroupDragPayload(*payload);
}

void DrawUserGroupReorderLine(float y, float x_min, float x_max)
{
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(x_min, y), ImVec2(x_max, y), color, 2.0f);
    draw_list->AddCircleFilled(ImVec2(x_min, y), 3.0f, color);
}

std::optional<UserGroupDragPayload> AcceptUserGroupReorderPayload(std::string_view view_id, bool& accepted)
{
    accepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUserGroupDragPayload, flags)) {
        if (std::optional<UserGroupDragPayload> drag = DecodeUserGroupDragPayload(*payload)) {
            if (drag->view_id == view_id) {
                accepted = true;
                if (payload->IsDelivery()) {
                    return drag;
                }
            }
        }
    }
    return std::nullopt;
}

UserGroupReorderGapResult RenderUserGroupReorderGap(bool highlight)
{
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const float height = std::max(ImGui::GetStyle().ItemSpacing.y * 2.0f, 6.0f);
    ImGui::InvisibleButton("##user_group_reorder_gap", ImVec2(width, height));
    const ImVec2 gap_min = ImGui::GetItemRectMin();
    const ImVec2 gap_max = ImGui::GetItemRectMax();

    UserGroupReorderGapResult result;
    result.line = UserGroupReorderLine{
        (gap_min.y + gap_max.y) * 0.5f,
        gap_min.x,
        gap_max.x,
    };
    result.min_y = gap_min.y;
    if (highlight) {
        DrawUserGroupReorderLine(result.line.y, result.line.x_min, result.line.x_max);
    }
    return result;
}

std::optional<UserGroupDragPayload> RenderUserGroupReorderTarget(
    std::string_view view_id,
    const ImRect& hit_rect,
    const UserGroupReorderLine& line,
    ImGuiID target_id)
{
    std::optional<UserGroupDragPayload> delivered;
    bool highlight = false;
    if (ImGui::BeginDragDropTargetCustom(hit_rect, target_id)) {
        delivered = AcceptUserGroupReorderPayload(view_id, highlight);
        ImGui::EndDragDropTarget();
    }
    if (highlight) {
        DrawUserGroupReorderLine(line.y, line.x_min, line.x_max);
    }
    return delivered;
}

void RenderDisabledText(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

bool RenderGroupVisibilityControl(
    GroupVisibilityState state,
    bool& next_visible,
    UiLanguage language)
{
    next_visible = true;
    if (state == GroupVisibilityState::SearchFiltered || state == GroupVisibilityState::Empty) {
        bool value = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##group_visibility", &value);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            const std::string_view tooltip =
                UiText(
                    language,
                    state == GroupVisibilityState::SearchFiltered
                        ? UiTextId::SearchFilteredGroupVisibility
                        : UiTextId::NoResolvedMarkersInGroup);
            ImGui::SetTooltip(
                "%.*s",
                static_cast<int>(tooltip.size()),
                tooltip.data());
        }
        return false;
    }

    bool value = state == GroupVisibilityState::AllVisible;
    if (state == GroupVisibilityState::Mixed) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    const bool changed = ImGui::Checkbox("##group_visibility", &value);
    if (state == GroupVisibilityState::Mixed) {
        ImGui::PopItemFlag();
    }
    if (ImGui::IsItemHovered()) {
        const std::string_view tooltip =
            UiText(
                language,
                UiTextId::ToggleGroupMarkerVisibility);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    if (changed) {
        next_visible = state == GroupVisibilityState::Mixed ? true : value;
    }
    return changed;
}

void RenderSharedReferenceMarker(UiLanguage language)
{
    ImGui::TextDisabled("*");
    if (ImGui::IsItemHovered()) {
        const std::string_view tooltip =
            UiText(
                language,
                UiTextId::SharedMarkerReference);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
}

float ActionIconButtonWidth()
{
    return ImGui::GetFrameHeight() * 0.5f;
}

bool HiddenActionIconButton(
    const char* id,
    const ImRect& hit_rect,
    ActionIcon icon,
    std::string_view tooltip,
    bool reveal_icon)
{
    const float height = ImGui::GetFrameHeight();
    const float width = std::max(1.0f, hit_rect.GetWidth());
    ImGui::SetCursorScreenPos(hit_rect.Min);
    const ImVec2 button_size(width, std::max(1.0f, hit_rect.GetHeight()));
    const bool clicked = ImGui::InvisibleButton(id, button_size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (hovered || active) {
        const ImU32 background = ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
        draw_list->AddRectFilled(min, max, background, 3.0f);
    }

    const bool draw_icon = reveal_icon || hovered || active;
    const ImU32 icon_color = ImGui::GetColorU32(ImGuiCol_Text);
    const float icon_width = std::min(ActionIconButtonWidth(), width);
    const float icon_left = min.x + std::max(0.0f, (width - icon_width) * 0.5f);
    const float icon_top = min.y + std::max(0.0f, (max.y - min.y - height) * 0.5f);
    const float stroke = 1.35f;

    if (draw_icon && icon == ActionIcon::Minus) {
        const float y = icon_top + height * 0.5f;
        draw_list->AddLine(
            ImVec2(icon_left + icon_width * 0.18f, y),
            ImVec2(icon_left + icon_width * 0.82f, y),
            icon_color,
            stroke);
    } else if (draw_icon && icon == ActionIcon::Trash) {
        const float left = icon_left + icon_width * 0.14f;
        const float right = icon_left + icon_width * 0.86f;
        const float handle_left = icon_left + icon_width * 0.38f;
        const float handle_right = icon_left + icon_width * 0.62f;
        const float top = icon_top + height * 0.25f;
        const float lid_y = icon_top + height * 0.34f;
        const float body_top = icon_top + height * 0.43f;
        const float body_bottom = icon_top + height * 0.73f;

        draw_list->AddLine(ImVec2(handle_left, top), ImVec2(handle_right, top), icon_color, stroke);
        draw_list->AddLine(ImVec2(left, lid_y), ImVec2(right, lid_y), icon_color, stroke);
        draw_list->AddRect(
            ImVec2(left + icon_width * 0.05f, body_top),
            ImVec2(right - icon_width * 0.05f, body_bottom),
            icon_color,
            2.0f,
            0,
            stroke);
        draw_list->AddLine(
            ImVec2(icon_left + icon_width * 0.43f, body_top + height * 0.06f),
            ImVec2(icon_left + icon_width * 0.43f, body_bottom - height * 0.05f),
            icon_color,
            1.0f);
        draw_list->AddLine(
            ImVec2(icon_left + icon_width * 0.57f, body_top + height * 0.06f),
            ImVec2(icon_left + icon_width * 0.57f, body_bottom - height * 0.05f),
            icon_color,
            1.0f);
    }

    if (hovered && !tooltip.empty()) {
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    return clicked;
}

}  // namespace

void SpectralLinesGroupingViewUi::Render(
    SpectralLinesPanelController& panel,
    const SpectrumSnapshotHandle& snapshot,
    const SpectralLineGroupingView& view,
    std::size_t line_list_marker_count,
    UiLanguage language)
{
    const bool editable = view.editable;
    const bool search_active = view.search_active;

    if (editable) {
        const std::string add_group_label =
            StableUiLabel(
                language,
                UiTextId::AddGroup,
                "AddSpectralLineGroup");
        if (ImGui::Button(add_group_label.c_str())) {
            (void)panel.Submit(SpectralLineStateIntent::AddUserGroup(view.id));
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMarkerReferenceDragPayload)) {
                if (std::optional<MarkerReferenceDragPayload> drag = DecodeMarkerReferenceDragPayload(*payload)) {
                    if (drag->view_id == view.id) {
                        const bool copy = ImGui::GetIO().KeyCtrl;
                        (void)panel.Submit(
                            copy ? SpectralLineStateIntent::CopyMarkerReferenceToNewGroup(
                                       view.id,
                                       drag->marker_id,
                                       drag->source_group_id)
                                 : SpectralLineStateIntent::MoveMarkerReferenceToNewGroup(
                                       view.id,
                                       drag->marker_id,
                                       drag->source_group_id));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
    }

    const auto plot_view = ProjectSpectralLineList(panel.PlotSource(), snapshot);
    const std::string marker_count =
        FormatUiText(
            language,
            UiTextId::PlotVisibleCatalogMarkerCount,
            plot_view.visible_markers.size(),
            line_list_marker_count);
    RenderDisabledText(marker_count);

    const std::optional<UserGroupDragPayload> active_user_group_drag =
        editable ? CurrentUserGroupDragPayload() : std::nullopt;
    const bool group_reorder_drag_active =
        editable && active_user_group_drag && active_user_group_drag->view_id == view.id;

    if (view.groups.empty()) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoGroupsInView));
        return;
    }

    std::optional<UserGroupReorderGapResult> current_reorder_gap;
    std::optional<float> previous_group_midpoint_y;
    bool group_context_popup_open = false;
    const auto render_reorder_gap = [&](const SpectralLineGroupView& target_group) {
        if (!group_reorder_drag_active) {
            current_reorder_gap = std::nullopt;
            return;
        }
        ImGui::PushID("group_reorder_gap");
        ImGui::PushID(target_group.id.c_str());
        current_reorder_gap = RenderUserGroupReorderGap(false);
        ImGui::PopID();
        ImGui::PopID();
    };

    for (std::size_t group_index = 0; group_index < view.groups.size(); ++group_index) {
        const SpectralLineGroupView& group = view.groups[group_index];
        render_reorder_gap(group);

        const bool group_has_search_matches = !group.marker_references.empty();
        const bool group_dimmed_by_search = group.dimmed_by_search;

        ImGui::PushID(group.id.c_str());
        ImGui::AlignTextToFramePadding();
        bool next_group_visible = true;
        if (RenderGroupVisibilityControl(
                group.visibility,
                next_group_visible,
                language)) {
            (void)panel.Submit(
                SpectralLineStateIntent::SetGroupMarkerVisibility(view.id, group.id, next_group_visible));
        }

        ImGui::SameLine();
        const bool ordinary_group = !group.is_unassigned;
        const bool group_context_active = group_context_view_id_ && group_context_group_id_ &&
                                          *group_context_view_id_ == view.id &&
                                          *group_context_group_id_ == group.id;
        ImGuiTreeNodeFlags group_flags = ImGuiTreeNodeFlags_SpanFullWidth;
        if (group_reorder_drag_active) {
            group_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        } else if (editable && ordinary_group) {
            group_flags |= ImGuiTreeNodeFlags_AllowOverlap;
        }
        if (group_context_active) {
            group_flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool group_was_expanded = group.expanded;
        if (group_reorder_drag_active) {
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        } else if (search_active && group_has_search_matches) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else {
            ImGui::SetNextItemOpen(group_was_expanded, ImGuiCond_Always);
        }
        if (group_dimmed_by_search) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (group_reorder_drag_active) {
            const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        }
        const bool group_open = ImGui::TreeNodeEx(
            "group",
            group_flags,
            "%s (%zu)",
            LocalizedGroupName(
                language,
                group)
                .c_str(),
            group.marker_references.size());
        const ImVec2 group_item_min = ImGui::GetItemRectMin();
        const ImVec2 group_item_max = ImGui::GetItemRectMax();
        const bool group_row_hovered = ImGui::IsItemHovered();
        if (group_reorder_drag_active) {
            ImGui::PopStyleColor(2);
        }
        if (group_dimmed_by_search) {
            ImGui::PopStyleColor();
        }
        if (!group_reorder_drag_active && ImGui::IsItemToggledOpen()) {
            (void)panel.Submit(SpectralLineStateIntent::SetGroupExpanded(view.id, group.id, group_open));
        }
        const bool group_contents_open = !group_reorder_drag_active && group_open;
        const float group_midpoint_y = (group_item_min.y + group_item_max.y) * 0.5f;

        if (group_reorder_drag_active && current_reorder_gap) {
            const float target_min_y = previous_group_midpoint_y.value_or(current_reorder_gap->min_y);
            const ImRect target_rect(
                ImVec2(current_reorder_gap->line.x_min, target_min_y),
                ImVec2(current_reorder_gap->line.x_max, group_midpoint_y));
            if (std::optional<UserGroupDragPayload> drop = RenderUserGroupReorderTarget(
                    view.id,
                    target_rect,
                    current_reorder_gap->line,
                    ImGui::GetID("user_group_reorder_target"))) {
                (void)panel.Submit(
                    SpectralLineStateIntent::ReorderUserGroupBefore(view.id, drop->group_id, group.id));
            }
        }

        bool group_deleted = false;
        if (editable && ordinary_group && !group_reorder_drag_active && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            group_context_view_id_ = view.id;
            group_context_group_id_ = group.id;
        }
        if (editable && ordinary_group && !group_reorder_drag_active &&
            ImGui::BeginPopupContextItem("user_group_context")) {
            group_context_popup_open = true;
            group_context_view_id_ = view.id;
            group_context_group_id_ = group.id;
            const std::string rename_label =
                StableUiLabel(
                    language,
                    UiTextId::Rename,
                    "RenameSpectralLineGroup");
            if (ImGui::Selectable(rename_label.c_str())) {
                renaming_group_view_id_ = view.id;
                renaming_group_id_ = group.id;
                renaming_group_original_name_ =
                    group.name;
                renaming_group_name_ =
                    LocalizedGroupName(
                        language,
                        group);
                renaming_group_edited_ = false;
                renaming_group_popup_requested_ = true;
            }
            const std::string delete_label =
                StableUiLabel(
                    language,
                    UiTextId::Delete,
                    "DeleteSpectralLineGroup");
            if (ImGui::Selectable(delete_label.c_str())) {
                group_deleted = panel.Submit(SpectralLineStateIntent::DeleteUserGroup(view.id, group.id)).changed;
                group_context_view_id_.reset();
                group_context_group_id_.reset();
            }
            ImGui::EndPopup();
        }
        if (group_deleted) {
            if (group_contents_open) {
                ImGui::TreePop();
            }
            ImGui::PopID();
            continue;
        }

        if (editable && ordinary_group && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string drag_payload = EncodeUserGroupDragPayload(view.id, group.id);
            ImGui::SetDragDropPayload(
                kUserGroupDragPayload,
                drag_payload.data(),
                static_cast<int>(drag_payload.size()));
            const std::string display_name =
                LocalizedGroupName(
                    language,
                    group);
            ImGui::TextUnformatted(display_name.c_str());
            RenderDisabledText(
                UiText(
                    language,
                    UiTextId::DropBetweenGroupsToReorder));
            ImGui::EndDragDropSource();
        }

        if (editable && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMarkerReferenceDragPayload)) {
                if (std::optional<MarkerReferenceDragPayload> drag = DecodeMarkerReferenceDragPayload(*payload)) {
                    if (drag->view_id == view.id) {
                        const bool copy = ImGui::GetIO().KeyCtrl;
                        (void)panel.Submit(
                            copy ? SpectralLineStateIntent::CopyMarkerReference(
                                       view.id,
                                       drag->marker_id,
                                       drag->source_group_id,
                                       group.id)
                                 : SpectralLineStateIntent::MoveMarkerReference(
                                       view.id,
                                       drag->marker_id,
                                       drag->source_group_id,
                                       group.id));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (editable && ordinary_group && !group_reorder_drag_active) {
            const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
            const float action_width = ImGui::GetFrameHeight();
            const ImRect delete_rect(
                ImVec2(std::max(group_item_min.x, group_item_max.x - action_width), group_item_min.y),
                group_item_max);
            group_deleted = HiddenActionIconButton(
                "delete_group",
                delete_rect,
                ActionIcon::Trash,
                UiText(
                    language,
                    UiTextId::DisbandGroup),
                group_row_hovered) &&
                            panel.Submit(SpectralLineStateIntent::DeleteUserGroup(view.id, group.id)).changed;
            ImGui::SetCursorScreenPos(saved_cursor);
        }
        if (group_deleted) {
            if (group_contents_open) {
                ImGui::TreePop();
            }
            ImGui::PopID();
            continue;
        }

        previous_group_midpoint_y = group_midpoint_y;

        if (group_contents_open) {
            for (const SpectralLineMarkerReferenceView& reference : group.marker_references) {
                const bool resolved = reference.resolved;
                const bool marker_visible = reference.visible;
                const std::string& label = reference.label;

                ImGui::PushID(reference.marker_id.c_str());

                ImGui::AlignTextToFramePadding();
                const ImRect marker_hover_rect = CurrentFullWidthFrameRect();
                RenderCtrlMarkerReferenceHover(marker_hover_rect);
                bool checkbox_value = marker_visible;
                if (!resolved) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox("##marker_visibility", &checkbox_value) && resolved) {
                    (void)panel.Submit(
                        SpectralLineStateIntent::SetMarkerVisibility(reference.marker_id, checkbox_value));
                }
                if (!resolved) {
                    ImGui::EndDisabled();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string_view tooltip =
                        UiText(
                            language,
                            resolved
                                ? UiTextId::ShowOnPlot
                                : UiTextId::UnresolvedMarkerNotPlotted);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }

                ImGui::SameLine();
                PlotSeriesColor color_selection = reference.color;
                const ImVec4 resolved_color = ResolvePlotSeriesColor(
                    color_selection,
                    ActiveSemanticPalette(),
                    reference.automatic_color_slot);
                float marker_rgba[4]{
                    resolved_color.x,
                    resolved_color.y,
                    resolved_color.z,
                    resolved_color.w,
                };
                if (!resolved || !panel.CanCustomize()) {
                    ImGui::BeginDisabled();
                }
                ImGui::SetNextItemWidth(ImGui::GetFrameHeight());
                if (ImGui::ColorEdit4(
                        "##marker_color",
                        marker_rgba,
                        ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_AlphaBar |
                            ImGuiColorEditFlags_AlphaPreviewHalf) &&
                    resolved) {
                    color_selection =
                        PlotSeriesColor::ExplicitColor({
                            .red = marker_rgba[0],
                            .green = marker_rgba[1],
                            .blue = marker_rgba[2],
                            .alpha = marker_rgba[3],
                        });
                    (void)panel.Submit(
                        SpectralLineStateIntent::SetMarkerColor(
                            reference.marker_id,
                            color_selection));
                }
                if (!resolved || !panel.CanCustomize()) {
                    ImGui::EndDisabled();
                }
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string_view tooltip =
                        UiText(language, UiTextId::CurveColor);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }

                ImGui::SameLine();
                ImGuiSelectableFlags marker_flags =
                    ImGuiSelectableFlags_None;
                if (editable && ordinary_group) {
                    marker_flags |=
                        ImGuiSelectableFlags_AllowOverlap;
                }
                const std::string marker_suffix =
                    resolved
                        ? reference.wavelength_text
                        : std::string(
                              UiText(
                                  language,
                                  UiTextId::Unresolved));
                if (!resolved || !marker_visible) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                const std::string marker_row_label =
                    label + "  " + marker_suffix +
                    "###marker";
                (void)ImGui::Selectable(
                    marker_row_label.c_str(),
                    false,
                    marker_flags,
                    ImVec2(
                        ImGui::GetContentRegionAvail().x,
                        ImGui::GetFrameHeight()));
                const ImVec2 marker_item_min = ImGui::GetItemRectMin();
                const ImVec2 marker_item_max = ImGui::GetItemRectMax();
                const bool marker_row_hovered = ImGui::IsItemHovered();
                if (!resolved || !marker_visible) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    if (resolved && !reference.notes.empty()) {
                        ImGui::SetTooltip("%s\n%s", marker_suffix.c_str(), reference.notes.c_str());
                    } else {
                        ImGui::SetTooltip("%s", marker_suffix.c_str());
                    }
                }
                if (editable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    SubmitMarkerReferenceDragPayload(
                        view.id,
                        group.id,
                        reference.marker_id,
                        label,
                        language);
                    ImGui::EndDragDropSource();
                } else if (editable && BeginCtrlMarkerReferenceDragDropSource(ImRect(marker_item_min, marker_item_max))) {
                    SubmitMarkerReferenceDragPayload(
                        view.id,
                        group.id,
                        reference.marker_id,
                        label,
                        language);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("marker_context")) {
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::Separator();
                    ImGui::BeginDisabled(
                        !resolved || !panel.CanCustomize() ||
                        color_selection.mode() ==
                            PlotSeriesColorMode::Auto);
                    const std::string reset_color_label =
                        StableUiLabel(
                            language,
                            UiTextId::ResetColorToAuto,
                            "ResetSpectralLineColorToAuto");
                    if (ImGui::Selectable(
                            reset_color_label.c_str())) {
                        (void)panel.Submit(
                            SpectralLineStateIntent::SetMarkerColor(
                                reference.marker_id,
                                PlotSeriesColor::Auto()));
                    }
                    ImGui::EndDisabled();
                    if (editable) {
                        ImGui::Separator();
                        const std::string copy_to_group_label =
                            StableUiLabel(
                                language,
                                UiTextId::CopyToGroup,
                                "CopySpectralLineMarkerToGroup");
                        if (ImGui::BeginMenu(
                                copy_to_group_label.c_str())) {
                            bool has_target = false;
                            for (const SpectralLineGroupView& target_group : view.groups) {
                                if (target_group.id == group.id || target_group.is_unassigned) {
                                    continue;
                                }
                                has_target = true;
                                const std::string target_label =
                                    LocalizedGroupName(
                                        language,
                                        target_group) +
                                    "###" +
                                    target_group.id;
                                if (ImGui::Selectable(target_label.c_str())) {
                                    (void)panel.Submit(SpectralLineStateIntent::CopyMarkerReference(
                                        view.id,
                                        reference.marker_id,
                                        group.id,
                                        target_group.id));
                                }
                            }
                            if (!has_target) {
                                RenderDisabledText(
                                    UiText(
                                        language,
                                        UiTextId::NoOtherGroups));
                            }
                            ImGui::EndMenu();
                        }
                    }
                    ImGui::EndPopup();
                }

                if (reference.shared) {
                    ImGui::SameLine();
                    RenderSharedReferenceMarker(language);
                }

                bool reference_removed = false;
                if (editable && ordinary_group) {
                    const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
                    const float action_width = ImGui::GetFrameHeight();
                    const ImRect remove_rect(
                        ImVec2(std::max(marker_item_min.x, marker_item_max.x - action_width), marker_item_min.y),
                        marker_item_max);
                    reference_removed = HiddenActionIconButton(
                        "remove_reference",
                        remove_rect,
                        ActionIcon::Minus,
                        UiText(
                            language,
                            UiTextId::RemoveFromThisGroup),
                        marker_row_hovered) &&
                                        panel.Submit(SpectralLineStateIntent::RemoveMarkerReference(
                                            view.id,
                                            reference.marker_id,
                                            group.id)).changed;
                    ImGui::SetCursorScreenPos(saved_cursor);
                }

                ImGui::PopID();
                if (reference_removed) {
                    break;
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (!group_context_popup_open && !renaming_group_popup_requested_) {
        group_context_view_id_.reset();
        group_context_group_id_.reset();
    }
}

void SpectralLinesGroupingViewUi::RenderPendingPopups(
    SpectralLinesPanelController& panel,
    UiLanguage language)
{
    const std::string rename_group_popup =
        StableUiLabel(
            language,
            UiTextId::RenameGroup,
            "RenameUserGroupPopup");
    if (renaming_group_popup_requested_) {
        ImGui::OpenPopup(rename_group_popup.c_str());
        renaming_group_popup_requested_ = false;
    }

    if (ImGui::BeginPopupModal(
            rename_group_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const std::string name_label =
            StableUiLabel(
                language,
                UiTextId::Name,
                "SpectralLineGroupName");
        const bool submitted = ImGui::InputText(
            name_label.c_str(),
            &renaming_group_name_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) {
            renaming_group_edited_ = true;
        }
        const bool valid_name =
            HasNonWhitespace(
                renaming_group_name_);
        const auto finish_rename = [this, &panel]() {
            if (renaming_group_view_id_ && renaming_group_id_) {
                const std::string submitted_name =
                    ResolveSpectralLineRenameSubmission(
                        renaming_group_name_,
                        renaming_group_original_name_,
                        renaming_group_edited_);
                (void)panel.Submit(SpectralLineStateIntent::RenameUserGroup(
                    *renaming_group_view_id_,
                    *renaming_group_id_,
                    submitted_name,
                    renaming_group_edited_
                        ? SpectralLineRenameEditState::Edited
                        : SpectralLineRenameEditState::Unedited));
            }
            renaming_group_view_id_.reset();
            renaming_group_id_.reset();
            renaming_group_name_.clear();
            renaming_group_original_name_.clear();
            renaming_group_edited_ = false;
            ImGui::CloseCurrentPopup();
        };
        if (!valid_name) {
            ImGui::BeginDisabled();
        }
        const std::string rename_label =
            StableUiLabel(
                language,
                UiTextId::Rename,
                "ConfirmRenameSpectralLineGroup");
        if (ImGui::Button(rename_label.c_str()) ||
            (submitted && valid_name)) {
            finish_rename();
        }
        if (!valid_name) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const std::string cancel_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "CancelRenameSpectralLineGroup");
        if (ImGui::Button(cancel_label.c_str())) {
            renaming_group_view_id_.reset();
            renaming_group_id_.reset();
            renaming_group_name_.clear();
            renaming_group_original_name_.clear();
            renaming_group_edited_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace spectiary
