#include "ui/shell_ui.h"

#include "domain/spectrum_fixture.h"
#include "domain/spectrum_loader.h"
#include "plot/spectrum_plot.h"

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kDockHostWindow = "SpecForge Dock Host";
constexpr const char* kMainPlotWindow = "Spectrum";
constexpr const char* kFilesWindow = "Files";
constexpr const char* kInfoTagsWindow = "Info & Tags";
constexpr const char* kSpectralLinesWindow = "Spectral Lines";
constexpr float kStatusBarHeight = 28.0f;
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);

class ScopedComInitialization {
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)),
          uninitialize_(SUCCEEDED(result_))
    {
    }

    ~ScopedComInitialization()
    {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool ready() const
    {
        return SUCCEEDED(result_);
    }

private:
    HRESULT result_ = E_FAIL;
    bool uninitialize_ = false;
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string NarrowPath(const std::filesystem::path& path)
{
    return PathToUtf8(path);
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string SourceKey(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, error);
    return LowerAscii(PathToUtf8(error ? path : absolute_path));
}

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        if (metadata[index].key == key) {
            return metadata[index].value;
        }
    }
    return {};
}

std::string_view MetadataDisplayValue(std::string_view value)
{
    if (value == "not_applied") {
        return "not applied";
    }
    if (value == "available_not_applied") {
        return "available, not applied";
    }
    if (value == "unreliable_not_applied") {
        return "unreliable, not applied";
    }
    if (value == "radial_velocity_low_speed") {
        return "RV / c low-speed approximation";
    }
    if (value == "pipeline_redshift") {
        return "pipeline redshift";
    }
    if (value == "zwarning_nonzero") {
        return "ZWARNING nonzero";
    }
    if (value == "invalid_pipeline_redshift") {
        return "invalid pipeline redshift";
    }
    return value;
}

void RenderMetadataLine(const char* label, std::string_view value, std::string_view suffix = {})
{
    if (value.empty()) {
        return;
    }
    value = MetadataDisplayValue(value);
    if (suffix.empty()) {
        ImGui::Text("%s: %.*s", label, static_cast<int>(value.size()), value.data());
    } else {
        ImGui::Text(
            "%s: %.*s %.*s",
            label,
            static_cast<int>(value.size()),
            value.data(),
            static_cast<int>(suffix.size()),
            suffix.data());
    }
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

std::string_view SourceStateLabel(const SpectrumSnapshotHandle& snapshot)
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

std::string SnapshotDisplayNameText(const SpectrumSnapshotHandle& snapshot, const std::filesystem::path& path)
{
    if (snapshot && !snapshot->source.display_name.empty()) {
        return snapshot->source.display_name;
    }
    return FileNameToUtf8(path);
}

std::string SnapshotTypeLabelText(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "unknown";
    }

    const std::string_view format = MetadataValue(snapshot->source.metadata, "format");
    if (!format.empty()) {
        return std::string{format};
    }

    const std::string_view source_type = MetadataValue(snapshot->source.metadata, "source_type");
    return source_type.empty() ? std::string{"unknown"} : std::string{source_type};
}

std::string SnapshotStateLabelText(const SpectrumSnapshotHandle& snapshot)
{
    return std::string{SourceStateLabel(snapshot)};
}

bool CreateOpenDialog(ComPtr<IFileOpenDialog>& dialog)
{
    return SUCCEEDED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf())));
}

std::optional<std::filesystem::path> DialogResultPath(IFileOpenDialog* dialog)
{
    if (dialog == nullptr) {
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) {
        return std::nullopt;
    }

    PWSTR filesystem_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &filesystem_path)) || filesystem_path == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path path(filesystem_path);
    CoTaskMemFree(filesystem_path);
    return path;
}

