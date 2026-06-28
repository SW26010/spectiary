#include "ui/shell_ui.h"

#include "domain/spectrum_loader.h"
#include "plot/spectrum_plot.h"

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kDockHostWindow = "SpecForge Dock Host###SpecForgeDockHostV2";
constexpr const char* kMainPlotWindow = "Spectrum###SpecForgeSpectrumV2";
constexpr const char* kInfoTagsWindow = "Info###SpecForgeInfoTagsV2";
constexpr const char* kSmoothingWindow = "Smoothing###SpecForgeSmoothingV1";
constexpr float kStatusBarSeparatorThickness = 1.0f;
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

float StatusBarHeight()
{
    return kStatusBarSeparatorThickness + ImGui::GetFrameHeight();
}

void RenderStatusBar(const ShellStatus& status, const ImVec2& size)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(
        min,
        ImVec2(max.x, min.y),
        ImGui::GetColorU32(ImGuiCol_Separator),
        kStatusBarSeparatorThickness);

    const float text_y = min.y + kStatusBarSeparatorThickness +
                         std::max(
                             0.0f,
                             (size.y - kStatusBarSeparatorThickness - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(min.x + style.FramePadding.x, text_y));
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

int SmoothingMethodIndex(SpectrumSmoothingMethod method)
{
    switch (method) {
    case SpectrumSmoothingMethod::Gaussian:
        return 1;
    case SpectrumSmoothingMethod::Median:
        return 2;
    case SpectrumSmoothingMethod::None:
    default:
        return 0;
    }
}

SpectrumSmoothingMethod SmoothingMethodFromIndex(int index)
{
    switch (index) {
    case 1:
        return SpectrumSmoothingMethod::Gaussian;
    case 2:
        return SpectrumSmoothingMethod::Median;
    case 0:
    default:
        return SpectrumSmoothingMethod::None;
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

bool CreateOpenDialog(ComPtr<IFileOpenDialog>& dialog)
{
    return SUCCEEDED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf())));
}

bool CreateSaveDialog(ComPtr<IFileSaveDialog>& dialog)
{
    return SUCCEEDED(CoCreateInstance(
            CLSID_FileSaveDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf())));
}

std::optional<std::filesystem::path> DialogResultPath(IFileDialog* dialog)
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

