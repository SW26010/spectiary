#include "ui/source_collection_panel.h"

#include "app/local_user_state.h"
#include "ui/sample_name_autocomplete.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kFilesWindow = "Files###SpecForgeFilesV2";
constexpr const char* kNavigationWindow = "Navigation###SpecForgeNavigationV1";
constexpr const char* kSampleNavigationNameMatchesWindow = "Sample name matches###SpecForgeSampleNameMatchesV1";
constexpr const char* kAnnotationsWindow = "Annotations###SpecForgeAnnotationsV1";
constexpr const char* kSampleAnnotationDragPayload = "SPECFORGE_SAMPLE_ANNOTATION_PATH";

SourceCollectionSessionIntent EditSourceCollection(SourceCollectionIntent intent)
{
    return SourceCollectionSessionIntent::EditSourceCollection(std::move(intent));
}

SourceCollectionSessionIntent RenameAnnotationDisplayName(
    std::filesystem::path path,
    std::string display_name)
{
    return EditSourceCollection(
        SourceCollectionIntent::RenameAnnotationResultDisplayName(
            std::move(path),
            std::move(display_name)));
}

SourceCollectionSessionIntent UpdateSampleNavigation(SampleNavigationIntent intent)
{
    return SourceCollectionSessionIntent::UpdateSampleNavigation(std::move(intent));
}

