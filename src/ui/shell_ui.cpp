#include "ui/shell_ui.h"

#include "domain/spectrum_fixture.h"
#include "domain/spectrum_loader.h"
#include "plot/spectrum_plot.h"

#include <Windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kDockHostWindow = "SpecForge Dock Host";
constexpr const char* kMainPlotWindow = "Spectrum";
constexpr const char* kFilesWindow = "Files";
constexpr const char* kInfoTagsWindow = "Info & Tags";
constexpr const char* kSpectralLinesWindow = "Spectral Lines";
constexpr float kStatusBarHeight = 28.0f;
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);

std::string NarrowPath(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    const auto match = std::find_if(metadata.begin(), metadata.end(), [key](const SpectrumMetadataEntry& entry) {
        return entry.key == key;
    });
    return match == metadata.end() ? std::string_view{} : std::string_view(match->value);
}

std::string_view SeverityLabel(SpectrumDiagnosticSeverity severity)
{
    switch (severity) {
    case SpectrumDiagnosticSeverity::Info:
        return "info";
    case SpectrumDiagnosticSeverity::Warning:
        return "warning";
    case SpectrumDiagnosticSeverity::Error:
        return "error";
    default:
        return "unknown";
    }
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

float RelativeLuminance(const ImVec4& color)
{
    return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

ImVec4 BlendColor(const ImVec4& color, const ImVec4& target, float amount)
{
    return ImVec4(
        color.x + (target.x - color.x) * amount,
        color.y + (target.y - color.y) * amount,
        color.z + (target.z - color.z) * amount,
        1.0f);
}

ImVec4 AdjustForDarkPlot(const ImVec4& color)
{
    if (RelativeLuminance(color) < 0.30f) {
        return BlendColor(color, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.42f);
    }
    return color;
}

std::optional<ImVec4> WindowsAccentColor()
{
    DWORD colorization_color = 0;
    BOOL opaque_blend = FALSE;
    if (FAILED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
        return std::nullopt;
    }

    const float red = static_cast<float>((colorization_color >> 16U) & 0xffU) / 255.0f;
    const float green = static_cast<float>((colorization_color >> 8U) & 0xffU) / 255.0f;
    const float blue = static_cast<float>(colorization_color & 0xffU) / 255.0f;
    return AdjustForDarkPlot(ImVec4(red, green, blue, 1.0f));
}

SpectrumPlotStyle ReadSystemSpectrumPlotStyle()
{
    SpectrumPlotStyle style;
    style.line_color = WindowsAccentColor().value_or(kFallbackSpectrumLineColor);
    return style;
}

std::string_view DiagnosticCodeLabel(SpectrumDiagnosticCode code)
{
    switch (code) {
    case SpectrumDiagnosticCode::OpenFailed:
        return "open_failed";
    case SpectrumDiagnosticCode::UnsupportedFormat:
        return "unsupported_format";
    case SpectrumDiagnosticCode::CatalogNotSpectrum:
        return "catalog_not_spectrum";
    case SpectrumDiagnosticCode::EmptyData:
        return "empty_data";
    case SpectrumDiagnosticCode::InvalidShape:
        return "invalid_shape";
    case SpectrumDiagnosticCode::MissingWavelength:
        return "missing_wavelength";
    case SpectrumDiagnosticCode::WavelengthFluxSizeMismatch:
        return "wavelength_flux_size_mismatch";
    case SpectrumDiagnosticCode::NoValidPixels:
        return "no_valid_pixels";
    case SpectrumDiagnosticCode::NonFiniteValuesFiltered:
        return "non_finite_values_filtered";
    case SpectrumDiagnosticCode::MaskFilteredPixels:
        return "mask_filtered_pixels";
    case SpectrumDiagnosticCode::IvarFilteredPixels:
        return "ivar_filtered_pixels";
    case SpectrumDiagnosticCode::AxisFrameUnknown:
        return "axis_frame_unknown";
    case SpectrumDiagnosticCode::RestFrameNotApplied:
        return "rest_frame_not_applied";
    case SpectrumDiagnosticCode::UnsupportedAxisForSpectralLines:
        return "unsupported_axis_for_spectral_lines";
    case SpectrumDiagnosticCode::None:
    default:
        return "none";
    }
}

bool HasDiagnosticAtLeast(const SpectrumSnapshotHandle& snapshot, SpectrumDiagnosticSeverity minimum)
{
    if (!snapshot) {
        return false;
    }
    const auto rank = [](SpectrumDiagnosticSeverity severity) {
        switch (severity) {
        case SpectrumDiagnosticSeverity::Error:
            return 2;
        case SpectrumDiagnosticSeverity::Warning:
            return 1;
        case SpectrumDiagnosticSeverity::Info:
        default:
            return 0;
        }
    };
    const int minimum_rank = rank(minimum);
    return std::any_of(snapshot->diagnostics.begin(), snapshot->diagnostics.end(), [rank, minimum_rank](const auto& d) {
        return rank(d.severity) >= minimum_rank;
    });
}

std::string SourceStateLabel(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "none";
    }
    if (HasDiagnosticAtLeast(snapshot, SpectrumDiagnosticSeverity::Error)) {
        return "error";
    }
    if (snapshot->capabilities.can_plot_current_spectrum) {
        return snapshot->diagnostics.empty() ? "loaded" : "loaded with diagnostics";
    }
    return "not plottable";
}

std::optional<std::filesystem::path> ShowOpenNpyDialog()
{
    std::array<wchar_t, 32768> filename = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"NumPy arrays (*.npy)\0*.npy\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrTitle = L"Open spectrum matrix";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::filesystem::path(filename.data());
}

void RenderDiagnosticRows(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot || snapshot->diagnostics.empty()) {
        ImGui::TextDisabled("No diagnostics");
        return;
    }

    for (const SpectrumDiagnostic& diagnostic : snapshot->diagnostics) {
        const std::string severity(SeverityLabel(diagnostic.severity));
        const std::string code(DiagnosticCodeLabel(diagnostic.code));
        ImGui::TextColored(SeverityColor(diagnostic.severity), "%s", severity.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", code.c_str());
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
        for (const SpectrumMetadataEntry& entry : diagnostic.metadata) {
            ImGui::BulletText("%s: %s", entry.key.c_str(), entry.value.c_str());
        }
    }
}

}  // namespace

