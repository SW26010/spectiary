#include "ui/sample_workflow_panel.h"

#include "app/local_user_state.h"
#include "ui/sample_annotation_labeling_rules.h"
#include "ui/sample_labeling_issue_text.h"

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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kLabelingWindow = "Labeling###SpecForgeLabelingV1";
constexpr const char* kFiltersWindow = "Sample Filters###SpecForgeFiltersV1";
constexpr const char* kSortingWindow = "Sample Sorting###SpecForgeSampleSortingV1";
constexpr const char* kSampleAnnotationDragPayload = "SPECFORGE_SAMPLE_ANNOTATION_PATH";

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

template <typename... Args>
std::string FormatUiText(
    UiLanguage language,
    UiTextId text_id,
    Args... args)
{
    const std::string_view format =
        UiText(language, text_id);
    const int required = std::snprintf(
        nullptr,
        0,
        format.data(),
        args...);
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

void RenderText(std::string_view text)
{
    ImGui::TextUnformatted(
        text.data(),
        text.data() + text.size());
}

void RenderDisabledText(std::string_view text)
{
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(text.size()),
        text.data());
}

std::string LocalizedSampleLabelValue(
    UiLanguage language,
    const SampleLabelSet& label_set,
    int code)
{
    if (code == kUnlabeledSampleLabelCode) {
        const bool use_cjk_punctuation =
            language ==
            UiLanguage::SimplifiedChinese;
        return std::string(
                   UiText(
                       language,
                       UiTextId::UnlabeledValue)) +
               (use_cjk_punctuation ? "（" : " (") +
               std::to_string(code) +
               (use_cjk_punctuation ? "）" : ")");
    }
    return FormatSampleLabelValue(
        label_set,
        code);
}

std::string LabelingTaskDisplayName(
    UiLanguage language,
    bool temporary,
    std::string_view stable_name)
{
    return temporary
        ? std::string(
              UiText(
                  language,
                  UiTextId::TemporaryLabelingTask))
        : std::string(stable_name);
}

std::string TemporaryDraftIdentityKey(
    std::string_view source_identity,
    std::string_view task_id)
{
    std::string key;
    key.reserve(source_identity.size() + task_id.size() + 1);
    key.append(source_identity);
    key.push_back('\n');
    key.append(task_id);
    return key;
}

void AppendRecoveryFingerprintField(
    std::string& fingerprint,
    std::string_view value)
{
    fingerprint += std::to_string(value.size());
    fingerprint.push_back(':');
    fingerprint.append(value);
    fingerprint.push_back('|');
}

void AppendRecoveryFingerprintField(
    std::string& fingerprint,
    std::size_t value)
{
    AppendRecoveryFingerprintField(
        fingerprint,
        std::to_string(value));
}

std::string TemporaryDraftRecoveryFingerprint(
    const SourceCollectionLabelingRecoveryDraftView& draft)
{
    std::string fingerprint;
    AppendRecoveryFingerprintField(
        fingerprint,
        static_cast<std::size_t>(draft.status));
    AppendRecoveryFingerprintField(
        fingerprint,
        draft.task_name);
    AppendRecoveryFingerprintField(
        fingerprint,
        draft.labeled_count);
    AppendRecoveryFingerprintField(
        fingerprint,
        draft.sample_count);
    AppendRecoveryFingerprintField(
        fingerprint,
        static_cast<std::size_t>(draft.save_state.kind));
    AppendRecoveryFingerprintField(
        fingerprint,
        draft.save_state.pending_count);
    AppendRecoveryFingerprintField(
        fingerprint,
        static_cast<std::size_t>(draft.save_state.message_kind));
    AppendRecoveryFingerprintField(
        fingerprint,
        draft.save_state.message);
    return fingerprint;
}

std::string TemporaryDraftRecoveryRowToken(
    const SourceCollectionLabelingView& labeling_view,
    std::size_t draft_index)
{
    const SourceCollectionLabelingRecoveryDraftView& draft =
        labeling_view.recovery_drafts.at(draft_index);
    const std::string fingerprint =
        TemporaryDraftRecoveryFingerprint(draft);
    std::size_t duplicate_ordinal = 0;
    for (std::size_t index = 0; index < draft_index; ++index) {
        const SourceCollectionLabelingRecoveryDraftView& previous =
            labeling_view.recovery_drafts[index];
        if (previous.task_id == draft.task_id &&
            TemporaryDraftRecoveryFingerprint(previous) == fingerprint) {
            ++duplicate_ordinal;
        }
    }

    std::string token = TemporaryDraftIdentityKey(
        labeling_view.source_identity,
        draft.task_id);
    token.push_back('\n');
    token.append(fingerprint);
    token.push_back('\n');
    token.append(std::to_string(duplicate_ordinal));
    return token;
}

UiTextId TemporaryDraftStatusTextId(
    SampleLabelingRecoveryDraftStatus status)
{
    switch (status) {
    case SampleLabelingRecoveryDraftStatus::Current:
        return UiTextId::TemporaryDraftCurrent;
    case SampleLabelingRecoveryDraftStatus::Conflicting:
        return UiTextId::TemporaryDraftConflicting;
    case SampleLabelingRecoveryDraftStatus::Stale:
        return UiTextId::TemporaryDraftStale;
    case SampleLabelingRecoveryDraftStatus::Recoverable:
    default:
        return UiTextId::TemporaryDraftRecoverable;
    }
}

std::string LocalizedSampleLabelShortcut(
    UiLanguage language,
    char shortcut)
{
    return shortcut == '\0'
        ? std::string(
              UiText(
                  language,
                  UiTextId::NoneValue))
        : FormatSampleLabelShortcut(shortcut);
}

std::string SampleLabelSaveMessageText(
    UiLanguage language,
    const SampleLabelSaveState& save_state)
{
    const std::string_view localized =
        UiText(
            language,
            save_state.message_kind);
    return localized.empty()
        ? save_state.message
        : std::string(localized);
}

std::string LocalizedFilterOptionText(
    UiLanguage language,
    const SampleFilterValueOption& option)
{
    if (!option.represents_unlabeled_value) {
        return option.display_text;
    }
    const bool use_cjk_punctuation =
        language ==
        UiLanguage::SimplifiedChinese;
    return std::string(
               UiText(
                   language,
                   UiTextId::UnlabeledValue)) +
           (use_cjk_punctuation ? "（" : " (") +
           option.key +
           (use_cjk_punctuation ? "）" : ")");
}

std::string SampleSortSourceDisplayName(
    UiLanguage language,
    const SourceCollectionSampleSortSourceView& source)
{
    if (source.id == "source-order") {
        return std::string(
            UiText(
                language,
                UiTextId::SourceOrder));
    }
    if (source.id == "sample-name") {
        return std::string(
            UiText(
                language,
                UiTextId::SampleNameSortSource));
    }
    return source.name;
}

std::string SampleFilterDiagnosticText(
    UiLanguage language,
    const SampleFilterDiagnostic& diagnostic)
{
    switch (diagnostic.kind) {
    case SampleFilterDiagnosticKind::SourceNotFilterable:
        return FormatUiText(
            language,
            UiTextId::FilterSourceNotFilterable,
            diagnostic.source_name.c_str());
    case SampleFilterDiagnosticKind::SampleCountChanged:
        return FormatUiText(
            language,
            UiTextId::FilterSampleCountChanged,
            diagnostic.source_name.c_str());
    case SampleFilterDiagnosticKind::SourceNotLoaded:
    default:
        return std::string(
            UiText(
                language,
                UiTextId::FilterSourceNotLoaded));
    }
}

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
    std::string_view tooltip,
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

    if (item_visible && hovered && !tooltip.empty()) {
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    return clicked;
}

bool ActionIconButton(
    const char* id,
    const ImVec2& size,
    ActionIcon icon,
    std::string_view tooltip,
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

    if (hovered && !tooltip.empty()) {
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    return clicked;
}

bool DirectionIconButton(
    const char* id,
    SampleNavigationSortDirection direction,
    bool active,
    std::string_view tooltip)
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

    if (ImGui::IsItemHovered() && !tooltip.empty()) {
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
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

const SourceCollectionAnnotationValueView* AcceptSampleLabelingTaskDrop(
    const SourceCollectionSessionView& session_view,
    bool task_switch_locked,
    bool& accepted)
{
    accepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSampleAnnotationDragPayload, flags)) {
        const std::filesystem::path annotation_path = Utf8ToPath(PayloadString(*payload));
        const SourceCollectionAnnotationValueView* annotation =
            FindAnnotationViewByPath(session_view, annotation_path);
        if (annotation == nullptr || !annotation->can_activate_labeling || task_switch_locked) {
            return nullptr;
        }
        accepted = true;
        if (payload->IsDelivery()) {
            return annotation;
        }
    }
    return nullptr;
}

SampleNavigationSortDirection OppositeSortDirection(SampleNavigationSortDirection direction)
{
    return direction == SampleNavigationSortDirection::Ascending
        ? SampleNavigationSortDirection::Descending
        : SampleNavigationSortDirection::Ascending;
}

std::string_view SortDirectionTooltip(
    UiLanguage language,
    SampleNavigationSortDirection direction)
{
    return direction == SampleNavigationSortDirection::Ascending
        ? UiText(
              language,
              UiTextId::Ascending)
        : UiText(
              language,
              UiTextId::Descending);
}