std::optional<std::filesystem::path> ShowAnnotationFilePicker()
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

    static constexpr COMDLG_FILTERSPEC kAnnotationFilters[] = {
        {L"NumPy annotation arrays", L"*.npy"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Open annotation file");
    dialog->SetFileTypes(
        static_cast<UINT>(sizeof(kAnnotationFilters) / sizeof(kAnnotationFilters[0])),
        kAnnotationFilters);
    dialog->SetFileTypeIndex(1);

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowLabelOutputFilePicker()
{
    ScopedComInitialization com;
    if (!com.ready()) {
        return std::nullopt;
    }

    ComPtr<IFileSaveDialog> dialog;
    if (!CreateSaveDialog(dialog)) {
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
    }

    static constexpr COMDLG_FILTERSPEC kLabelOutputFilters[] = {
        {L"NumPy label arrays", L"*.npy"},
        {L"All files", L"*.*"},
    };
    dialog->SetTitle(L"Choose label output");
    dialog->SetFileTypes(
        static_cast<UINT>(sizeof(kLabelOutputFilters) / sizeof(kLabelOutputFilters[0])),
        kLabelOutputFilters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"npy");

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
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

ShellUi::ShellUi()
    : session_(LoadSpectrumSnapshotFromPath)
{
    RefreshSystemColors();
    const SourceCollectionSessionView& restored_view = SessionView();
    if (restored_view.navigation.has_active_source) {
        source_collection_panel_ui_.SyncNavigationInputs(
            restored_view,
            [this](SourceCollectionSessionIntent intent) {
                return SubmitSessionCommandForPanel(std::move(intent));
            });
    }
}

ShellUi::~ShellUi()
{
    (void)session_.FlushStateCaches();
    spectral_lines_panel_.FlushCache();
}

void ShellUi::Render(const ShellStatus& status)
{
    session_view_cache_.reset();
    session_view_cache_dirty_ = false;
    label_shortcut_context_active_ = false;
    spectral_lines_panel_.SetFrameIndex(status.frame_index);
    RenderDockHost(status);
    if (panel_visibility_.files) {
        RenderFilesPanel();
    }
    if (panel_visibility_.navigation) {
        RenderNavigationPanel();
    }
    if (panel_visibility_.annotations) {
        RenderAnnotationsPanel();
    }
    if (panel_visibility_.smoothing) {
        RenderSmoothingPanel();
    }
    RenderMainPlot(status);
    if (panel_visibility_.labeling) {
        RenderLabelingPanel();
    }
    if (panel_visibility_.filters) {
        RenderFiltersPanel();
    }
    if (panel_visibility_.info) {
        RenderInfoTagsPanel();
    }
    if (panel_visibility_.spectral_lines) {
        RenderSpectralLinesPanel();
    }
    session_.MaybeSaveStateCaches(status.frame_index);
    spectral_lines_panel_.MaybeSaveCache(status.frame_index);
    session_view_cache_.reset();
    session_view_cache_dirty_ = false;
}

void ShellUi::RefreshSystemColors()
{
    spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetPlotStyle(ReadSystemSpectrumPlotStyle()));
}

void ShellUi::OpenSource(const std::filesystem::path& path, std::size_t spectrum_index)
{
    (void)SubmitSessionCommand(
        SourceCollectionSessionIntent::EditSourceCollection(SourceCollectionIntent::Open(path, spectrum_index)));
}

SpectrumSnapshotHandle ShellUi::current_snapshot() const
{
    return session_.View().snapshot;
}

void ShellUi::OpenSourceFromFilePicker()
{
    if (std::optional<std::filesystem::path> path = ShowSourceFilePicker()) {
        OpenSource(*path);
    }
}

void ShellUi::OpenSourceFromFolderPicker()
{
    if (std::optional<std::filesystem::path> path = ShowSourceFolderPicker()) {
        OpenSource(*path);
    }
}

void ShellUi::OpenAnnotationFromFilePicker()
{
    if (std::optional<std::filesystem::path> path = ShowAnnotationFilePicker()) {
        SourceCollectionSessionResult result =
            SubmitSessionCommand(SourceCollectionSessionIntent::EditSourceCollection(
                SourceCollectionIntent::AddReadOnlyAnnotationResult(*path)));
        if (result.loaded) {
            panel_visibility_.annotations = true;
        }
    }
}

const SourceCollectionSessionView& ShellUi::SessionView()
{
    if (!session_view_cache_ || session_view_cache_dirty_) {
        session_view_cache_ = session_.View();
        session_view_cache_dirty_ = false;
    }
    return *session_view_cache_;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommand(SourceCollectionSessionIntent command)
{
    SourceCollectionSessionResult result = session_.Submit(std::move(command));
    session_view_cache_dirty_ = true;
    HandleSessionAction(result.action);
    return result;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommandForPanel(SourceCollectionSessionIntent command)
{
    SourceCollectionSessionResult result = session_.Submit(std::move(command));
    session_view_cache_dirty_ = true;
    return result;
}

void ShellUi::HandleSessionAction(const SourceCollectionSessionAction& action)
{
    if (action.snapshot_changed) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::ResetForSnapshotChange());
    }
    if (action.workflow_changed) {
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
    }
    if (action.navigation_inputs_changed) {
        source_collection_panel_ui_.SyncNavigationInputs(SessionView(), [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        });
    }
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
                                  ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(kDockHostWindow, nullptr, host_flags);
    ImGui::PopStyleVar(2);

    RenderMainMenuBar();

    const ImGuiID dockspace_id = ImGui::GetID("SpecForgeDockSpaceSampleNavigationV1");
    const ImVec2 content_origin = ImGui::GetCursorScreenPos();
    const ImVec2 content_size = ImGui::GetContentRegionAvail();
    const float status_bar_height = StatusBarHeight();
    ImVec2 dockspace_size(content_size.x, std::max(0.0f, content_size.y - status_bar_height));

    if (!layout_seeded_) {
        layout_seeded_ = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            SeedInitialDockLayout(dockspace_id, dockspace_size);
        }
    }

    ImGui::DockSpace(dockspace_id, dockspace_size, ImGuiDockNodeFlags_None);
    ImGui::SetCursorScreenPos(ImVec2(content_origin.x, content_origin.y + dockspace_size.y));
    RenderStatusBar(status, ImVec2(content_size.x, status_bar_height));

    ImGui::End();
}

void ShellUi::RenderMainMenuBar()
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open File...")) {
            OpenSourceFromFilePicker();
        }
        if (ImGui::MenuItem("Open Folder...")) {
            OpenSourceFromFolderPicker();
        }
        const bool can_open_annotation = SessionView().can_add_read_only_annotation;
        if (!can_open_annotation) {
            ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Open File as Annotation...")) {
            OpenAnnotationFromFilePicker();
        }
        if (!can_open_annotation) {
            ImGui::EndDisabled();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Show all panels")) {
            panel_visibility_.files = true;
            panel_visibility_.navigation = true;
            panel_visibility_.annotations = true;
            panel_visibility_.labeling = true;
            panel_visibility_.filters = true;
            panel_visibility_.smoothing = true;
            panel_visibility_.info = true;
            panel_visibility_.spectral_lines = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("Files", nullptr, &panel_visibility_.files);
        ImGui::MenuItem("Navigation", nullptr, &panel_visibility_.navigation);
        ImGui::MenuItem("Annotations", nullptr, &panel_visibility_.annotations);
        ImGui::MenuItem("Labeling", nullptr, &panel_visibility_.labeling);
        ImGui::MenuItem("Filters", nullptr, &panel_visibility_.filters);
        ImGui::MenuItem("Smoothing", nullptr, &panel_visibility_.smoothing);
        ImGui::MenuItem("Information", nullptr, &panel_visibility_.info);
        ImGui::MenuItem("Spectral Lines", nullptr, &panel_visibility_.spectral_lines);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void ShellUi::RenderFilesPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(source_collection_panel_ui_.RenderFiles(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        &panel_visibility_.files,
        []() {
            return ShowSourceFilePicker();
        },
        []() {
            return ShowSourceFolderPicker();
        }));
}

