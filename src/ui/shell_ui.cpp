#include "ui/shell_ui.h"

#include "plot/spectrum_plot.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace specforge {
namespace {

constexpr const char* kDockHostWindow = "SpecForge Dock Host";
constexpr const char* kMainPlotWindow = "Spectrum";
constexpr const char* kFilesWindow = "Files";
constexpr const char* kInfoTagsWindow = "Info & Tags";
constexpr const char* kSpectralLinesWindow = "Spectral Lines";
constexpr float kStatusBarHeight = 28.0f;

std::string NarrowPath(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

}  // namespace

ShellUi::ShellUi() : spectrum_(MakeSmallSyntheticSpectrum())
{
}

void ShellUi::Render(const ShellStatus& status)
{
    RenderDockHost(status);
    RenderFilesPanel();
    RenderInfoTagsPanel();
    RenderMainPlot();
    RenderSpectralLinesPanel();
}

void ShellUi::RenderDockHost(const ShellStatus& status)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(kDockHostWindow, nullptr, host_flags);
    ImGui::PopStyleVar(2);

    const ImGuiID dockspace_id = ImGui::GetID("SpecForgeDockSpaceFourPaneV1");
    ImVec2 dockspace_size = ImGui::GetContentRegionAvail();
    dockspace_size.y = std::max(0.0f, dockspace_size.y - kStatusBarHeight);

    if (!layout_seeded_) {
        layout_seeded_ = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            SeedInitialDockLayout(dockspace_id, dockspace_size);
        }
    }

    ImGui::DockSpace(dockspace_id, dockspace_size, ImGuiDockNodeFlags_None);

    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
    ImGui::TextUnformatted("Ready");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Frame %llu", static_cast<unsigned long long>(status.frame_index));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("%ux%u", status.client_width, status.client_height);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted(status.profile_open ? "Profile active" : "Profile off");
    if (status.profile_open && ImGui::IsItemHovered()) {
        const std::string profile_path = NarrowPath(status.profile_path);
        ImGui::SetTooltip("%s", profile_path.c_str());
    }

    ImGui::End();
}

void ShellUi::RenderFilesPanel()
{
    ImGui::Begin(kFilesWindow);

    ImGui::TextUnformatted("Files");
    ImGui::Separator();

    ImGui::BeginDisabled();
    ImGui::Button("Open...");
    ImGui::SameLine();
    ImGui::Button("Recent");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::BeginTable("files_table", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("small synthetic fixture");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("spectrum");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("loaded");

        ImGui::EndTable();
    }

    ImGui::End();
}

void ShellUi::RenderInfoTagsPanel()
{
    ImGui::Begin(kInfoTagsWindow);

    ImGui::TextUnformatted("Information");
    ImGui::Separator();
    ImGui::Text("Name: %s", spectrum_.name.c_str());
    ImGui::Text("Points: %zu", std::min(spectrum_.wavelength.size(), spectrum_.flux.size()));
    ImGui::TextUnformatted("X: wavelength");
    ImGui::TextUnformatted("Y: flux");

    ImGui::Spacing();
    ImGui::TextUnformatted("Tags");
    ImGui::Separator();
    ImGui::SmallButton("synthetic");
    ImGui::SameLine();
    ImGui::SmallButton("fixture");
    ImGui::BeginDisabled();
    ImGui::SameLine();
    ImGui::SmallButton("+ tag");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Fit view")) {
        plot_state_.fit_next_frame = true;
    }
    ImGui::Checkbox("Show points", &plot_state_.show_points);

    ImGui::End();
}

void ShellUi::RenderMainPlot()
{
    ImGui::Begin(kMainPlotWindow);
    RenderSpectrumPlot(spectrum_, plot_state_);
    ImGui::End();
}

void ShellUi::RenderSpectralLinesPanel()
{
    ImGui::Begin(kSpectralLinesWindow);
    ImGui::TextUnformatted("Spectral Lines");
    ImGui::Separator();

    ImGui::BeginDisabled();
    bool public_lines = true;
    bool hidden_lines = false;
    ImGui::Checkbox("Public", &public_lines);
    ImGui::SameLine();
    ImGui::Checkbox("Hidden", &hidden_lines);
    const char* catalogs[] = {"No catalog loaded"};
    int catalog_index = 0;
    ImGui::Combo("Catalog", &catalog_index, catalogs, 1);
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::BeginTable("spectral_lines_table", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Line");
        ImGui::TableSetupColumn("Wavelength");
        ImGui::TableSetupColumn("Group");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("H alpha");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("6562.8");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("reference");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Na D");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("5892.0");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("reference");

        ImGui::EndTable();
    }

    ImGui::End();
}

void ShellUi::SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size)
{
    // DockBuilder is an internal docking-branch API, so keep it limited to the
    // first-layout seed. Runtime docking and persistence remain standard ImGui.
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID center_id = dockspace_id;
    ImGuiID left_id = 0;
    ImGuiID right_id = 0;
    ImGuiID files_id = 0;
    ImGuiID info_tags_id = 0;

    ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.24f, &left_id, &center_id);
    ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, &right_id, &center_id);
    ImGui::DockBuilderSplitNode(left_id, ImGuiDir_Down, 0.50f, &info_tags_id, &files_id);

    ImGui::DockBuilderDockWindow(kFilesWindow, files_id);
    ImGui::DockBuilderDockWindow(kInfoTagsWindow, info_tags_id);
    ImGui::DockBuilderDockWindow(kMainPlotWindow, center_id);
    ImGui::DockBuilderDockWindow(kSpectralLinesWindow, right_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

}  // namespace specforge
