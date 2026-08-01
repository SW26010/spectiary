#include "ui/source_collection_panel.h"

#include "app/local_user_state.h"
#include "ui/sample_name_autocomplete.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kFilesWindow = "Files###SpecForgeFilesV2";
constexpr const char* kNavigationWindow = "Navigation###SpecForgeNavigationV1";
constexpr const char* kAnnotationsWindow = "Annotations###SpecForgeAnnotationsV1";
constexpr const char* kSampleAnnotationDragPayload = "SPECFORGE_SAMPLE_ANNOTATION_PATH";
constexpr const char* kSampleNavigationSourceInput =
    "##SampleNavigationSample";
constexpr const char* kSampleNavigationSequenceInput =
    "##SampleNavigationSequence";

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

void RenderText(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

UiTextId SourceCollectionDiagnosticTextId(
    SourceCollectionManifestDiagnosticKind kind)
{
    switch (kind) {
    case SourceCollectionManifestDiagnosticKind::
        SampleNamesIgnored:
        return UiTextId::SampleNamesFileIgnored;
    case SourceCollectionManifestDiagnosticKind::
        AnnotationMetadataIgnored:
        return UiTextId::AnnotationMetadataFileIgnored;
    case SourceCollectionManifestDiagnosticKind::
        AnnotationIgnored:
    default:
        return UiTextId::AnnotationFileIgnored;
    }
}

void RenderSourceCollectionDiagnosticTooltip(
    const SourceCollectionManifestDiagnostic& diagnostic,
    UiLanguage language)
{
    if (diagnostic.detail.empty()) {
        return;
    }

    ImGui::BeginTooltip();
    RenderText(
        UiText(
            language,
            UiTextId::DiagnosticDetails));
    ImGui::Separator();
    ImGui::PushTextWrapPos(
        ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(
        diagnostic.detail.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void RenderSourceCollectionDiagnostic(
    const SourceCollectionManifestDiagnostic& diagnostic,
    UiLanguage language)
{
    std::string filename = NarrowPath(
        diagnostic.path.filename());
    if (filename.empty()) {
        filename = UiText(
            language,
            UiTextId::UnknownValue);
    }
    const std::string_view summary = UiText(
        language,
        SourceCollectionDiagnosticTextId(
            diagnostic.kind));
    ImGui::TextDisabled(
        summary.data(),
        filename.c_str());
    if (ImGui::IsItemHovered()) {
        RenderSourceCollectionDiagnosticTooltip(
            diagnostic,
            language);
    }
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
    std::string_view tooltip,
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

    if (hovered && !tooltip.empty()) {
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    return clicked;
}

bool TrashIconButton(
    const char* id,
    const ImRect& hit_rect,
    UiLanguage language)
{
    return ActionIconButton(
        id,
        hit_rect,
        ActionIcon::Trash,
        UiText(language, UiTextId::RemoveFromList),
        false);
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
    const SourceCollectionNavigationView& navigation)
{
    std::string synchronized_source_row;
    if (navigation.current_index) {
        synchronized_source_row =
            std::to_string(*navigation.current_index + 1);
    }
    std::string synchronized_sequence_position;
    if (navigation.sequence_active &&
        navigation.current_sequence_position) {
        synchronized_sequence_position =
            std::to_string(
                *navigation.current_sequence_position + 1);
    }
    const bool navigation_topology_changed =
        synchronized_navigation_topology_revision_ &&
        *synchronized_navigation_topology_revision_ !=
            navigation.sequence_topology_revision;
    synchronized_navigation_topology_revision_ =
        navigation.sequence_topology_revision;
    SyncNavigationNumberInput(
        source_row_input_,
        synchronized_source_row,
        navigation.row_location_available,
        navigation_topology_changed,
        kSampleNavigationSourceInput);
    SyncNavigationNumberInput(
        sequence_position_input_,
        synchronized_sequence_position,
        navigation.sequence_active &&
            navigation.sequence_count > 0,
        navigation_topology_changed,
        kSampleNavigationSequenceInput);
    displayed_sample_name_ = navigation.current_sample_name;
    CopyToBuffer(sample_name_query_buffer_, displayed_sample_name_);
    ClearSampleNameSearch();
}

void SourceCollectionPanelUi::FinalizeNavigationInputEdits(
    PanelSessionInteraction& interaction)
{
    if (std::optional<NavigationNumberInputCommit> source_commit =
            FinalizeNavigationNumberInput(
                source_row_input_,
                kSampleNavigationSourceInput,
                interaction)) {
        SourceCollectionNavigationView navigation =
            interaction.View().navigation;
        ApplyNavigationNumberInputCommit(
            std::move(*source_commit),
            navigation.row_location_available,
            navigation.sample_count,
            navigation.sequence_topology_revision,
            &SampleNavigationRequest::LocateRow,
            navigation,
            interaction);
        SyncNavigationInputs(navigation);
    }

    if (std::optional<NavigationNumberInputCommit> sequence_commit =
            FinalizeNavigationNumberInput(
                sequence_position_input_,
                kSampleNavigationSequenceInput,
                interaction)) {
        SourceCollectionNavigationView navigation =
            interaction.View().navigation;
        ApplyNavigationNumberInputCommit(
            std::move(*sequence_commit),
            navigation.sequence_active &&
                navigation.sequence_count > 0,
            navigation.sequence_count,
            navigation.sequence_topology_revision,
            &SampleNavigationRequest::LocateSequencePosition,
            navigation,
            interaction);
        SyncNavigationInputs(navigation);
    }
}

void SourceCollectionPanelUi::SyncNavigationNumberInput(
    NavigationNumberInputEdit& input,
    std::string_view synchronized_value,
    bool enabled,
    bool topology_changed,
    const char* input_id)
{
    const bool edit_was_active = input.edit_active;
    if (!input.edit_active || topology_changed || !enabled) {
        CopyToBuffer(input.buffer, synchronized_value);
    }
    if (topology_changed) {
        ResetNavigationNumberInputEdit(input);
        input.reload_deactivate_pending = false;
        ReloadNavigationNumberInputFromBuffer(
            input,
            input_id);
    } else if (!enabled) {
        ResetNavigationNumberInputEdit(input);
        input.reload_deactivate_pending = false;
        if (edit_was_active) {
            ReloadNavigationNumberInputFromBuffer(
                input,
                input_id);
        }
    }
}

void SourceCollectionPanelUi::RenderNavigationNumberInput(
    NavigationNumberInputEdit& input,
    const char* input_id,
    bool enabled,
    std::size_t target_count,
    std::uint64_t topology_revision,
    NavigationRequestFactory make_request,
    SourceCollectionNavigationView& navigation,
    PanelSessionInteraction& interaction)
{
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool cancel_requested =
        input.edit_active &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const std::string value_before_input =
        input.buffer.data();
    input.input_rendered_since_finalize = true;
    const bool commit_requested =
        ImGui::InputText(
            input_id,
            input.buffer.data(),
            input.buffer.size(),
            ImGuiInputTextFlags_CharsDecimal |
                ImGuiInputTextFlags_EnterReturnsTrue);
    if (!enabled) {
        ImGui::EndDisabled();
    }

    const bool reload_deactivate_requested =
        input.reload_deactivate_pending;
    if (reload_deactivate_requested) {
        input.reload_deactivate_pending = false;
        if (ImGui::IsItemActive()) {
            ImGui::ClearActiveID();
        }
    }
    const bool deactivated_after_edit =
        ImGui::IsItemDeactivatedAfterEdit();
    const bool deactivated = ImGui::IsItemDeactivated();
    if (ImGui::IsItemActivated()) {
        input.edit_active = true;
        input.edit_dirty = false;
        input.edit_initial_value = value_before_input;
        input.edit_topology_revision = topology_revision;
    }
    if (input.edit_active &&
        (ImGui::IsItemEdited() ||
         input.edit_initial_value != input.buffer.data())) {
        input.edit_dirty = true;
    }
    const bool edit_is_current =
        input.edit_active &&
        input.edit_topology_revision &&
        *input.edit_topology_revision == topology_revision;
    if (!reload_deactivate_requested &&
        !cancel_requested &&
        edit_is_current &&
        enabled &&
        commit_requested) {
        ApplyNavigationNumberInputCommit(
            NavigationNumberInputCommit{
                .draft = input.buffer.data(),
                .topology_revision =
                    *input.edit_topology_revision,
            },
            enabled,
            target_count,
            topology_revision,
            make_request,
            navigation,
            interaction);
    }
    if (!reload_deactivate_requested &&
        !cancel_requested &&
        !commit_requested &&
        edit_is_current &&
        enabled &&
        deactivated_after_edit) {
        input.blur_commit =
            NavigationNumberInputCommit{
                .draft = input.buffer.data(),
                .topology_revision =
                    *input.edit_topology_revision,
            };
    }
    if (reload_deactivate_requested ||
        cancel_requested ||
        commit_requested ||
        deactivated) {
        ResetNavigationNumberInputEdit(input);
        SyncNavigationInputs(navigation);
    }
}

std::optional<SourceCollectionPanelUi::NavigationNumberInputCommit>
SourceCollectionPanelUi::FinalizeNavigationNumberInput(
    NavigationNumberInputEdit& input,
    const char* input_id,
    PanelSessionInteraction& interaction)
{
    const bool input_rendered =
        std::exchange(
            input.input_rendered_since_finalize,
            false);
    if (input.edit_active && !input_rendered) {
        if (input.edit_dirty && input.edit_topology_revision) {
            input.blur_commit =
                NavigationNumberInputCommit{
                    .draft = input.buffer.data(),
                    .topology_revision =
                        *input.edit_topology_revision,
                };
        }
        ResetNavigationNumberInputEdit(input);
        SyncNavigationInputs(
            interaction.View().navigation);
        // The widget was not submitted, so it cannot run its ordinary
        // deactivation path. Reload its retained ImGui state now to prevent
        // the hidden draft from being restored when the panel returns.
        ReloadNavigationNumberInputFromBuffer(
            input,
            input_id);
    }

    std::optional<NavigationNumberInputCommit> commit =
        std::move(input.blur_commit);
    input.blur_commit.reset();
    return commit;
}

void SourceCollectionPanelUi::ApplyNavigationNumberInputCommit(
    NavigationNumberInputCommit commit,
    bool enabled,
    std::size_t target_count,
    std::uint64_t topology_revision,
    NavigationRequestFactory make_request,
    SourceCollectionNavigationView& navigation,
    PanelSessionInteraction& interaction)
{
    if (!enabled ||
        target_count == 0 ||
        topology_revision != commit.topology_revision) {
        return;
    }

    const std::optional<std::size_t> target_number =
        ParseSampleNumber(commit.draft);
    if (!target_number || *target_number > target_count) {
        return;
    }

    PanelSessionInteraction::Update update =
        interaction.Submit(
            UpdateSampleNavigation(
                SampleNavigationIntent::Move(
                    make_request(*target_number - 1))));
    navigation = update.view.get().navigation;
}

void SourceCollectionPanelUi::ResetNavigationNumberInputEdit(
    NavigationNumberInputEdit& input)
{
    input.edit_active = false;
    input.edit_dirty = false;
    input.edit_initial_value.clear();
    input.edit_topology_revision.reset();
}

void SourceCollectionPanelUi::ReloadNavigationNumberInputFromBuffer(
    NavigationNumberInputEdit& input,
    const char* input_id)
{
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiWindow* window =
        ImGui::FindWindowByName(kNavigationWindow);
    if (window == nullptr) {
        return;
    }
    const ImGuiID item_id = window->GetID(input_id);
    if (GImGui->InputTextDeactivatedState.ID == item_id) {
        GImGui->InputTextDeactivatedState.ClearFreeMemory();
    }
    ImGuiInputTextState* input_state =
        ImGui::GetInputTextState(item_id);
    if (input_state == nullptr) {
        return;
    }
    input_state->ReloadUserBufAndSelectAll();
    input.reload_deactivate_pending = true;
}

void SourceCollectionPanelUi::RenderFiles(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open,
    const SourceCollectionPathPicker& choose_source_file,
    const SourceCollectionPathPicker& choose_source_folder,
    const SourceCollectionPathOpener& open_source)
{
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::Files,
        "SpecForgeFilesV2");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }

    const SourceCollectionSessionView& view = interaction.View();
    RenderText(UiText(language, UiTextId::Files));
    ImGui::Separator();

    const std::string add_file_label = StableUiLabel(
        language,
        UiTextId::AddFile,
        "SpecForgeFilesAddFile");
    if (ImGui::Button(add_file_label.c_str())) {
        if (std::optional<std::filesystem::path> path = choose_source_file()) {
            open_source(*path);
        }
    }
    ImGui::SameLine();
    const std::string add_folder_label = StableUiLabel(
        language,
        UiTextId::AddFolder,
        "SpecForgeFilesAddFolder");
    if (ImGui::Button(add_folder_label.c_str())) {
        if (std::optional<std::filesystem::path> path = choose_source_folder()) {
            open_source(*path);
        }
    }
    ImGui::SameLine();
    const std::vector<SourceCollectionSourceView>& sources = view.sources;
    const std::optional<std::size_t> current_source_index = view.current_source_index;
    const SpectrumSnapshotHandle& snapshot = view.snapshot;
    const std::string source_count =
        std::to_string(sources.size()) + " " +
        std::string(UiText(
            language,
            sources.size() == 1
                ? UiTextId::SourceSingular
                : UiTextId::SourcesPlural));
    RenderDisabledText(source_count);

    ImGui::Spacing();
    const bool has_active_source =
        !sources.empty() && current_source_index && *current_source_index < sources.size() && snapshot &&
        !snapshot->source.path.empty();
    if (!has_active_source) {
        RenderDisabledText(
            UiText(language, UiTextId::NoSourcesInSession));
    } else if (ImGui::BeginTable(
                   "files_table",
                   4,
                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::SourceColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            200.0f,
            ImGui::GetID("SpecForgeFilesSourceColumn"));
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::TypeColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            72.0f,
            ImGui::GetID("SpecForgeFilesTypeColumn"));
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::StateColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            128.0f,
            ImGui::GetID("SpecForgeFilesStateColumn"));
        ImGui::TableSetupColumn(
            "",
            ImGuiTableColumnFlags_WidthFixed,
            32.0f,
            ImGui::GetID("SpecForgeFilesActionsColumn"));
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
                (void)interaction.Submit(
                    EditSourceCollection(
                        SourceCollectionIntent::SwitchActive(index)));
            }
            if (ImGui::IsItemHovered()) {
                const std::string path = NarrowPath(entry.path);
                ImGui::SetTooltip("%s", path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            const std::string_view type = entry.type
                ? SourceTypeDisplayText(
                      language,
                      *entry.type)
                : UiText(language, UiTextId::UnknownSourceType);
            if (TableCellTextButton("type", type, ImGui::GetColorU32(ImGuiCol_Text))) {
                (void)interaction.Submit(
                    EditSourceCollection(
                        SourceCollectionIntent::SwitchActive(index)));
            }

            ImGui::TableSetColumnIndex(2);
            ImU32 state_color = ImGui::GetColorU32(is_current ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (TableCellTextButton(
                    "state",
                    UiText(language, entry.state),
                    state_color)) {
                (void)interaction.Submit(
                    EditSourceCollection(
                        SourceCollectionIntent::SwitchActive(index)));
            }

            ImGui::TableSetColumnIndex(3);
            const ImRect remove_cell =
                ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
            if (TrashIconButton(
                    "remove",
                    remove_cell,
                    language)) {
                source_to_remove = index;
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (source_to_remove) {
            (void)interaction.Submit(
                EditSourceCollection(
                    SourceCollectionIntent::Remove(*source_to_remove)));
        }
    }
    ImGui::End();
}

void SourceCollectionPanelUi::RenderNavigation(
    PanelSessionInteraction& interaction,
    bool* open,
    SampleWorkflowShortcut& shortcut)
{
    RenderNavigation(
        interaction,
        UiLanguage::English,
        open,
        shortcut);
}

void SourceCollectionPanelUi::RenderNavigation(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open,
    SampleWorkflowShortcut& shortcut)
{
    shortcut = {};
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::Navigation,
        "SpecForgeNavigationV1");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }
    SourceCollectionNavigationView navigation =
        interaction.View().navigation;
    if (!navigation.has_active_source) {
        RenderDisabledText(
            UiText(language, UiTextId::NoActiveSource));
        const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        shortcut = RouteSampleWorkflowShortcut({
            .focused = shortcut_focused,
            .hovered = shortcut_hovered,
            .navigation_enabled = true});
        ImGui::End();
        return;
    }

    const std::size_t navigation_count = navigation.sample_count;

    RenderText(UiText(language, UiTextId::SourceSample));
    ImGui::SameLine();
    const float sample_input_width =
        std::max(72.0f, ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::SetNextItemWidth(sample_input_width);
    RenderNavigationNumberInput(
        source_row_input_,
        kSampleNavigationSourceInput,
        navigation.row_location_available,
        navigation_count,
        navigation.sequence_topology_revision,
        &SampleNavigationRequest::LocateRow,
        navigation,
        interaction);
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
        PanelSessionInteraction::Update update =
            interaction.SubmitNavigation(
                UpdateSampleNavigation(
                    SampleNavigationIntent::Move(
                        SampleNavigationRequest::Previous())),
                SampleNavigationRequestKind::Previous);
        navigation = update.view.get().navigation;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const std::string_view tooltip = UiText(
            language,
            UiTextId::PreviousSampleShortcut);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    if (!can_previous) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_next) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("+##NextSample", sample_step_button_size)) {
        PanelSessionInteraction::Update update =
            interaction.SubmitNavigation(
                UpdateSampleNavigation(
                    SampleNavigationIntent::Move(
                        SampleNavigationRequest::Next())),
                SampleNavigationRequestKind::Next);
        navigation = update.view.get().navigation;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const std::string_view tooltip = UiText(
            language,
            UiTextId::NextSampleShortcut);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    if (!can_next) {
        ImGui::EndDisabled();
    }
    if (navigation.current_sample_name.empty() && !navigation.current_sample_display_name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", navigation.current_sample_display_name.c_str());
    }
    if (navigation.sequence_active) {
        RenderText(UiText(language, UiTextId::Sequence));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sample_input_width);
        RenderNavigationNumberInput(
            sequence_position_input_,
            kSampleNavigationSequenceInput,
            navigation.sequence_count > 0,
            navigation.sequence_count,
            navigation.sequence_topology_revision,
            &SampleNavigationRequest::LocateSequencePosition,
            navigation,
            interaction);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Text(
            "/%llu",
            static_cast<unsigned long long>(
                navigation.sequence_count));
    }

    RenderSampleNameSearch(
        std::move(navigation),
        interaction,
        language);

    const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    shortcut = RouteSampleWorkflowShortcut({
        .focused = shortcut_focused,
        .hovered = shortcut_hovered,
        .navigation_enabled = true});
    ImGui::End();
}

void SourceCollectionPanelUi::RenderSampleNameSearch(
    SourceCollectionNavigationView navigation,
    PanelSessionInteraction& interaction,
    UiLanguage language)
{
    const std::size_t navigation_index = navigation.current_index.value_or(0);
    RenderText(UiText(language, UiTextId::SampleName));
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
        BeginSampleNameSearch(navigation);
    }

    if (sample_name_changed) {
        PanelSessionInteraction::Update update =
            interaction.Submit(
                UpdateSampleNavigation(
                    SampleNavigationIntent::SetSampleNameQuery(
                        sample_name_query_buffer_.data())));
        navigation = update.view.get().navigation;
        if (navigation.exact_sample_name_match) {
            CommitSampleNameSearch(
                *navigation.exact_sample_name_match,
                navigation.exact_sample_name,
                interaction);
            return;
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
        const std::string matches_window =
            StableUiLabel(
                language,
                UiTextId::SampleNameMatches,
                "SpecForgeSampleNameMatchesV1");
        if (ImGui::Begin(
                matches_window.c_str(),
                nullptr,
                dropdown_flags)) {
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
        CommitSampleNameSearch(
            *selected_row,
            selected_name,
            interaction);
        return;
    }

    if (ShouldRestoreSampleNameSearch(
            sample_name_search_active_,
            selected_row.has_value(),
            sample_name_input_active,
            sample_name_dropdown_interacting)) {
        RestoreFailedSampleNameSearch(interaction);
    }
}

void SourceCollectionPanelUi::RenderAnnotations(
    PanelSessionInteraction& interaction,
    UiLanguage language,
    bool* open,
    const SourceCollectionPathPicker& choose_annotation_file)
{
    const std::string window_label = StableUiLabel(
        language,
        UiTextId::Annotations,
        "SpecForgeAnnotationsV1");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }

    const SourceCollectionSessionView& session_view =
        interaction.View();
    const SpectrumSnapshotHandle& snapshot = session_view.snapshot;
    const SourceCollectionNavigationView& navigation = session_view.navigation;
    if (!snapshot || !navigation.has_active_source || snapshot->source.path.empty()) {
        RenderDisabledText(
            UiText(language, UiTextId::NoActiveSource));
        ImGui::End();
        return;
    }

    const std::string add_file_label = StableUiLabel(
        language,
        UiTextId::AddFile,
        "SpecForgeAnnotationsAddFile");
    if (ImGui::Button(add_file_label.c_str())) {
        if (std::optional<std::filesystem::path> path = choose_annotation_file()) {
            (void)interaction.Submit(
                EditSourceCollection(
                    SourceCollectionIntent::
                        AddReadOnlyAnnotationResult(*path)));
        }
    }

    if (!navigation.annotation_diagnostics.empty()) {
        for (const SourceCollectionManifestDiagnostic&
                 diagnostic :
             navigation.annotation_diagnostics) {
            RenderSourceCollectionDiagnostic(
                diagnostic,
                language);
        }
        ImGui::Separator();
    }

    if (navigation.current_annotations.empty()) {
        RenderDisabledText(
            UiText(language, UiTextId::NoReadOnlyAnnotations));
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "sample_annotations",
            4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::DisplayNameColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            220.0f,
            ImGui::GetID(
                "SpecForgeAnnotationsDisplayNameColumn"));
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::TypeColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            72.0f,
            ImGui::GetID(
                "SpecForgeAnnotationsTypeColumn"));
        ImGui::TableSetupColumn(
            UiText(language, UiTextId::ValueColumn).data(),
            ImGuiTableColumnFlags_WidthFixed,
            160.0f,
            ImGui::GetID(
                "SpecForgeAnnotationsValueColumn"));
        ImGui::TableSetupColumn(
            "",
            ImGuiTableColumnFlags_WidthFixed,
            32.0f,
            ImGui::GetID(
                "SpecForgeAnnotationsActionsColumn"));
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
                UiText(language, annotation.relationship),
                ImGui::GetColorU32(ImGuiCol_Text));

            ImGui::TableSetColumnIndex(2);
            std::string value_text = annotation.missing
                ? std::string(UiText(
                      language,
                      UiTextId::AnnotationMissing))
                : annotation.display_text;
            if (!annotation.missing && annotation.output_missing) {
                value_text += " ";
                value_text += UiText(
                    language,
                    UiTextId::AnnotationOutputMissing);
            } else if (!annotation.missing && annotation.metadata_missing) {
                value_text += " ";
                value_text += UiText(
                    language,
                    UiTextId::AnnotationMetadataMissing);
            } else if (
                !annotation.missing &&
                annotation.diagnostic) {
                value_text += " ";
                value_text += UiText(
                    language,
                    UiTextId::AnnotationMetadataIgnored);
            }
            const bool disabled_value =
                annotation.missing || annotation.output_missing || annotation.metadata_missing ||
                annotation.diagnostic.has_value();
            (void)TableCellTextButton(
                "annotation_value",
                value_text,
                ImGui::GetColorU32(disabled_value ? ImGuiCol_TextDisabled : ImGuiCol_Text));
            if (annotation.diagnostic &&
                ImGui::IsItemHovered()) {
                RenderSourceCollectionDiagnosticTooltip(
                    *annotation.diagnostic,
                    language);
            }

            ImGui::TableSetColumnIndex(3);
            if (annotation.can_remove_annotation) {
                const ImRect remove_cell =
                    ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), ImGui::TableGetColumnIndex());
                if (TrashIconButton(
                        "remove_annotation",
                        remove_cell,
                        language)) {
                    annotation_to_remove = annotation.path;
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (annotation_to_remove) {
            (void)interaction.Submit(
                EditSourceCollection(
                    SourceCollectionIntent::
                        RemoveReadOnlyAnnotationResult(
                            *annotation_to_remove)));
        }
        if (annotation_to_rename) {
            (void)interaction.Submit(
                RenameAnnotationDisplayName(
                    std::move(annotation_to_rename->first),
                    std::move(annotation_to_rename->second)));
        }
    }

    ImGui::End();
}