void ShellUi::RenderNavigationPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(source_collection_panel_ui_.RenderNavigation(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        &panel_visibility_.navigation));
}

void ShellUi::RenderAnnotationsPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(source_collection_panel_ui_.RenderAnnotations(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        &panel_visibility_.annotations,
        []() {
            return ShowAnnotationFilePicker();
        }));
}

void ShellUi::RenderLabelingPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(sample_workflow_panel_ui_.RenderLabeling(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        label_shortcut_context_active_,
        &panel_visibility_.labeling,
        []() {
            return ShowLabelOutputFilePicker();
        }));
}

void ShellUi::RenderFiltersPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(sample_workflow_panel_ui_.RenderFilters(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        &panel_visibility_.filters));
}

void ShellUi::RenderSmoothingPanel()
{
    if (!ImGui::Begin(kSmoothingWindow, &panel_visibility_.smoothing)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Smoothing");
    ImGui::Separator();

    const SpectrumSnapshotHandle snapshot = SessionView().snapshot;
    if (!snapshot || !snapshot->capabilities.can_plot_current_spectrum) {
        ImGui::TextDisabled("No plottable spectrum");
        ImGui::End();
        return;
    }

    SpectrumViewSessionView view = spectrum_view_session_.View();
    bool show_smoothed = view.show_smoothed;
    if (ImGui::Checkbox("Show smoothed curve", &show_smoothed)) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetShowSmoothed(show_smoothed));
        view = spectrum_view_session_.View();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::ResetSmoothing());
        view = spectrum_view_session_.View();
    }

    static constexpr const char* kMethodLabels[] = {"None", "Gaussian", "Median"};
    int method_index = SmoothingMethodIndex(view.smoothing.method);
    if (ImGui::Combo("Method", &method_index, kMethodLabels, IM_ARRAYSIZE(kMethodLabels))) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetSmoothingMethod(SmoothingMethodFromIndex(method_index)));
        view = spectrum_view_session_.View();
    }

    if (view.smoothing.method == SpectrumSmoothingMethod::Gaussian) {
        float sigma = static_cast<float>(view.smoothing.gaussian_sigma);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::DragFloat("Sigma", &sigma, 0.05f, 0.01f, 100.0f, "%.2f")) {
            spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetGaussianSigma(static_cast<double>(sigma)));
            view = spectrum_view_session_.View();
        }
    } else if (view.smoothing.method == SpectrumSmoothingMethod::Median) {
        int kernel_size = view.smoothing.median_kernel_size;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Kernel size", &kernel_size, 2, 10)) {
            spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetMedianKernelSize(kernel_size));
            view = spectrum_view_session_.View();
        }
        const int effective_kernel_size =
            spectrum_view_session_.EffectiveMedianKernelSize(snapshot->current_spectrum.point_count);
        if (effective_kernel_size != view.smoothing.median_kernel_size) {
            ImGui::TextDisabled("Effective kernel: %d", effective_kernel_size);
        }
    }

    if (view.smoothing.method == SpectrumSmoothingMethod::None && view.show_smoothed) {
        ImGui::TextDisabled("No smoothing method selected");
    }

    if (!view.smoothing_active) {
        ImGui::BeginDisabled();
    }
    bool show_raw_when_smoothed = view.show_raw_when_smoothed;
    if (ImGui::Checkbox("Show raw overlay", &show_raw_when_smoothed)) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetShowRawWhenSmoothed(show_raw_when_smoothed));
    }
    if (!view.smoothing_active) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void ShellUi::RenderInfoTagsPanel()
{
    if (!ImGui::Begin(kInfoTagsWindow, &panel_visibility_.info)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Information");
    ImGui::Separator();
    const SpectrumSnapshotHandle snapshot = SessionView().snapshot;
    if (snapshot) {
        const CurrentSpectrumSnapshot& current = snapshot->current_spectrum;
        ImGui::Text("Name: %s", current.name.empty() ? "(none)" : current.name.c_str());
        ImGui::Text("Points: %zu", current.point_count);
        ImGui::Text("X: %s", snapshot->axis.x_label.empty() ? "unknown" : snapshot->axis.x_label.c_str());
        ImGui::Text("Y: %s", snapshot->axis.y_label.empty() ? "unknown" : snapshot->axis.y_label.c_str());
        RenderMetadataLine("Wavelength medium", MetadataValue(snapshot->source.metadata, "wavelength_medium"));
        RenderMetadataLine(
            "Observer correction",
            MetadataValue(snapshot->source.metadata, "observer_frame_correction"));
        RenderMetadataLine(
            "Radial velocity",
            MetadataValue(snapshot->source.metadata, "radial_velocity_km_s"),
            "km/s");
        RenderMetadataLine("RV source", MetadataValue(snapshot->source.metadata, "radial_velocity_source"));
        RenderMetadataLine("Redshift", MetadataValue(snapshot->source.metadata, "redshift"));
        RenderMetadataLine("Redshift warning", MetadataValue(snapshot->source.metadata, "redshift_warning"));
        RenderMetadataLine("Target z", MetadataValue(snapshot->source.metadata, "target_redshift"));
        RenderMetadataLine("Target z source", MetadataValue(snapshot->source.metadata, "target_redshift_source"));
        RenderMetadataLine("Target z status", MetadataValue(snapshot->source.metadata, "target_redshift_status"));
        RenderMetadataLine("Target z warning", MetadataValue(snapshot->source.metadata, "target_redshift_warning"));
        RenderMetadataLine(
            "Heliocentric correction",
            MetadataValue(snapshot->source.metadata, "heliocentric_correction_km_s"),
            "km/s");
        RenderMetadataLine(
            "Target rest frame",
            MetadataValue(snapshot->source.metadata, "target_rest_frame_status"));
        RenderMetadataLine(
            "Rest-frame correction",
            MetadataValue(snapshot->source.metadata, "rest_frame_correction_status"));
    } else {
        ImGui::TextDisabled("No snapshot");
    }

    ImGui::Spacing();
    if (ImGui::Button("Fit view")) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::RequestFitView());
    }
    bool show_points = spectrum_view_session_.View().show_points;
    if (ImGui::Checkbox("Show points", &show_points)) {
        spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetShowPoints(show_points));
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Diagnostics");
    ImGui::Separator();
    RenderDiagnosticRows(snapshot);

    ImGui::End();
}