std::optional<std::filesystem::path> ShowSourceFilePicker()
{
    ScopedComInitialization com;
    if (!com.ready()) {
        return std::nullopt;
    }

    ComPtr<IFileOpenDialog> dialog;
    if (!CreateOpenDialog(dialog)) {
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
    }

    static constexpr COMDLG_FILTERSPEC kSourceFilters[] = {
        {L"Spectrum sources", L"*.npy;*.csv;*.fits;*.fit;*.fts;*.fits.gz"},
        {L"NumPy arrays", L"*.npy"},
        {L"CSV files", L"*.csv"},
        {L"FITS files", L"*.fits;*.fit;*.fts;*.fits.gz"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Add source file");
    dialog->SetFileTypes(static_cast<UINT>(sizeof(kSourceFilters) / sizeof(kSourceFilters[0])), kSourceFilters);
    dialog->SetFileTypeIndex(1);

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowSourceFolderPicker()
{
    ScopedComInitialization com;
    if (!com.ready()) {
        return std::nullopt;
    }

    ComPtr<IFileOpenDialog> dialog;
    if (!CreateOpenDialog(dialog)) {
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_PICKFOLDERS;
        dialog->SetOptions(options);
    }
    dialog->SetTitle(L"Add source folder");

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
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

float TrashIconButtonWidth()
{
    return ImGui::GetFrameHeight() * 0.5f;
}

bool TrashIconButton(const char* id, const ImRect& hit_rect)
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

    const ImU32 icon_color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float icon_width = std::min(TrashIconButtonWidth(), width);
    const float icon_left = min.x + std::max(0.0f, (width - icon_width) * 0.5f);
    const float left = icon_left + icon_width * 0.14f;
    const float right = icon_left + icon_width * 0.86f;
    const float handle_left = icon_left + icon_width * 0.38f;
    const float handle_right = icon_left + icon_width * 0.62f;
    const float icon_top = min.y + std::max(0.0f, (max.y - min.y - height) * 0.5f);
    const float top = icon_top + height * 0.25f;
    const float lid_y = icon_top + height * 0.34f;
    const float body_top = icon_top + height * 0.43f;
    const float body_bottom = icon_top + height * 0.73f;
    const float stroke = 1.35f;

    draw_list->AddLine(ImVec2(handle_left, top), ImVec2(handle_right, top), icon_color, stroke);
    draw_list->AddLine(ImVec2(left, lid_y), ImVec2(right, lid_y), icon_color, stroke);
    draw_list->AddRect(ImVec2(left + icon_width * 0.05f, body_top), ImVec2(right - icon_width * 0.05f, body_bottom), icon_color, 2.0f, 0, stroke);
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

    if (hovered) {
        ImGui::SetTooltip("Remove from list");
    }
    return clicked;
}

void RenderDiagnosticRows(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot || snapshot->diagnostics.empty()) {
        ImGui::TextDisabled("No diagnostics");
        return;
    }

    for (const SpectrumDiagnostic& diagnostic : snapshot->diagnostics) {
        const std::string_view severity = SeverityLabel(diagnostic.severity);
        const std::string_view code = DiagnosticCodeLabel(diagnostic.code);
        ImGui::TextColored(
            SeverityColor(diagnostic.severity),
            "%.*s",
            static_cast<int>(severity.size()),
            severity.data());
        ImGui::SameLine();
        ImGui::TextDisabled("%.*s", static_cast<int>(code.size()), code.data());
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
    SpectrumSnapshotHandle loaded_snapshot = LoadSpectrumSnapshotFromPath(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot));
}

SpectrumSnapshotHandle ShellUi::current_snapshot() const
{
    return snapshot_;
}

std::size_t ShellUi::AddOrUpdateSource(
    const std::filesystem::path& path,
    SpectrumSnapshotHandle snapshot,
    std::size_t spectrum_index)
{
    const std::string key = SourceKey(path);
    const auto match = std::find_if(sources_.begin(), sources_.end(), [&key](const SourceListEntry& entry) {
        return entry.key == key;
    });
    if (match != sources_.end()) {
        match->path = path;
        match->display_name = SnapshotDisplayNameText(snapshot, path);
        match->type_label = SnapshotTypeLabelText(snapshot);
        match->state_label = SnapshotStateLabelText(snapshot);
        match->cached_snapshot = std::move(snapshot);
        match->last_spectrum_index = spectrum_index;
        return static_cast<std::size_t>(std::distance(sources_.begin(), match));
    }

    SourceListEntry entry;
    entry.path = path;
    entry.key = key;
    entry.display_name = SnapshotDisplayNameText(snapshot, path);
    entry.type_label = SnapshotTypeLabelText(snapshot);
    entry.state_label = SnapshotStateLabelText(snapshot);
    entry.cached_snapshot = std::move(snapshot);
    entry.last_spectrum_index = spectrum_index;
    sources_.push_back(std::move(entry));
    return sources_.size() - 1;
}

void ShellUi::ActivateSource(std::size_t source_index)
{
    if (source_index >= sources_.size()) {
        return;
    }

    SourceListEntry& entry = sources_[source_index];
    current_source_index_ = source_index;
    SetSnapshot(entry.cached_snapshot);
}

void ShellUi::RemoveSource(std::size_t source_index)
{
    if (source_index >= sources_.size()) {
        return;
    }

    const bool removed_current = current_source_index_ && *current_source_index_ == source_index;
    std::optional<std::size_t> next_current_index;
    if (removed_current && sources_.size() > 1) {
        next_current_index = source_index + 1 < sources_.size() ? source_index : source_index - 1;
    }

    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(source_index));

    if (removed_current) {
        current_source_index_.reset();
        if (next_current_index) {
            ActivateSource(*next_current_index);
        } else {
            SetSnapshot(MakeSmallSyntheticSpectrumSnapshot());
        }
        return;
    }

    if (current_source_index_ && *current_source_index_ > source_index) {
        current_source_index_ = *current_source_index_ - 1;
    }
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
    if (status.profile_open && status.profile_path != nullptr && ImGui::IsItemHovered()) {
        const std::string profile_path = NarrowPath(*status.profile_path);
        ImGui::SetTooltip("%s", profile_path.c_str());
    }

    ImGui::End();
}

void ShellUi::RenderFilesPanel()
{
    ImGui::Begin(kFilesWindow);

    ImGui::TextUnformatted("Files");
    ImGui::Separator();

    if (ImGui::Button("Add file...")) {
        if (std::optional<std::filesystem::path> path = ShowSourceFilePicker()) {
            OpenSource(*path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add folder...")) {
        if (std::optional<std::filesystem::path> path = ShowSourceFolderPicker()) {
            OpenSource(*path);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu source%s", sources_.size(), sources_.size() == 1 ? "" : "s");

    ImGui::Spacing();
    if (sources_.empty()) {
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
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            const SourceListEntry& entry = sources_[index];
            const bool is_current = current_source_index_ && *current_source_index_ == index;

            ImGui::TableNextRow();
            if (is_current) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(index));
            if (TableCellTextButton("source", entry.display_name, ImGui::GetColorU32(ImGuiCol_Text))) {
                ActivateSource(index);
            }
            if (ImGui::IsItemHovered()) {
                const std::string path = NarrowPath(entry.path);
                ImGui::SetTooltip("%s", path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (TableCellTextButton("type", entry.type_label, ImGui::GetColorU32(ImGuiCol_Text))) {
                ActivateSource(index);
            }

            ImGui::TableSetColumnIndex(2);
            ImU32 state_color = ImGui::GetColorU32(is_current ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (TableCellTextButton("state", entry.state_label, state_color)) {
                ActivateSource(index);
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
            RemoveSource(*source_to_remove);
        }
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
        RenderMetadataLine("Wavelength medium", MetadataValue(snapshot_->source.metadata, "wavelength_medium"));
        RenderMetadataLine(
            "Observer correction",
            MetadataValue(snapshot_->source.metadata, "observer_frame_correction"));
        RenderMetadataLine(
            "Radial velocity",
            MetadataValue(snapshot_->source.metadata, "radial_velocity_km_s"),
            "km/s");
        RenderMetadataLine("RV source", MetadataValue(snapshot_->source.metadata, "radial_velocity_source"));
        RenderMetadataLine("Redshift", MetadataValue(snapshot_->source.metadata, "redshift"));
        RenderMetadataLine("Redshift warning", MetadataValue(snapshot_->source.metadata, "redshift_warning"));
        RenderMetadataLine("Target z", MetadataValue(snapshot_->source.metadata, "target_redshift"));
        RenderMetadataLine("Target z source", MetadataValue(snapshot_->source.metadata, "target_redshift_source"));
        RenderMetadataLine("Target z status", MetadataValue(snapshot_->source.metadata, "target_redshift_status"));
        RenderMetadataLine("Target z warning", MetadataValue(snapshot_->source.metadata, "target_redshift_warning"));
        RenderMetadataLine(
            "Heliocentric correction",
            MetadataValue(snapshot_->source.metadata, "heliocentric_correction_km_s"),
            "km/s");
        RenderMetadataLine(
            "Target rest frame",
            MetadataValue(snapshot_->source.metadata, "target_rest_frame_status"));
        RenderMetadataLine(
            "Rest-frame correction",
            MetadataValue(snapshot_->source.metadata, "rest_frame_correction_status"));
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
        ImGui::TableSetupColumn("Vacuum Angstrom");
        ImGui::TableSetupColumn("Group");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("H alpha");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("6564.614");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("reference");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Na D");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("5891.6 / 5897.6");
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
