#include "ui/shell_ui.h"

#include "domain/spectrum_loader.h"
#include "domain/source_path_identity.h"
#include "plot/spectrum_plot.h"
#include "ui/sample_workflow_shortcut.h"

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
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
constexpr const char* kImmersivePlotWindow = "Spectrum###SpecForgeSpectrumImmersiveV1";
constexpr const char* kInfoTagsWindow = "Info###SpecForgeInfoTagsV2";
constexpr const char* kSmoothingWindow = "Smoothing###SpecForgeSmoothingV1";
constexpr float kStatusBarSeparatorThickness = 1.0f;
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);

enum class ImmersivePlotAxisImplementation {
    NativeImPlot,
    CustomEdgeOverlay,
};

// Change this line to compare native ImPlot axes with the custom edge overlay.
constexpr ImmersivePlotAxisImplementation kImmersivePlotAxisImplementation =
    ImmersivePlotAxisImplementation::NativeImPlot;

SpectrumPlotDisplayOptions MakeImmersivePlotDisplayOptions()
{
    SpectrumPlotDisplayOptions display_options;
    display_options.include_edge_pixels = true;

    switch (kImmersivePlotAxisImplementation) {
    case ImmersivePlotAxisImplementation::NativeImPlot:
        display_options.native_transparent_axes = true;
        break;
    case ImmersivePlotAxisImplementation::CustomEdgeOverlay:
        display_options.edge_axis_overlay = true;
        break;
    }

    return display_options;
}

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

void RenderStatusBar(
    const ShellStatus& status,
    const ImVec2& size,
    bool source_load_active,
    std::string_view source_load_error)
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
    if (!source_load_error.empty()) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
            "Load failed: %.*s",
            static_cast<int>(source_load_error.size()),
            source_load_error.data());
    } else {
        ImGui::TextUnformatted(source_load_active ? "Loading source..." : "Ready");
    }
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
    dialog->SetTitle(L"Save labeling annotation");
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

ShellUi::ShellUi(PlotTouchpadGestureSource* touchpad_gestures)
    : session_(LoadSpectrumSnapshotFromPath, SourceCollectionSessionRestoreMode::Deferred),
      touchpad_gestures_(touchpad_gestures)
{
    panel_visibility_ = panel_visibility_state_.Load();
    RefreshSystemColors();
    BeginDeferredSourceRestore();
}

ShellUi::~ShellUi()
{
    (void)panel_visibility_state_.Flush(panel_visibility_);
    (void)session_.FlushStateCaches();
    (void)spectral_lines_panel_.Flush();
    if (session_view_cache_) {
        source_load_queue_.RetireResource(
            std::make_shared<SourceCollectionSessionView>(std::move(*session_view_cache_)));
        session_view_cache_.reset();
    }
    for (BackgroundRetirementHandle& resource : session_.ReleaseBackgroundResourcesForShutdown()) {
        source_load_queue_.RetireResource(std::move(resource));
    }
}

void ShellUi::Render(const ShellStatus& status)
{
    DrainSourceLoads();
    sample_workflow_shortcut_ = {};
    if (immersive_plot_mode_) {
        RenderImmersivePlot(status);
        HandleSampleWorkflowShortcut();
        return;
    }
    const PanelVisibilityState previous_panel_visibility = panel_visibility_;
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
    if (panel_visibility_.sorting) {
        RenderSortingPanel();
    }
    if (panel_visibility_.information) {
        RenderInfoTagsPanel();
    }
    if (panel_visibility_.spectral_lines) {
        RenderSpectralLinesPanel();
    }
    HandleSampleWorkflowShortcut();
    panel_visibility_state_.MarkDirtyIfChanged(
        previous_panel_visibility,
        panel_visibility_);
}

void ShellUi::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    DrainSourceLoads();
    source_load_service_deadline_ = source_load_queue_.NeedsService()
        ? std::optional<LocalUserStateSaveScheduler::TimePoint>{now + std::chrono::milliseconds(16)}
        : std::nullopt;
    panel_visibility_state_.RunMaintenance(panel_visibility_, now);
    session_.RunMaintenance(now);
    // Maintenance can change save-status fields exposed by the derived session view.
    session_view_cache_dirty_ = true;
    spectral_lines_panel_.RunMaintenance(now);
}