void SourceCollectionPanelUi::BeginSampleNameSearch(
    const SourceCollectionNavigationView& navigation)
{
    sample_name_search_active_ = true;
    displayed_sample_name_ = navigation.current_sample_name;
    sample_name_search_restore_name_ = displayed_sample_name_;
}

void SourceCollectionPanelUi::ClearSampleNameSearch()
{
    sample_name_matches_open_ = false;
    sample_name_search_active_ = false;
    sample_name_search_restore_name_.clear();
}

void SourceCollectionPanelUi::RestoreFailedSampleNameSearch(
    PanelSessionInteraction& interaction)
{
    CopyToBuffer(sample_name_query_buffer_, sample_name_search_restore_name_);
    (void)interaction.Submit(
        UpdateSampleNavigation(
            SampleNavigationIntent::SetSampleNameQuery(
                sample_name_search_restore_name_)));
    ClearSampleNameSearch();
}

void SourceCollectionPanelUi::CommitSampleNameSearch(
    std::size_t target_row,
    const std::string& matched_name,
    PanelSessionInteraction& interaction)
{
    CopyToBuffer(sample_name_query_buffer_, matched_name);
    (void)interaction.Submit(
        UpdateSampleNavigation(
            SampleNavigationIntent::CommitSampleNameSelection(
                target_row,
                matched_name)));
    ClearSampleNameSearch();
}

}  // namespace specforge
