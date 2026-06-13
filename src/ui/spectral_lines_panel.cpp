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

void SpectralLinesPanelUi::Render(SpectralLinesPanelController& panel, const SpectrumSnapshotHandle& snapshot)
{
    const SpectralLineCatalog& catalog = panel.catalog();
    const CatalogIdentity& identity = panel.catalog_identity();
    const std::optional<GroupingView>& catalog_grouping_view = panel.catalog_grouping_view();
    CatalogUserState& user_state = panel.user_state();
    std::array<char, 96>& filter = panel.filter_buffer();

    ImGui::Begin(kSpectralLinesWindow);
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

    const char* selected_catalog = identity.display_name.c_str();
    if (ImGui::BeginCombo("Catalog", selected_catalog)) {
        ImGui::Selectable(selected_catalog, true);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", identity.id.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &panel.show_labels());

    ImGui::Spacing();
    if (!catalog.load_error.empty()) {
        const std::string error = "Catalog load failed: " + catalog.load_error;
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), error);
    } else if (catalog.markers.empty()) {
        ImGui::TextDisabled("No public catalog markers loaded.");
    }
    if (!panel.warning().empty()) {
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), panel.warning());
    }

    ImGui::InputTextWithHint(
        "Search",
        "id, label, catalog group, or plot label",
        filter.data(),
        filter.size());

    ImGui::Spacing();
    panel.NormalizeViewSelection();

    std::optional<GroupingView> pending_duplicate;
    std::optional<GroupingView> pending_rename;
    std::optional<GroupingView> pending_delete;
    const auto create_new_view = [&panel]() {
        panel.CreateUserGroupingView();
    };

    if (ImGui::BeginTabBar("spectral_line_grouping_views", ImGuiTabBarFlags_Reorderable)) {
        if (catalog_grouping_view) {
            const bool selected = user_state.active_view_id == catalog_grouping_view->id;
            const ImGuiTabItemFlags flags = panel.ShouldSelectTab(catalog_grouping_view->id)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(catalog_grouping_view->name.c_str(), nullptr, flags)) {
                if (!selected) {
                    panel.SetActiveView(catalog_grouping_view->id);
                }
                panel.AcknowledgeTabSelection(catalog_grouping_view->id);
                if (ImGui::BeginPopupContextItem("catalog_grouping_view_context")) {
                    if (ImGui::Selectable("Duplicate as user view")) {
                        pending_duplicate = *catalog_grouping_view;
                    }
                    ImGui::EndPopup();
                }
                grouping_view_ui_.Render(panel, snapshot, *catalog_grouping_view, nullptr);
                ImGui::EndTabItem();
            }
        }

        for (std::size_t index = 0; index < user_state.grouping_views.size(); ++index) {
            GroupingView& user_view = user_state.grouping_views[index];
            const bool selected = user_state.active_view_id == user_view.id;
            const ImGuiTabItemFlags flags = panel.ShouldSelectTab(user_view.id)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(user_view.name.c_str(), nullptr, flags)) {
                if (!selected) {
                    panel.SetActiveView(user_view.id);
                }
                panel.AcknowledgeTabSelection(user_view.id);
                GroupingView effective_view = EffectiveUserGroupingView(user_view, catalog, identity);
                if (ImGui::BeginPopupContextItem("user_grouping_view_context")) {
                    if (ImGui::Selectable("Duplicate")) {
                        pending_duplicate = effective_view;
                    }
                    if (ImGui::Selectable("Rename")) {
                        pending_rename = user_view;
                    }
                    if (ImGui::Selectable("Delete")) {
                        pending_delete = user_view;
                    }
                    ImGui::EndPopup();
                }
                grouping_view_ui_.Render(panel, snapshot, effective_view, &user_view);
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
        panel.DuplicateUserGroupingView(*pending_duplicate);
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
                panel.RenameUserGroupingView(*renaming_grouping_view_id_, renaming_grouping_view_name_.data());
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
                panel.DeleteUserGroupingView(*deleting_grouping_view_id_);
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

    if (!catalog_grouping_view && user_state.grouping_views.empty()) {
        ImGui::TextDisabled("This catalog has no catalog grouping view.");
        if (ImGui::Button("+ New grouping view")) {
            create_new_view();
        }
    }

    ImGui::End();
}

}  // namespace specforge