std::optional<LocalUserStateSaveScheduler::TimePoint> ShellUi::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        panel_visibility_state_.NextMaintenanceDeadline();
    const auto consider = [&deadline](std::optional<LocalUserStateSaveScheduler::TimePoint> candidate) {
        if (candidate && (!deadline || *candidate < *deadline)) {
            deadline = candidate;
        }
    };
    consider(session_.NextMaintenanceDeadline());
    consider(spectral_lines_panel_.NextMaintenanceDeadline());
    if (source_load_queue_.NeedsService()) {
        if (!source_load_service_deadline_) {
            source_load_service_deadline_ =
                LocalUserStateSaveScheduler::Clock::now() + std::chrono::milliseconds(16);
        }
        consider(source_load_service_deadline_);
    } else {
        source_load_service_deadline_.reset();
    }
    return deadline;
}

void ShellUi::RefreshSystemColors()
{
    spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetPlotStyle(ReadSystemSpectrumPlotStyle()));
}

void ShellUi::EnterImmersivePlotMode()
{
    spectrum_view_session_.Submit(SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    immersive_plot_mode_ = true;
}

void ShellUi::ExitImmersivePlotMode()
{
    spectrum_view_session_.Submit(SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    immersive_plot_mode_ = false;
}

bool ShellUi::TakeImmersivePlotModeToggleRequest()
{
    const bool requested = immersive_plot_toggle_requested_;
    immersive_plot_toggle_requested_ = false;
    return requested;
}

bool ShellUi::immersive_plot_mode() const
{
    return immersive_plot_mode_;
}

bool ShellUi::latency_sensitive_plot_interaction_active() const
{
    return spectrum_view_session_.PlotPanActive();
}

void ShellUi::OpenSource(const std::filesystem::path& path, std::size_t spectrum_index)
{
    BeginSourceActivationIntent(true);
    if (deferred_restore_active_) {
        deferred_restore_active_path_ = path;
    }
    (void)QueueSourceLoad(
        path,
        spectrum_index,
        session_.AnnotationPathsForSource(path),
        PendingSourceLoadPurpose::ExplicitOpen);
}

std::uint64_t ShellUi::QueueSourceLoad(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    std::vector<std::filesystem::path> annotation_paths,
    PendingSourceLoadPurpose purpose)
{
    const std::string path_key = SourcePathIdentityKey(path);
    for (auto pending = pending_source_loads_.begin(); pending != pending_source_loads_.end();) {
        if (pending->second.path_key != path_key) {
            ++pending;
            continue;
        }
        source_load_queue_.Cancel(pending->first);
        deferred_restore_task_ids_.erase(pending->first);
        pending = pending_source_loads_.erase(pending);
    }

    const std::uint64_t generation = ++source_load_generations_[path_key];
    const std::optional<SourceCollectionLoadHint> hint = session_.LoadHintForSource(path);
    const std::uint64_t task_id = source_load_queue_.Enqueue(
        {
            .path = path,
            .spectrum_index = spectrum_index,
            .annotation_paths = std::move(annotation_paths),
            .reuse_identity = hint ? std::optional<SourceCollectionIdentity>{hint->identity} : std::nullopt,
        });
    pending_source_loads_.emplace(
        task_id,
        PendingSourceLoad{
            .path = path,
            .path_key = path_key,
            .spectrum_index = spectrum_index,
            .generation = generation,
            .activation_epoch = source_activation_epoch_,
            .purpose = purpose,
        });
    if (purpose == PendingSourceLoadPurpose::DeferredRestore) {
        deferred_restore_task_ids_.insert(task_id);
    }
    source_load_error_.clear();
    source_load_service_deadline_ = LocalUserStateSaveScheduler::Clock::now();
    return task_id;
}

void ShellUi::BeginSourceActivationIntent(bool preserve_pending_explicit_opens)
{
    AdvanceSourceActivationIntent(
        source_activation_epoch_,
        pending_source_loads_,
        preserve_pending_explicit_opens,
        [this](std::uint64_t task_id) { source_load_queue_.Cancel(task_id); });
}

void ShellUi::AdvanceSourceActivationIntent(
    std::uint64_t& activation_epoch,
    std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
    bool preserve_pending_explicit_opens,
    const std::function<void(std::uint64_t)>& cancel)
{
    ++activation_epoch;
    for (auto pending = pending_loads.begin(); pending != pending_loads.end();) {
        PendingSourceLoad& ticket = pending->second;
        if (ticket.purpose == PendingSourceLoadPurpose::DeferredRestore) {
            ++pending;
            continue;
        }
        if (preserve_pending_explicit_opens &&
            ticket.purpose == PendingSourceLoadPurpose::ExplicitOpen) {
            ticket.activation_epoch = activation_epoch;
            ++pending;
            continue;
        }
        cancel(pending->first);
        pending = pending_loads.erase(pending);
    }
}

bool ShellUi::CompletionStartsSourceActivationIntent(
    PendingSourceLoadPurpose purpose,
    bool loaded)
{
    return loaded && purpose == PendingSourceLoadPurpose::ExplicitOpen;
}

void ShellUi::QueueSessionFollowUp(
    const SourceCollectionSessionResult& result,
    bool deferred_restore)
{
    if (!result.follow_up_spectrum_index) {
        return;
    }
    const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
    if (!snapshot || snapshot->source.path.empty()) {
        return;
    }
    (void)QueueSourceLoad(
        snapshot->source.path,
        *result.follow_up_spectrum_index,
        session_.AnnotationPathsForSource(snapshot->source.path),
        deferred_restore ? PendingSourceLoadPurpose::DeferredRestore
                         : PendingSourceLoadPurpose::SessionFollowUp);
}

void ShellUi::DrainSourceLoads()
{
    for (SourceCollectionLoadCompletion& completion : source_load_queue_.TakeCompleted()) {
        const auto pending = pending_source_loads_.find(completion.task_id);
        if (pending == pending_source_loads_.end()) {
            if (completion.prepared) {
                source_load_queue_.RetirePrepared(std::move(*completion.prepared));
            }
            continue;
        }

        PendingSourceLoad ticket = std::move(pending->second);
        pending_source_loads_.erase(pending);
        deferred_restore_task_ids_.erase(completion.task_id);
        const auto current_generation = source_load_generations_.find(ticket.path_key);
        const bool activation_current =
            ticket.purpose == PendingSourceLoadPurpose::DeferredRestore ||
            ticket.activation_epoch == source_activation_epoch_;
        const bool current = activation_current &&
                             current_generation != source_load_generations_.end() &&
                             current_generation->second == ticket.generation &&
                             SourcePathIdentityKey(completion.path) == ticket.path_key &&
                             completion.spectrum_index == ticket.spectrum_index;
        if (!current) {
            if (completion.prepared) {
                source_load_queue_.RetirePrepared(std::move(*completion.prepared));
            }
            continue;
        }
        if (!completion.prepared) {
            source_load_error_ = completion.error_message.empty()
                ? "Background source loading failed."
                : std::move(completion.error_message);
            continue;
        }

        PreparedSourceCollection prepared = std::move(*completion.prepared);
        if (session_view_cache_) {
            source_load_queue_.RetireResource(
                std::make_shared<SourceCollectionSessionView>(std::move(*session_view_cache_)));
            session_view_cache_.reset();
        }
        std::vector<SpectrumValueVector> plot_resources =
            spectrum_view_session_.RetainHeavySnapshotResources();
        SourceCollectionSessionResult result = session_.OpenPreparedSource(
            prepared.path,
            prepared.spectrum_index,
            std::move(prepared.snapshot),
            std::move(prepared.payload));
        session_view_cache_dirty_ = true;
        HandleSessionAction(result.action);
        for (BackgroundRetirementHandle& resource : result.background_retirement) {
            source_load_queue_.RetireResource(std::move(resource));
        }
        for (SpectrumValueVector& resource : plot_resources) {
            source_load_queue_.RetireResource(std::move(resource));
        }
        const bool starts_activation_intent =
            CompletionStartsSourceActivationIntent(ticket.purpose, result.loaded);
        if (!result.loaded) {
            source_load_error_ = result.message.empty()
                ? "The prepared source result was no longer applicable."
                : std::move(result.message);
        } else {
            source_load_error_.clear();
            if (starts_activation_intent) {
                // A newer successful explicit open owns the final activation.
                // Preserve later explicit opens, but cancel follow-ups queued by
                // earlier explicit results processed in this drain.
                BeginSourceActivationIntent(true);
            }
        }
        if (result.follow_up_spectrum_index) {
            (void)QueueSourceLoad(
                ticket.path,
                *result.follow_up_spectrum_index,
                session_.AnnotationPathsForSource(ticket.path),
                ticket.purpose == PendingSourceLoadPurpose::DeferredRestore
                    ? PendingSourceLoadPurpose::DeferredRestore
                    : PendingSourceLoadPurpose::SessionFollowUp);
        }
        RestoreDeferredActiveSourceIfAvailable();
    }
    FinishDeferredSourceRestoreIfReady();
}

void ShellUi::BeginDeferredSourceRestore()
{
    std::optional<SourceCollectionDeferredRestorePlan> plan =
        session_.TakeDeferredRestorePlan();
    if (!plan || plan->sources.empty()) {
        return;
    }

    deferred_restore_active_ = true;
    if (plan->active_source_index && *plan->active_source_index < plan->sources.size()) {
        deferred_restore_active_path_ = plan->sources[*plan->active_source_index].path;
    }

    std::vector<SourceCollectionLoadRequest> requests;
    std::vector<PendingSourceLoad> tickets;
    requests.reserve(plan->sources.size());
    tickets.reserve(plan->sources.size());
    for (SourceCollectionSavedSource& source : plan->sources) {
        const std::string path_key = SourcePathIdentityKey(source.path);
        const std::uint64_t generation = ++source_load_generations_[path_key];
        tickets.push_back(PendingSourceLoad{
            .path = source.path,
            .path_key = path_key,
            .spectrum_index = source.last_spectrum_index,
            .generation = generation,
            .activation_epoch = source_activation_epoch_,
            .purpose = PendingSourceLoadPurpose::DeferredRestore,
        });
        requests.push_back(SourceCollectionLoadRequest{
            .path = std::move(source.path),
            .spectrum_index = source.last_spectrum_index,
            .annotation_paths = std::move(source.annotation_paths),
        });
    }
    const std::vector<std::uint64_t> task_ids =
        source_load_queue_.EnqueueBatch(std::move(requests));
    for (std::size_t index = 0; index < task_ids.size(); ++index) {
        pending_source_loads_.emplace(task_ids[index], std::move(tickets[index]));
        deferred_restore_task_ids_.insert(task_ids[index]);
    }
    source_load_service_deadline_ = LocalUserStateSaveScheduler::Clock::now();
}

void ShellUi::FinishDeferredSourceRestoreIfReady()
{
    if (!deferred_restore_active_ || !deferred_restore_task_ids_.empty()) {
        return;
    }
    RestoreDeferredActiveSourceIfAvailable();
    session_.FinishDeferredRestore();
    deferred_restore_active_ = false;
    deferred_restore_active_path_.reset();
}

void ShellUi::RestoreDeferredActiveSourceIfAvailable()
{
    if (!deferred_restore_active_ || !deferred_restore_active_path_) {
        return;
    }
    const std::string active_path_key =
        SourcePathIdentityKey(*deferred_restore_active_path_);
    const SourceCollectionSessionView& view = SessionView();
    for (std::size_t index = 0; index < view.sources.size(); ++index) {
        if (SourcePathIdentityKey(view.sources[index].path) != active_path_key) {
            continue;
        }
        if (view.current_source_index && *view.current_source_index == index) {
            return;
        }
        SourceCollectionSessionResult result = session_.Submit(
            SourceCollectionSessionIntent::EditSourceCollection(
                SourceCollectionIntent::SwitchActive(index)));
        RetireSessionResources(result);
        session_view_cache_dirty_ = true;
        HandleSessionAction(result.action);
        QueueSessionFollowUp(result, true);
        return;
    }
}

SpectrumSnapshotHandle ShellUi::current_snapshot() const
{
    return session_.CurrentSampleSnapshot();
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
        if (session_view_cache_) {
            source_load_queue_.RetireResource(
                std::make_shared<SourceCollectionSessionView>(std::move(*session_view_cache_)));
            session_view_cache_.reset();
        }
        session_view_cache_ = session_.View();
        session_view_cache_dirty_ = false;
    }
    return *session_view_cache_;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommand(SourceCollectionSessionIntent command)
{
    if (command.SupersedesPendingSourceActivation()) {
        BeginSourceActivationIntent(false);
    }
    SourceCollectionSessionResult result = session_.Submit(std::move(command));
    if (deferred_restore_active_) {
        const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
        if (snapshot && !snapshot->source.path.empty()) {
            deferred_restore_active_path_ = snapshot->source.path;
        }
    }
    QueueSessionFollowUp(result);
    RetireSessionResources(result);
    session_view_cache_dirty_ = true;
    HandleSessionAction(result.action);
    return result;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommandForPanel(SourceCollectionSessionIntent command)
{
    if (command.SupersedesPendingSourceActivation()) {
        BeginSourceActivationIntent(false);
    }
    SourceCollectionSessionResult result = session_.Submit(std::move(command));
    if (deferred_restore_active_) {
        const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
        if (snapshot && !snapshot->source.path.empty()) {
            deferred_restore_active_path_ = snapshot->source.path;
        }
    }
    QueueSessionFollowUp(result);
    RetireSessionResources(result);
    session_view_cache_dirty_ = true;
    return result;
}

void ShellUi::RetireSessionResources(SourceCollectionSessionResult& result)
{
    for (BackgroundRetirementHandle& resource : result.background_retirement) {
        source_load_queue_.RetireResource(std::move(resource));
    }
    result.background_retirement.clear();
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
    RenderStatusBar(
        status,
        ImVec2(content_size.x, status_bar_height),
        source_load_queue_.NeedsService(),
        source_load_error_);

    ImGui::End();
}

void ShellUi::RenderImmersivePlot(const ShellStatus& status)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(kImmersivePlotWindow, nullptr, flags);
    ImGui::PopStyleVar(3);

    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    RenderSpectrumPlot(
        snapshot,
        spectrum_view_session_.PlotStateForRender(),
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        spectrum_view_session_.PlotStyleForRender(),
        SpectrumPlotOverlays{
            spectral_lines.visible_markers.data(),
            spectral_lines.visible_markers.size(),
            spectral_lines.marker_labels_visible,
            spectral_lines.layout_scope_id},
        MakeImmersivePlotDisplayOptions(),
        touchpad_gestures_);

    const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const SourceCollectionLabelingView& labeling = SessionView().labeling;
    QueueSampleWorkflowShortcut(RouteSampleWorkflowShortcut(
        {
            .focused = shortcut_focused,
            .hovered = shortcut_hovered,
            .allow_hover_fallback = true,
            .navigation_enabled = true,
            .labeling_enabled = labeling.has_active_task,
        },
        labeling.label_set));
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
        if (ImGui::MenuItem("Immersive Plot Mode", "F11", immersive_plot_mode_)) {
            immersive_plot_toggle_requested_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show all panels")) {
            panel_visibility_.files = true;
            panel_visibility_.navigation = true;
            panel_visibility_.annotations = true;
            panel_visibility_.labeling = true;
            panel_visibility_.filters = true;
            panel_visibility_.sorting = true;
            panel_visibility_.smoothing = true;
            panel_visibility_.information = true;
            panel_visibility_.spectral_lines = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("Files", nullptr, &panel_visibility_.files);
        ImGui::MenuItem("Navigation", nullptr, &panel_visibility_.navigation);
        ImGui::MenuItem("Annotations", nullptr, &panel_visibility_.annotations);
        ImGui::MenuItem("Labeling", nullptr, &panel_visibility_.labeling);
        ImGui::MenuItem("Sample Filters", nullptr, &panel_visibility_.filters);
        ImGui::MenuItem("Sample Sorting", nullptr, &panel_visibility_.sorting);
        ImGui::MenuItem("Smoothing", nullptr, &panel_visibility_.smoothing);
        ImGui::MenuItem("Information", nullptr, &panel_visibility_.information);
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
        },
        [this](const std::filesystem::path& path) {
            OpenSource(path);
        }));
}

