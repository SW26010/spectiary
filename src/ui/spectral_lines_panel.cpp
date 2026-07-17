#include "ui/spectral_lines_panel.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {
namespace {

constexpr const char* kSpectralLinesWindow = "Spectral Lines###SpecForgeSpectralLinesV2";
constexpr const char* kRenameGroupingViewPopup = "Rename grouping view###SpecForgeRenameGroupingViewPopup";
constexpr const char* kDeleteGroupingViewPopup = "Delete grouping view###SpecForgeDeleteGroupingViewPopup";

bool HasNonWhitespace(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
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
    switch (severity) {
    case SpectrumDiagnosticSeverity::Error:
        return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
    case SpectrumDiagnosticSeverity::Warning:
        return ImVec4(0.95f, 0.74f, 0.30f, 1.0f);
    case SpectrumDiagnosticSeverity::Info:
    default:
        return ImVec4(0.62f, 0.70f, 0.78f, 1.0f);
    }
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
    const CatalogUserStateView state = panel.View();
    if (!grouping_view_search_initialized_) {
        std::snprintf(
            grouping_view_search_.data(),
            grouping_view_search_.size(),
            "%s",
            state.grouping_view_search.c_str());
        grouping_view_search_initialized_ = true;
    }

    if (!ImGui::Begin(kSpectralLinesWindow, open)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Spectral Lines");
    ImGui::Separator();

    const bool can_show_lines = snapshot && snapshot->capabilities.can_show_spectral_lines;
    if (!can_show_lines) {
        ImGui::TextWrapped("Current snapshot does not expose a wavelength axis for spectral-line overlays.");
    } else if (snapshot->capabilities.requires_rest_frame_warning) {
        ImGui::TextColored(
            SeverityColor(SpectrumDiagnosticSeverity::Warning),
            "Wavelength frame is unknown; rest-frame overlays are reference-only.");
    }

    const char* selected_catalog = state.catalog_display_name.c_str();
    if (ImGui::BeginCombo("Catalog", selected_catalog)) {
        ImGui::Selectable(selected_catalog, true);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", state.catalog_id.c_str());
    }
    ImGui::SameLine();
    bool marker_labels_visible = state.marker_labels_visible;
    if (ImGui::Checkbox("Labels", &marker_labels_visible)) {
        (void)panel.Submit(CatalogUserStateIntent::SetMarkerLabelsVisible(marker_labels_visible));
    }

    ImGui::Spacing();
    if (!state.catalog_load_error.empty()) {
        const std::string error = "Catalog load failed: " + state.catalog_load_error;
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), error);
    } else if (state.catalog_marker_count == 0) {
        ImGui::TextDisabled("No public catalog markers loaded.");
    }
    if (!state.warning.empty()) {
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), state.warning);
    }

    if (ImGui::InputTextWithHint(
            "Search",
            "id, label, catalog group, or plot label",
            grouping_view_search_.data(),
            grouping_view_search_.size())) {
        (void)panel.Submit(
            CatalogUserStateIntent::SetGroupingViewSearch(grouping_view_search_.data()));
    }

    ImGui::Spacing();

    std::optional<SpectralLineGroupingView> pending_duplicate;
    std::optional<SpectralLineGroupingView> pending_rename;
    std::optional<SpectralLineGroupingView> pending_delete;
    const auto create_new_view = [&panel]() {
        (void)panel.Submit(CatalogUserStateIntent::CreateUserGroupingView());
    };

    if (ImGui::BeginTabBar("spectral_line_grouping_views", ImGuiTabBarFlags_Reorderable)) {
        for (const SpectralLineGroupingView& grouping_view : state.grouping_views) {
            const ImGuiTabItemFlags flags = grouping_view.selection_requested
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            const std::string tab_label = grouping_view.name + "###" + grouping_view.id;
            if (ImGui::BeginTabItem(tab_label.c_str(), nullptr, flags)) {
                if (!grouping_view.active) {
                    (void)panel.Submit(CatalogUserStateIntent::SelectGroupingView(grouping_view.id));
                }
                (void)panel.Submit(
                    CatalogUserStateIntent::AcknowledgeGroupingViewSelection(grouping_view.id));
                if (ImGui::BeginPopupContextItem(
                        grouping_view.editable ? "user_grouping_view_context"
                                               : "catalog_grouping_view_context")) {
                    if (ImGui::Selectable(
                            grouping_view.editable ? "Duplicate" : "Duplicate as user view")) {
                        pending_duplicate = grouping_view;
                    }
                    if (grouping_view.editable && ImGui::Selectable("Rename")) {
                        pending_rename = grouping_view;
                    }
                    if (grouping_view.editable && ImGui::Selectable("Delete")) {
                        pending_delete = grouping_view;
                    }
                    ImGui::EndPopup();
                }
                grouping_view_ui_.Render(panel, snapshot, grouping_view, state.catalog_marker_count);
                ImGui::EndTabItem();
            }
        }

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            create_new_view();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("New user grouping view");
        }
        ImGui::EndTabBar();
    }

    if (pending_duplicate) {
        (void)panel.Submit(CatalogUserStateIntent::DuplicateGroupingView(pending_duplicate->id));
    }
    if (pending_rename) {
        renaming_grouping_view_id_ = pending_rename->id;
        std::snprintf(
            renaming_grouping_view_name_.data(),
            renaming_grouping_view_name_.size(),
            "%s",
            pending_rename->name.c_str());
        ImGui::OpenPopup(kRenameGroupingViewPopup);
    }
    if (pending_delete) {
        deleting_grouping_view_id_ = pending_delete->id;
        deleting_grouping_view_name_ = pending_delete->name;
        ImGui::OpenPopup(kDeleteGroupingViewPopup);
    }
    grouping_view_ui_.RenderPendingPopups(panel);

    if (ImGui::BeginPopupModal(kRenameGroupingViewPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted = ImGui::InputText(
            "Name",
            renaming_grouping_view_name_.data(),
            renaming_grouping_view_name_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid_name = HasNonWhitespace(renaming_grouping_view_name_.data());
        const auto finish_rename = [this, &panel]() {
            if (renaming_grouping_view_id_) {
                (void)panel.Submit(CatalogUserStateIntent::RenameUserGroupingView(
                    *renaming_grouping_view_id_,
                    renaming_grouping_view_name_.data()));
            }
            renaming_grouping_view_id_.reset();
            renaming_grouping_view_name_.fill('\0');
            ImGui::CloseCurrentPopup();
        };
        if (!valid_name) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Rename") || (submitted && valid_name)) {
            finish_rename();
        }
        if (!valid_name) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            renaming_grouping_view_id_.reset();
            renaming_grouping_view_name_.fill('\0');
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal(kDeleteGroupingViewPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete grouping view \"%s\"?", deleting_grouping_view_name_.c_str());
        ImGui::TextDisabled("Catalog markers and marker visibility are not deleted.");
        if (ImGui::Button("Delete")) {
            if (deleting_grouping_view_id_) {
                (void)panel.Submit(
                    CatalogUserStateIntent::DeleteUserGroupingView(*deleting_grouping_view_id_));
            }
            deleting_grouping_view_id_.reset();
            deleting_grouping_view_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            deleting_grouping_view_id_.reset();
            deleting_grouping_view_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!state.has_catalog_grouping_view && state.user_grouping_view_count == 0) {
        ImGui::TextDisabled("This catalog has no catalog grouping view.");
        if (ImGui::Button("+ New grouping view")) {
            create_new_view();
        }
    }

    ImGui::End();
}

}  // namespace specforge