void ShellUi::RenderMainPlot(const ShellStatus& status)
{
    ImGui::Begin(kMainPlotWindow);
    label_shortcut_context_active_ =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const SpectrumSnapshotHandle snapshot = SessionView().snapshot;
    const std::vector<const SpectralLineMarker*> spectral_lines =
        spectral_lines_panel_.FilteredMarkers(snapshot, false);
    RenderSpectrumPlot(
        snapshot,
        spectrum_view_session_.PlotStateForRender(),
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        spectrum_view_session_.PlotStyleForRender(),
        SpectrumPlotOverlays{spectral_lines.data(), spectral_lines.size(), spectral_lines_panel_.show_labels()});
    ImGui::End();
}

void ShellUi::RenderSpectralLinesPanel()
{
    spectral_lines_panel_ui_.Render(spectral_lines_panel_, SessionView().snapshot, &panel_visibility_.spectral_lines);
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
    ImGuiID smoothing_id = 0;
    ImGuiID info_group_id = 0;
    ImGuiID navigation_id = 0;
    ImGuiID left_lower_id = 0;
    ImGuiID annotations_id = 0;
    ImGuiID labeling_id = 0;
    ImGuiID filters_id = 0;
    ImGuiID right_upper_id = 0;
    ImGuiID spectral_lines_id = 0;

    ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.24f, &left_id, &center_id);
    ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, &right_id, &center_id);
    ImGui::DockBuilderSplitNode(left_id, ImGuiDir_Down, 0.66f, &left_lower_id, &navigation_id);
    ImGui::DockBuilderSplitNode(left_lower_id, ImGuiDir_Down, 0.50f, &info_group_id, &files_id);
    ImGui::DockBuilderSplitNode(info_group_id, ImGuiDir_Down, 0.42f, &smoothing_id, &info_tags_id);
    ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.38f, &right_upper_id, &spectral_lines_id);
    ImGui::DockBuilderSplitNode(right_upper_id, ImGuiDir_Down, 0.50f, &labeling_id, &annotations_id);
    ImGui::DockBuilderSplitNode(labeling_id, ImGuiDir_Down, 0.50f, &filters_id, &labeling_id);

    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::FilesWindowName(), files_id);
    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::NavigationWindowName(), navigation_id);
    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::AnnotationsWindowName(), annotations_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::LabelingWindowName(), labeling_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::FiltersWindowName(), filters_id);
    ImGui::DockBuilderDockWindow(kInfoTagsWindow, info_tags_id);
    ImGui::DockBuilderDockWindow(kSmoothingWindow, smoothing_id);
    ImGui::DockBuilderDockWindow(kMainPlotWindow, center_id);
    ImGui::DockBuilderDockWindow(SpectralLinesPanelUi::WindowName(), spectral_lines_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

}  // namespace specforge