void ShellUi::RenderNavigationPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    SampleWorkflowShortcut shortcut;
    HandleSessionAction(source_collection_panel_ui_.RenderNavigation(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        [this]() -> const SourceCollectionSessionView& {
            return SessionView();
        },
        &panel_visibility_.navigation,
        shortcut));
    QueueSampleWorkflowShortcut(shortcut);
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
    SampleWorkflowShortcut shortcut;
    HandleSessionAction(sample_workflow_panel_ui_.RenderLabeling(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        [this]() -> const SourceCollectionSessionView& {
            return SessionView();
        },
        &panel_visibility_.labeling,
        []() {
            return ShowLabelOutputFilePicker();
        },
        shortcut));
    QueueSampleWorkflowShortcut(shortcut);
}

void ShellUi::RenderFiltersPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(sample_workflow_panel_ui_.RenderFilters(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        [this]() -> const SourceCollectionSessionView& {
            return SessionView();
        },
        &panel_visibility_.filters));
}

void ShellUi::RenderSortingPanel()
{
    const SourceCollectionSessionView& view = SessionView();
    HandleSessionAction(sample_workflow_panel_ui_.RenderSorting(
        view,
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(std::move(command));
        },
        [this]() -> const SourceCollectionSessionView& {
            return SessionView();
        },
        &panel_visibility_.sorting));
}