ShellUi::ShellUi() : snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
    RefreshSystemColors();
}

void ShellUi::Render(const ShellStatus& status)
{
    RenderDockHost(status);
    RenderFilesPanel();
    RenderInfoTagsPanel();
    RenderMainPlot(status);
    RenderSpectralLinesPanel();
}

void ShellUi::RefreshSystemColors()
{
    plot_style_ = ReadSystemSpectrumPlotStyle();
}

void ShellUi::OpenSource(const std::filesystem::path& path, std::size_t spectrum_index)
{
    SetSnapshot(LoadSpectrumSnapshotFromPath(path, spectrum_index));
}

SpectrumSnapshotHandle ShellUi::current_snapshot() const
{
    return snapshot_;
}

void ShellUi::SetSnapshot(SpectrumSnapshotHandle snapshot)
{
    snapshot_ = std::move(snapshot);
    plot_state_ = SpectrumPlotState{};
}

void ShellUi::SwitchSpectrum(int direction)
{
    if (!snapshot_ || snapshot_->source.path.empty() || !snapshot_->capabilities.can_switch_spectrum) {
        return;
    }

    const std::size_t current_index = snapshot_->collection.current_index;
    if (direction < 0) {
        if (current_index == 0) {
            return;
        }
        OpenSource(snapshot_->source.path, current_index - 1);
        return;
    }

    const std::size_t next_index = current_index + 1;
    if (next_index >= snapshot_->collection.spectrum_count) {
        return;
    }
    OpenSource(snapshot_->source.path, next_index);
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

    if (ImGui::Button("Open...")) {
        if (std::optional<std::filesystem::path> path = ShowOpenNpyDialog()) {
            OpenSource(*path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
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
        const std::string source_name = snapshot_ ? snapshot_->source.display_name : std::string("none");
        ImGui::TextUnformatted(source_name.c_str());
        if (snapshot_ && !snapshot_->source.path.empty() && ImGui::IsItemHovered()) {
            const std::string path = NarrowPath(snapshot_->source.path);
            ImGui::SetTooltip("%s", path.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        const std::string source_type(
            snapshot_ ? MetadataValue(snapshot_->source.metadata, "source_type") : std::string_view{});
        ImGui::TextUnformatted(source_type.empty() ? "unknown" : source_type.c_str());
        ImGui::TableSetColumnIndex(2);
        const std::string state = SourceStateLabel(snapshot_);
        ImGui::TextUnformatted(state.c_str());

        ImGui::EndTable();
    }

    ImGui::End();
}

void ShellUi::RenderInfoTagsPanel()
{
    ImGui::Begin(kInfoTagsWindow);

    ImGui::TextUnformatted("Information");
    ImGui::Separator();
    if (snapshot_) {
        const CurrentSpectrumSnapshot& current = snapshot_->current_spectrum;
        ImGui::Text("Name: %s", current.name.empty() ? "(none)" : current.name.c_str());
        ImGui::Text("Points: %zu", current.point_count);
        ImGui::Text("X: %s", snapshot_->axis.x_label.empty() ? "unknown" : snapshot_->axis.x_label.c_str());
        ImGui::Text("Y: %s", snapshot_->axis.y_label.empty() ? "unknown" : snapshot_->axis.y_label.c_str());
        if (snapshot_->collection.spectrum_count > 0) {
            ImGui::Text(
                "Spectrum: %zu / %zu",
                snapshot_->collection.current_index + 1,
                snapshot_->collection.spectrum_count);
        }

        const bool can_previous = snapshot_->collection.can_move_previous;
        const bool can_next = snapshot_->collection.can_move_next;
        if (!can_previous) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Previous")) {
            SwitchSpectrum(-1);
        }
        if (!can_previous) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!can_next) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Next")) {
            SwitchSpectrum(1);
        }
        if (!can_next) {
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextDisabled("No snapshot");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Tags");
    ImGui::Separator();
    if (snapshot_) {
        bool first_tag = true;
        for (const SpectrumMetadataEntry& entry : snapshot_->source.metadata) {
            if (entry.key != "source_type" && entry.key != "format") {
                continue;
            }
            if (!first_tag) {
                ImGui::SameLine();
            }
            first_tag = false;
            ImGui::SmallButton(entry.value.c_str());
        }
        if (first_tag) {
            ImGui::TextDisabled("No tags");
        }
    }
    ImGui::BeginDisabled();
    ImGui::SameLine();
    ImGui::SmallButton("+ tag");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Fit view")) {
        plot_state_.fit_next_frame = true;
    }
    ImGui::Checkbox("Show points", &plot_state_.show_points);

    ImGui::Spacing();
    ImGui::TextUnformatted("Diagnostics");
    ImGui::Separator();
    RenderDiagnosticRows(snapshot_);

    ImGui::End();
}

void ShellUi::RenderMainPlot(const ShellStatus& status)
{
    ImGui::Begin(kMainPlotWindow);
    RenderSpectrumPlot(
        snapshot_,
        plot_state_,
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        plot_style_);
    ImGui::End();
}

void ShellUi::RenderSpectralLinesPanel()
{
    ImGui::Begin(kSpectralLinesWindow);
    ImGui::TextUnformatted("Spectral Lines");
    ImGui::Separator();

    const bool can_show_lines = snapshot_ && snapshot_->capabilities.can_show_spectral_lines;
    if (!can_show_lines) {
        ImGui::TextWrapped("Current snapshot does not expose a wavelength axis for spectral-line overlays.");
    } else if (snapshot_->capabilities.requires_rest_frame_warning) {
        ImGui::TextColored(
            SeverityColor(SpectrumDiagnosticSeverity::Warning),
            "Wavelength frame is unknown; rest-frame overlays are reference-only.");
    }

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
    if (!can_show_lines) {
        ImGui::BeginDisabled();
    }
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
    if (!can_show_lines) {
        ImGui::EndDisabled();
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