enum class ActionIcon {
    Trash,
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string NarrowPath(const std::filesystem::path& path)
{
    return UserPathDisplayText(path);
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

std::optional<std::size_t> ParseRowIndex(std::string_view text)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::size_t value = 0;
    for (const char character : trimmed) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

std::optional<std::size_t> ParseSampleNumber(std::string_view text)
{
    const std::optional<std::size_t> value = ParseRowIndex(text);
    if (!value || *value == 0) {
        return std::nullopt;
    }
    return value;
}

template <std::size_t Size>
void CopyToBuffer(std::array<char, Size>& buffer, std::string_view text)
{
    static_assert(Size > 0);
    std::fill(buffer.begin(), buffer.end(), '\0');
    const std::size_t copy_size = std::min(text.size(), Size - 1);
    std::copy_n(text.begin(), copy_size, buffer.begin());
}

void RenderDisabledText(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

bool TableCellTextButton(const char* id, std::string_view text, ImU32 text_color)
{
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetFrameHeight();

    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImVec2 max(min.x + width, min.y + height);
    const float text_y = min.y + std::max(0.0f, (height - ImGui::GetTextLineHeight()) * 0.5f);
    const ImVec2 text_pos(min.x + style.FramePadding.x, text_y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::PushClipRect(min, max, true);
    draw_list->AddText(text_pos, text_color, text.data(), text.data() + text.size());
    ImGui::PopClipRect();

    return clicked;
}

bool TableCellRightAlignedTextButton(const char* id, std::string_view text, ImU32 text_color)
{
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetFrameHeight();

    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImVec2 max(min.x + width, min.y + height);
    const ImVec2 text_size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const float text_y = min.y + std::max(0.0f, (height - ImGui::GetTextLineHeight()) * 0.5f);
    const ImVec2 text_pos(max.x - style.FramePadding.x - text_size.x, text_y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::PushClipRect(min, max, true);
    draw_list->AddText(text_pos, text_color, text.data(), text.data() + text.size());
    ImGui::PopClipRect();

    return clicked;
}

float TrashIconButtonWidth()
{
    return ImGui::GetFrameHeight() * 0.5f;
}

bool ActionIconButton(
    const char* id,
    const ImRect& hit_rect,
    ActionIcon icon,
    const char* tooltip,
    bool reveal_on_hover)
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

    const bool reveal_icon = !reveal_on_hover || hovered || active;
    const ImU32 icon_color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float icon_width = std::min(TrashIconButtonWidth(), width);
    const float icon_left = min.x + std::max(0.0f, (width - icon_width) * 0.5f);
    const float icon_top = min.y + std::max(0.0f, (max.y - min.y - height) * 0.5f);
    const float stroke = 1.35f;

    if (reveal_icon && icon == ActionIcon::Trash) {
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

    if (hovered && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool TrashIconButton(const char* id, const ImRect& hit_rect)
{
    return ActionIconButton(id, hit_rect, ActionIcon::Trash, "Remove from list", false);
}

}  // namespace

const char* SourceCollectionPanelUi::FilesWindowName()
{
    return kFilesWindow;
}

const char* SourceCollectionPanelUi::NavigationWindowName()
{
    return kNavigationWindow;
}

const char* SourceCollectionPanelUi::AnnotationsWindowName()
{
    return kAnnotationsWindow;
}

void SourceCollectionPanelUi::SyncNavigationInputs(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit)
{
    const SourceCollectionNavigationView& navigation = session_view.navigation;
    const std::optional<std::size_t> navigation_index = navigation.current_index;
    if (navigation_index) {
        std::snprintf(
            row_index_buffer_.data(),
            row_index_buffer_.size(),
            "%zu",
            *navigation_index + 1);
    } else {
        row_index_buffer_.fill('\0');
    }
    CopyToBuffer(sample_name_query_buffer_, navigation.current_sample_name);
    (void)submit(UpdateSampleNavigation(SampleNavigationIntent::SetSampleNameQuery(navigation.current_sample_name)));
    ClearSampleNameSearch();
}

SourceCollectionSessionAction SourceCollectionPanelUi::RenderFiles(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    bool* open,
    const SourceCollectionPathPicker& choose_source_file,
    const SourceCollectionPathPicker& choose_source_folder,
    const SourceCollectionPathOpener& open_source)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kFilesWindow, open)) {
        ImGui::End();
        return action;
    }

    SourceCollectionSessionView view = session_view;
    ImGui::TextUnformatted("Files");
    ImGui::Separator();

    if (ImGui::Button("Add file...")) {
        if (std::optional<std::filesystem::path> path = choose_source_file()) {
            open_source(*path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add folder...")) {
        if (std::optional<std::filesystem::path> path = choose_source_folder()) {
            open_source(*path);
        }
    }
    ImGui::SameLine();
    const std::vector<SourceCollectionSourceView>& sources = view.sources;
    const std::optional<std::size_t> current_source_index = view.current_source_index;
    const SpectrumSnapshotHandle& snapshot = view.snapshot;
    const std::string source_count =
        std::to_string(sources.size()) + (sources.size() == 1 ? " source" : " sources");
    RenderDisabledText(source_count);

    ImGui::Spacing();
    const bool has_active_source =
        !sources.empty() && current_source_index && *current_source_index < sources.size() && snapshot &&
        !snapshot->source.path.empty();
    if (!has_active_source) {
        ImGui::TextDisabled("No sources added in this session.");
    } else if (ImGui::BeginTable(
                   "files_table",
                   4,
                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 128.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableHeadersRow();

        std::optional<std::size_t> source_to_remove;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const SourceCollectionSourceView& entry = sources[index];
            const bool is_current = current_source_index && *current_source_index == index;

            ImGui::TableNextRow();
            if (is_current) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(index));
            if (TableCellTextButton("source", entry.display_name, ImGui::GetColorU32(ImGuiCol_Text))) {
                SourceCollectionSessionResult result =
                    submit(EditSourceCollection(SourceCollectionIntent::SwitchActive(index)));
                MergeSourceCollectionSessionAction(action, result.action);
            }
            if (ImGui::IsItemHovered()) {
                const std::string path = NarrowPath(entry.path);
                ImGui::SetTooltip("%s", path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (TableCellTextButton("type", entry.type_label, ImGui::GetColorU32(ImGuiCol_Text))) {
                SourceCollectionSessionResult result =
                    submit(EditSourceCollection(SourceCollectionIntent::SwitchActive(index)));
                MergeSourceCollectionSessionAction(action, result.action);
            }

            ImGui::TableSetColumnIndex(2);
            ImU32 state_color = ImGui::GetColorU32(is_current ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (TableCellTextButton("state", entry.state_label, state_color)) {
                SourceCollectionSessionResult result =
                    submit(EditSourceCollection(SourceCollectionIntent::SwitchActive(index)));
                MergeSourceCollectionSessionAction(action, result.action);
            }

            ImGui::TableSetColumnIndex(3);
            const ImRect remove_cell =
                ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
            if (TrashIconButton("remove", remove_cell)) {
                source_to_remove = index;
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (source_to_remove) {
            SourceCollectionSessionResult result =
                submit(EditSourceCollection(SourceCollectionIntent::Remove(*source_to_remove)));
            MergeSourceCollectionSessionAction(action, result.action);
        }
    }
    ImGui::End();
    return action;
}

SourceCollectionSessionAction SourceCollectionPanelUi::RenderNavigation(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    const SourceCollectionSessionViewReader& read_view,
    bool* open,
    SampleWorkflowShortcut& shortcut)
{
    SourceCollectionSessionAction action;
    shortcut = {};
    if (!ImGui::Begin(kNavigationWindow, open)) {
        ImGui::End();
        return action;
    }
    SourceCollectionSessionView view = session_view;
    SourceCollectionNavigationView navigation = view.navigation;
    if (!navigation.has_active_source) {
        ImGui::TextDisabled("No active source");
        const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        shortcut = RouteSampleWorkflowShortcut({
            .focused = shortcut_focused,
            .hovered = shortcut_hovered,
            .navigation_enabled = true});
        ImGui::End();
        return action;
    }

    const std::size_t navigation_index = navigation.current_index.value_or(0);
    const std::size_t navigation_count = navigation.sample_count;

    ImGui::TextUnformatted("source sample:");
    ImGui::SameLine();
    const float sample_input_width =
        std::max(72.0f, ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::SetNextItemWidth(sample_input_width);
    if (!navigation.row_location_available) {
        ImGui::BeginDisabled();
    }
    const bool index_changed = ImGui::InputText(
        "##SampleNavigationSample",
        row_index_buffer_.data(),
        row_index_buffer_.size(),
        ImGuiInputTextFlags_CharsDecimal);
    if (!navigation.row_location_available) {
        ImGui::EndDisabled();
    }
    const bool index_deactivated_after_edit = ImGui::IsItemDeactivatedAfterEdit();
    if (index_changed && navigation.row_location_available) {
        const std::optional<std::size_t> target_sample = ParseSampleNumber(row_index_buffer_.data());
        if (target_sample && *target_sample <= navigation_count) {
            const std::size_t target_row = *target_sample - 1;
            if (target_row != navigation_index) {
                SourceCollectionSessionResult result =
                    submit(UpdateSampleNavigation(SampleNavigationIntent::Move(
                        SampleNavigationRequest::LocateRow(target_row))));
                MergeSourceCollectionSessionAction(action, result.action);
                view = read_view();
                navigation = view.navigation;
            }
        }
    }
    if (index_deactivated_after_edit) {
        SyncNavigationInputs(view, submit);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Text("/%llu", static_cast<unsigned long long>(navigation_count));
    ImGui::SameLine();
    const bool can_previous = navigation.can_move_previous;
    const bool can_next = navigation.can_move_next;
    const ImVec2 sample_step_button_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    if (!can_previous) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("-##PreviousSample", sample_step_button_size)) {
        SourceCollectionSessionResult result =
            submit(UpdateSampleNavigation(SampleNavigationIntent::Move(SampleNavigationRequest::Previous())));
        MergeSourceCollectionSessionAction(action, result.action);
        view = read_view();
        navigation = view.navigation;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Previous sample (Left Arrow)");
    }
    if (!can_previous) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_next) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("+##NextSample", sample_step_button_size)) {
        SourceCollectionSessionResult result =
            submit(UpdateSampleNavigation(SampleNavigationIntent::Move(SampleNavigationRequest::Next())));
        MergeSourceCollectionSessionAction(action, result.action);
        view = read_view();
        navigation = view.navigation;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Next sample (Right Arrow)");
    }
    if (!can_next) {
        ImGui::EndDisabled();
    }
    if (navigation.current_sample_name.empty() && !navigation.current_sample_display_name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", navigation.current_sample_display_name.c_str());
    }
    if (navigation.sequence_active) {
        if (navigation.current_sequence_position) {
            ImGui::Text(
                "sequence: %llu / %llu",
                static_cast<unsigned long long>(*navigation.current_sequence_position + 1),
                static_cast<unsigned long long>(navigation.sequence_count));
        } else {
            ImGui::Text("sequence: - / %llu", static_cast<unsigned long long>(navigation.sequence_count));
        }
    }

    view.navigation = std::move(navigation);
    MergeSourceCollectionSessionAction(action, RenderSampleNameSearch(std::move(view), submit, read_view));

    const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    shortcut = RouteSampleWorkflowShortcut({
        .focused = shortcut_focused,
        .hovered = shortcut_hovered,
        .navigation_enabled = true});
    ImGui::End();
    return action;
}

SourceCollectionSessionAction SourceCollectionPanelUi::RenderSampleNameSearch(
    SourceCollectionSessionView session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    const SourceCollectionSessionViewReader& read_view)
{
    SourceCollectionSessionAction action;
    SourceCollectionNavigationView navigation = std::move(session_view.navigation);
    const std::size_t navigation_index = navigation.current_index.value_or(0);
    ImGui::TextUnformatted("name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    const bool sample_name_changed = ImGui::InputText(
        "##SampleNavigationName",
        sample_name_query_buffer_.data(),
        sample_name_query_buffer_.size());
    const float sample_name_input_width = ImGui::GetItemRectSize().x;
    const float sample_name_input_left = ImGui::GetItemRectMin().x;
    const float sample_name_input_bottom = ImGui::GetItemRectMax().y;
    const bool sample_name_input_activated = ImGui::IsItemActivated();
    const bool sample_name_input_active = ImGui::IsItemActive();

    if (sample_name_input_activated || (sample_name_changed && !sample_name_search_active_)) {
        BeginSampleNameSearch(session_view);
    }

    if (sample_name_changed) {
        SourceCollectionSessionResult result =
            submit(UpdateSampleNavigation(SampleNavigationIntent::SetSampleNameQuery(sample_name_query_buffer_.data())));
        MergeSourceCollectionSessionAction(action, result.action);
        navigation = read_view().navigation;
        if (navigation.exact_sample_name_match) {
            MergeSourceCollectionSessionAction(
                action,
                CommitSampleNameSearch(*navigation.exact_sample_name_match, navigation.exact_sample_name, submit));
            return action;
        }
        if (navigation.has_partial_sample_name_matches) {
            sample_name_matches_open_ = true;
        } else {
            sample_name_matches_open_ = false;
        }
    }

    const bool has_partial_matches = navigation.has_partial_sample_name_matches;
    if (has_partial_matches && sample_name_input_active) {
        sample_name_matches_open_ = true;
    } else if (!has_partial_matches) {
        sample_name_matches_open_ = false;
    }

    bool sample_name_dropdown_interacting = false;
    std::optional<std::size_t> selected_row;
    std::string selected_name;
    if (sample_name_matches_open_ && has_partial_matches) {
        const float row_height = ImGui::GetTextLineHeightWithSpacing();
        const std::size_t visible_rows = std::min<std::size_t>(navigation.sample_name_matches.size(), 12);
        const float dropdown_height =
            row_height * static_cast<float>(visible_rows) + ImGui::GetStyle().WindowPadding.y * 2.0f;
        const ImVec2 dropdown_min(sample_name_input_left, sample_name_input_bottom);
        const ImVec2 dropdown_max(
            sample_name_input_left + sample_name_input_width,
            sample_name_input_bottom + dropdown_height);
        sample_name_dropdown_interacting = ImGui::IsMouseHoveringRect(dropdown_min, dropdown_max, false);
        constexpr ImGuiWindowFlags dropdown_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
        ImGui::SetNextWindowPos(dropdown_min, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(sample_name_input_width, dropdown_height), ImGuiCond_Always);
        if (ImGui::Begin(kSampleNavigationNameMatchesWindow, nullptr, dropdown_flags)) {
            sample_name_dropdown_interacting =
                sample_name_dropdown_interacting ||
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            for (const SourceCollectionSampleNameMatchView& match : navigation.sample_name_matches) {
                ImGui::PushID(static_cast<int>(match.row));
                if (ImGui::Selectable(match.name.c_str(), match.row == navigation_index)) {
                    selected_row = match.row;
                    selected_name = match.name;
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
    }

    if (selected_row) {
        MergeSourceCollectionSessionAction(action, CommitSampleNameSearch(*selected_row, selected_name, submit));
        return action;
    }

    if (ShouldRestoreSampleNameSearch(
            sample_name_search_active_,
            selected_row.has_value(),
            sample_name_input_active,
            sample_name_dropdown_interacting)) {
        MergeSourceCollectionSessionAction(action, RestoreFailedSampleNameSearch(submit));
    }

    return action;
}

SourceCollectionSessionAction SourceCollectionPanelUi::RenderAnnotations(
    const SourceCollectionSessionView& session_view,
    const SourceCollectionSessionIntentSubmitter& submit,
    bool* open,
    const SourceCollectionPathPicker& choose_annotation_file)
{
    SourceCollectionSessionAction action;
    if (!ImGui::Begin(kAnnotationsWindow, open)) {
        ImGui::End();
        return action;
    }

    const SpectrumSnapshotHandle& snapshot = session_view.snapshot;
    const SourceCollectionNavigationView& navigation = session_view.navigation;
    if (!snapshot || !navigation.has_active_source || snapshot->source.path.empty()) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return action;
    }

    if (ImGui::Button("Add file...")) {
        if (std::optional<std::filesystem::path> path = choose_annotation_file()) {
            SourceCollectionSessionResult result = submit(EditSourceCollection(
                SourceCollectionIntent::AddReadOnlyAnnotationResult(*path)));
            MergeSourceCollectionSessionAction(action, result.action);
        }
    }

    if (!navigation.annotation_messages.empty()) {
        for (const std::string& message : navigation.annotation_messages) {
            ImGui::TextDisabled("%s", message.c_str());
        }
        ImGui::Separator();
    }

    if (navigation.current_annotations.empty()) {
        ImGui::TextDisabled("No read-only annotations");
        ImGui::End();
        return action;
    }

    if (ImGui::BeginTable(
            "sample_annotations",
            4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn("Display name", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableHeadersRow();

        std::optional<std::filesystem::path> annotation_to_remove;
        std::optional<std::pair<std::filesystem::path, std::string>> annotation_to_rename;
        for (std::size_t annotation_index = 0; annotation_index < navigation.current_annotations.size();
             ++annotation_index) {
            const SourceCollectionAnnotationValueView& annotation =
                navigation.current_annotations[annotation_index];
            ImGui::PushID(static_cast<int>(annotation_index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string annotation_path_text = PathToUtf8(annotation.path);
            const bool editing_annotation_name =
                annotation.can_rename_annotation &&
                !annotation_path_text.empty() &&
                annotation_display_name_edit_key_ == annotation_path_text;
            if (editing_annotation_name) {
                ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
                if (annotation_display_name_focus_pending_) {
                    ImGui::SetKeyboardFocusHere();
                    annotation_display_name_focus_pending_ = false;
                }
                const bool submitted = ImGui::InputText(
                    "##annotation_display_name",
                    annotation_display_name_buffer_.data(),
                    annotation_display_name_buffer_.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::IsItemHovered()) {
                    const std::string path = NarrowPath(annotation.path);
                    ImGui::SetTooltip("%s", path.c_str());
                }
                if (submitted || ImGui::IsItemDeactivatedAfterEdit()) {
                    std::string requested_name = TrimAscii(annotation_display_name_buffer_.data());
                    annotation_to_rename = std::make_pair(annotation.path, std::move(requested_name));
                }
                if (submitted || ImGui::IsItemDeactivated()) {
                    annotation_display_name_edit_key_.clear();
                }
            } else {
                const bool edit_requested = TableCellRightAlignedTextButton(
                    "annotation_name",
                    annotation.name,
                    ImGui::GetColorU32(ImGuiCol_Text));
                if (ImGui::IsItemHovered()) {
                    const std::string path = NarrowPath(annotation.path);
                    ImGui::SetTooltip("%s", path.c_str());
                }
                if (edit_requested && annotation.can_rename_annotation && !annotation_path_text.empty()) {
                    annotation_display_name_edit_key_ = annotation_path_text;
                    CopyToBuffer(annotation_display_name_buffer_, annotation.name);
                    annotation_display_name_focus_pending_ = true;
                }
            }
            if ((annotation.can_activate_labeling || annotation.can_filter_samples || annotation.can_sort_samples) &&
                !annotation.path.empty() &&
                !editing_annotation_name &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                const std::string payload = PathToUtf8(annotation.path);
                ImGui::SetDragDropPayload(
                    kSampleAnnotationDragPayload,
                    payload.c_str(),
                    payload.size() + 1);
                ImGui::TextUnformatted(annotation.name.c_str());
                ImGui::EndDragDropSource();
            }

            ImGui::TableSetColumnIndex(1);
            (void)TableCellTextButton(
                "annotation_type",
                annotation.relationship_label,
                ImGui::GetColorU32(ImGuiCol_Text));

            ImGui::TableSetColumnIndex(2);
            std::string value_text = annotation.missing ? "(missing)" : annotation.display_text;
            if (!annotation.missing && annotation.output_missing) {
                value_text += " (output missing)";
            } else if (!annotation.missing && annotation.metadata_missing) {
                value_text += " (metadata missing)";
            } else if (!annotation.missing && !annotation.message.empty()) {
                value_text += " (metadata ignored)";
            }
            const bool disabled_value =
                annotation.missing || annotation.output_missing || annotation.metadata_missing ||
                !annotation.message.empty();
            (void)TableCellTextButton(
                "annotation_value",
                value_text,
                ImGui::GetColorU32(disabled_value ? ImGuiCol_TextDisabled : ImGuiCol_Text));
            if (!annotation.message.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", annotation.message.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            if (annotation.can_remove_annotation) {
                const ImRect remove_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                if (TrashIconButton("remove_annotation", remove_cell)) {
                    annotation_to_remove = annotation.path;
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (annotation_to_remove) {
            SourceCollectionSessionResult result = submit(EditSourceCollection(
                SourceCollectionIntent::RemoveReadOnlyAnnotationResult(*annotation_to_remove)));
            MergeSourceCollectionSessionAction(action, result.action);
        }
        if (annotation_to_rename) {
            SourceCollectionSessionResult result = submit(RenameAnnotationDisplayName(
                std::move(annotation_to_rename->first),
                std::move(annotation_to_rename->second)));
            MergeSourceCollectionSessionAction(action, result.action);
        }
    }

    ImGui::End();
    return action;
}

void SourceCollectionPanelUi::BeginSampleNameSearch(const SourceCollectionSessionView& session_view)
{
    sample_name_search_active_ = true;
    sample_name_search_restore_name_ = session_view.navigation.current_sample_name;
}

void SourceCollectionPanelUi::ClearSampleNameSearch()
{
    sample_name_matches_open_ = false;
    sample_name_search_active_ = false;
    sample_name_search_restore_name_.clear();
}

SourceCollectionSessionAction SourceCollectionPanelUi::RestoreFailedSampleNameSearch(
    const SourceCollectionSessionIntentSubmitter& submit)
{
    CopyToBuffer(sample_name_query_buffer_, sample_name_search_restore_name_);
    SourceCollectionSessionResult result =
        submit(UpdateSampleNavigation(SampleNavigationIntent::SetSampleNameQuery(sample_name_search_restore_name_)));
    ClearSampleNameSearch();
    return result.action;
}

SourceCollectionSessionAction SourceCollectionPanelUi::CommitSampleNameSearch(
    std::size_t target_row,
    const std::string& matched_name,
    const SourceCollectionSessionIntentSubmitter& submit)
{
    CopyToBuffer(sample_name_query_buffer_, matched_name);
    SourceCollectionSessionResult result =
        submit(UpdateSampleNavigation(SampleNavigationIntent::CommitSampleNameSelection(target_row, matched_name)));
    ClearSampleNameSearch();
    return result.action;
}

}  // namespace specforge