void ShellUi::RenderSmoothingPanel()
{
    if (!ImGui::Begin(kSmoothingWindow, &panel_visibility_.smoothing)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Smoothing");
    ImGui::Separator();

    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
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
    if (!ImGui::Begin(kInfoTagsWindow, &panel_visibility_.information)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Information");
    ImGui::Separator();
    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
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
    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    RenderSpectrumPlot(
        snapshot,
        spectrum_view_session_.PlotStateForRender(),
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        spectrum_view_session_.PlotStyleForRender(),
        SpectrumPlotOverlays{
            spectral_lines.visible_markers.data(),
            spectral_lines.visible_markers.size(),
            spectral_lines.marker_labels_visible,
            spectral_lines.layout_scope_id},
        {},
        touchpad_gestures_);
    const bool shortcut_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool shortcut_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const SourceCollectionLabelingView& labeling = SessionView().labeling;
    QueueSampleWorkflowShortcut(RouteSampleWorkflowShortcut(
        {
            .focused = shortcut_focused,
            .hovered = shortcut_hovered,
            .allow_hover_fallback = true,
            .navigation_enabled = true,
            .labeling_enabled = labeling.has_active_task,
        },
        labeling.label_set));
    ImGui::End();
}

void ShellUi::QueueSampleWorkflowShortcut(SampleWorkflowShortcut shortcut)
{
    if (shortcut.kind != SampleWorkflowShortcutKind::None) {
        sample_workflow_shortcut_ = shortcut;
    }
}

void ShellUi::HandleSampleWorkflowShortcut()
{
    switch (sample_workflow_shortcut_.kind) {
    case SampleWorkflowShortcutKind::None:
        return;
    case SampleWorkflowShortcutKind::PreviousSample:
        (void)SubmitSessionCommand(SourceCollectionSessionIntent::UpdateSampleNavigation(
            SampleNavigationIntent::Move(SampleNavigationRequest::Previous())));
        return;
    case SampleWorkflowShortcutKind::NextSample:
        (void)SubmitSessionCommand(SourceCollectionSessionIntent::UpdateSampleNavigation(
            SampleNavigationIntent::Move(SampleNavigationRequest::Next())));
        return;
    case SampleWorkflowShortcutKind::UndoLabelWrite:
        (void)SubmitSessionCommand(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UndoLastLabelWrite()));
        return;
    case SampleWorkflowShortcutKind::AssignLabel:
        (void)SubmitSessionCommand(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(
                sample_workflow_shortcut_.label_code)));
        return;
    }
}

void ShellUi::RenderSpectralLinesPanel()
{
    spectral_lines_panel_ui_.Render(
        spectral_lines_panel_,
        session_.CurrentSampleSnapshot(),
        &panel_visibility_.spectral_lines);
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
    ImGuiID sorting_id = 0;
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
    ImGui::DockBuilderSplitNode(filters_id, ImGuiDir_Down, 0.50f, &sorting_id, &filters_id);

    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::FilesWindowName(), files_id);
    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::NavigationWindowName(), navigation_id);
    ImGui::DockBuilderDockWindow(SourceCollectionPanelUi::AnnotationsWindowName(), annotations_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::LabelingWindowName(), labeling_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::FiltersWindowName(), filters_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::SortingWindowName(), sorting_id);
    ImGui::DockBuilderDockWindow(kInfoTagsWindow, info_tags_id);
    ImGui::DockBuilderDockWindow(kSmoothingWindow, smoothing_id);
    ImGui::DockBuilderDockWindow(kMainPlotWindow, center_id);
    ImGui::DockBuilderDockWindow(SpectralLinesPanelUi::WindowName(), spectral_lines_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

}  // namespace specforge
