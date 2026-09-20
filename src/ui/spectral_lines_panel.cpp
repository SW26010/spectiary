#include "overlays/spectral_line_projection.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_name_localization.h"
#include "ui/theme.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace spectiary {
namespace {

constexpr const char* kSpectralLinesWindow =
    "Spectral Lines###SpectralLinesV2";

bool HasNonWhitespace(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
}

std::string LocalizedLineListName(
    UiLanguage language,
    const SpectralLinePanelView& state)
{
    if (!state.user_owned && state.line_list_id ==
        "public-spectral-lines.v1") {
        return std::string(
            UiText(
                language,
                UiTextId::BuiltInLineList));
    }
    return state.line_list_display_name;
}

void RenderWrappedStatusText(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

ImVec4 SeverityColor(SpectrumDiagnosticSeverity severity)
{
    const SemanticPalette& palette =
        ActiveSemanticPalette();
    switch (severity) {
    case SpectrumDiagnosticSeverity::Error:
        return palette.error;
    case SpectrumDiagnosticSeverity::Warning:
        return palette.warning;
    case SpectrumDiagnosticSeverity::Info:
    default:
        return palette.muted;
    }
}

void RenderLocalizedDiagnosticStatus(
    UiLanguage language,
    UiTextId message_id,
    std::string_view diagnostic_detail)
{
    std::string message(
        UiText(
            language,
            message_id));
    if (!diagnostic_detail.empty()) {
        message += "\n";
        message += diagnostic_detail;
    }
    RenderWrappedStatusText(
        SeverityColor(
            SpectrumDiagnosticSeverity::Warning),
        message);
}

}  // namespace

const char* SpectralLinesPanelUi::WindowName()
{
    return kSpectralLinesWindow;
}

void SpectralLinesPanelUi::Render(
    SpectralLinesPanelController& panel,
    const SpectrumSnapshotHandle& snapshot,
    bool* open)
{
    Render(
        panel,
        snapshot,
        UiLanguage::English,
        open);
}

void SpectralLinesPanelUi::Render(
    SpectralLinesPanelController& panel,
    const SpectrumSnapshotHandle& snapshot,
    UiLanguage language,
    bool* open)
{
    const SpectralLinePanelView state = panel.View();
    if (generation_ != state.generation) {
        generation_ = state.generation;
        grouping_view_search_initialized_ = false;
        renaming_grouping_view_id_.reset();
        deleting_grouping_view_id_.reset();
        grouping_view_ui_ = {};
    }
    if (!grouping_view_search_initialized_) {
        std::snprintf(
            grouping_view_search_.data(),
            grouping_view_search_.size(),
            "%s",
            state.grouping_view_search.c_str());
        grouping_view_search_initialized_ = true;
    }

    const std::string window_label = StableUiLabel(
        language,
        UiTextId::SpectralLines,
        "SpectralLinesV2");
    if (!ImGui::Begin(window_label.c_str(), open)) {
        ImGui::End();
        return;
    }
    ImGui::PushID(static_cast<int>(generation_));
    const std::string_view heading =
        UiText(
            language,
            UiTextId::SpectralLines);
    ImGui::TextUnformatted(
        heading.data(),
        heading.data() + heading.size());
    ImGui::Separator();

    const bool can_show_lines = snapshot && snapshot->capabilities.can_show_spectral_lines;
    if (!can_show_lines) {
        const std::string_view unavailable =
            UiText(
                language,
                UiTextId::CurrentSnapshotHasNoWavelengthAxis);
        ImGui::TextWrapped(
            "%.*s",
            static_cast<int>(unavailable.size()),
            unavailable.data());
    } else if (snapshot->capabilities.requires_rest_frame_warning) {
        const std::string_view warning =
            UiText(
                language,
                UiTextId::UnknownWavelengthFrameWarning);
        ImGui::TextColored(
            SeverityColor(SpectrumDiagnosticSeverity::Warning),
            "%.*s",
            static_cast<int>(warning.size()),
            warning.data());
    }

    const std::string selected_line_list = LocalizedLineListName(language, state);
    const std::string line_list_label = StableUiLabel(language, UiTextId::SpectralLineList, "SpectralLineList");
    if (ImGui::BeginCombo(line_list_label.c_str(), selected_line_list.c_str())) {
        if (ImGui::Selectable(UiText(language, UiTextId::BuiltInLineList).data(), !state.user_owned))
            (void)panel.SelectBuiltInLineList();
        if (!state.user_line_list_path.empty()) {
            const auto label = state.user_line_list_name + "###opened_user_line_list";
            if (ImGui::Selectable(label.c_str(), state.user_owned)) (void)panel.SelectOpenedUserLineList();
        }
        ImGui::Separator();
        if (ImGui::Selectable(UiText(language, UiTextId::OpenLineList).data())) open_requested_ = true;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", state.user_owned ? state.user_line_list_path.c_str() : state.line_list_id.c_str());
    }
    // Finish this frame after switching; no stale view can submit to the new owner.
    if (panel.Generation() != state.generation) {
        ImGui::PopID(); ImGui::End(); return;
    }
    ImGui::SameLine();
    bool marker_labels_visible = state.marker_labels_visible;
    const std::string labels_label =
        StableUiLabel(
            language,
            UiTextId::Labels,
            "SpectralLineLabels");
    if (ImGui::Checkbox(
            labels_label.c_str(),
            &marker_labels_visible)) {
        (void)panel.Submit(SpectralLineStateIntent::SetMarkerLabelsVisible(marker_labels_visible));
    }

    ImGui::Spacing();
    if (!state.open_error.empty())
        RenderLocalizedDiagnosticStatus(language, UiTextId::LineListOpenFailed, state.open_error);
    ImGui::TextDisabled("%s", state.coordinate_description.c_str());
    if (state.user_owned)
        ImGui::TextWrapped("%s", UiText(language, UiTextId::LineListReadOnly).data());
    const auto projection = ProjectSpectralLineList(panel.PlotSource(), snapshot);
    if (projection.status == SpectralLineProjectionStatus::UnsupportedCoordinates)
        RenderLocalizedDiagnosticStatus(language, UiTextId::LineListCoordinatesUnsupported, {});
    if (!state.color_schemes.empty()) {
        std::string selected;
        for (const auto& [id, name] : state.color_schemes) if (id == state.active_color_scheme_id) selected = name;
        const auto label = StableUiLabel(language, UiTextId::LineListColorScheme, "LineListColorScheme");
        if (ImGui::BeginCombo(label.c_str(), selected.c_str())) {
            for (const auto& [id, name] : state.color_schemes) {
                ImGui::PushID(id.c_str());
                if (ImGui::Selectable((name + "###scheme").c_str(), id == state.active_color_scheme_id))
                    (void)panel.Submit(SpectralLineStateIntent::SelectColorScheme(id));
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    if (!state.line_list_load_error.empty()) {
        const std::string error =
            std::string(
                UiText(
                    language,
                    UiTextId::LineListOpenFailed)) +
            state.line_list_load_error;
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), error);
    } else if (state.line_list_marker_count == 0) {
        const std::string_view no_markers =
            UiText(
                language,
                UiTextId::LineListNoMarkers);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(no_markers.size()),
            no_markers.data());
    }
    switch (state.persistence.load_issue) {
    case SpectralLineCacheLoadIssueKind::ReadFailed:
        RenderLocalizedDiagnosticStatus(
            language,
            UiTextId::SpectralLineCacheReadFailed,
            state.persistence.load_diagnostic_detail);
        break;
    case SpectralLineCacheLoadIssueKind::InvalidDocument:
        RenderLocalizedDiagnosticStatus(
            language,
            UiTextId::SpectralLineCacheInvalid,
            state.persistence.load_diagnostic_detail);
        break;
    case SpectralLineCacheLoadIssueKind::
        UnsupportedFormatOrSchema:
        RenderLocalizedDiagnosticStatus(
            language,
            UiTextId::SpectralLineCacheUnsupported,
            state.persistence.load_diagnostic_detail);
        break;
    case SpectralLineCacheLoadIssueKind::None:
        break;
    }
    if (state.persistence.retrying) {
        RenderLocalizedDiagnosticStatus(
            language,
            UiTextId::SpectralLinePersistenceRetrying,
            state.persistence.save_diagnostic_detail);
    } else if (state.persistence.recovered) {
        RenderWrappedStatusText(
            ActiveSemanticPalette().success,
            UiText(
                language,
                UiTextId::SpectralLinePersistenceRecovered));
    }

    const std::string search_label =
        StableUiLabel(
            language,
            UiTextId::Search,
            "SpectralLineSearch");
    if (ImGui::InputTextWithHint(
            search_label.c_str(),
            UiText(
                language,
                UiTextId::SpectralLineSearchHint)
                .data(),
            grouping_view_search_.data(),
            grouping_view_search_.size())) {
        (void)panel.Submit(
            SpectralLineStateIntent::SetGroupingViewSearch(grouping_view_search_.data()));
    }

    ImGui::Spacing();

    std::optional<SpectralLineGroupingView> pending_duplicate;
    std::optional<SpectralLineGroupingView> pending_rename;
    std::optional<SpectralLineGroupingView> pending_delete;
    const auto create_new_view = [&panel]() {
        (void)panel.Submit(SpectralLineStateIntent::CreateUserGroupingView());
    };

    if (ImGui::BeginTabBar("spectral_line_grouping_views", ImGuiTabBarFlags_Reorderable)) {
        for (const SpectralLineGroupingView& grouping_view : state.grouping_views) {
            const ImGuiTabItemFlags flags = grouping_view.selection_requested
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            const std::string grouping_view_display_name = grouping_view.id.empty()
                ? std::string(UiText(language, UiTextId::LineListMarkers))
                : LocalizedSpectralLineName(
                    language,
                    grouping_view.name,
                    grouping_view.generated_name);
            const std::string tab_label =
                grouping_view_display_name +
                "###" +
                grouping_view.id;
            if (ImGui::BeginTabItem(tab_label.c_str(), nullptr, flags)) {
                if (!grouping_view.active) {
                    (void)panel.Submit(SpectralLineStateIntent::SelectGroupingView(grouping_view.id));
                }
                (void)panel.Submit(
                    SpectralLineStateIntent::AcknowledgeGroupingViewSelection(grouping_view.id));
                if (ImGui::BeginPopupContextItem(
                        grouping_view.editable ? "user_grouping_view_context"
                                               : "catalog_grouping_view_context")) {
                    const std::string duplicate_label =
                        StableUiLabel(
                            language,
                            grouping_view.editable
                                ? UiTextId::Duplicate
                                : UiTextId::DuplicateAsUserView,
                            "DuplicateSpectralLineGroupingView");
                    if (!state.user_owned && ImGui::Selectable(
                            duplicate_label.c_str())) {
                        pending_duplicate = grouping_view;
                    }
                    const std::string rename_label =
                        StableUiLabel(
                            language,
                            UiTextId::Rename,
                            "RenameSpectralLineGroupingView");
                    if (grouping_view.editable &&
                        ImGui::Selectable(rename_label.c_str())) {
                        pending_rename = grouping_view;
                    }
                    const std::string delete_label =
                        StableUiLabel(
                            language,
                            UiTextId::Delete,
                            "DeleteSpectralLineGroupingView");
                    if (grouping_view.editable &&
                        ImGui::Selectable(delete_label.c_str())) {
                        pending_delete = grouping_view;
                    }
                    ImGui::EndPopup();
                }
                grouping_view_ui_.Render(
                    panel,
                    snapshot,
                    grouping_view,
                    state.line_list_marker_count,
                    language);
                ImGui::EndTabItem();
            }
        }

        if (!state.user_owned && ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            create_new_view();
        }
        if (!state.user_owned && ImGui::IsItemHovered()) {
            const std::string_view tooltip =
                UiText(
                    language,
                    UiTextId::NewUserGroupingView);
            ImGui::SetTooltip(
                "%.*s",
                static_cast<int>(tooltip.size()),
                tooltip.data());
        }
        ImGui::EndTabBar();
    }

    if (pending_duplicate) {
        (void)panel.Submit(SpectralLineStateIntent::DuplicateGroupingView(pending_duplicate->id));
    }
    if (pending_rename) {
        renaming_grouping_view_id_ = pending_rename->id;
        renaming_grouping_view_original_name_ =
            pending_rename->name;
        renaming_grouping_view_name_ =
            LocalizedSpectralLineName(
                language,
                pending_rename->name,
                pending_rename->generated_name);
        renaming_grouping_view_edited_ = false;
        const std::string rename_popup =
            StableUiLabel(
                language,
                UiTextId::RenameGroupingView,
                "RenameGroupingViewPopup");
        ImGui::OpenPopup(rename_popup.c_str());
    }
    if (pending_delete) {
        deleting_grouping_view_id_ = pending_delete->id;
        deleting_grouping_view_name_ =
            LocalizedSpectralLineName(
                language,
                pending_delete->name,
                pending_delete->generated_name);
        const std::string delete_popup =
            StableUiLabel(
                language,
                UiTextId::DeleteGroupingView,
                "DeleteGroupingViewPopup");
        ImGui::OpenPopup(delete_popup.c_str());
    }
    grouping_view_ui_.RenderPendingPopups(
        panel,
        language);

    const std::string rename_popup =
        StableUiLabel(
            language,
            UiTextId::RenameGroupingView,
            "RenameGroupingViewPopup");
    if (ImGui::BeginPopupModal(
            rename_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const std::string name_label =
            StableUiLabel(
                language,
                UiTextId::Name,
                "SpectralLineGroupingViewName");
        const bool submitted = ImGui::InputText(
            name_label.c_str(),
            &renaming_grouping_view_name_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) {
            renaming_grouping_view_edited_ = true;
        }
        const bool valid_name =
            HasNonWhitespace(
                renaming_grouping_view_name_);
        const auto finish_rename = [this, &panel]() {
            if (renaming_grouping_view_id_) {
                const std::string submitted_name =
                    ResolveSpectralLineRenameSubmission(
                        renaming_grouping_view_name_,
                        renaming_grouping_view_original_name_,
                        renaming_grouping_view_edited_);
                (void)panel.Submit(SpectralLineStateIntent::RenameUserGroupingView(
                    *renaming_grouping_view_id_,
                    submitted_name,
                    renaming_grouping_view_edited_
                        ? SpectralLineRenameEditState::Edited
                        : SpectralLineRenameEditState::Unedited));
            }
            renaming_grouping_view_id_.reset();
            renaming_grouping_view_name_.clear();
            renaming_grouping_view_original_name_.clear();
            renaming_grouping_view_edited_ = false;
            ImGui::CloseCurrentPopup();
        };
        if (!valid_name) {
            ImGui::BeginDisabled();
        }
        const std::string rename_label =
            StableUiLabel(
                language,
                UiTextId::Rename,
                "ConfirmRenameSpectralLineGroupingView");
        if (ImGui::Button(rename_label.c_str()) ||
            (submitted && valid_name)) {
            finish_rename();
        }
        if (!valid_name) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const std::string cancel_rename_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "CancelRenameSpectralLineGroupingView");
        if (ImGui::Button(cancel_rename_label.c_str())) {
            renaming_grouping_view_id_.reset();
            renaming_grouping_view_name_.clear();
            renaming_grouping_view_original_name_.clear();
            renaming_grouping_view_edited_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const std::string delete_popup =
        StableUiLabel(
            language,
            UiTextId::DeleteGroupingView,
            "DeleteGroupingViewPopup");
    if (ImGui::BeginPopupModal(
            delete_popup.c_str(),
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(
            UiText(
                language,
                UiTextId::DeleteGroupingViewQuestion)
                .data(),
            deleting_grouping_view_name_.c_str());
        const std::string_view deletion_scope =
            UiText(
                language,
                UiTextId::CatalogMarkersRemainAfterViewDeletion);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(deletion_scope.size()),
            deletion_scope.data());
        const std::string delete_label =
            StableUiLabel(
                language,
                UiTextId::Delete,
                "ConfirmDeleteSpectralLineGroupingView");
        if (ImGui::Button(delete_label.c_str())) {
            if (deleting_grouping_view_id_) {
                (void)panel.Submit(
                    SpectralLineStateIntent::DeleteUserGroupingView(*deleting_grouping_view_id_));
            }
            deleting_grouping_view_id_.reset();
            deleting_grouping_view_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string cancel_delete_label =
            StableUiLabel(
                language,
                UiTextId::Cancel,
                "CancelDeleteSpectralLineGroupingView");
        if (ImGui::Button(cancel_delete_label.c_str())) {
            deleting_grouping_view_id_.reset();
            deleting_grouping_view_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!state.user_owned && !state.has_base_grouping_view && state.user_grouping_view_count == 0) {
        const std::string_view no_grouping =
            UiText(
                language,
                UiTextId::NoCatalogGroupingView);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(no_grouping.size()),
            no_grouping.data());
        const std::string new_grouping_view_label =
            StableUiLabel(
                language,
                UiTextId::NewGroupingView,
                "NewSpectralLineGroupingView");
        if (ImGui::Button(
                new_grouping_view_label.c_str())) {
            create_new_view();
        }
    }

    ImGui::PopID();
    ImGui::End();
}

}  // namespace spectiary
