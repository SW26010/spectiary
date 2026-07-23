#include "ui/shell_ui.h"

#include "domain/spectrum_loader.h"
#include "domain/source_path_identity.h"
#include "plot/spectrum_plot.h"
#include "ui/profile_recording_ui_state.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/top_bar_status_hover.h"
#include "ui/top_bar_status_layout.h"

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
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);

std::int64_t ElapsedNavigationResolutionNanoseconds(
    NavigationLatencyTimePoint started_at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               NavigationLatencyTrace::Now() - started_at)
        .count();
}

std::int64_t NavigationSteadyNanoseconds(
    NavigationLatencyTimePoint at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               at.time_since_epoch())
        .count();
}

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

void RenderTopBarStatus(
    const ShellStatus& status,
    bool source_load_active,
    std::string_view source_load_error)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect menu_bar_rect = window->MenuBarRect();
    const ImRect menu_clip_rect = window->ClipRect;
    const float menu_end_x = ImGui::GetCursorScreenPos().x;
    const float right_x = menu_clip_rect.Max.x - style.FramePadding.x;
    const float available_width =
        std::max(0.0f, right_x - menu_end_x - style.ItemSpacing.x * 2.0f);

    const bool operation_error = !source_load_error.empty();
    const bool operation_important = operation_error || source_load_active;
    const std::string operation_text = operation_error
        ? "Load failed"
        : (source_load_active ? "Loading source..." : "Ready");
    const std::string frame_text =
        "Frame " + std::to_string(static_cast<unsigned long long>(status.frame_index));
    const std::string dimensions_text =
        std::to_string(status.client_width) + "x" + std::to_string(status.client_height);
    const ProfileRecordingUiPresentation recording_presentation =
        ResolveProfileRecordingUiPresentation(status.profile_open, status.profile_stopping);

    constexpr float kRecordingIndicatorRadius = 4.0f;
    constexpr float kRecordingIndicatorSpacing = 5.0f;
    const float separator_text_width = ImGui::CalcTextSize("|").x;
    const float recording_indicator_width = recording_presentation.show_recording_indicator
        ? kRecordingIndicatorRadius * 2.0f + kRecordingIndicatorSpacing
        : 0.0f;
    const TopBarStatusWidths widths{
        .operation = ImGui::CalcTextSize(operation_text.c_str()).x,
        .frame = ImGui::CalcTextSize(frame_text.c_str()).x,
        .dimensions = ImGui::CalcTextSize(dimensions_text.c_str()).x,
        .profile = recording_indicator_width +
                   ImGui::CalcTextSize(
                       recording_presentation.status_text.data(),
                       recording_presentation.status_text.data() +
                           recording_presentation.status_text.size())
                       .x,
        .separator = style.ItemSpacing.x * 2.0f + separator_text_width,
    };
    const TopBarStatusLayout layout = ResolveTopBarStatusLayout(
        available_width,
        widths,
        operation_important,
        status.profile_open || status.profile_stopping);
    if (layout.width <= 0.0f) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float text_height = ImGui::GetTextLineHeight();
    const float text_y =
        menu_bar_rect.Min.y + std::max(0.0f, (menu_bar_rect.GetHeight() - text_height) * 0.5f);
    float cursor_x = right_x - layout.width;
    bool has_component = false;
    ImRect operation_rect;
    ImRect profile_rect;

    const auto draw_separator = [&]() {
        if (!has_component) {
            return;
        }
        cursor_x += style.ItemSpacing.x;
        draw_list->AddText(
            ImVec2(cursor_x, text_y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "|");
        cursor_x += separator_text_width + style.ItemSpacing.x;
    };
    const auto draw_text = [&](std::string_view text, ImU32 color) {
        draw_list->AddText(
            ImVec2(cursor_x, text_y),
            color,
            text.data(),
            text.data() + text.size());
        cursor_x += ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        has_component = true;
    };

    draw_list->PushClipRect(menu_clip_rect.Min, menu_clip_rect.Max, true);
    if (!source_load_error.empty()) {
        if (layout.show_operation) {
            const float operation_start_x = cursor_x;
            draw_text(operation_text, IM_COL32(242, 89, 77, 255));
            operation_rect = ImRect(
                ImVec2(operation_start_x, text_y),
                ImVec2(cursor_x, text_y + text_height));
        }
    } else if (layout.show_operation) {
        const float operation_start_x = cursor_x;
        draw_text(
            operation_text,
            ImGui::GetColorU32(
                source_load_active ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        operation_rect = ImRect(
            ImVec2(operation_start_x, text_y),
            ImVec2(cursor_x, text_y + text_height));
    }
    if (layout.show_frame) {
        draw_separator();
        draw_text(frame_text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    if (layout.show_dimensions) {
        draw_separator();
        draw_text(dimensions_text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    if (layout.show_profile) {
        draw_separator();
        const float profile_start_x = cursor_x;
        if (recording_presentation.show_recording_indicator) {
            draw_list->AddCircleFilled(
                ImVec2(
                    cursor_x + kRecordingIndicatorRadius,
                    text_y + text_height * 0.5f),
                kRecordingIndicatorRadius,
                IM_COL32(235, 64, 58, 255));
            cursor_x += kRecordingIndicatorRadius * 2.0f + kRecordingIndicatorSpacing;
        }
        draw_text(
            recording_presentation.status_text,
            status.profile_open || status.profile_stopping
                ? ImGui::GetColorU32(ImGuiCol_Text)
                : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        profile_rect = ImRect(
            ImVec2(profile_start_x, text_y),
            ImVec2(cursor_x, text_y + text_height));
    }
    draw_list->PopClipRect();

    if (layout.show_operation && operation_error &&
        IsTopBarStatusHoverTarget(operation_rect.Min, operation_rect.Max)) {
        ImGui::SetTooltip(
            "Load failed:\n%.*s",
            static_cast<int>(source_load_error.size()),
            source_load_error.data());
    }
    if (layout.show_profile &&
        IsTopBarStatusHoverTarget(profile_rect.Min, profile_rect.Max) &&
        (status.profile_path != nullptr || !status.profile_status_message.empty())) {
        const std::string profile_path =
            status.profile_path != nullptr ? NarrowPath(*status.profile_path) : std::string();
        if (!profile_path.empty() && !status.profile_status_message.empty()) {
            ImGui::SetTooltip(
                "%.*s\n%s",
                static_cast<int>(status.profile_status_message.size()),
                status.profile_status_message.data(),
                profile_path.c_str());
        } else if (!profile_path.empty()) {
            ImGui::SetTooltip("%s", profile_path.c_str());
        } else {
            ImGui::SetTooltip(
                "%.*s",
                static_cast<int>(status.profile_status_message.size()),
                status.profile_status_message.data());
        }
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

std::optional<std::filesystem::path> ShowFolderPicker(const wchar_t* title)
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
    dialog->SetTitle(title);

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowSourceFolderPicker()
{
    return ShowFolderPicker(L"Add source folder");
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

ShellUi::ShellUi(
    SourceCollectionSession session,
    SourceCollectionLoadQueue source_load_queue)
    : session_(std::move(session)),
      source_load_queue_(std::move(source_load_queue)),
      persist_local_state_(false)
{
}

ShellUi::~ShellUi()
{
    if (persist_local_state_) {
        (void)panel_visibility_state_.Flush(panel_visibility_);
        (void)session_.FlushStateCaches();
        (void)spectral_lines_panel_.Flush();
    }
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
    current_frame_index_ = status.frame_index;
    spectrum_draw_submission_.reset();
    latency_tracing_enabled_ = status.latency_trace_recording_active;
    if (!latency_tracing_enabled_) {
        pending_keyboard_previous_at_.reset();
        pending_keyboard_next_at_.reset();
        presentable_navigation_trace_.reset();
        presentable_navigation_snapshot_.reset();
        navigation_traces_.clear();
        presentable_source_load_trace_.reset();
        presentable_source_load_snapshot_.reset();
        source_load_traces_.clear();
    }
    DrainSourceLoads();
    sample_workflow_shortcut_ = {};
    if (immersive_plot_mode_) {
        RenderImmersivePlot(status);
        HandleSampleWorkflowShortcut();
        pending_keyboard_previous_at_.reset();
        pending_keyboard_next_at_.reset();
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
    RenderSettingsPanel(status);
    HandleSampleWorkflowShortcut();
    pending_keyboard_previous_at_.reset();
    pending_keyboard_next_at_.reset();
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

void ShellUi::RegisterSourceLoadCompletionReadyCallback(
    SourceCollectionLoadQueue::CompletionReadyCallback callback)
{
    source_load_queue_.RegisterCompletionReadyCallback(std::move(callback));
}

void ShellUi::UnregisterSourceLoadCompletionReadyCallback()
{
    source_load_queue_.UnregisterCompletionReadyCallback();
}

void ShellUi::RefreshSystemColors()
{
    spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetPlotStyle(ReadSystemSpectrumPlotStyle()));
}

void ShellUi::SetSpectralLineLabelFont(ImFont* font)
{
    spectral_line_label_font_ = font;
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

bool ShellUi::TakeProfileRecordingToggleRequest()
{
    return settings_panel_ui_.TakeProfileRecordingToggleRequest();
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
    CancelSnapshotPrefetch();
    SourceLoadLatencyTraceHandle source_load_trace = StartSourceLoadTrace(
        spectrum_index,
        NavigationLatencyTrace::Now());
    BeginSourceActivationIntent(true);
    if (session_.CancelActivePendingSampleNavigation()) {
        session_view_cache_dirty_ = true;
    }
    if (deferred_restore_active_) {
        deferred_restore_active_path_ = path;
    }
    (void)QueueSourceLoad(
        path,
        spectrum_index,
        session_.AnnotationPathsForSource(path),
        PendingSourceLoadPurpose::ExplicitOpen,
        {},
        std::move(source_load_trace));
}

std::uint64_t ShellUi::QueueSourceLoad(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    std::vector<std::filesystem::path> annotation_paths,
    PendingSourceLoadPurpose purpose,
    NavigationLatencyTraceHandle navigation_trace,
    SourceLoadLatencyTraceHandle source_load_trace,
    std::optional<SampleNavigationDirection> prefetch_direction)
{
    CancelSnapshotPrefetch();
    const std::string path_key = SourcePathIdentityKey(path);
    for (auto pending = pending_source_loads_.begin(); pending != pending_source_loads_.end();) {
        if (pending->second.path_key != path_key) {
            ++pending;
            continue;
        }
        if (pending->second.navigation_trace) {
            (void)pending->second.navigation_trace->MarkTerminal(
                NavigationLatencyOutcome::Superseded);
        }
        if (pending->second.source_load_trace) {
            (void)pending->second.source_load_trace->MarkTerminal(
                SourceLoadLatencyOutcome::Superseded);
        }
        source_load_queue_.Cancel(pending->first);
        deferred_restore_task_ids_.erase(pending->first);
        pending = pending_source_loads_.erase(pending);
    }

    const std::uint64_t generation = ++source_load_generations_[path_key];
    std::optional<SourceCollectionLoadHint> hint =
        session_.LoadHintForSource(path, spectrum_index);
    NavigationLatencyAttemptHandle latency_attempt;
    if (navigation_trace) {
        navigation_trace->SetTargetIndex(spectrum_index);
        latency_attempt = navigation_trace->BeginLoadAttempt(spectrum_index);
    } else if (source_load_trace) {
        source_load_trace->SetTargetIndex(spectrum_index);
        latency_attempt = source_load_trace->BeginLoadAttempt(spectrum_index);
    }
    const std::uint64_t task_id = source_load_queue_.Enqueue(
        {
            .path = path,
            .spectrum_index = spectrum_index,
            .annotation_paths = std::move(annotation_paths),
            .reuse_identity = hint ? std::optional<SourceCollectionIdentity>{hint->identity} : std::nullopt,
            .context_reuse_proof =
                hint ? hint->context_reuse_proof
                     : std::optional<SourceCollectionContextReuseProof>{},
            .base_live_workflow_revision =
                hint ? std::optional<std::uint64_t>{hint->live_workflow_revision}
                     : std::nullopt,
            .folder_listing_generation_hint =
                hint ? hint->folder_listing_generation_hint
                     : SourceCollectionFolderListingGenerationHandle{},
            .resident_snapshot =
                hint ? std::move(hint->resident_snapshot)
                     : std::optional<SourceCollectionResidentSnapshot>{},
            .latency_attempt = std::move(latency_attempt),
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
            .navigation_trace = std::move(navigation_trace),
            .source_load_trace = std::move(source_load_trace),
            .prefetch_direction = prefetch_direction,
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
    CancelSnapshotPrefetch();
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
        if (ticket.navigation_trace) {
            (void)ticket.navigation_trace->MarkTerminal(
                NavigationLatencyOutcome::Superseded);
        }
        if (ticket.source_load_trace) {
            (void)ticket.source_load_trace->MarkTerminal(
                SourceLoadLatencyOutcome::Superseded);
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

std::optional<ShellUi::PendingSourceLoad> ShellUi::TakeCurrentPendingSourceLoad(
    const SourceCollectionLoadCompletion& completion,
    std::uint64_t activation_epoch,
    std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
    const std::unordered_map<std::string, std::uint64_t>& source_load_generations)
{
    const auto pending = pending_loads.find(completion.task_id);
    if (pending == pending_loads.end()) {
        return std::nullopt;
    }

    PendingSourceLoad ticket = std::move(pending->second);
    pending_loads.erase(pending);
    const auto current_generation = source_load_generations.find(ticket.path_key);
    const bool activation_current =
        ticket.purpose == PendingSourceLoadPurpose::DeferredRestore ||
        ticket.activation_epoch == activation_epoch;
    const bool current = activation_current &&
                         current_generation != source_load_generations.end() &&
                         current_generation->second == ticket.generation &&
                         SourcePathIdentityKey(completion.path) == ticket.path_key &&
                         completion.spectrum_index == ticket.spectrum_index;
    if (!current && ticket.navigation_trace) {
        (void)ticket.navigation_trace->MarkTerminal(
            NavigationLatencyOutcome::Superseded);
    }
    if (!current && ticket.source_load_trace) {
        (void)ticket.source_load_trace->MarkTerminal(
            SourceLoadLatencyOutcome::Superseded);
    }
    return current ? std::optional<PendingSourceLoad>{std::move(ticket)} : std::nullopt;
}

void ShellUi::QueueSessionFollowUp(
    const SourceCollectionSessionResult& result,
    bool deferred_restore,
    NavigationLatencyTraceHandle navigation_trace,
    std::optional<SampleNavigationDirection> prefetch_direction)
{
    CancelSourceFollowUps(result);
    if (!result.follow_up_spectrum_index) {
        return;
    }
    const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
    if (!snapshot || snapshot->source.path.empty()) {
        return;
    }
    if (HasMatchingSourceFollowUp(
            snapshot->source.path,
            *result.follow_up_spectrum_index,
            pending_source_loads_)) {
        if (navigation_trace) {
            (void)navigation_trace->MarkTerminal(NavigationLatencyOutcome::Coalesced);
        }
        return;
    }
    (void)QueueSourceLoad(
        snapshot->source.path,
        *result.follow_up_spectrum_index,
        session_.AnnotationPathsForSource(snapshot->source.path),
        deferred_restore ? PendingSourceLoadPurpose::DeferredRestore
                         : PendingSourceLoadPurpose::SessionFollowUp,
        std::move(navigation_trace),
        {},
        prefetch_direction);
}

void ShellUi::CancelSourceFollowUps(const SourceCollectionSessionResult& result)
{
    CancelSourceFollowUpsForResultInState(
        result,
        pending_source_loads_,
        deferred_restore_task_ids_,
        [this](std::uint64_t task_id) { source_load_queue_.Cancel(task_id); });
}

void ShellUi::CancelSourceFollowUpsForResultInState(
    const SourceCollectionSessionResult& result,
    std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
    std::unordered_set<std::uint64_t>& deferred_restore_task_ids,
    const std::function<void(std::uint64_t)>& cancel)
{
    if (!result.canceled_source_follow_up_path) {
        return;
    }
    CancelSourceFollowUpsForPathInState(
        *result.canceled_source_follow_up_path,
        pending_loads,
        deferred_restore_task_ids,
        cancel);
}

void ShellUi::CancelSourceFollowUpsForPathInState(
    const std::filesystem::path& path,
    std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
    std::unordered_set<std::uint64_t>& deferred_restore_task_ids,
    const std::function<void(std::uint64_t)>& cancel)
{
    const std::string path_key = SourcePathIdentityKey(path);
    for (auto pending = pending_loads.begin(); pending != pending_loads.end();) {
        if (pending->second.path_key != path_key ||
            pending->second.purpose == PendingSourceLoadPurpose::ExplicitOpen) {
            ++pending;
            continue;
        }
        if (pending->second.navigation_trace) {
            (void)pending->second.navigation_trace->MarkTerminal(
                NavigationLatencyOutcome::Superseded);
        }
        if (pending->second.source_load_trace) {
            (void)pending->second.source_load_trace->MarkTerminal(
                SourceLoadLatencyOutcome::Superseded);
        }
        cancel(pending->first);
        deferred_restore_task_ids.erase(pending->first);
        pending = pending_loads.erase(pending);
    }
}

bool ShellUi::HasMatchingSourceFollowUp(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads)
{
    const std::string path_key = SourcePathIdentityKey(path);
    return std::any_of(
        pending_loads.begin(),
        pending_loads.end(),
        [&path_key, spectrum_index](const auto& entry) {
            const PendingSourceLoad& pending = entry.second;
            return pending.path_key == path_key &&
                   pending.spectrum_index == spectrum_index &&
                   pending.purpose != PendingSourceLoadPurpose::ExplicitOpen;
        });
}

bool ShellUi::CancelFailedPendingSampleNavigation(
    SourceCollectionSession& session,
    const PendingSourceLoad& ticket)
{
    if (ticket.purpose == PendingSourceLoadPurpose::ExplicitOpen) {
        return false;
    }
    return session.CancelPendingSampleNavigation(ticket.path, ticket.spectrum_index);
}

void ShellUi::DrainSourceLoads()
{
    DrainSourceLoadCompletions(source_load_queue_.TakeCompleted());
    ServiceSnapshotPrefetch();
}

void ShellUi::ScheduleSnapshotPrefetch(
    SampleNavigationDirection direction)
{
    pending_snapshot_prefetch_direction_ = direction;
}

void ShellUi::ServiceSnapshotPrefetch()
{
    if (!pending_snapshot_prefetch_direction_ ||
        active_snapshot_prefetch_ ||
        !pending_source_loads_.empty() ||
        deferred_restore_active_ ||
        latency_sensitive_plot_interaction_active()) {
        return;
    }

    const SampleNavigationDirection direction =
        *pending_snapshot_prefetch_direction_;
    pending_snapshot_prefetch_direction_.reset();
    std::optional<SourceCollectionSnapshotPrefetchPlan> plan =
        session_.PlanSnapshotPrefetch(
            direction,
            snapshot_prefetch_policy_);
    if (!plan) {
        return;
    }

    const NavigationLatencyTimePoint scheduled_at =
        NavigationLatencyTrace::Now();
    const std::uint64_t prefetch_id =
        next_snapshot_prefetch_id_++;
    const std::string path_key =
        SourcePathIdentityKey(plan->path);
    const auto generation = source_load_generations_.find(path_key);
    const std::uint64_t expected_generation =
        generation == source_load_generations_.end()
        ? 0
        : generation->second;
    SourceCollectionLoadHint hint =
        std::move(plan->load_hint);
    const std::uint64_t task_id =
        source_load_queue_.EnqueuePrefetch({
            .path = plan->path,
            .spectrum_index = plan->spectrum_index,
            .annotation_paths =
                std::move(plan->annotation_paths),
            .reuse_identity = hint.identity,
            .context_reuse_proof =
                std::move(hint.context_reuse_proof),
            .base_live_workflow_revision =
                hint.live_workflow_revision,
            .folder_listing_generation_hint =
                std::move(
                    hint
                        .folder_listing_generation_hint),
            .snapshot_only = true,
        });
    if (task_id == 0) {
        pending_snapshot_prefetch_direction_ = direction;
        return;
    }
    active_snapshot_prefetch_ = PendingSnapshotPrefetch{
        .prefetch_id = prefetch_id,
        .task_id = task_id,
        .path = std::move(plan->path),
        .path_key = path_key,
        .spectrum_index = plan->spectrum_index,
        .generation = expected_generation,
        .activation_epoch = source_activation_epoch_,
        .direction = direction,
        .scheduled_at = scheduled_at,
    };
    source_load_service_deadline_ =
        LocalUserStateSaveScheduler::Clock::now();
}

void ShellUi::CancelSnapshotPrefetch()
{
    pending_snapshot_prefetch_direction_.reset();
    if (!active_snapshot_prefetch_) {
        return;
    }
    if (active_snapshot_prefetch_->invalidated) {
        return;
    }

    if (source_load_queue_.Cancel(
            active_snapshot_prefetch_->task_id)) {
        active_snapshot_prefetch_->
            cancel_requested_at =
                NavigationLatencyTrace::Now();
        active_snapshot_prefetch_->invalidated = true;
        return;
    }
    // The worker already published. Keep the ticket until the UI drains that
    // immutable result, then classify it as stale without accepting it.
    active_snapshot_prefetch_->invalidated = true;
}

void ShellUi::DrainSnapshotPrefetchCompletion(
    SourceCollectionLoadCompletion completion)
{
    PendingSnapshotPrefetch ticket =
        std::move(*active_snapshot_prefetch_);
    active_snapshot_prefetch_.reset();
    if (completion.canceled) {
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Canceled,
            ticket.scheduled_at,
            completion.worker_terminal_at,
            ticket.cancel_requested_at);
        return;
    }
    const auto generation =
        source_load_generations_.find(ticket.path_key);
    const bool current =
        !ticket.invalidated &&
        ticket.activation_epoch == source_activation_epoch_ &&
        generation != source_load_generations_.end() &&
        generation->second == ticket.generation &&
        SourcePathIdentityKey(completion.path) ==
            ticket.path_key &&
        completion.spectrum_index == ticket.spectrum_index;
    if (!current) {
        if (completion.prepared) {
            source_load_queue_.RetirePrepared(
                std::move(*completion.prepared));
        }
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Stale,
            ticket.scheduled_at);
        return;
    }

    if (!completion.prepared) {
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            completion.stale
                ? NavigationPrefetchOutcome::Stale
                : NavigationPrefetchOutcome::Failed,
            ticket.scheduled_at);
        return;
    }

    PreparedSourceCollection prepared =
        std::move(*completion.prepared);
    if (!std::holds_alternative<
            PreparedSourceCollectionReuse>(
            prepared.payload) ||
        !prepared.context_reuse_proof) {
        source_load_queue_.RetirePrepared(
            std::move(prepared));
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Stale,
            ticket.scheduled_at);
        return;
    }

    SourceCollectionSnapshotPrefetchStoreResult stored =
        session_.StorePrefetchedSnapshot(
            prepared.path,
            SourceCollectionResidentSnapshot{
                prepared.spectrum_index,
                std::move(prepared.snapshot),
                std::move(
                    *prepared.context_reuse_proof),
                std::move(
                    prepared
                        .folder_listing_generation),
                SourceCollectionResidentSnapshotOrigin::
                    Prefetch,
                ticket.prefetch_id,
                ticket.task_id,
                NavigationSteadyNanoseconds(
                    ticket.scheduled_at),
                ticket.direction,
            });
    for (BackgroundRetirementHandle& resource :
         stored.background_retirement) {
        source_load_queue_.RetireResource(
            std::move(resource));
    }
    RecordSnapshotPrefetchOutcome(
        ticket.prefetch_id,
        ticket.task_id,
        ticket.spectrum_index,
        ticket.direction,
        stored.stored
            ? NavigationPrefetchOutcome::Completed
            : NavigationPrefetchOutcome::Stale,
        ticket.scheduled_at);
}

void ShellUi::RecordSnapshotPrefetchOutcome(
    std::uint64_t prefetch_id,
    std::uint64_t source_task_id,
    std::size_t target_index,
    SampleNavigationDirection direction,
    NavigationPrefetchOutcome outcome,
    NavigationLatencyTimePoint scheduled_at,
    NavigationLatencyTimePoint terminal_at,
    NavigationLatencyTimePoint cancel_requested_at)
{
    if (terminal_at ==
        NavigationLatencyTimePoint{}) {
        terminal_at = NavigationLatencyTrace::Now();
    }
    navigation_prefetch_reports_.push_back({
        .prefetch_id = prefetch_id,
        .source_task_id = source_task_id,
        .target_index = target_index,
        .direction = direction,
        .outcome = outcome,
        .scheduled_at = scheduled_at,
        .cancel_requested_at =
            cancel_requested_at,
        .terminal_at = terminal_at,
    });
}

std::optional<SampleNavigationDirection>
ShellUi::PrefetchDirectionForInputKind(
    NavigationLatencyInputKind kind)
{
    switch (kind) {
    case NavigationLatencyInputKind::KeyboardPrevious:
    case NavigationLatencyInputKind::UiPrevious:
        return SampleNavigationDirection::Previous;
    case NavigationLatencyInputKind::KeyboardNext:
    case NavigationLatencyInputKind::UiNext:
    case NavigationLatencyInputKind::AutoAdvance:
        return SampleNavigationDirection::Next;
    }
    return std::nullopt;
}

void ShellUi::DrainSourceLoadCompletions(
    std::vector<SourceCollectionLoadCompletion> completions)
{
    for (SourceCollectionLoadCompletion& completion : completions) {
        if (active_snapshot_prefetch_ &&
            completion.task_id ==
                active_snapshot_prefetch_->task_id) {
            DrainSnapshotPrefetchCompletion(
                std::move(completion));
            continue;
        }
        deferred_restore_task_ids_.erase(completion.task_id);
        std::optional<PendingSourceLoad> current_ticket = TakeCurrentPendingSourceLoad(
            completion,
            source_activation_epoch_,
            pending_source_loads_,
            source_load_generations_);
        if (!current_ticket) {
            if (completion.prepared) {
                source_load_queue_.RetirePrepared(std::move(*completion.prepared));
            }
            continue;
        }

        PendingSourceLoad ticket = std::move(*current_ticket);
        if (completion.latency_attempt) {
            completion.latency_attempt->MarkCompletionDrained();
        }
        if (!completion.prepared) {
            source_load_error_ = completion.error_message.empty()
                ? "Background source loading failed."
                : std::move(completion.error_message);
            if (CancelFailedPendingSampleNavigation(session_, ticket)) {
                session_view_cache_dirty_ = true;
            }
            if (ticket.navigation_trace) {
                (void)ticket.navigation_trace->MarkTerminal(
                    NavigationLatencyOutcome::Failed);
            }
            if (ticket.source_load_trace) {
                (void)ticket.source_load_trace->MarkTerminal(
                    SourceLoadLatencyOutcome::Failed);
            }
            continue;
        }

        PreparedSourceCollection prepared = std::move(*completion.prepared);
        if (ticket.navigation_trace) {
            NavigationSnapshotCacheKind cache_kind =
                NavigationSnapshotCacheKind::None;
            if (prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::History) {
                cache_kind = NavigationSnapshotCacheKind::History;
            } else if (
                prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::Prefetch) {
                cache_kind = NavigationSnapshotCacheKind::Prefetch;
            }
            ticket.navigation_trace->SetCacheKind(cache_kind);
        }
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
            std::move(prepared.payload),
            std::move(prepared.folder_listing_generation),
            std::move(prepared.context_reuse_proof));
        const bool completes_navigation_trace =
            result.loaded && !result.follow_up_spectrum_index && ticket.navigation_trace;
        const bool completes_source_load_trace =
            result.loaded && !result.follow_up_spectrum_index &&
            ticket.source_load_trace;
        if (completes_navigation_trace) {
            if (presentable_navigation_trace_ &&
                presentable_navigation_trace_ != ticket.navigation_trace) {
                (void)presentable_navigation_trace_->MarkTerminal(
                    NavigationLatencyOutcome::Superseded);
            }
            presentable_navigation_trace_ = ticket.navigation_trace;
            presentable_navigation_snapshot_ = session_.CurrentSampleSnapshot();
            ticket.navigation_trace->MarkSnapshotActivated(current_frame_index_);
        }
        if (completes_source_load_trace) {
            if (presentable_source_load_trace_ &&
                presentable_source_load_trace_ != ticket.source_load_trace) {
                (void)presentable_source_load_trace_->MarkTerminal(
                    SourceLoadLatencyOutcome::Superseded);
            }
            presentable_source_load_trace_ = ticket.source_load_trace;
            presentable_source_load_snapshot_ = session_.CurrentSampleSnapshot();
            ticket.source_load_trace->MarkSnapshotActivated(current_frame_index_);
        }
        CancelSourceFollowUps(result);
        session_view_cache_dirty_ = true;
        HandleSessionAction(result.action);
        if (completes_navigation_trace) {
            ticket.navigation_trace->MarkUiUpdated();
        }
        if (completes_source_load_trace) {
            ticket.source_load_trace->MarkUiUpdated();
        }
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
            (void)CancelFailedPendingSampleNavigation(session_, ticket);
            if (ticket.navigation_trace) {
                (void)ticket.navigation_trace->MarkTerminal(
                    NavigationLatencyOutcome::Rejected);
            }
            if (ticket.source_load_trace) {
                (void)ticket.source_load_trace->MarkTerminal(
                    SourceLoadLatencyOutcome::Rejected);
            }
        } else {
            source_load_error_.clear();
            if (starts_activation_intent) {
                // A newer successful explicit open owns the final activation.
                // Preserve later explicit opens, but cancel follow-ups queued by
                // earlier explicit results processed in this drain.
                BeginSourceActivationIntent(true);
            }
        }
        if (result.loaded && !result.follow_up_spectrum_index &&
            prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::Prefetch &&
            prepared.snapshot_prefetch_id != 0 &&
            prepared.snapshot_prefetch_task_id != 0 &&
            prepared.snapshot_prefetch_scheduled_ns > 0) {
            RecordSnapshotPrefetchOutcome(
                prepared.snapshot_prefetch_id,
                prepared.snapshot_prefetch_task_id,
                prepared.spectrum_index,
                prepared.snapshot_prefetch_direction,
                NavigationPrefetchOutcome::Consumed,
                NavigationLatencyTimePoint{
                    std::chrono::nanoseconds{
                        prepared
                            .snapshot_prefetch_scheduled_ns}});
        }
        if (result.follow_up_spectrum_index) {
            if (ticket.navigation_trace) {
                ticket.navigation_trace->SetTargetIndex(*result.follow_up_spectrum_index);
            }
            if (ticket.source_load_trace) {
                ticket.source_load_trace->SetTargetIndex(
                    *result.follow_up_spectrum_index);
            }
            (void)QueueSourceLoad(
                ticket.path,
                *result.follow_up_spectrum_index,
                session_.AnnotationPathsForSource(ticket.path),
                ticket.purpose == PendingSourceLoadPurpose::DeferredRestore
                    ? PendingSourceLoadPurpose::DeferredRestore
                    : PendingSourceLoadPurpose::SessionFollowUp,
                ticket.navigation_trace,
                ticket.source_load_trace,
                ticket.prefetch_direction);
        } else if (result.loaded && ticket.prefetch_direction) {
            ScheduleSnapshotPrefetch(
                *ticket.prefetch_direction);
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

std::vector<NavigationPrefetchReport>
ShellUi::TakeNavigationPrefetchReports()
{
    return std::exchange(
        navigation_prefetch_reports_,
        {});
}

void ShellUi::RecordNavigationKeyInput(
    NavigationLatencyInputKind kind,
    NavigationLatencyTimePoint at)
{
    if (kind == NavigationLatencyInputKind::KeyboardPrevious) {
        pending_keyboard_previous_at_ = at;
    } else if (kind == NavigationLatencyInputKind::KeyboardNext) {
        pending_keyboard_next_at_ = at;
    }
}

std::optional<NavigationLatencyTimePoint> ShellUi::TakeNavigationKeyInput(
    NavigationLatencyInputKind kind)
{
    if (kind == NavigationLatencyInputKind::KeyboardPrevious) {
        return std::exchange(pending_keyboard_previous_at_, std::nullopt);
    }
    if (kind == NavigationLatencyInputKind::KeyboardNext) {
        return std::exchange(pending_keyboard_next_at_, std::nullopt);
    }
    return std::nullopt;
}

void ShellUi::RecordSpectrumDrawSubmission(
    std::uint64_t frame_index,
    unsigned int viewport_id,
    SpectrumSnapshotHandle snapshot)
{
    spectrum_draw_submission_ = SpectrumDrawSubmission{
        frame_index,
        viewport_id,
        std::move(snapshot),
    };
    SupersedePresentableNavigationIfSnapshotChanged(
        spectrum_draw_submission_->snapshot);
    SupersedePresentableSourceLoadIfSnapshotChanged(
        spectrum_draw_submission_->snapshot);
}

void ShellUi::SupersedePresentableNavigationIfSnapshotChanged(
    const SpectrumSnapshotHandle& current_snapshot)
{
    if (!presentable_navigation_trace_ ||
        presentable_navigation_snapshot_ == current_snapshot) {
        return;
    }
    (void)presentable_navigation_trace_->MarkTerminal(
        NavigationLatencyOutcome::Superseded);
    presentable_navigation_trace_.reset();
    presentable_navigation_snapshot_.reset();
}

void ShellUi::SupersedePresentableSourceLoadIfSnapshotChanged(
    const SpectrumSnapshotHandle& current_snapshot)
{
    if (!presentable_source_load_trace_ ||
        presentable_source_load_snapshot_ == current_snapshot) {
        return;
    }
    (void)presentable_source_load_trace_->MarkTerminal(
        SourceLoadLatencyOutcome::Superseded);
    presentable_source_load_trace_.reset();
    presentable_source_load_snapshot_.reset();
}

std::vector<NavigationLatencyReport> ShellUi::CompleteFramePresentations(
    std::uint64_t frame_index,
    std::span<const NavigationLatencyPresentation> presentations)
{
    std::vector<NavigationLatencyReport> reports;
    SupersedePresentableNavigationIfSnapshotChanged(
        session_.CurrentSampleSnapshot());
    for (auto trace = navigation_traces_.begin(); trace != navigation_traces_.end();) {
        const bool matching_draw_submission =
            trace->second == presentable_navigation_trace_ &&
            spectrum_draw_submission_ &&
            spectrum_draw_submission_->frame_index == frame_index &&
            spectrum_draw_submission_->snapshot == presentable_navigation_snapshot_;
        if (matching_draw_submission) {
            for (const NavigationLatencyPresentation& presentation : presentations) {
                if (presentation.viewport_id != spectrum_draw_submission_->viewport_id) {
                    continue;
                }
                (void)trace->second->MarkPresentedForViewport(
                    frame_index,
                    presentation.viewport_id,
                    presentation.completed_at);
                break;
            }
        }
        std::optional<NavigationLatencyReport> report = trace->second->TerminalReport();
        if (!report) {
            ++trace;
            continue;
        }
        reports.push_back(std::move(*report));
        if (trace->second == presentable_navigation_trace_) {
            presentable_navigation_trace_.reset();
            presentable_navigation_snapshot_.reset();
        }
        trace = navigation_traces_.erase(trace);
    }
    return reports;
}

std::vector<SourceLoadLatencyReport>
ShellUi::CompleteSourceLoadFramePresentations(
    std::uint64_t frame_index,
    std::span<const NavigationLatencyPresentation> presentations)
{
    std::vector<SourceLoadLatencyReport> reports;
    SupersedePresentableSourceLoadIfSnapshotChanged(
        session_.CurrentSampleSnapshot());
    for (auto trace = source_load_traces_.begin();
         trace != source_load_traces_.end();) {
        const bool matching_draw_submission =
            trace->second == presentable_source_load_trace_ &&
            spectrum_draw_submission_ &&
            spectrum_draw_submission_->frame_index == frame_index &&
            spectrum_draw_submission_->snapshot ==
                presentable_source_load_snapshot_;
        if (matching_draw_submission) {
            for (const NavigationLatencyPresentation& presentation :
                 presentations) {
                if (presentation.viewport_id !=
                    spectrum_draw_submission_->viewport_id) {
                    continue;
                }
                (void)trace->second->MarkPresentedForViewport(
                    frame_index,
                    presentation.viewport_id,
                    presentation.completed_at);
                break;
            }
        }
        std::optional<SourceLoadLatencyReport> report =
            trace->second->TerminalReport();
        if (!report) {
            ++trace;
            continue;
        }
        reports.push_back(std::move(*report));
        if (trace->second == presentable_source_load_trace_) {
            presentable_source_load_trace_.reset();
            presentable_source_load_snapshot_.reset();
        }
        trace = source_load_traces_.erase(trace);
    }
    return reports;
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

NavigationLatencyTraceHandle ShellUi::StartNavigationTrace(
    const SourceCollectionSessionResult& result,
    std::optional<std::size_t> from_index,
    NavigationLatencyTimePoint requested_at,
    NavigationLatencyTimePoint target_resolved_at,
    NavigationTargetResolutionReport target_resolution,
    std::optional<NavigationTraceOrigin> navigation_origin)
{
    if (!latency_tracing_enabled_ || !navigation_origin || !from_index ||
        !result.follow_up_spectrum_index) {
        return {};
    }

    const bool keyboard_origin =
        navigation_origin->kind == NavigationLatencyInputKind::KeyboardPrevious ||
        navigation_origin->kind == NavigationLatencyInputKind::KeyboardNext;
    if (keyboard_origin && !navigation_origin->input_at) {
        return {};
    }
    const std::uint64_t navigation_id = next_navigation_trace_id_++;
    const NavigationLatencyTimePoint input_at = navigation_origin->input_at.value_or(requested_at);
    auto trace = std::make_shared<NavigationLatencyTrace>(
        navigation_id,
        *from_index,
        *result.follow_up_spectrum_index,
        navigation_origin->kind,
        input_at,
        requested_at,
        target_resolved_at,
        std::move(target_resolution));
    navigation_traces_.emplace(navigation_id, trace);
    return trace;
}

SourceLoadLatencyTraceHandle ShellUi::StartSourceLoadTrace(
    std::size_t target_index,
    NavigationLatencyTimePoint accepted_at)
{
    if (!latency_tracing_enabled_) {
        return {};
    }
    const std::uint64_t source_load_id = next_source_load_trace_id_++;
    auto trace = std::make_shared<SourceLoadLatencyTrace>(
        source_load_id,
        target_index,
        SourceLoadLatencyRequestKind::ExplicitOpen,
        accepted_at);
    source_load_traces_.emplace(source_load_id, trace);
    return trace;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommand(
    SourceCollectionSessionIntent command,
    std::optional<NavigationTraceOrigin> navigation_origin)
{
    const std::optional<SampleNavigationDirection>
        prefetch_direction = navigation_origin
        ? PrefetchDirectionForInputKind(navigation_origin->kind)
        : std::nullopt;
    CancelSnapshotPrefetch();
    const bool trace_requested =
        latency_tracing_enabled_ && navigation_origin.has_value();
    const NavigationLatencyTimePoint requested_at = trace_requested
        ? NavigationLatencyTrace::Now()
        : NavigationLatencyTimePoint{};
    NavigationTargetResolutionReport target_resolution;
    std::optional<std::size_t> from_index;
    if (trace_requested) {
        from_index = session_.EffectiveSampleNavigationIndex();
        target_resolution.effective_index_ns =
            ElapsedNavigationResolutionNanoseconds(requested_at);
    }
    const NavigationLatencyTimePoint pending_activation_started_at =
        trace_requested ? NavigationLatencyTrace::Now()
                        : NavigationLatencyTimePoint{};
    const bool supersedes_source_activation =
        session_.SupersedesPendingSourceActivation(command);
    if (supersedes_source_activation) {
        BeginSourceActivationIntent(false);
    }
    if (trace_requested) {
        target_resolution.pending_activation_supersede_ns +=
            ElapsedNavigationResolutionNanoseconds(
                pending_activation_started_at);
    }
    SourceCollectionSessionResult result = session_.Submit(
        std::move(command),
        trace_requested ? &target_resolution : nullptr);
    const NavigationLatencyTimePoint target_resolved_at = trace_requested
        ? NavigationLatencyTrace::Now()
        : NavigationLatencyTimePoint{};
    NavigationLatencyTraceHandle navigation_trace = StartNavigationTrace(
        result,
        from_index,
        requested_at,
        target_resolved_at,
        std::move(target_resolution),
        std::move(navigation_origin));
    if (deferred_restore_active_ && supersedes_source_activation) {
        const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
        if (snapshot && !snapshot->source.path.empty()) {
            deferred_restore_active_path_ = snapshot->source.path;
        }
    }
    QueueSessionFollowUp(
        result,
        false,
        std::move(navigation_trace),
        prefetch_direction);
    RetireSessionResources(result);
    session_view_cache_dirty_ = true;
    HandleSessionAction(result.action);
    return result;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommandForPanel(
    SourceCollectionSessionIntent command,
    std::optional<NavigationLatencyInputKind> navigation_kind)
{
    const std::optional<SampleNavigationDirection>
        prefetch_direction = navigation_kind
        ? PrefetchDirectionForInputKind(*navigation_kind)
        : std::nullopt;
    CancelSnapshotPrefetch();
    const bool trace_requested =
        latency_tracing_enabled_ && navigation_kind.has_value();
    const NavigationLatencyTimePoint requested_at = trace_requested
        ? NavigationLatencyTrace::Now()
        : NavigationLatencyTimePoint{};
    NavigationTargetResolutionReport target_resolution;
    std::optional<std::size_t> from_index;
    if (trace_requested) {
        from_index = session_.EffectiveSampleNavigationIndex();
        target_resolution.effective_index_ns =
            ElapsedNavigationResolutionNanoseconds(requested_at);
    }
    const NavigationLatencyTimePoint pending_activation_started_at =
        trace_requested ? NavigationLatencyTrace::Now()
                        : NavigationLatencyTimePoint{};
    const bool supersedes_source_activation =
        session_.SupersedesPendingSourceActivation(command);
    if (supersedes_source_activation) {
        BeginSourceActivationIntent(false);
    }
    if (trace_requested) {
        target_resolution.pending_activation_supersede_ns +=
            ElapsedNavigationResolutionNanoseconds(
                pending_activation_started_at);
    }
    SourceCollectionSessionResult result = session_.Submit(
        std::move(command),
        trace_requested ? &target_resolution : nullptr);
    const NavigationLatencyTimePoint target_resolved_at = trace_requested
        ? NavigationLatencyTrace::Now()
        : NavigationLatencyTimePoint{};
    NavigationLatencyTraceHandle navigation_trace = StartNavigationTrace(
        result,
        from_index,
        requested_at,
        target_resolved_at,
        std::move(target_resolution),
        navigation_kind
            ? std::optional<NavigationTraceOrigin>{NavigationTraceOrigin{
                  *navigation_kind,
                  requested_at}}
            : std::nullopt);
    if (deferred_restore_active_ && supersedes_source_activation) {
        const SpectrumSnapshotHandle snapshot = session_.CurrentSourceSnapshot();
        if (snapshot && !snapshot->source.path.empty()) {
            deferred_restore_active_path_ = snapshot->source.path;
        }
    }
    QueueSessionFollowUp(
        result,
        false,
        std::move(navigation_trace),
        prefetch_direction);
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
        source_collection_panel_ui_.SyncNavigationInputs(SessionView());
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

    RenderMainMenuBar(status);

    const ImGuiID dockspace_id = ImGui::GetID("SpecForgeDockSpaceSampleNavigationV1");
    const ImVec2 dockspace_size = ImGui::GetContentRegionAvail();

    if (!layout_seeded_) {
        layout_seeded_ = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            SeedInitialDockLayout(dockspace_id, dockspace_size);
        }
    }

    ImGui::DockSpace(dockspace_id, dockspace_size, ImGuiDockNodeFlags_None);

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
    const bool plot_visible = ImGui::Begin(kImmersivePlotWindow, nullptr, flags);
    ImGui::PopStyleVar(3);
    if (!plot_visible) {
        ImGui::End();
        return;
    }
    const unsigned int viewport_id = ImGui::GetWindowViewport()->ID;

    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    const bool plot_submitted = RenderSpectrumPlot(
        snapshot,
        spectrum_view_session_.PlotStateForRender(),
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        spectrum_view_session_.PlotStyleForRender(),
        SpectrumPlotOverlays{
            .spectral_lines = spectral_lines.visible_markers.data(),
            .spectral_line_count = spectral_lines.visible_markers.size(),
            .show_spectral_line_labels = spectral_lines.marker_labels_visible,
            .layout_scope_id = spectral_lines.layout_scope_id,
            .spectral_line_label_font = spectral_line_label_font_,
        },
        MakeImmersivePlotDisplayOptions(),
        touchpad_gestures_);
    if (plot_submitted) {
        RecordSpectrumDrawSubmission(status.frame_index, viewport_id, snapshot);
    }

    if (status.profile_open) {
        constexpr const char* kRecordingLabel = "REC  Performance";
        const ImVec2 text_size = ImGui::CalcTextSize(kRecordingLabel);
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 label_min(
            window_pos.x + window_size.x - text_size.x - 30.0f,
            window_pos.y + 12.0f);
        const ImVec2 label_max(label_min.x + text_size.x + 18.0f, label_min.y + text_size.y + 10.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(label_min, label_max, IM_COL32(22, 22, 24, 220), 4.0f);
        draw_list->AddCircleFilled(
            ImVec2(label_min.x + 9.0f, label_min.y + 5.0f + text_size.y * 0.5f),
            3.5f,
            IM_COL32(235, 64, 58, 255));
        draw_list->AddText(ImVec2(label_min.x + 17.0f, label_min.y + 5.0f), IM_COL32_WHITE, kRecordingLabel);
    }

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

void ShellUi::RenderMainMenuBar(const ShellStatus& status)
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

    if (ImGui::MenuItem("Settings")) {
        settings_panel_ui_.Open();
    }

    RenderTopBarStatus(status, source_load_queue_.NeedsService(), source_load_error_);

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
        [this](SourceCollectionSessionIntent command, SampleNavigationRequestKind kind) {
            const NavigationLatencyInputKind input_kind =
                kind == SampleNavigationRequestKind::Previous
                ? NavigationLatencyInputKind::UiPrevious
                : NavigationLatencyInputKind::UiNext;
            return SubmitSessionCommandForPanel(std::move(command), input_kind);
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
        [this](SourceCollectionSessionIntent command) {
            return SubmitSessionCommandForPanel(
                std::move(command),
                NavigationLatencyInputKind::AutoAdvance);
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
    if (!ImGui::Begin(kMainPlotWindow)) {
        ImGui::End();
        return;
    }
    const unsigned int viewport_id = ImGui::GetWindowViewport()->ID;
    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    const bool plot_submitted = RenderSpectrumPlot(
        snapshot,
        spectrum_view_session_.PlotStateForRender(),
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        spectrum_view_session_.PlotStyleForRender(),
        SpectrumPlotOverlays{
            .spectral_lines = spectral_lines.visible_markers.data(),
            .spectral_line_count = spectral_lines.visible_markers.size(),
            .show_spectral_line_labels = spectral_lines.marker_labels_visible,
            .layout_scope_id = spectral_lines.layout_scope_id,
            .spectral_line_label_font = spectral_line_label_font_,
        },
        {},
        touchpad_gestures_);
    if (plot_submitted) {
        RecordSpectrumDrawSubmission(status.frame_index, viewport_id, snapshot);
    }
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

void ShellUi::RenderSettingsPanel(const ShellStatus& status)
{
    settings_panel_ui_.Render({
        .profile_open = status.profile_open,
        .profile_stopping = status.profile_stopping,
        .profile_path = status.profile_path,
        .profile_status_message = status.profile_status_message,
    });
    if (settings_panel_ui_.TakeProfileOutputDirectorySelectionRequest()) {
        if (std::optional<std::filesystem::path> directory =
                ShowFolderPicker(L"Choose performance profile output folder")) {
            settings_panel_ui_.ApplyProfileOutputDirectorySelection(
                std::move(*directory));
        }
    }
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
        (void)SubmitSessionCommand(
            SourceCollectionSessionIntent::UpdateSampleNavigation(
                SampleNavigationIntent::Move(SampleNavigationRequest::Previous())),
            NavigationTraceOrigin{
                NavigationLatencyInputKind::KeyboardPrevious,
                TakeNavigationKeyInput(NavigationLatencyInputKind::KeyboardPrevious)});
        return;
    case SampleWorkflowShortcutKind::NextSample:
        (void)SubmitSessionCommand(
            SourceCollectionSessionIntent::UpdateSampleNavigation(
                SampleNavigationIntent::Move(SampleNavigationRequest::Next())),
            NavigationTraceOrigin{
                NavigationLatencyInputKind::KeyboardNext,
                TakeNavigationKeyInput(NavigationLatencyInputKind::KeyboardNext)});
        return;
    case SampleWorkflowShortcutKind::UndoLabelWrite:
        (void)SubmitSessionCommand(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            ActiveSampleWorkflowIntent::UndoLastLabelWrite()));
        return;
    case SampleWorkflowShortcutKind::AssignLabel:
        (void)SubmitSessionCommand(
            SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
                ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(
                    sample_workflow_shortcut_.label_code)),
            NavigationTraceOrigin{NavigationLatencyInputKind::AutoAdvance});
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