SampleSortSourceRowAction RenderSampleSortSourceRow(
    const SourceCollectionSampleSortSourceView& source_view,
    SampleNavigationSortDirection direction,
    UiLanguage language)
{
    SampleSortSourceRowAction action;
    const std::string display_name =
        SampleSortSourceDisplayName(
            language,
            source_view);
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
    if (DirectionIconButton(
            "direction",
            direction,
            source_view.selected,
            SortDirectionTooltip(
                language,
                direction))) {
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
            ? display_name
            : UserPathDisplayText(source_view.annotation_path);
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
    const ImRect label_rect(label_min, ImVec2(label_min.x + label_width, label_min.y + frame_height));
    ImGui::RenderTextClipped(
        label_rect.Min,
        label_rect.Max,
        display_name.c_str(),
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
                UiText(
                    language,
                    UiTextId::RemoveSampleSorting),
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

std::string_view SaveStateLabel(
    UiLanguage language,
    SampleLabelSaveStateKind kind)
{
    switch (kind) {
    case SampleLabelSaveStateKind::InternalDraftOnly:
        return UiText(
            language,
            UiTextId::InternalAutosaveDraft);
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return UiText(
            language,
            UiTextId::AutosavedToOutput);
    case SampleLabelSaveStateKind::Pending:
        return UiText(
            language,
            UiTextId::PendingSave);
    case SampleLabelSaveStateKind::Failed:
        return UiText(
            language,
            UiTextId::SaveFailedValue);
    default:
        return UiText(
            language,
            UiTextId::UnknownValue);
    }
}

std::string SaveStateStatusText(
    UiLanguage language,
    const SampleLabelSaveState& save_state)
{
    const std::string save_state_text(
        SaveStateLabel(
            language,
            save_state.kind));
    std::string save_status =
        FormatUiText(
            language,
            UiTextId::SaveStatus,
            save_state_text.c_str());
    if (save_state.kind == SampleLabelSaveStateKind::Pending ||
        save_state.kind == SampleLabelSaveStateKind::Failed) {
        save_status +=
            " (" +
            std::to_string(save_state.pending_count) +
            ")";
    }
    return save_status;
}

std::string_view SaveStateReminder(
    UiLanguage language,
    const SourceCollectionLabelingView& labeling_view)
{
    switch (labeling_view.save_state.kind) {
    case SampleLabelSaveStateKind::InternalDraftOnly:
        return UiText(
            language,
            UiTextId::LabelSaveStateInternalDraft);
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return UiText(
            language,
            UiTextId::LabelSaveStateAutosaved);
    case SampleLabelSaveStateKind::Pending:
        return UiText(
            language,
            UiTextId::LabelSaveStatePending);
    case SampleLabelSaveStateKind::Failed:
        if (labeling_view.active_task_is_temporary) {
            return UiText(
                language,
                UiTextId::LabelSaveStateTemporaryFailed);
        }
        return UiText(
            language,
            UiTextId::LabelSaveStateOutputFailed);
    default:
        return UiText(
            language,
            UiTextId::LabelSaveStateUnknown);
    }
}

bool AnnotationActivationNeedsConfirmation(SampleAnnotationWorkflowRelationship relationship)
{
    return relationship != SampleAnnotationWorkflowRelationship::LocalLabelingTask;
}

}  // namespace

std::string SampleWorkflowPanelUi::RecoveryDraftRowToken(
    const SourceCollectionLabelingView& labeling_view,
    std::size_t draft_index)
{
    return TemporaryDraftRecoveryRowToken(
        labeling_view,
        draft_index);
}

std::string SampleWorkflowTemporaryDraftTaskName(
    UiLanguage language,
    std::string_view task_name)
{
    return task_name.empty() ||
            task_name == kTemporarySampleLabelingTaskName
        ? std::string(
              UiText(
                  language,
                  UiTextId::TemporaryLabelingTask))
        : std::string(task_name);
}

std::string SampleWorkflowSaveStateText(
    UiLanguage language,
    const SampleLabelSaveState& save_state)
{
    return SaveStateStatusText(language, save_state);
}

std::string SampleWorkflowSaveMessageText(
    UiLanguage language,
    const SampleLabelSaveState& save_state)
{
    return SampleLabelSaveMessageText(language, save_state);
}

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
    pending_delete_task_is_temporary_ = false;
    pending_delete_task_is_recovery_ = false;
    pending_delete_task_source_identity_.clear();
    pending_delete_task_id_.clear();
    pending_annotation_activation_relationship_ = SampleAnnotationWorkflowRelationship::PlainAnnotation;
}

void SampleWorkflowPanelUi::ClearLabelingOperationMessage()
{
    labeling_operation_message_.clear();
    labeling_operation_text_id_.reset();
}

void SampleWorkflowPanelUi::ResetLabelShortcutCapture()
{
    label_shortcut_capture_active_ = false;
    pending_conflicting_shortcut_ = '\0';
    label_shortcut_notice_kind_ =
        LabelShortcutNoticeKind::None;
    label_shortcut_notice_shortcut_.clear();
    label_shortcut_notice_owner_.clear();
}

std::string SampleWorkflowPanelUi::LabelShortcutNotice(
    UiLanguage language) const
{
    switch (label_shortcut_notice_kind_) {
    case LabelShortcutNoticeKind::CaptureInstructions:
        return std::string(
            UiText(
                language,
                UiTextId::ShortcutCaptureInstructions));
    case LabelShortcutNoticeKind::UnboundOnSave:
        return std::string(
            UiText(
                language,
                UiTextId::ShortcutUnboundOnSave));
    case LabelShortcutNoticeKind::Unsupported:
        return std::string(
            UiText(
                language,
                UiTextId::ShortcutKeysOnly));
    case LabelShortcutNoticeKind::Selected:
        return FormatUiText(
            language,
            UiTextId::ShortcutSelected,
            label_shortcut_notice_shortcut_.c_str());
    case LabelShortcutNoticeKind::WillMove:
        return FormatUiText(
            language,
            UiTextId::ShortcutWillMove,
            label_shortcut_notice_shortcut_.c_str(),
            label_shortcut_notice_owner_.c_str());
    case LabelShortcutNoticeKind::Conflict:
        return FormatUiText(
            language,
            UiTextId::ShortcutConflict,
            label_shortcut_notice_shortcut_.c_str(),
            label_shortcut_notice_owner_.c_str(),
            label_shortcut_notice_shortcut_.c_str());
    case LabelShortcutNoticeKind::None:
    default:
        return {};
    }
}

void SampleWorkflowPanelUi::RenderLabeling(
    PanelSessionInteraction& interaction,
    bool* open,
    const std::function<std::optional<std::filesystem::path>()>& choose_output_path,
    SampleWorkflowShortcut& shortcut)
{
    RenderLabeling(
        interaction,
        UiLanguage::English,
        open,
        choose_output_path,
        shortcut);
}

void SampleWorkflowPanelUi::CaptureLabelingOperationResult(
    const SourceCollectionSessionResult& result,
    UiLanguage language)
{
    const SampleLabelingIssueTextDescriptor issue_text =
        SampleLabelingIssueTextFor(result.labeling_issue);
    const UiTextId message_text_id = issue_text.text_id;
    if (message_text_id != UiTextId::Count) {
        labeling_operation_text_id_ = message_text_id;
        labeling_operation_message_ =
            UiText(
                language,
                message_text_id);
        return;
    }
    if (!result.message.empty()) {
        labeling_operation_text_id_.reset();
        labeling_operation_message_ =
            result.message;
    } else {
        ClearLabelingOperationMessage();
    }
}

void SampleWorkflowPanelUi::RenderLabeling(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open,
    const std::function<std::optional<std::filesystem::path>()>& choose_output_path,
    SampleWorkflowShortcut& shortcut)
{
    labeling_selector_rect_.reset();
    labeling_pause_rect_.reset();
    labeling_delete_rect_.reset();
    labeling_recovery_rect_.reset();
    temporary_labeling_action_rect_.reset();
    labeling_delete_confirmation_rect_.reset();
    recovery_action_rects_.clear();
    recovery_identity_rects_.clear();
    const auto capture_result = [this, language](
                                    SourceCollectionSessionResult result) {
        CaptureLabelingOperationResult(
            result,
            language);
        return result;
    };
    const auto submit = [&interaction, &capture_result](
                            SourceCollectionSessionIntent intent) {
        return capture_result(
            std::move(
                interaction.Submit(
                    std::move(intent))
                    .result));
    };
    const auto capture_delete_result = [this, language](
                                          SourceCollectionSessionResult result) {
        if (result.labeling_issue ==
            SampleLabelingOperationResult::Issue::EditTargetChanged) {
            labeling_operation_text_id_ =
                UiTextId::LabelingDeleteTargetChanged;
            labeling_operation_message_ =
                UiText(
                    language,
                    UiTextId::LabelingDeleteTargetChanged);
        }
        return result;
    };
    const auto submit_auto_advance =
        [&interaction, &capture_result](
            SourceCollectionSessionIntent intent) {
            return capture_result(
                std::move(
                    interaction.SubmitAutoAdvance(
                        std::move(intent))
                        .result));
    };
    shortcut = {};
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::Labeling,
        "SpecForgeLabelingV1");
    const std::string annotation_to_labeling_popup =
        StableUiLabel(
            language,
            UiTextId::UseAnnotationAsLabelingTask,
            "SpecForgeAnnotationToLabelingPopup");
    const std::string delete_labeling_task_popup =
        StableUiLabel(
            language,
            UiTextId::DeleteLabelingTaskQuestion,
            "SpecForgeDeleteLabelingTaskPopup");
    const std::string delete_sample_label_popup =
        StableUiLabel(
            language,
            UiTextId::DeleteLabelQuestion,
            "SpecForgeDeleteSampleLabelPopup");
    const std::string change_sample_label_code_popup =
        StableUiLabel(
            language,
            UiTextId::ChangeUsedLabelCodeQuestion,
            "SpecForgeChangeSampleLabelCodePopup");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ResetLabelShortcutCapture();
        ImGui::End();
        return;
    }
    const bool labeling_context_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool labeling_context_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const auto route_latest_labeling_shortcuts = [&](bool blocked) {
        const SourceCollectionLabelingView& latest_labeling =
            interaction.View().labeling;
        shortcut = RouteSampleWorkflowShortcut(
            {
                .focused = labeling_context_focused,
                .hovered = labeling_context_hovered,
                .labeling_enabled = latest_labeling.has_active_task,
                .blocked = blocked,
            },
            latest_labeling.label_set);
    };

    const SourceCollectionSessionView& session_view =
        interaction.View();
    const SourceCollectionLabelingView labeling_view = session_view.labeling;
    const std::optional<std::size_t> current_index = labeling_view.current_index;
    std::unordered_map<std::string, std::size_t> labeling_task_id_counts;
    if (!labeling_view.task_ids.empty()) {
        for (const std::string& task_id : labeling_view.task_ids) {
            ++labeling_task_id_counts[task_id];
        }
    } else {
        // Keep lightweight panel fixtures useful while production projections
        // are expected to include every formal and temporary task identity.
        for (const SourceCollectionLabelingRecoveryDraftView& draft :
             labeling_view.recovery_drafts) {
            ++labeling_task_id_counts[draft.task_id];
        }
    }
    const auto recovery_task_id_is_ambiguous =
        [&labeling_task_id_counts](std::string_view task_id) {
            const auto found = labeling_task_id_counts.find(std::string(task_id));
            return found != labeling_task_id_counts.end() && found->second > 1;
        };
    bool recovery_action_submitted = false;
    bool open_delete_task_popup = false;
    const auto render_recovery_drafts = [&]() {
        const ImVec2 recovery_top = ImGui::GetCursorScreenPos();
        const auto finish_recovery_layout =
            [this, &recovery_top](const ImVec2 recovery_bottom) {
                labeling_recovery_rect_ = {
                    recovery_top.x,
                    recovery_top.y,
                    recovery_bottom.x,
                    recovery_bottom.y};
            };
        const std::string current_source_prefix =
            labeling_view.source_identity + "\n";
        const auto render_recovery_task_identity =
            [this, language](std::string_view task_id, std::string_view key) {
                const std::string task_id_value(task_id);
                const std::string task_identity = FormatUiText(
                    language,
                    UiTextId::TemporaryDraftTaskIdentity,
                    task_id_value.c_str());
                RenderDisabledText(task_identity);
                const ImRect rect = GImGui->LastItemData.Rect;
                recovery_identity_rects_.insert_or_assign(
                    std::string(key),
                    std::array<float, 4>{
                        rect.Min.x,
                        rect.Min.y,
                        rect.Max.x,
                        rect.Max.y});
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s",
                        task_identity.c_str());
                }
            };
        if (labeling_view.recovery_drafts.empty()) {
            std::erase_if(
                retained_recovery_drafts_,
                [&current_source_prefix](const std::string& key) {
                    return key.compare(
                               0,
                               current_source_prefix.size(),
                               current_source_prefix) == 0;
                });
            std::erase_if(
                recovery_draft_fingerprints_,
                [&current_source_prefix](const auto& entry) {
                    return entry.first.compare(
                               0,
                               current_source_prefix.size(),
                               current_source_prefix) == 0;
                });
            finish_recovery_layout(ImGui::GetCursorScreenPos());
            return;
        }

        std::unordered_set<std::string> visible_recovery_drafts;
        std::unordered_map<std::string, std::string>
            visible_recovery_fingerprints;
        visible_recovery_drafts.reserve(
            labeling_view.recovery_drafts.size());
        visible_recovery_fingerprints.reserve(
            labeling_view.recovery_drafts.size());
        for (std::size_t draft_index = 0;
             draft_index < labeling_view.recovery_drafts.size();
             ++draft_index) {
            const SourceCollectionLabelingRecoveryDraftView& draft =
                labeling_view.recovery_drafts[draft_index];
            const std::string key = RecoveryDraftRowToken(
                labeling_view,
                draft_index);
            visible_recovery_drafts.insert(key);
            visible_recovery_fingerprints.insert_or_assign(
                key,
                TemporaryDraftRecoveryFingerprint(draft));
        }
        std::erase_if(
            retained_recovery_drafts_,
            [
                &current_source_prefix,
                &visible_recovery_drafts,
                &visible_recovery_fingerprints,
                this](const std::string& key) {
                if (key.compare(
                        0,
                        current_source_prefix.size(),
                        current_source_prefix) != 0) {
                    return false;
                }
                if (!visible_recovery_drafts.contains(key)) {
                    return true;
                }
                const auto current =
                    visible_recovery_fingerprints.find(key);
                const auto previous =
                    recovery_draft_fingerprints_.find(key);
                return current != visible_recovery_fingerprints.end() &&
                       previous != recovery_draft_fingerprints_.end() &&
                       current->second != previous->second;
            });
        std::erase_if(
            recovery_draft_fingerprints_,
            [
                &current_source_prefix,
                &visible_recovery_drafts](const auto& entry) {
                return entry.first.compare(
                           0,
                           current_source_prefix.size(),
                           current_source_prefix) == 0 &&
                       !visible_recovery_drafts.contains(entry.first);
            });
        for (const auto& [key, fingerprint] : visible_recovery_fingerprints) {
            recovery_draft_fingerprints_.insert_or_assign(key, fingerprint);
        }

        ImGui::Separator();
        RenderText(
            UiText(
                language,
                UiTextId::TemporaryDraftRecovery));
        const std::string source_identity = FormatUiText(
            language,
            UiTextId::TemporaryDraftSourceIdentity,
            labeling_view.source_identity.c_str());
        RenderDisabledText(source_identity);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", source_identity.c_str());
        }
        const bool compact_layout =
            ImGui::GetContentRegionAvail().x < 720.0f;
        const auto render_recovery_actions = [&](
                                                const SourceCollectionLabelingRecoveryDraftView& draft,
                                                std::string_view key,
                                                bool retained,
                                                bool duplicate_task_id) {
            if (draft.status ==
                SampleLabelingRecoveryDraftStatus::Current) {
                RenderDisabledText(
                    UiText(
                        language,
                        UiTextId::TemporaryDraftCurrent));
                return;
            }

            const auto record_action_rect =
                [this, key](std::string_view stable_id) {
                    const ImRect rect = GImGui->LastItemData.Rect;
                    std::string action_key(key);
                    action_key.push_back('\n');
                    action_key.append(stable_id);
                    recovery_action_rects_.insert_or_assign(
                        std::move(action_key),
                        std::array<float, 4>{
                            rect.Min.x,
                            rect.Min.y,
                            rect.Max.x,
                            rect.Max.y});
                };
            const bool recovery_activation_disabled =
                labeling_view.has_active_task &&
                !labeling_view.can_deactivate_task;
            const bool recover_disabled =
                recovery_activation_disabled || duplicate_task_id;
            const std::string recover_label = StableUiLabel(
                language,
                UiTextId::RecoverTemporaryDraft,
                "SpecForgeRecoverTemporaryDraft");
            if (recover_disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(recover_label.c_str())) {
                (void)submit(
                    ChangeActiveSampleWorkflow(
                        ActiveSampleWorkflowIntent::
                            RecoverTemporaryLabelingTask(
                                labeling_view.source_identity,
                        draft.task_id)));
                recovery_action_submitted = true;
            }
            record_action_rect("SpecForgeRecoverTemporaryDraft");
            if (recover_disabled) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string_view tooltip = duplicate_task_id
                        ? UiText(
                              language,
                              UiTextId::TemporaryDraftDuplicateIdentity)
                        : UiText(
                              language,
                              UiTextId::OutputAutosaveCloseBlocked);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }
            }
            if (!compact_layout) {
                ImGui::SameLine();
            }
            const std::string keep_label = StableUiLabel(
                language,
                retained
                    ? UiTextId::TemporaryDraftKept
                    : UiTextId::KeepTemporaryDraft,
                "SpecForgeKeepTemporaryDraft");
            if (retained) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(keep_label.c_str())) {
                retained_recovery_drafts_.insert(std::string(key));
            }
            if (retained) {
                ImGui::EndDisabled();
            }
            record_action_rect("SpecForgeKeepTemporaryDraft");
            if (!compact_layout) {
                ImGui::SameLine();
            }
            const std::string delete_draft_label = StableUiLabel(
                language,
                UiTextId::DeleteTemporaryDraft,
                "SpecForgeDeleteTemporaryDraft");
            if (duplicate_task_id) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(delete_draft_label.c_str())) {
                pending_delete_task_name_ = draft.task_name;
                pending_delete_task_is_temporary_ = true;
                pending_delete_task_is_recovery_ = true;
                pending_delete_task_source_identity_ =
                    labeling_view.source_identity;
                pending_delete_task_id_ = draft.task_id;
                open_delete_task_popup = true;
            }
            record_action_rect("SpecForgeDeleteTemporaryDraft");
            if (duplicate_task_id) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string_view tooltip = UiText(
                        language,
                        UiTextId::TemporaryDraftDuplicateIdentity);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }
                RenderDisabledText(
                    UiText(
                        language,
                        UiTextId::TemporaryDraftDuplicateIdentity));
            }
        };
        if (compact_layout) {
            for (std::size_t draft_index = 0;
                 draft_index < labeling_view.recovery_drafts.size();
                 ++draft_index) {
                const SourceCollectionLabelingRecoveryDraftView& draft =
                    labeling_view.recovery_drafts[draft_index];
                const std::string key = RecoveryDraftRowToken(
                    labeling_view,
                    draft_index);
                const std::string row_id =
                    key + "#row";
                const bool retained =
                    retained_recovery_drafts_.contains(key);
                const bool duplicate_task_id =
                    recovery_task_id_is_ambiguous(draft.task_id);
                ImGui::PushID(row_id.c_str());
                ImGui::BeginGroup();
                RenderText(
                    SampleWorkflowTemporaryDraftTaskName(
                        language,
                        draft.task_name));
                render_recovery_task_identity(
                    draft.task_id,
                    key);
                RenderText(
                    UiText(
                        language,
                        TemporaryDraftStatusTextId(draft.status)));
                RenderText(
                    FormatUiText(
                        language,
                        UiTextId::LabelingProgress,
                        static_cast<unsigned long long>(draft.labeled_count),
                        static_cast<unsigned long long>(draft.sample_count)));
                RenderText(
                    SampleWorkflowSaveStateText(
                        language,
                        draft.save_state));
                const std::string save_message =
                    SampleWorkflowSaveMessageText(
                        language,
                        draft.save_state);
                if (!save_message.empty()) {
                    RenderDisabledText(save_message);
                }
                render_recovery_actions(
                    draft,
                    key,
                    retained,
                    duplicate_task_id);
                ImGui::EndGroup();
                ImGui::Separator();
                ImGui::PopID();
            }
            finish_recovery_layout(ImGui::GetCursorScreenPos());
            return;
        }
        if (ImGui::BeginTable(
                "temporary_draft_recovery",
                6,
                ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_NoHostExtendX)) {
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftTaskColumn)
                    .data());
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftIdentityColumn)
                    .data());
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftStatusColumn)
                    .data());
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftProgressColumn)
                    .data());
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftSaveColumn)
                    .data());
            ImGui::TableSetupColumn(
                UiText(
                    language,
                    UiTextId::TemporaryDraftActionsColumn)
                    .data(),
                ImGuiTableColumnFlags_WidthFixed,
                400.0f);
            ImGui::TableHeadersRow();

            for (std::size_t draft_index = 0;
                 draft_index < labeling_view.recovery_drafts.size();
                 ++draft_index) {
                const SourceCollectionLabelingRecoveryDraftView& draft =
                    labeling_view.recovery_drafts[draft_index];
                const std::string key = RecoveryDraftRowToken(
                    labeling_view,
                    draft_index);
                const std::string row_id =
                    key + "#row";
                const bool retained =
                    retained_recovery_drafts_.contains(key);
                const bool duplicate_task_id =
                    recovery_task_id_is_ambiguous(draft.task_id);
                ImGui::PushID(row_id.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string task_name =
                    SampleWorkflowTemporaryDraftTaskName(
                        language,
                        draft.task_name);
                RenderText(task_name);
                ImGui::TableSetColumnIndex(1);
                render_recovery_task_identity(
                    draft.task_id,
                    key);
                ImGui::TableSetColumnIndex(2);
                RenderText(
                    UiText(
                        language,
                        TemporaryDraftStatusTextId(draft.status)));
                ImGui::TableSetColumnIndex(3);
                const std::string progress = FormatUiText(
                    language,
                    UiTextId::LabelingProgress,
                    static_cast<unsigned long long>(draft.labeled_count),
                    static_cast<unsigned long long>(draft.sample_count));
                RenderText(progress);
                ImGui::TableSetColumnIndex(4);
                RenderText(
                    SampleWorkflowSaveStateText(
                        language,
                        draft.save_state));
                const std::string save_message =
                    SampleWorkflowSaveMessageText(
                        language,
                        draft.save_state);
                if (!save_message.empty()) {
                    RenderDisabledText(save_message);
                }
                ImGui::TableSetColumnIndex(5);
                render_recovery_actions(
                    draft,
                    key,
                    retained,
                    duplicate_task_id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        finish_recovery_layout(ImGui::GetCursorScreenPos());
    };
    const auto clear_pending_delete = [this]() {
        pending_delete_task_name_.clear();
        pending_delete_task_is_temporary_ = false;
        pending_delete_task_is_recovery_ = false;
        pending_delete_task_source_identity_.clear();
        pending_delete_task_id_.clear();
    };
    const auto render_delete_task_popup = [&]() {
        if (!ImGui::BeginPopupModal(
                delete_labeling_task_popup.c_str(),
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            return false;
        }

        const std::string pending_delete_task_display_name =
            pending_delete_task_is_recovery_
            ? SampleWorkflowTemporaryDraftTaskName(
                  language,
                  pending_delete_task_name_)
            : LabelingTaskDisplayName(
                  language,
                  pending_delete_task_is_temporary_,
                  pending_delete_task_name_);
        const std::string delete_task_message =
            FormatUiText(
                language,
                UiTextId::DeleteLocalTaskMessage,
                pending_delete_task_display_name.c_str());
        ImGui::TextWrapped(
            "%s",
            delete_task_message.c_str());
        if (pending_delete_task_is_recovery_) {
            RenderDisabledText(
                FormatUiText(
                    language,
                    UiTextId::TemporaryDraftTaskIdentity,
                    pending_delete_task_id_.c_str()));
            RenderDisabledText(
                FormatUiText(
                    language,
                    UiTextId::TemporaryDraftSourceIdentity,
                    pending_delete_task_source_identity_.c_str()));
        }
        const std::string delete_task_label =
            StableUiLabel(
                language,
                UiTextId::DeleteTask,
                "SpecForgeConfirmDeleteLabelingTask");
        if (ImGui::Button(
                delete_task_label.c_str())) {
            const bool deleting_recovery_draft =
                pending_delete_task_is_recovery_;
            const SourceCollectionLabelingView& latest_labeling =
                interaction.View().labeling;
            if (!deleting_recovery_draft &&
                (!latest_labeling.has_active_source ||
                 !latest_labeling.has_active_task ||
                 latest_labeling.source_identity !=
                     pending_delete_task_source_identity_ ||
                 latest_labeling.task_id != pending_delete_task_id_)) {
                labeling_operation_text_id_ =
                    UiTextId::LabelingDeleteTargetChanged;
                labeling_operation_message_ =
                    UiText(
                        language,
                        UiTextId::LabelingDeleteTargetChanged);
                clear_pending_delete();
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return true;
            }

            const bool had_active_task =
                latest_labeling.has_active_source &&
                latest_labeling.has_active_task;
            const std::string active_source_before =
                had_active_task ? latest_labeling.source_identity : std::string{};
            const std::string active_task_before =
                had_active_task ? latest_labeling.task_id : std::string{};
            SourceCollectionSessionResult delete_result;
            if (deleting_recovery_draft) {
                delete_result = capture_delete_result(
                    submit(
                        ChangeActiveSampleWorkflow(
                            ActiveSampleWorkflowIntent::
                                DeleteTemporaryLabelingTask(
                                    pending_delete_task_source_identity_,
                                    pending_delete_task_id_))));
            } else {
                delete_result = capture_delete_result(
                    submit(
                        ChangeActiveSampleWorkflow(
                            ActiveSampleWorkflowIntent::
                                DeleteActiveLabelingTask())));
            }
            const SourceCollectionLabelingView& after_delete_labeling =
                interaction.View().labeling;
            const bool active_identity_changed =
                had_active_task !=
                    (after_delete_labeling.has_active_source &&
                     after_delete_labeling.has_active_task) ||
                (had_active_task &&
                 (after_delete_labeling.source_identity != active_source_before ||
                  after_delete_labeling.task_id != active_task_before));
            if ((!deleting_recovery_draft &&
                 (delete_result.action.workflow_changed ||
                  active_identity_changed)) ||
                (deleting_recovery_draft && active_identity_changed)) {
                editing_label_code_.reset();
                ResetLabelShortcutCapture();
            }
            clear_pending_delete();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return true;
        }
        const ImRect delete_confirmation_rect = GImGui->LastItemData.Rect;
        labeling_delete_confirmation_rect_ = {
            delete_confirmation_rect.Min.x,
            delete_confirmation_rect.Min.y,
            delete_confirmation_rect.Max.x,
            delete_confirmation_rect.Max.y};
        ImGui::SameLine();
        const std::string cancel_delete_task_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "SpecForgeCancelDeleteLabelingTask");
        if (ImGui::Button(
                cancel_delete_task_label.c_str())) {
            clear_pending_delete();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return true;
    };
    const auto render_labeling_operation_message = [&]() {
        const std::string_view labeling_operation_message =
            labeling_operation_text_id_
            ? UiText(language, *labeling_operation_text_id_)
            : std::string_view(labeling_operation_message_);
        if (!labeling_operation_message.empty()) {
            ImGui::TextWrapped(
                "%s",
                std::string(labeling_operation_message).c_str());
        }
    };
    const auto render_labeling_state_diagnostics = [&]() {
        if (labeling_view.state_save_failed) {
            RenderDisabledText(
                FormatUiText(
                    language,
                    UiTextId::LocalTaskRecord,
                    labeling_view.state_save_error.c_str()));
        }
        if (!labeling_view.state_load_warning.empty()) {
            RenderDisabledText(
                FormatUiText(
                    language,
                    UiTextId::LocalTaskRecord,
                    labeling_view.state_load_warning.c_str()));
        }
    };
    if (!labeling_view.has_active_source || !current_index) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        render_labeling_operation_message();
        render_labeling_state_diagnostics();
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoActiveSource));
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return;
    }

    const bool task_switch_locked = labeling_view.has_active_task && !labeling_view.can_deactivate_task;
    const std::string pause_label = StableUiLabel(
        language,
        UiTextId::Pause,
        "SpecForgePauseLabelingTask");
    const std::string delete_task_control_label =
        StableUiLabel(
            language,
            UiTextId::Delete,
            "SpecForgeDeleteLabelingTask");
    const ImGuiStyle& style = ImGui::GetStyle();
    float reserved_button_width = 0.0f;
    if (labeling_view.has_active_task) {
        reserved_button_width =
            ImGui::CalcTextSize(
                pause_label.c_str(),
                nullptr,
                true)
                .x +
            ImGui::CalcTextSize(
                delete_task_control_label.c_str(),
                nullptr,
                true)
                .x +
            style.FramePadding.x * 4.0f +
            style.ItemSpacing.x * 2.0f;
    }
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - reserved_button_width));
    const std::string selector_preview =
        labeling_view.has_active_task
        ? LabelingTaskDisplayName(
              language,
              labeling_view.active_task_is_temporary,
              labeling_view.task_name)
        : std::string(
              UiText(
                  language,
                  UiTextId::SelectLabelingTask));
    const bool selector_open = ImGui::BeginCombo(
        "##labeling_task_selector",
        selector_preview.c_str());
    const ImRect selector_rect = GImGui->LastItemData.Rect;
    const ImGuiID selector_id = GImGui->LastItemData.ID;
    labeling_selector_rect_ = {
        selector_rect.Min.x,
        selector_rect.Min.y,
        selector_rect.Max.x,
        selector_rect.Max.y};
    if (selector_open) {
        const bool temporary_selected =
            labeling_view.has_active_task && labeling_view.active_task_is_temporary;
        const bool has_recovery_drafts =
            !labeling_view.recovery_drafts.empty();
        const bool ambiguous_recovery_identity = std::any_of(
            labeling_view.recovery_drafts.begin(),
            labeling_view.recovery_drafts.end(),
            [&recovery_task_id_is_ambiguous](
                const SourceCollectionLabelingRecoveryDraftView& draft) {
                return recovery_task_id_is_ambiguous(draft.task_id);
            });
        const UiTextId temporary_action_text_id =
            (!labeling_view.has_temporary_task && !has_recovery_drafts)
            ? UiTextId::NewLabelingTask
            : temporary_selected
            ? UiTextId::TemporaryLabelingDraft
            : UiTextId::ResumeLabelingDraft;
        const bool show_generic_temporary_action =
            (!labeling_view.has_temporary_task && !has_recovery_drafts) ||
            temporary_selected ||
            labeling_view.recovery_drafts.size() <= 1;
        if (show_generic_temporary_action) {
            const std::string temporary_action =
                StableUiLabel(
                    language,
                    temporary_action_text_id,
                    "SpecForgeTemporaryLabelingTaskAction");
            const bool temporary_action_disabled =
                (task_switch_locked && !temporary_selected) ||
                ambiguous_recovery_identity;
            if (temporary_action_disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(
                    temporary_action.c_str(),
                    temporary_selected) &&
                !temporary_selected &&
                !temporary_action_disabled) {
                if (has_recovery_drafts) {
                    (void)submit(
                        ChangeActiveSampleWorkflow(
                            ActiveSampleWorkflowIntent::
                                RecoverTemporaryLabelingTask(
                                    labeling_view.source_identity,
                                    labeling_view.recovery_drafts.front().task_id)));
                } else {
                    (void)submit(
                        ChangeActiveSampleWorkflow(
                            ActiveSampleWorkflowIntent::
                                StartOrResumeTemporaryLabelingTask()));
                }
            }
            const ImVec2 temporary_action_min = ImGui::GetItemRectMin();
            const ImVec2 temporary_action_max = ImGui::GetItemRectMax();
            temporary_labeling_action_rect_ = {
                temporary_action_min.x,
                temporary_action_min.y,
                temporary_action_max.x,
                temporary_action_max.y};
            if (temporary_action_disabled) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string_view tooltip =
                        ambiguous_recovery_identity
                        ? UiText(
                              language,
                              UiTextId::TemporaryDraftDuplicateIdentity)
                        : UiText(
                              language,
                              UiTextId::OutputAutosaveCloseBlocked);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }
            }
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
                (void)submit(
                    ChangeActiveSampleWorkflow(
                        ActiveSampleWorkflowIntent::
                            ActivateLabelingTaskFromAnnotation(
                                annotation.path)));
            }
            if (disabled) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    bool labeling_drop_accepted = false;
    const SourceCollectionAnnotationValueView* dropped_annotation = nullptr;
    if (ImGui::BeginDragDropTargetCustom(selector_rect, selector_id)) {
        dropped_annotation =
            AcceptSampleLabelingTaskDrop(session_view, task_switch_locked, labeling_drop_accepted);
        ImGui::EndDragDropTarget();
    }
    if (labeling_drop_accepted) {
        ImGui::GetWindowDrawList()->AddRect(
            selector_rect.Min,
            selector_rect.Max,
            ImGui::GetColorU32(ImGuiCol_DragDropTarget),
            3.0f,
            0,
            2.0f);
    }
    if (dropped_annotation != nullptr) {
        if (AnnotationActivationNeedsConfirmation(dropped_annotation->relationship)) {
            pending_annotation_activation_path_ = dropped_annotation->path;
            pending_annotation_activation_name_ = dropped_annotation->name;
            pending_annotation_activation_relationship_ = dropped_annotation->relationship;
            ImGui::OpenPopup(
                annotation_to_labeling_popup.c_str());
        } else {
            (void)submit(
                ChangeActiveSampleWorkflow(
                    ActiveSampleWorkflowIntent::
                        ActivateLabelingTaskFromAnnotation(
                            dropped_annotation->path)));
        }
    }

    if (labeling_view.has_active_task) {
        ImGui::SameLine();
        if (!labeling_view.can_deactivate_task) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(pause_label.c_str())) {
            (void)submit(
                ChangeActiveSampleWorkflow(
                    ActiveSampleWorkflowIntent::
                        DeactivateActiveLabelingTask()));
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            route_latest_labeling_shortcuts(false);
            ImGui::End();
            return;
        }
        const ImRect pause_rect = GImGui->LastItemData.Rect;
        labeling_pause_rect_ = {
            pause_rect.Min.x,
            pause_rect.Min.y,
            pause_rect.Max.x,
            pause_rect.Max.y};
        if (!labeling_view.can_deactivate_task) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const std::string_view tooltip = UiText(
                    language,
                    UiTextId::OutputAutosaveCloseBlocked);
                ImGui::SetTooltip(
                    "%.*s",
                    static_cast<int>(tooltip.size()),
                    tooltip.data());
            }
        }
        ImGui::SameLine();
        if (!labeling_view.can_delete_task) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(
                delete_task_control_label.c_str())) {
            pending_delete_task_name_ = labeling_view.task_name;
            pending_delete_task_is_temporary_ =
                labeling_view.active_task_is_temporary;
            pending_delete_task_is_recovery_ = false;
            pending_delete_task_source_identity_ =
                labeling_view.source_identity;
            pending_delete_task_id_ = labeling_view.task_id;
            ImGui::OpenPopup(
                delete_labeling_task_popup.c_str());
        }
        const ImRect delete_rect = GImGui->LastItemData.Rect;
        labeling_delete_rect_ = {
            delete_rect.Min.x,
            delete_rect.Min.y,
            delete_rect.Max.x,
            delete_rect.Max.y};
        if (!labeling_view.can_delete_task) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const std::string_view tooltip = UiText(
                    language,
                    UiTextId::OutputAutosaveDeleteBlocked);
                ImGui::SetTooltip(
                    "%.*s",
                    static_cast<int>(tooltip.size()),
                    tooltip.data());
            }
        }
    }

    render_recovery_drafts();
    if (open_delete_task_popup) {
        ImGui::OpenPopup(
            delete_labeling_task_popup.c_str());
    }
    const bool delete_task_popup_visible = render_delete_task_popup();
    if (open_delete_task_popup || delete_task_popup_visible ||
        !pending_delete_task_id_.empty()) {
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return;
    }
    if (recovery_action_submitted &&
        interaction.PendingAction().workflow_changed) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return;
    }
    render_labeling_operation_message();

    if (ImGui::BeginPopupModal(
            annotation_to_labeling_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string editable_message =
            FormatUiText(
                language,
                UiTextId::UseAnnotationEditableMessage,
                pending_annotation_activation_name_.c_str());
        ImGui::TextWrapped(
            "%s",
            editable_message.c_str());
        const std::string_view in_place_warning =
            UiText(
                language,
                UiTextId::EditAnnotationInPlaceWarning);
        ImGui::TextWrapped(
            "%.*s",
            static_cast<int>(in_place_warning.size()),
            in_place_warning.data());
        if (pending_annotation_activation_relationship_ ==
            SampleAnnotationWorkflowRelationship::PlainAnnotation) {
            RenderDisabledText(
                UiText(
                    language,
                    UiTextId::MetadataSidecarWillBeCreated));
        } else {
            RenderDisabledText(
                UiText(
                    language,
                    UiTextId::ExistingLabelMetadataReused));
        }
        const std::string use_annotation_label =
            StableUiLabel(
                language,
                UiTextId::UseAnnotation,
                "SpecForgeConfirmUseAnnotation");
        if (ImGui::Button(
                use_annotation_label.c_str())) {
            (void)submit(
                ChangeActiveSampleWorkflow(
                    ActiveSampleWorkflowIntent::
                        ActivateLabelingTaskFromAnnotation(
                            pending_annotation_activation_path_)));
            pending_annotation_activation_path_.clear();
            pending_annotation_activation_name_.clear();
            pending_annotation_activation_relationship_ =
                SampleAnnotationWorkflowRelationship::PlainAnnotation;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string cancel_annotation_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "SpecForgeCancelUseAnnotation");
        if (ImGui::Button(
                cancel_annotation_label.c_str())) {
            pending_annotation_activation_path_.clear();
            pending_annotation_activation_name_.clear();
            pending_annotation_activation_relationship_ =
                SampleAnnotationWorkflowRelationship::PlainAnnotation;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (interaction.PendingAction().workflow_changed) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return;
    }
    if (!labeling_view.has_active_task) {
        editing_label_code_.reset();
        ResetLabelShortcutCapture();
        render_labeling_state_diagnostics();
        route_latest_labeling_shortcuts(false);
        ImGui::End();
        return;
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
        UiText(
            language,
            UiTextId::LabelingProgress)
            .data(),
        static_cast<unsigned long long>(labeling_view.labeled_count),
        static_cast<unsigned long long>(labeling_view.sample_count));
    const std::string current_value =
        LocalizedSampleLabelValue(
            language,
            labeling_view.label_set,
            current_code);
    ImGui::Text(
        UiText(
            language,
            UiTextId::CurrentLabelValue)
            .data(),
        current_value.c_str());
    if (labeling_view.remembered_position && *labeling_view.remembered_position < labeling_view.sample_count &&
        *labeling_view.remembered_position != *current_index) {
        ImGui::Text(
            UiText(
                language,
                UiTextId::RememberedRow)
                .data(),
            static_cast<unsigned long long>(*labeling_view.remembered_position));
        ImGui::SameLine();
        const bool resume_available = CanResumeRememberedRow(
            labeling_view,
            *labeling_view.remembered_position,
            labeling_view.sample_count);
        if (!resume_available) {
            ImGui::BeginDisabled();
        }
        const std::string resume_label =
            StableUiLabel(
                language,
                UiTextId::Resume,
                "SpecForgeResumeRememberedLabelingRow");
        if (ImGui::Button(
                resume_label.c_str())) {
            (void)submit(
                UpdateSampleNavigation(
                    SampleNavigationIntent::Move(
                        SampleNavigationRequest::
                            LocateSourceRowInSequence(
                                *labeling_view.remembered_position))));
        }
        if (!resume_available) {
            ImGui::EndDisabled();
        }
    }

    const std::string save_status =
        SampleWorkflowSaveStateText(
            language,
            labeling_view.save_state);
    RenderText(save_status);
    const std::string save_message =
        SampleWorkflowSaveMessageText(
            language,
            labeling_view.save_state);
    if (!save_message.empty()) {
        RenderDisabledText(save_message);
    }
    const std::string_view save_state_reminder =
        SaveStateReminder(
            language,
            labeling_view);
    RenderDisabledText(save_state_reminder);
    render_labeling_state_diagnostics();
    if (labeling_view.output_path) {
        const std::string path = UserPathDisplayText(*labeling_view.output_path);
        ImGui::TextDisabled("%s", path.c_str());
    }

    ImGui::Spacing();
    bool auto_advance = labeling_view.auto_advance;
    const std::string auto_advance_label =
        StableUiLabel(
            language,
            UiTextId::AutoAdvance,
            "SpecForgeLabelingAutoAdvance");
    if (ImGui::Checkbox(
            auto_advance_label.c_str(),
            &auto_advance)) {
        (void)submit(
            ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::
                    SetActiveLabelingAutoAdvance(auto_advance)));
    }
    ImGui::SameLine();
    if (!auto_advance) {
        ImGui::BeginDisabled();
    }
    bool skip_labeled_on_advance = labeling_view.skip_labeled_on_advance;
    const std::string skip_labeled_label =
        StableUiLabel(
            language,
            UiTextId::SkipLabeled,
            "SpecForgeLabelingSkipLabeled");
    if (ImGui::Checkbox(
            skip_labeled_label.c_str(),
            &skip_labeled_on_advance)) {
        (void)submit(
            ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::
                    SetActiveLabelingSkipLabeledOnAdvance(
                        skip_labeled_on_advance)));
    }
    if (!auto_advance) {
        ImGui::EndDisabled();
    }
    if (labeling_view.active_task_is_temporary) {
        ImGui::SameLine();
        const std::string save_to_label =
            StableUiLabel(
                language,
                UiTextId::SaveTo,
                "SpecForgeSaveLabelingTaskTo");
        if (ImGui::Button(
                save_to_label.c_str())) {
            if (std::optional<std::filesystem::path> path = choose_output_path()) {
                (void)submit(
                    ChangeActiveSampleWorkflow(
                        ActiveSampleWorkflowIntent::
                            SetActiveLabelingOutputPath(*path)));
            }
        }
    }

    bool block_shortcuts_this_frame = label_shortcut_capture_active_;
    std::optional<int> label_code_to_assign;
    bool clear_label_requested = false;

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    RenderText(
        UiText(
            language,
            UiTextId::Labels));
    ImGui::SameLine();
    const bool add_label_requested = ImGui::SmallButton("+##AddSampleLabel");
    if (ImGui::IsItemHovered()) {
        const std::string_view tooltip =
            UiText(
                language,
                UiTextId::AddLabel);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    if (add_label_requested) {
        const int code = NextAvailableSampleLabelCode(labeling_view.label_set);
        const std::string name =
            FormatUiText(
                language,
                UiTextId::DefaultLabelName,
                code);
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UpsertActiveLabel(SampleLabelDefinition{code, name, '\0'})));
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
        ImGui::TableSetupColumn(
            UiText(
                language,
                UiTextId::Name)
                .data(),
            ImGuiTableColumnFlags_WidthStretch,
            1.0f,
            ImGui::GetID(
                "SpecForgeSampleLabelNameColumn"));
        ImGui::TableSetupColumn(
            UiText(
                language,
                UiTextId::Code)
                .data(),
            ImGuiTableColumnFlags_WidthFixed,
            52.0f,
            ImGui::GetID(
                "SpecForgeSampleLabelCodeColumn"));
        ImGui::TableSetupColumn(
            UiText(
                language,
                UiTextId::Shortcut)
                .data(),
            ImGuiTableColumnFlags_WidthFixed,
            132.0f,
            ImGui::GetID(
                "SpecForgeSampleLabelShortcutColumn"));
        ImGui::TableSetupColumn(
            "##Edit",
            ImGuiTableColumnFlags_WidthFixed,
            32.0f,
            ImGui::GetID(
                "SpecForgeSampleLabelEditColumn"));
        ImGui::TableSetupColumn(
            "##Delete",
            ImGuiTableColumnFlags_WidthFixed,
            32.0f,
            ImGui::GetID(
                "SpecForgeSampleLabelDeleteColumn"));
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
                const std::string clear_shortcut_label =
                    StableUiLabel(
                        language,
                        UiTextId::Clear,
                        "label_shortcut");
                const float clear_button_width = has_shortcut
                    ? ImGui::CalcTextSize(
                          clear_shortcut_label.c_str(),
                          nullptr,
                          true)
                              .x +
                          ImGui::GetStyle()
                                  .FramePadding.x *
                              2.0f
                    : 0.0f;
                const float shortcut_button_width = std::max(
                    1.0f,
                    ImGui::GetContentRegionAvail().x -
                        (has_shortcut ? clear_button_width + ImGui::GetStyle().ItemSpacing.x : 0.0f));
                const std::string shortcut_button_label =
                    (label_shortcut_capture_active_
                         ? std::string{
                               UiText(
                                   language,
                                   UiTextId::PressKey)}
                         : LocalizedSampleLabelShortcut(
                               language,
                               label_shortcut_edit_buffer_[0])) +
                    "###label_shortcut_capture";
                if (ImGui::Button(shortcut_button_label.c_str(), ImVec2(shortcut_button_width, 0.0f))) {
                    block_shortcuts_this_frame = true;
                    if (label_shortcut_capture_active_) {
                        ResetLabelShortcutCapture();
                    } else {
                        label_shortcut_capture_active_ = true;
                        pending_conflicting_shortcut_ = '\0';
                        label_shortcut_notice_kind_ =
                            LabelShortcutNoticeKind::
                                CaptureInstructions;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    const std::string_view tooltip =
                        UiText(
                            language,
                            label_shortcut_capture_active_
                                ? UiTextId::
                                      ShortcutCaptureWaiting
                                : UiTextId::
                                      CaptureLabelShortcut);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
                }
                if (has_shortcut) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(
                            clear_shortcut_label.c_str())) {
                        block_shortcuts_this_frame = true;
                        label_shortcut_edit_buffer_.fill('\0');
                        ResetLabelShortcutCapture();
                        label_shortcut_notice_kind_ =
                            LabelShortcutNoticeKind::
                                UnboundOnSave;
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
                        label_shortcut_notice_kind_ =
                            LabelShortcutNoticeKind::
                                UnboundOnSave;
                        break;
                    case SampleLabelShortcutCaptureKind::Unsupported:
                        pending_conflicting_shortcut_ = '\0';
                        label_shortcut_notice_kind_ =
                            LabelShortcutNoticeKind::
                                Unsupported;
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
                        const std::string display_shortcut =
                            LocalizedSampleLabelShortcut(
                                language,
                                captured_shortcut);
                        if (captured_owner == nullptr) {
                            label_shortcut_edit_buffer_.fill('\0');
                            label_shortcut_edit_buffer_[0] = captured_shortcut;
                            ResetLabelShortcutCapture();
                            label_shortcut_notice_kind_ =
                                LabelShortcutNoticeKind::
                                    Selected;
                            label_shortcut_notice_shortcut_ =
                                display_shortcut;
                        } else if (selection.kind == SampleLabelShortcutSelectionKind::Accepted) {
                            const std::string previous_owner_name = captured_owner->name;
                            label_shortcut_edit_buffer_.fill('\0');
                            label_shortcut_edit_buffer_[0] = captured_shortcut;
                            ResetLabelShortcutCapture();
                            label_shortcut_notice_kind_ =
                                LabelShortcutNoticeKind::
                                    WillMove;
                            label_shortcut_notice_shortcut_ =
                                display_shortcut;
                            label_shortcut_notice_owner_ =
                                previous_owner_name;
                        } else {
                            pending_conflicting_shortcut_ = captured_shortcut;
                            label_shortcut_notice_kind_ =
                                LabelShortcutNoticeKind::
                                    Conflict;
                            label_shortcut_notice_shortcut_ =
                                display_shortcut;
                            label_shortcut_notice_owner_ =
                                captured_owner->name;
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
                    {},
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
                            const std::string_view tooltip =
                                UiText(
                                    language,
                                    UiTextId::NameRequired);
                            ImGui::SetTooltip(
                                "%.*s",
                                static_cast<int>(tooltip.size()),
                                tooltip.data());
                        } else if (!requested_code) {
                            const std::string_view tooltip =
                                UiText(
                                    language,
                                    UiTextId::CodeIntegerRequired);
                            ImGui::SetTooltip(
                                "%.*s",
                                static_cast<int>(tooltip.size()),
                                tooltip.data());
                        } else if (code_reserved) {
                            const std::string_view tooltip =
                                UiText(
                                    language,
                                    UiTextId::UnlabeledCodeReserved);
                            ImGui::SetTooltip(
                                "%.*s",
                                static_cast<int>(tooltip.size()),
                                tooltip.data());
                        } else if (code_conflicts) {
                            ImGui::SetTooltip(
                                UiText(
                                    language,
                                    UiTextId::CodeAlreadyUsed)
                                    .data(),
                                *requested_code);
                        } else {
                            const std::string_view tooltip =
                                UiText(
                                    language,
                                    UiTextId::ShortcutOneCharacter);
                            ImGui::SetTooltip(
                                "%.*s",
                                static_cast<int>(tooltip.size()),
                                tooltip.data());
                        }
                    }
                } else if (
                    save_hovered && requested_code && *requested_code != label.code && usage_count > 0) {
                    ImGui::SetTooltip(
                        UiText(
                            language,
                            UiTextId::LabelCodeRewriteCount)
                            .data(),
                        static_cast<unsigned long long>(usage_count));
                } else if (save_hovered && shortcut_owner != labeling_view.label_set.labels.end()) {
                    ImGui::SetTooltip(
                        UiText(
                            language,
                            UiTextId::SavingMovesShortcut)
                            .data(),
                        shortcut_owner->name.c_str());
                } else if (save_hovered) {
                    const std::string_view tooltip =
                        UiText(
                            language,
                            UiTextId::SaveLabel);
                    ImGui::SetTooltip(
                        "%.*s",
                        static_cast<int>(tooltip.size()),
                        tooltip.data());
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
                        UiText(
                            language,
                            UiTextId::CancelEditing),
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
                const std::string shortcut_text =
                    LocalizedSampleLabelShortcut(
                        language,
                        label.shortcut);
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
                    UiText(
                        language,
                        UiTextId::EditLabel),
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

                const std::string delete_tooltip =
                    usage_count == 0
                    ? std::string(
                          UiText(
                              language,
                              UiTextId::DeleteLabel))
                    : FormatUiText(
                          language,
                          UiTextId::DeleteLabelAndClear,
                          static_cast<unsigned long long>(
                              usage_count));
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

    const std::string label_shortcut_notice =
        LabelShortcutNotice(language);
    if (editing_label_code_ &&
        !label_shortcut_notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::BeginChild(
                "##label_shortcut_notice",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::TextWrapped(
                "%s",
                label_shortcut_notice.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (open_change_label_code_popup) {
        ImGui::OpenPopup(
            change_sample_label_code_popup.c_str());
    }
    if (ImGui::BeginPopupModal(
            change_sample_label_code_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            UiText(
                language,
                UiTextId::LabelCodeAssignedCount)
                .data(),
            pending_label_code_change_original_code_.value_or(kUnlabeledSampleLabelCode),
            static_cast<unsigned long long>(pending_label_code_change_usage_count_));
        ImGui::TextWrapped(
            UiText(
                language,
                UiTextId::LabelCodeRewriteAll)
                .data(),
            pending_label_code_change_.code);
        const std::string change_code_label =
            StableUiLabel(
                language,
                UiTextId::ChangeCode,
                "SpecForgeConfirmChangeSampleLabelCode");
        if (ImGui::Button(
                change_code_label.c_str()) &&
            pending_label_code_change_original_code_) {
            SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::UpdateActiveLabel(
                    *pending_label_code_change_original_code_,
                    pending_label_code_change_,
                    true)));
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
        const std::string cancel_change_code_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "SpecForgeCancelChangeSampleLabelCode");
        if (ImGui::Button(
                cancel_change_code_label.c_str())) {
            pending_label_code_change_original_code_.reset();
            pending_label_code_change_ = {};
            pending_label_code_change_usage_count_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (open_delete_label_popup) {
        ImGui::OpenPopup(
            delete_sample_label_popup.c_str());
    }
    if (ImGui::BeginPopupModal(
            delete_sample_label_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            UiText(
                language,
                UiTextId::LabelAssignedCount)
                .data(),
            pending_delete_label_name_.c_str(),
            static_cast<unsigned long long>(pending_delete_label_usage_count_));
        const std::string_view delete_values_text =
            UiText(
                language,
                UiTextId::DeleteLabelChangesValues);
        ImGui::TextWrapped(
            "%.*s",
            static_cast<int>(delete_values_text.size()),
            delete_values_text.data());
        const std::string_view remove_filter_text =
            UiText(
                language,
                UiTextId::DeleteLabelRemovesFilter);
        ImGui::TextWrapped(
            "%.*s",
            static_cast<int>(remove_filter_text.size()),
            remove_filter_text.data());
        const std::string delete_label =
            StableUiLabel(
                language,
                UiTextId::DeleteLabel,
                "SpecForgeConfirmDeleteSampleLabel");
        if (ImGui::Button(
                delete_label.c_str()) &&
            pending_delete_label_code_) {
            label_code_to_remove = pending_delete_label_code_;
            pending_delete_label_code_.reset();
            pending_delete_label_name_.clear();
            pending_delete_label_usage_count_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string cancel_delete_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "SpecForgeCancelDeleteSampleLabel");
        if (ImGui::Button(
                cancel_delete_label.c_str())) {
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
        if (result.changed) {
            editing_label_code_.reset();
            ResetLabelShortcutCapture();
            label_name_focus_pending_ = false;
        }
    }
    if (label_code_to_remove) {
        SourceCollectionSessionResult result = submit(ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::RemoveActiveLabel(*label_code_to_remove)));
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
    const std::string clear_current_label =
        StableUiLabel(
            language,
            UiTextId::Clear,
            "SpecForgeClearCurrentSampleLabel");
    if (ImGui::Button(
            clear_current_label.c_str())) {
        label_code_to_assign.reset();
        clear_label_requested = true;
    }
    if (clear_disabled) {
        ImGui::EndDisabled();
    }

    if (label_code_to_assign) {
        (void)submit_auto_advance(
            ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::
                    AssignActiveLabelToCurrentSample(
                        *label_code_to_assign)));
    } else if (clear_label_requested) {
        (void)submit_auto_advance(
            ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::
                    ClearActiveLabelForCurrentSample()));
    }

    route_latest_labeling_shortcuts(block_shortcuts_this_frame);

    ImGui::End();
}

void SampleWorkflowPanelUi::RenderFilters(
    PanelSessionInteraction& interaction,
    bool* open)
{
    RenderFilters(
        interaction,
        UiLanguage::English,
        open);
}

void SampleWorkflowPanelUi::RenderFilters(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open)
{
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::SampleFilters,
        "SpecForgeFiltersV1");
    const std::string add_sample_filter_source_popup =
        StableUiLabel(
            language,
            UiTextId::AddSampleFilterSource,
            "SpecForgeAddSampleFilterSourcePopup");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }

    const SourceCollectionSessionView& session_view =
        interaction.View();
    const SourceCollectionFilterView* filter_view = &session_view.filter;
    if (!filter_view->has_active_source) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoActiveSource));
        ImGui::End();
        return;
    }

    if (ImGui::SmallButton("+##AddSampleFilterSource")) {
        ImGui::OpenPopup(
            add_sample_filter_source_popup.c_str());
    }
    if (ImGui::IsItemHovered()) {
        const std::string_view tooltip =
            UiText(
                language,
                UiTextId::AddAnnotationSampleFilter);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    std::optional<std::string> source_to_collapse;
    if (ImGui::BeginPopup(
            add_sample_filter_source_popup.c_str())) {
        if (filter_view->available_sources.empty()) {
            RenderDisabledText(
                UiText(
                    language,
                    UiTextId::NoAvailableAnnotations));
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
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleFiltering(
                        SampleFilteringIntent::AddSource(
                            *source_to_add)));
            filter_view = &update.view.get().filter;
            source_to_collapse = *source_to_add;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (source_to_collapse) {
        CollapseSampleFilterSourceTree(*source_to_collapse);
    }

    ImGui::SameLine();
    const std::string reset_filters_label =
        StableUiLabel(
            language,
            UiTextId::ResetSampleFilters,
            "SpecForgeResetSampleFilters");
    if (ImGui::Button(
            reset_filters_label.c_str())) {
        PanelSessionInteraction::Update update =
            interaction.Submit(
                ApplySampleFiltering(
                    SampleFilteringIntent::Clear()));
        filter_view = &update.view.get().filter;
    }

    ImGui::Spacing();

    ImGui::Text(
        UiText(
            language,
            UiTextId::VisibleSamples)
            .data(),
        static_cast<unsigned long long>(filter_view->evaluation.included_count),
        static_cast<unsigned long long>(filter_view->sample_count));
    if (filter_view->navigation_filter_active && !filter_view->current_sample_in_filter) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::CurrentSampleOutsideFilters));
    }

    for (const SampleFilterDiagnostic& diagnostic :
         filter_view->evaluation.diagnostics) {
        const std::string message =
            SampleFilterDiagnosticText(
                language,
                diagnostic);
        RenderDisabledText(message);
    }

    ImGui::Separator();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 filter_drop_min(window->WorkRect.Min.x, ImGui::GetCursorScreenPos().y);
    if (filter_view->sources.empty()) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoSampleFilters));
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
                UiText(
                    language,
                    UiTextId::RemoveSampleFilter),
                source_row_hovered)) {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleFiltering(
                        SampleFilteringIntent::RemoveSource(
                            source_view.id)));
            filter_view = &update.view.get().filter;
            removed_source = true;
        }
        if (tree_open && !removed_source) {
            for (const SampleFilterValueOption& option : source_view.options) {
                bool selected = source_view.selected_value_keys.find(option.key) !=
                                source_view.selected_value_keys.end();
                std::string label =
                    LocalizedFilterOptionText(
                        language,
                        option);
                label += "  ";
                label += std::to_string(option.sample_count);
                ImGui::PushID(option.key.c_str());
                if (ImGui::Checkbox(label.c_str(), &selected)) {
                    (void)interaction.Submit(
                        ApplySampleFiltering(
                            SampleFilteringIntent::
                                SetFilterValueSelected(
                                    source_view.id,
                                    option.key,
                                    selected)));
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
        (void)interaction.Submit(
            ApplySampleFiltering(
                SampleFilteringIntent::AddSource(
                    *dropped_source)));
        CollapseSampleFilterSourceTree(*dropped_source);
    }

    ImGui::End();
}

void SampleWorkflowPanelUi::RenderSorting(
    PanelSessionInteraction& interaction,
    bool* open)
{
    RenderSorting(
        interaction,
        UiLanguage::English,
        open);
}

void SampleWorkflowPanelUi::RenderSorting(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open)
{
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::SampleSorting,
        "SpecForgeSampleSortingV1");
    const std::string add_sample_sort_source_popup =
        StableUiLabel(
            language,
            UiTextId::AddSampleSortSource,
            "SpecForgeAddSampleSortSourcePopup");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }

    const SourceCollectionSessionView& session_view =
        interaction.View();
    const SourceCollectionSampleSortingView* sorting_view = &session_view.sorting;
    if (!sorting_view->has_active_source) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoActiveSource));
        ImGui::End();
        return;
    }

    if (ImGui::SmallButton("+##AddSampleSortSource")) {
        ImGui::OpenPopup(
            add_sample_sort_source_popup.c_str());
    }
    if (ImGui::IsItemHovered()) {
        const std::string_view tooltip =
            UiText(
                language,
                UiTextId::AddAnnotationSampleSorting);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }

    ImGui::SameLine();
    bool reset_sorting_checked =
        !sorting_view->active ||
        (sorting_view->active_source_id == "source-order" &&
            sorting_view->direction == SampleNavigationSortDirection::Ascending);
    const std::string reset_sorting_label =
        StableUiLabel(
            language,
            UiTextId::ResetSorting,
            "SpecForgeResetSampleSorting");
    if (ImGui::Checkbox(
            reset_sorting_label.c_str(),
            &reset_sorting_checked) &&
        reset_sorting_checked) {
        PanelSessionInteraction::Update update =
            interaction.Submit(
                ApplySampleSorting(
                    SampleSortingIntent::Clear()));
        sorting_view = &update.view.get().sorting;
    }

    if (ImGui::BeginPopup(
            add_sample_sort_source_popup.c_str())) {
        if (sorting_view->available_sources.empty()) {
            RenderDisabledText(
                UiText(
                    language,
                    UiTextId::NoAvailableAnnotations));
        }
        std::optional<std::string> source_to_add;
        for (const SourceCollectionSampleSortSourceView& source_view : sorting_view->available_sources) {
            ImGui::PushID(source_view.id.c_str());
            const std::string display_name =
                SampleSortSourceDisplayName(
                    language,
                    source_view);
            if (ImGui::Selectable(display_name.c_str())) {
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
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        SampleSortingIntent::AddSource(
                            *source_to_add)));
            sorting_view = &update.view.get().sorting;
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
    source_order_view.name =
        UiText(
            language,
            UiTextId::SourceOrder);
    source_order_view.selected = !sorting_view->active || sorting_view->active_source_id == source_order_view.id;
    const SampleNavigationSortDirection source_order_direction =
        source_order_view.selected ? sorting_view->direction : sorting_view->source_order_direction;
    const SampleSortSourceRowAction source_order_action =
        RenderSampleSortSourceRow(
            source_order_view,
            source_order_direction,
            language);
    if (source_order_action.toggle_direction) {
        if (sorting_view->active && sorting_view->active_source_id == source_order_view.id) {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    sorting_view->direction ==
                            SampleNavigationSortDirection::
                                Descending
                        ? ApplySampleSorting(
                              SampleSortingIntent::Clear())
                        : ApplySampleSorting(
                              SampleSortingIntent::
                                  SetSortDirection(
                                      SampleNavigationSortDirection::
                                          Descending)));
            sorting_view = &update.view.get().sorting;
        } else if (source_order_view.selected) {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        SampleSortingIntent::
                            SetSortDirection(
                                SampleNavigationSortDirection::
                                    Descending)));
            sorting_view = &update.view.get().sorting;
            update = interaction.Submit(
                ApplySampleSorting(
                    SampleSortingIntent::SetSortSource(
                        source_order_view.id)));
            sorting_view = &update.view.get().sorting;
        } else {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        SampleSortingIntent::Clear()));
            sorting_view = &update.view.get().sorting;
        }
    } else if (source_order_action.activate) {
        PanelSessionInteraction::Update update =
            interaction.Submit(
                source_order_direction ==
                        SampleNavigationSortDirection::Descending
                    ? ApplySampleSorting(
                          SampleSortingIntent::SetSortSource(
                              source_order_view.id))
                    : ApplySampleSorting(
                          SampleSortingIntent::Clear()));
        sorting_view = &update.view.get().sorting;
    }

    bool has_sort_source = false;
    bool stop_rendering_sources = false;
    const std::vector<SourceCollectionSampleSortSourceView>& sort_sources = sorting_view->sources;
    for (const SourceCollectionSampleSortSourceView& source_view : sort_sources) {
        has_sort_source = true;
        const SampleSortSourceRowAction row_action =
            RenderSampleSortSourceRow(
                source_view,
                source_view.direction,
                language);
        if (row_action.remove) {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        SampleSortingIntent::RemoveSource(
                            source_view.id)));
            sorting_view = &update.view.get().sorting;
            stop_rendering_sources = true;
        } else if (row_action.toggle_direction) {
            const SampleNavigationSortDirection next_direction = source_view.selected
                ? OppositeSortDirection(source_view.direction)
                : source_view.direction;
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        source_view.selected
                            ? SampleSortingIntent::
                                  SetSortDirection(
                                      next_direction)
                            : SampleSortingIntent::
                                  SetSortSource(
                                      source_view.id)));
            sorting_view = &update.view.get().sorting;
            stop_rendering_sources = true;
        } else if (row_action.activate) {
            PanelSessionInteraction::Update update =
                interaction.Submit(
                    ApplySampleSorting(
                        SampleSortingIntent::SetSortSource(
                            source_view.id)));
            sorting_view = &update.view.get().sorting;
            stop_rendering_sources = true;
        }
        if (stop_rendering_sources) {
            break;
        }
    }
    if (!has_sort_source) {
        RenderDisabledText(
            UiText(
                language,
                UiTextId::NoComparableSortSources));
    }

    const ImVec2 sort_drop_content_end = ImGui::GetCursorScreenPos();
    const ImRect sort_drop_rect(
        sort_drop_min,
        ImVec2(
            window->WorkRect.Max.x,
            std::max(sort_drop_content_end.y + ImGui::GetStyle().ItemSpacing.y, window->WorkRect.Max.y)));
    if (std::optional<std::string> dropped_source =
            RenderSampleSortDropTarget(*sorting_view, sort_drop_rect)) {
        PanelSessionInteraction::Update update =
            interaction.Submit(
                ApplySampleSorting(
                    SampleSortingIntent::AddSource(
                        *dropped_source)));
        sorting_view = &update.view.get().sorting;
    }

    ImGui::End();
}

}  // namespace specforge
