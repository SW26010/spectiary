#include "ui/shell_ui.h"
#include "ui/spectrum_persistence_migration.h"

#include "app/runtime_paths.h"
#include "platform/win32_process_launcher.h"
#include "platform/win32_text.h"
#include "ui/immersive_context_overlay.h"
#include "ui/profile_recording_ui_state.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/theme.h"
#include "ui/top_bar_status_hover.h"
#include "ui/top_bar_status_layout.h"

#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kDockHostWindow = "Main Dock Host###DockHostV2";
constexpr const char* kMainPlotWindow = "Spectrum###SpectrumV2";
constexpr const char* kInfoTagsWindow = "Info###InfoTagsV2";
constexpr const char* kCurveDisplayWindow = "Curve Display###SmoothingV1";
constexpr std::array<const char*, kApplicationPanelCount>
    kApplicationPanelWindowIds{
        "###FilesV2",
        "###NavigationV1",
        "###AnnotationsV1",
        "###LabelingV1",
        "###FiltersV1",
        "###SampleSortingV1",
        "###SmoothingV1",
        "###InfoTagsV2",
        "###SpectralLinesV2",
    };

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

void RenderUiText(UiLanguage language, UiTextId text_id)
{
    const std::string_view text = UiText(language, text_id);
    ImGui::TextUnformatted(
        text.data(),
        text.data() + text.size());
}

void RenderDisabledUiText(
    UiLanguage language,
    UiTextId text_id)
{
    const std::string_view text = UiText(language, text_id);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(text.size()),
        text.data());
}

std::string_view LabelValueSeparator(UiLanguage language)
{
    return language == UiLanguage::SimplifiedChinese
        ? std::string_view{"："}
        : std::string_view{": "};
}

bool RenderTopBarStatus(
    const ShellStatus& status,
    bool source_load_active,
    std::span<const SourceCollectionLoadFailure>
        source_load_failures,
    const LocalUserStateHealthView& persistence,
    UiLanguage language)
{
    const SemanticPalette& palette =
        ActiveSemanticPalette();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImRect menu_bar_rect = window->MenuBarRect();
    const ImRect menu_clip_rect = window->ClipRect;
    const float menu_end_x = ImGui::GetCursorScreenPos().x;
    const float right_x = menu_clip_rect.Max.x - style.FramePadding.x;
    const float available_width =
        std::max(0.0f, right_x - menu_end_x - style.ItemSpacing.x * 2.0f);

    const bool operation_error =
        !source_load_failures.empty();
    const bool persistence_retrying =
        persistence.kind ==
        LocalUserStateHealthKind::Retrying;
    const bool persistence_warning =
        persistence.kind ==
        LocalUserStateHealthKind::Warning;
    const bool persistence_recovered =
        persistence.kind ==
        LocalUserStateHealthKind::Recovered;
    const bool persistence_important =
        persistence_retrying || persistence_warning ||
        persistence_recovered;
    const bool show_persistence =
        !operation_error && !source_load_active &&
        persistence_important;
    const bool operation_important =
        operation_error || source_load_active ||
        persistence_important;
    std::string operation_text(
        UiText(language, UiTextId::Ready));
    if (operation_error) {
        operation_text =
            UiText(language, UiTextId::LoadFailed);
    } else if (source_load_active) {
        operation_text =
            UiText(language, UiTextId::LoadingSource);
    } else if (persistence_retrying) {
        operation_text =
            UiText(language, UiTextId::StateSaveRetrying);
    } else if (persistence_warning) {
        operation_text =
            UiText(language, UiTextId::StateWarning);
    } else if (persistence_recovered) {
        operation_text =
            UiText(language, UiTextId::StateRecovered);
    }
    std::optional<std::string> frame_text;
    if (status.application_frame_timing_sample) {
        frame_text = FormatTopBarFrameRate(
            *status.application_frame_timing_sample,
            UiText(language, UiTextId::ApplicationFrameRate));
    }
    const std::string dimensions_text =
        std::to_string(status.client_width) + "x" + std::to_string(status.client_height);
    ProfileRecordingUiPresentation recording_presentation =
        ResolveProfileRecordingUiPresentation(status.profile_open, status.profile_stopping);
    recording_presentation.status_text =
        status.profile_stopping
        ? UiText(
            language,
            UiTextId::FinishingRecording)
        : (status.profile_open
            ? UiText(
                language,
                UiTextId::PerformanceRecording)
            : std::string_view{});

    constexpr float kRecordingIndicatorRadius = 4.0f;
    constexpr float kRecordingIndicatorSpacing = 5.0f;
    const float separator_text_width = ImGui::CalcTextSize("|").x;
    const float recording_indicator_width = recording_presentation.show_recording_indicator
        ? kRecordingIndicatorRadius * 2.0f + kRecordingIndicatorSpacing
        : 0.0f;
    const TopBarStatusWidths widths{
        .operation = ImGui::CalcTextSize(operation_text.c_str()).x,
        .frame = frame_text
            ? ImGui::CalcTextSize(frame_text->c_str()).x
            : 0.0f,
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
        return false;
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
    if (operation_error) {
        if (layout.show_operation) {
            const float operation_start_x = cursor_x;
            draw_text(
                operation_text,
                ImGui::GetColorU32(palette.error));
            operation_rect = ImRect(
                ImVec2(operation_start_x, text_y),
                ImVec2(cursor_x, text_y + text_height));
        }
    } else if (layout.show_operation) {
        const float operation_start_x = cursor_x;
        draw_text(
            operation_text,
            show_persistence
                ? (persistence_recovered
                       ? ImGui::GetColorU32(palette.success)
                       : ImGui::GetColorU32(palette.warning))
                : ImGui::GetColorU32(
                      source_load_active
                          ? ImGuiCol_Text
                          : ImGuiCol_TextDisabled));
        operation_rect = ImRect(
            ImVec2(operation_start_x, text_y),
            ImVec2(cursor_x, text_y + text_height));
    }
    if (layout.show_frame && frame_text) {
        draw_separator();
        draw_text(*frame_text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
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
                ImGui::GetColorU32(palette.error));
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
        IsTopBarStatusHoverTarget(
            operation_rect.Min,
            operation_rect.Max)) {
        const std::string_view dismiss_hint = UiText(
            language,
            UiTextId::LoadFailedDismissHint);
        const std::string source_load_error =
            FormatSourceCollectionLoadFailures(
                language,
                source_load_failures);
        ImGui::SetTooltip(
            "%.*s\n%s",
            static_cast<int>(dismiss_hint.size()),
            dismiss_hint.data(),
            source_load_error.c_str());
    }
    if (layout.show_operation && show_persistence &&
        IsTopBarStatusHoverTarget(
            operation_rect.Min,
            operation_rect.Max)) {
        std::string tooltip;
        for (const LocalUserStateHealthMessage& message :
             persistence.messages) {
            if (!tooltip.empty()) {
                tooltip.push_back('\n');
            }
            tooltip +=
                FormatLocalUserStateHealthMessage(
                    language,
                    message);
        }
        ImGui::SetTooltip(
            "%s",
            tooltip.empty()
                ? operation_text.c_str()
                : tooltip.c_str());
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
    return layout.show_operation && operation_error &&
        IsTopBarStatusLeftClickTarget(
            operation_rect.Min,
            operation_rect.Max);
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

std::string_view MetadataDisplayValue(
    UiLanguage language,
    std::string_view value)
{
    if (value == "not_applied") {
        return UiText(
            language,
            UiTextId::MetadataNotApplied);
    }
    if (value == "available_not_applied") {
        return UiText(
            language,
            UiTextId::MetadataAvailableNotApplied);
    }
    if (value == "unreliable_not_applied") {
        return UiText(
            language,
            UiTextId::MetadataUnreliableNotApplied);
    }
    if (value == "radial_velocity_low_speed") {
        return UiText(
            language,
            UiTextId::MetadataLowSpeedApproximation);
    }
    if (value == "pipeline_redshift") {
        return UiText(
            language,
            UiTextId::MetadataPipelineRedshift);
    }
    if (value == "zwarning_nonzero") {
        return UiText(
            language,
            UiTextId::MetadataZWarningNonzero);
    }
    if (value == "invalid_pipeline_redshift") {
        return UiText(
            language,
            UiTextId::MetadataInvalidPipelineRedshift);
    }
    return value;
}

void RenderMetadataLine(
    UiLanguage language,
    UiTextId label_id,
    std::string_view value,
    std::string_view suffix = {})
{
    if (value.empty()) {
        return;
    }
    const std::string_view label = UiText(language, label_id);
    const std::string_view separator =
        LabelValueSeparator(language);
    value = MetadataDisplayValue(language, value);
    if (suffix.empty()) {
        ImGui::Text(
            "%.*s%.*s%.*s",
            static_cast<int>(label.size()),
            label.data(),
            static_cast<int>(separator.size()),
            separator.data(),
            static_cast<int>(value.size()),
            value.data());
    } else {
        ImGui::Text(
            "%.*s%.*s%.*s %.*s",
            static_cast<int>(label.size()),
            label.data(),
            static_cast<int>(separator.size()),
            separator.data(),
            static_cast<int>(value.size()),
            value.data(),
            static_cast<int>(suffix.size()),
            suffix.data());
    }
}

std::string_view SeverityLabel(
    UiLanguage language,
    SpectrumDiagnosticSeverity severity)
{
    switch (severity) {
    case SpectrumDiagnosticSeverity::Info:
        return UiText(
            language,
            UiTextId::DiagnosticSeverityInfo);
    case SpectrumDiagnosticSeverity::Warning:
        return UiText(
            language,
            UiTextId::DiagnosticSeverityWarning);
    case SpectrumDiagnosticSeverity::Error:
        return UiText(
            language,
            UiTextId::DiagnosticSeverityError);
    default:
        return UiText(
            language,
            UiTextId::DiagnosticSeverityUnknown);
    }
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

std::optional<std::filesystem::path> ShowSourceFilePicker(
    UiLanguage language)
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

    const std::array<std::wstring, 5> filter_names = {
        Utf8ToWide(UiText(
            language,
            UiTextId::SpectrumSourcesFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::NumpyArraysFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::CsvFilesFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::FitsFilesFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::AllFilesFilter)),
    };
    const std::array<COMDLG_FILTERSPEC, 5>
        source_filters = {{
            {
                filter_names[0].c_str(),
                L"*.npy;*.csv;*.fits;*.fit;*.fts;*.fits.gz",
            },
            {filter_names[1].c_str(), L"*.npy"},
            {filter_names[2].c_str(), L"*.csv"},
            {
                filter_names[3].c_str(),
                L"*.fits;*.fit;*.fts;*.fits.gz",
            },
            {filter_names[4].c_str(), L"*.*"},
        }};
    const std::wstring title = Utf8ToWide(UiText(
        language,
        UiTextId::AddSourceFileDialog));
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(
        static_cast<UINT>(source_filters.size()),
        source_filters.data());
    dialog->SetFileTypeIndex(1);

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowFolderPicker(
    UiLanguage language,
    UiTextId title_id)
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
    const std::wstring title = Utf8ToWide(
        UiText(language, title_id));
    dialog->SetTitle(title.c_str());

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowSourceFolderPicker(
    UiLanguage language)
{
    return ShowFolderPicker(
        language,
        UiTextId::AddSourceFolderDialog);
}

std::optional<std::filesystem::path> ShowAnnotationFilePicker(
    UiLanguage language)
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

    const std::array<std::wstring, 2> filter_names = {
        Utf8ToWide(UiText(
            language,
            UiTextId::SampleAnnotationFilesFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::AllFilesFilter)),
    };
    const std::array<COMDLG_FILTERSPEC, 2>
        annotation_filters = {{
            {filter_names[0].c_str(), L"*.npy;*.asdf;*.csv"},
            {filter_names[1].c_str(), L"*.*"},
        }};
    const std::wstring title = Utf8ToWide(UiText(
        language,
        UiTextId::OpenAnnotationFileDialog));
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(
        static_cast<UINT>(annotation_filters.size()),
        annotation_filters.data());
    dialog->SetFileTypeIndex(1);

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowLabelOutputFilePicker(
    UiLanguage language,
    std::string_view suggested_filename)
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

    const std::array<std::wstring, 2> filter_names = {
        Utf8ToWide(UiText(
            language,
            UiTextId::SampleAnnotationFilesFilter)),
        Utf8ToWide(UiText(
            language,
            UiTextId::AllFilesFilter)),
    };
    const std::array<COMDLG_FILTERSPEC, 2>
        label_output_filters = {{
            {filter_names[0].c_str(), L"*.asdf"},
            {filter_names[1].c_str(), L"*.*"},
        }};
    const std::wstring title = Utf8ToWide(UiText(
        language,
        UiTextId::SaveLabelingAnnotationDialog));
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(
        static_cast<UINT>(label_output_filters.size()),
        label_output_filters.data());
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"asdf");
    const std::wstring wide_suggested_filename =
        Utf8ToWide(suggested_filename);
    dialog->SetFileName(wide_suggested_filename.c_str());

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(show_result)) {
        return std::nullopt;
    }

    return DialogResultPath(dialog.Get());
}

std::optional<std::filesystem::path> ShowLabelValuesExportFilePicker(
    UiLanguage language,
    SampleLabelExportFormat format,
    std::string_view suggested_filename)
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
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
            FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
    }

    const bool csv_export =
        format == SampleLabelExportFormat::Csv;
    const std::wstring filter_name = Utf8ToWide(UiText(
        language,
        csv_export
            ? UiTextId::CsvFilesFilter
            : UiTextId::NumpyLabelArraysFilter));
    const COMDLG_FILTERSPEC export_filter = {
        filter_name.c_str(),
        csv_export ? L"*.csv" : L"*.npy"};
    const std::wstring title = Utf8ToWide(UiText(
        language,
        UiTextId::ExportLabelValuesDialog));
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(1, &export_filter);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(
        csv_export ? L"csv" : L"npy");
    const std::wstring wide_suggested_filename =
        Utf8ToWide(suggested_filename);
    dialog->SetFileName(wide_suggested_filename.c_str());

    const HRESULT show_result = dialog->Show(GetActiveWindow());
    if (show_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
        FAILED(show_result)) {
        return std::nullopt;
    }
    const std::optional<std::filesystem::path> path =
        DialogResultPath(dialog.Get());
    return path
        ? std::optional<std::filesystem::path>{
              EnsureSampleLabelExportPathExtension(
                  *path,
                  format)}
        : std::nullopt;
}

void RenderDiagnosticRows(
    const SpectrumSnapshotHandle& snapshot,
    UiLanguage language)
{
    if (!snapshot || snapshot->diagnostics.empty()) {
        RenderDisabledUiText(
            language,
            UiTextId::NoDiagnostics);
        return;
    }

    for (const SpectrumDiagnostic& diagnostic : snapshot->diagnostics) {
        const std::string_view severity = SeverityLabel(
            language,
            diagnostic.severity);
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

SourceCollectionSession SourceCollectionSessionForRuntimePaths(
    const RuntimePaths& paths,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy)
{
    return SourceCollectionSession(
        paths.source_session_state_path,
        paths.sample_navigation_state_path,
        paths.sample_labeling_state_path,
        paths.sample_workflow_state_path,
        labeling_state_cache_load_policy, paths);
}

SourceCollectionLoadQueue SourceCollectionLoadQueueForRuntimePaths(
    const RuntimePaths& paths,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy)
{
    return SourceCollectionLoadQueue({
        .labeling_state_cache_path =
            paths.sample_labeling_state_path,
        .workflow_state_cache_path =
            paths.sample_workflow_state_path,
        .navigation_state_cache_path =
            paths.sample_navigation_state_path,
        .labeling_state_cache_load_policy =
            labeling_state_cache_load_policy,
        .runtime_paths = paths,
    });
}

}  // namespace

std::string ShellLocalStateFlushResult::FailureMessage(
    UiLanguage language) const
{
    if (all_saved()) {
        return {};
    }
    std::string message(
        UiText(
            language,
            UiTextId::LocalStateFlushIntro));
    message += "\n\n";
    message += UiText(
        language,
        UiTextId::UnsavedAreas);
    const auto append_area = [&](
                                 LocalUserStateArea area) {
        message += "\n- ";
        message += UiText(language, area);
    };
    if (!application_settings.language_saved) {
        append_area(LocalUserStateArea::Language);
    }
    if (!application_settings.appearance_saved) {
        append_area(LocalUserStateArea::Appearance);
    }
    if (!application_settings.ui_scale_saved) {
        append_area(LocalUserStateArea::UiScale);
    }
    if (!application_settings.input_saved) {
        append_area(LocalUserStateArea::Input);
    }
    if (!application_settings.external_source_saved) {
        append_area(LocalUserStateArea::ExternalSource);
    }
    if (!application_settings.profile_output_directory_saved) {
        append_area(LocalUserStateArea::ProfileOutputDirectory);
    }
    if (!application_settings.panel_visibility_saved) {
        append_area(LocalUserStateArea::PanelVisibility);
    }
    if (!source_collection.source_session_saved) {
        append_area(LocalUserStateArea::SourceSession);
    }
    if (!source_collection.navigation_saved) {
        append_area(LocalUserStateArea::SampleNavigation);
    }
    if (!source_collection.labeling_saved) {
        append_area(LocalUserStateArea::SampleLabeling);
    }
    if (!source_collection.workflow_saved) {
        append_area(LocalUserStateArea::SampleWorkflow);
    }
    if (!spectrum_plot_preferences_saved) {
        append_area(LocalUserStateArea::SpectrumPlotPreferences);
    }
    if (!spectrum_viewport_state_saved) {
        append_area(LocalUserStateArea::SpectrumViewportState);
    }
    if (!spectral_lines_saved) {
        append_area(LocalUserStateArea::SpectralLines);
    }
    message += "\n\n";
    message += UiText(
        language,
        UiTextId::LocalStateMayNotRestore);
    return message;
}

ShellUi::ShellUi(
    const SpectiaryStartup& startup,
    PlotTouchpadGestureSource* touchpad_gestures,
    SampleLabelingStateCacheLoadPolicy
        labeling_state_cache_load_policy)
    : session_(SourceCollectionSessionForRuntimePaths(
          startup.runtime_paths(),
          labeling_state_cache_load_policy)),
      source_activation_(
          session_,
          SourceCollectionLoadQueueForRuntimePaths(
              startup.runtime_paths(),
              labeling_state_cache_load_policy)),
      panel_session_interaction_(
          session_,
          source_activation_),
      spectral_lines_panel_(
          startup.runtime_paths()
              .public_spectral_line_catalog_path,
          startup.runtime_paths()
              .spectral_line_user_state_path),
      source_collection_panel_ui_(startup.runtime_paths()),
      settings_panel_ui_(
          SettingsPanelEnvironmentForStartup(startup)),
      application_settings_(
          ApplicationSettingsStorageForRuntimePaths(
              startup.runtime_paths())),
      spectrum_plot_preferences_path_(startup.runtime_paths().spectrum_plot_preferences_path),
      spectrum_viewport_state_path_(
          startup.runtime_paths().spectrum_viewport_state_path),
      spectrum_viewport_state_persistence_(
          std::chrono::milliseconds(250),
          std::chrono::seconds(1)),
      touchpad_gestures_(touchpad_gestures),
      sample_workflow_panel_ui_(startup.runtime_paths())
{
    SpectrumViewportStateLoadResult viewport_state =
        LoadSpectrumViewportState(
            spectrum_viewport_state_path_);
    auto preferences = LoadSpectrumPlotPreferences(spectrum_plot_preferences_path_);
    MigrateLegacySpectrumViewState(
        startup.runtime_paths(), preferences, viewport_state);
    spectrum_viewport_state_writeback_allowed_ =
        viewport_state.issue_kind ==
        VersionedJsonCacheLoadIssueKind::None;
    if (!viewport_state.warning.empty()) {
        spectrum_viewport_state_persistence_.SetLoadWarning(
            std::move(viewport_state.warning),
            std::move(
                viewport_state.diagnostic_detail));
    }
    spectrum_plot_preferences_writeback_allowed_ =
        preferences.issue_kind == VersionedJsonCacheLoadIssueKind::None;
    spectrum_plot_preferences_persistence_.SetLoadWarning(
        std::move(preferences.warning), std::move(preferences.diagnostic_detail));
    spectrum_view_session_.Submit(
        SpectrumViewSessionCommand::SetPlotColors(preferences.state.plot_colors));
    observed_plot_colors_ = preferences.state.plot_colors;
    if (viewport_state.state.locked) {
        startup_spectrum_viewport_state_ =
            std::move(viewport_state.state);
        startup_spectrum_view_mutation_revision_ =
            spectrum_view_session_.
                ViewportMutationRevision();
    }
    BindSourceCollectionActivationPresentationLifecycle(
        source_activation_,
        spectrum_view_session_,
        [this](
            std::optional<std::string>
                source_collection_identity) {
            RestoreDeferredSpectrumViewport(
                std::move(
                    source_collection_identity));
        });
    observed_viewport_state_ = CurrentSpectrumViewportState();
    observed_viewport_revision_ = spectrum_view_session_.ViewportMutationRevision();
    BeginDeferredSourceRestore();
}

ShellUi::ShellUi(
    SourceCollectionSession session,
    SourceCollectionLoadQueue source_load_queue)
    : session_(std::move(session)),
      source_activation_(
          session_,
          std::move(source_load_queue)),
      panel_session_interaction_(
          session_,
          source_activation_),
      spectral_lines_panel_(
          std::filesystem::path{},
          std::filesystem::path{}),
      settings_panel_ui_(
          SettingsPanelEnvironment{}),
      application_settings_(
          ApplicationSettingsStorage{.persistent = false}),
      spectrum_viewport_state_persistence_(
          std::chrono::milliseconds(250),
          std::chrono::seconds(1)),
      persist_local_state_(false)
{
    BindSourceCollectionActivationPresentationLifecycle(
        source_activation_,
        spectrum_view_session_,
        [this](
            std::optional<std::string>
                source_collection_identity) {
            RestoreDeferredSpectrumViewport(
                std::move(
                    source_collection_identity));
        });
}

ShellUi::~ShellUi()
{
    const ShellLocalStateFlushResult result =
        FlushLocalState();
    if (!result.all_saved()) {
        std::string message = result.FailureMessage(
            application_settings_.View().language);
        if (!message.empty()) {
            message.push_back('\n');
            OutputDebugStringA(message.c_str());
        }
    }
}

SpectrumViewportState
ShellUi::CurrentSpectrumViewportState() const
{
    SpectrumViewportState state;
    // Until source restoration completes, retain the saved snapshot unless a
    // live viewport intent has superseded it. Closing early must not erase it.
    if (startup_spectrum_viewport_state_ &&
        startup_spectrum_view_mutation_revision_ ==
            spectrum_view_session_.ViewportMutationRevision()) {
        return *startup_spectrum_viewport_state_;
    }
    const std::optional<PlotViewLimits> locked_limits =
        spectrum_view_session_.LockedViewportLimits();
    const std::optional<std::string>
        source_collection_identity =
            session_.CurrentSourceCollectionIdentity();
    if (locked_limits && source_collection_identity) {
        state.locked = true;
        state.source_collection_identity =
            *source_collection_identity;
        state.limits = *locked_limits;
    }
    return state;
}

LocalUserStatePersistenceLifecycle::SaveResult
ShellUi::SaveSpectrumViewportState()
{
    std::string error;
    const bool saved = spectiary::SaveSpectrumViewportState(
        spectrum_viewport_state_path_,
        CurrentSpectrumViewportState(),
        &error);
    return {
        .saved = saved,
        .error = std::move(error),
    };
}

LocalUserStatePersistenceLifecycle::SaveResult
ShellUi::SaveSpectrumPlotPreferences()
{
    std::string error;
    const bool saved = spectiary::SaveSpectrumPlotPreferences(
        spectrum_plot_preferences_path_,
        SpectrumPlotPreferences{spectrum_view_session_.View().plot_colors}, &error);
    return {.saved = saved, .error = std::move(error)};
}

void ShellUi::ObserveSpectrumPersistenceChanges(
    LocalUserStateSaveScheduler::TimePoint now)
{
    const auto colors = spectrum_view_session_.View().plot_colors;
    if (colors != observed_plot_colors_) {
        observed_plot_colors_ = colors;
        spectrum_plot_preferences_persistence_.MarkDirtyAt(now);
    }
    const auto viewport = CurrentSpectrumViewportState();
    const auto revision = spectrum_view_session_.ViewportMutationRevision();
    // Identity changes and fitted/restored limits can change the persisted
    // snapshot without advancing the user-interaction revision.
    if (revision != observed_viewport_revision_ ||
        viewport != observed_viewport_state_) {
        observed_viewport_revision_ = revision;
        observed_viewport_state_ = viewport;
        spectrum_viewport_state_persistence_.MarkDirtyAt(now);
    }
}

ShellLocalStateFlushResult ShellUi::FlushLocalState()
{
    if (local_state_flush_result_) {
        return *local_state_flush_result_;
    }

    ShellLocalStateFlushResult result;
    if (persist_local_state_) {
        ObserveSpectrumPersistenceChanges(LocalUserStateSaveScheduler::Clock::now());
        result.application_settings =
            application_settings_.Flush();
        result.source_collection =
            session_.FlushStateCachesWithStatus();
        if (spectrum_viewport_state_writeback_allowed_) {
            result.spectrum_viewport_state_saved =
                spectrum_viewport_state_persistence_.Flush(
                    [this]() {
                        return SaveSpectrumViewportState();
                    }) !=
                LocalUserStatePersistenceLifecycle::
                    FlushOutcome::Failed;
        }
        if (spectrum_plot_preferences_writeback_allowed_) {
            result.spectrum_plot_preferences_saved =
                spectrum_plot_preferences_persistence_.Flush([this]() {
                    return SaveSpectrumPlotPreferences();
                }) != LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
        }
        result.spectral_lines_saved =
            spectral_lines_panel_.Flush();
    }
    persist_local_state_ = false;
    local_state_flush_result_ = result;
    return result;
}

bool ShellUi::PrepareLabelingForInteractiveClose()
{
    return session_.PrepareLabelingForInteractiveClose();
}

void ShellUi::Render(const ShellStatus& status)
{
    automation_panel_presentation_candidate_.reset();
    source_activation_.BeginFrame(
        status.latency_trace_recording_active,
        status.frame_index,
        status.profile);
    if (!status.latency_trace_recording_active) {
        pending_keyboard_previous_at_.reset();
        pending_keyboard_next_at_.reset();
    }
    DrainSourceLoads();
    sample_workflow_shortcut_ = {};
    if (immersive_plot_mode_) {
        RenderImmersivePlot(status);
        sample_workflow_panel_ui_.FinalizeTaskNameEdit(
            panel_session_interaction_,
            application_settings_.View().language);
        source_collection_panel_ui_.FinalizeNavigationInputEdits(
            panel_session_interaction_);
        HandleSessionAction(
            panel_session_interaction_.TakeAction());
        HandleSampleWorkflowShortcut();
        pending_keyboard_previous_at_.reset();
        pending_keyboard_next_at_.reset();
        ObserveSpectrumPersistenceChanges(LocalUserStateSaveScheduler::Clock::now());
        return;
    }
    RenderDockHost(status);
    const ApplicationSettingsView settings =
        application_settings_.View();
    const PanelVisibilityState& panel_visibility =
        settings.panel_visibility;
    RecordPanelVisibilityDrawSubmission(
        status.frame_index,
        ImGui::GetMainViewport()->ID,
        panel_visibility);
    if (panel_visibility.files) {
        RenderFilesPanel(
            panel_visibility.files,
            settings.language);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Files);
    }
    if (panel_visibility.navigation) {
        RenderNavigationPanel(
            panel_visibility.navigation);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Navigation);
    }
    if (panel_visibility.annotations) {
        RenderAnnotationsPanel(
            panel_visibility.annotations);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Annotations);
    }
    if (panel_visibility.smoothing) {
        RenderSmoothingPanel(
            panel_visibility.smoothing);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Smoothing);
    }
    RenderMainPlot(status);
    if (panel_visibility.labeling) {
        RenderLabelingPanel(
            panel_visibility.labeling);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Labeling);
    }
    if (panel_visibility.filters) {
        RenderFiltersPanel(
            panel_visibility.filters);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Filters);
    }
    if (panel_visibility.sorting) {
        RenderSortingPanel(
            panel_visibility.sorting);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Sorting);
    }
    if (panel_visibility.information) {
        RenderInfoTagsPanel(
            panel_visibility.information);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::Information);
    }
    if (panel_visibility.spectral_lines) {
        RenderSpectralLinesPanel(
            panel_visibility.spectral_lines);
        RecordPanelWindowDrawSubmission(
            ApplicationPanel::SpectralLines);
    }
    RenderSettingsPanel(status);
    if (!application_settings_.View().panel_visibility.labeling) {
        // The window close button and the View menu both stop future
        // RenderLabeling calls. Finalize while this frame can still dispatch
        // the guarded rename intent.
        sample_workflow_panel_ui_.FinalizeTaskNameEdit(
            panel_session_interaction_,
            application_settings_.View().language);
    }
    // Blur commits must observe every same-frame panel mutation, especially
    // sample filtering and sorting rendered after Navigation.
    source_collection_panel_ui_.FinalizeNavigationInputEdits(
        panel_session_interaction_);
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    HandleSampleWorkflowShortcut();
    pending_keyboard_previous_at_.reset();
    pending_keyboard_next_at_.reset();
    ObserveSpectrumPersistenceChanges(LocalUserStateSaveScheduler::Clock::now());
}

void ShellUi::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    DrainSourceLoads();
    const ApplicationSettingsView settings_before =
        application_settings_.View();
    application_settings_.RunMaintenance(now);
    const ApplicationSettingsView settings_after =
        application_settings_.View();
    if (settings_before.ui_scale_percentage !=
        settings_after.ui_scale_percentage) {
        applied_ui_scale_percentage_ =
            settings_after.ui_scale_percentage;
    }
    if (settings_before.language != settings_after.language) {
        applied_ui_language_ = settings_after.language;
    }
    if (settings_before.theme_selection !=
        settings_after.theme_selection) {
        applied_theme_selection_ =
            settings_after.theme_selection;
    }
    HandleSessionAction(
        source_activation_.RunMaintenance(now));
    spectral_lines_panel_.RunMaintenance(now);
    ObserveSpectrumPersistenceChanges(now);
    if (persist_local_state_ && spectrum_plot_preferences_writeback_allowed_) {
        (void)spectrum_plot_preferences_persistence_.RunMaintenance(now, [this]() {
            return SaveSpectrumPlotPreferences();
        });
    }
    if (persist_local_state_ && spectrum_viewport_state_writeback_allowed_) {
        (void)spectrum_viewport_state_persistence_.
            RunMaintenance(
                now,
                [this]() {
                    return SaveSpectrumViewportState();
                });
    }
}

std::optional<LocalUserStateSaveScheduler::TimePoint> ShellUi::NextMaintenanceDeadline() const
{
    std::optional<LocalUserStateSaveScheduler::TimePoint> deadline =
        application_settings_.NextMaintenanceDeadline();
    const auto consider = [&deadline](std::optional<LocalUserStateSaveScheduler::TimePoint> candidate) {
        if (candidate && (!deadline || *candidate < *deadline)) {
            deadline = candidate;
        }
    };
    consider(
        source_activation_.
            NextMaintenanceDeadline());
    consider(spectral_lines_panel_.NextMaintenanceDeadline());
    if (persist_local_state_ && spectrum_plot_preferences_writeback_allowed_) {
        consider(spectrum_plot_preferences_persistence_.NextMaintenanceDeadline());
    }
    if (persist_local_state_ && spectrum_viewport_state_writeback_allowed_) {
        consider(
            spectrum_viewport_state_persistence_.
                NextMaintenanceDeadline());
    }
    return deadline;
}

void ShellUi::RegisterSourceLoadCompletionReadyCallback(
    SourceCollectionLoadQueue::CompletionReadyCallback callback)
{
    source_activation_.RegisterCompletionReadyCallback(
        std::move(callback));
}

void ShellUi::UnregisterSourceLoadCompletionReadyCallback()
{
    source_activation_.UnregisterCompletionReadyCallback();
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

bool ShellUi::TakeFrameCaptureRequest()
{
    return settings_panel_ui_.TakeFrameCaptureRequest();
}

std::optional<int> ShellUi::TakeAppliedUiScalePercentage()
{
    return std::exchange(
        applied_ui_scale_percentage_,
        std::nullopt);
}

std::optional<UiLanguage>
ShellUi::TakeAppliedUiLanguage()
{
    return std::exchange(
        applied_ui_language_,
        std::nullopt);
}

std::optional<ThemeSelection>
ShellUi::TakeAppliedThemeSelection()
{
    return std::exchange(
        applied_theme_selection_,
        std::nullopt);
}

int ShellUi::ui_scale_percentage() const
{
    return application_settings_.View().ui_scale_percentage;
}

UiLanguage ShellUi::ui_language() const
{
    return application_settings_.View().language;
}

ThemeSelection ShellUi::theme_selection() const
{
    return application_settings_.View().theme_selection;
}

std::filesystem::path ShellUi::profile_output_directory() const
{
    return application_settings_.View().profile_output_directory;
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
    (void)source_activation_.OpenSource(
        path,
        spectrum_index);
}

void ShellUi::OpenExternalSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    (void)source_activation_.OpenExternalSource(
        path,
        application_settings_.View().open_external_source_as_folder,
        spectrum_index);
}

SourceCollectionActivationTransaction::
    SourceOpenOperation
ShellUi::OpenSourceForAutomation(
    const std::filesystem::path& path)
{
    return source_activation_.OpenSourceForAutomation(
        path);
}

SourceCollectionActivationTransaction::
    SourceOpenOperationOutcome
ShellUi::ObserveSourceOpenForAutomation(
    const SourceCollectionActivationTransaction::
        SourceOpenOperation& operation) const
{
    return source_activation_.
        ObserveSourceOpenOperation(operation);
}

ShellAutomationSourceOperation ShellUi::BeginSourceOpenForAutomation(
    const std::filesystem::path& path)
{
    const auto operation = OpenSourceForAutomation(path);
    return [this, operation]() {
        const auto outcome = ObserveSourceOpenForAutomation(operation);
        using InternalState = SourceCollectionActivationTransaction::SourceOpenOperationState;
        using State = ShellAutomationSourceOutcome::State;
        State state = State::Pending;
        switch (outcome.state) {
        case InternalState::Pending: state = State::Pending; break;
        case InternalState::Succeeded: state = State::Succeeded; break;
        case InternalState::Failed: state = State::Failed; break;
        case InternalState::Canceled: state = State::Canceled; break;
        }
        return ShellAutomationSourceOutcome{state, outcome.source_path,
            outcome.source_id, outcome.spectrum_count, outcome.spectrum_index,
            outcome.spectrum_name};
    };
}

const SourceCollectionActivationTransaction::
    PresentedSpectrumObservation&
ShellUi::PresentedSpectrumForAutomation() const noexcept
{
    return source_activation_.
        presented_spectrum_observation();
}

std::uint64_t
ShellUi::ActivationGenerationForAutomation() const noexcept
{
    return source_activation_.
        activation_generation();
}

ShellAutomationNavigationResult
ShellUi::GotoSpectrumForAutomation(
    std::optional<std::size_t> index,
    std::optional<std::string_view> name)
{
    ShellAutomationNavigationResult automation;
    const SourceCollectionSessionView& view =
        SessionView();
    if (!view.navigation.has_active_source ||
        !view.snapshot) {
        automation.error =
            ShellAutomationNavigationError::
                NoActiveSource;
        return automation;
    }
    if (!view.current_sample_snapshot ||
        !view.navigation.current_index) {
        automation.error =
            ShellAutomationNavigationError::
                SourceNotReady;
        return automation;
    }

    automation.source_id =
        view.snapshot->source.id;
    std::size_t target_index = 0;
    std::string target_name;
    if (index) {
        if (*index >=
            view.navigation.sample_count) {
            automation.error =
                ShellAutomationNavigationError::
                    IndexOutOfRange;
            return automation;
        }
        target_index = *index;
    } else if (name) {
        const ExactSampleNameResolution resolution =
            session_.ResolveExactSampleName(*name);
        if (!resolution.names_available) {
            automation.error =
                ShellAutomationNavigationError::
                    NameUnavailable;
            return automation;
        }
        if (resolution.matching_rows.empty()) {
            automation.error =
                ShellAutomationNavigationError::
                    NameNotFound;
            return automation;
        }
        if (resolution.matching_rows.size() != 1U) {
            automation.error =
                ShellAutomationNavigationError::
                    NameAmbiguous;
            return automation;
        }
        if (!resolution.first_match_in_active_sequence) {
            automation.error =
                ShellAutomationNavigationError::
                    FilteredOut;
            return automation;
        }
        target_index =
            resolution.matching_rows.front();
        target_name = std::string(*name);
    } else {
        automation.error =
            ShellAutomationNavigationError::
                Rejected;
        return automation;
    }

    const std::size_t previous_index =
        *view.navigation.current_index;
    const std::size_t spectrum_count =
        view.navigation.sample_count;
    SourceCollectionSessionResult result =
        SubmitSessionCommand(
            SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    SampleNavigationIntent::Move(
                        SampleNavigationRequest::
                            LocateSourceRowInSequence(
                                target_index))));
    if (result.navigation.blocked_by_filter) {
        automation.error =
            ShellAutomationNavigationError::
                FilteredOut;
        return automation;
    }
    if (!result.navigation.target_found) {
        automation.error =
            ShellAutomationNavigationError::
                Rejected;
        return automation;
    }

    automation.target = {
        .present = true,
        .index = target_index,
        .name = std::move(target_name),
        .count = spectrum_count,
    };
    automation.changed =
        previous_index != target_index;
    automation.pending =
        result.follow_up_spectrum_index.has_value();
    return automation;
}

ShellAutomationLabelAssignmentResult
ShellUi::AssignLabelForAutomation(int code)
{
    ShellAutomationLabelAssignmentResult automation;
    const SourceCollectionSessionView& view =
        SessionView();
    if (!view.current_sample_snapshot ||
        !view.navigation.current_index) {
        automation.error =
            ShellAutomationLabelError::
                NoCurrentSpectrum;
        return automation;
    }
    if (!view.labeling.has_active_task) {
        automation.error =
            ShellAutomationLabelError::
                NoActiveTask;
        return automation;
    }
    if (!ContainsSampleLabelCode(
            view.labeling.label_set,
            code)) {
        automation.error =
            ShellAutomationLabelError::
                LabelNotFound;
        return automation;
    }

    automation.source_id =
        view.snapshot ? view.snapshot->source.id
                      : std::string{};
    automation.task_id =
        view.labeling.task_id;
    automation.spectrum = {
        .present = true,
        .index = *view.navigation.current_index,
        .name = view.navigation.current_sample_name,
        .count = view.navigation.sample_count,
    };

    SourceCollectionSessionResult result =
        SubmitSessionCommand(
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    ActiveSampleWorkflowIntent::
                        AssignActiveLabelToCurrentSample(
                            code)),
            SourceCollectionActivationTransaction::
                NavigationIntent{
                    NavigationLatencyInputKind::
                        AutoAdvance});
    if (!result.label_write ||
        !result.label_write->write.accepted) {
        automation.error =
            ShellAutomationLabelError::Rejected;
        return automation;
    }

    const SampleLabelingWriteOperationResult&
        write = *result.label_write;
    automation.previous_code =
        write.write.previous_code;
    automation.new_code =
        write.write.current_code;
    automation.changed =
        write.write.changed;
    automation.state_save_scheduled =
        write.operation.state_save_scheduled;
    automation.state_save_attempted =
        write.operation.state_save_attempted;
    automation.state_saved =
        write.operation.state_saved;
    automation.output_save_attempted =
        write.operation.output_save_attempted;
    automation.output_saved =
        write.operation.output_saved;
    automation.output_retry_scheduled =
        write.operation.output_retry_scheduled;
    automation.navigation_pending =
        result.follow_up_spectrum_index.has_value();
    return automation;
}

ApplicationSettingsResult
ShellUi::SetUiLanguageForAutomation(
    UiLanguage language)
{
    return ApplyApplicationSettingsIntent(
        ApplicationSettingsIntent::SetLanguage(
            language),
        {});
}

ApplicationSettingsResult
ShellUi::SetUiScaleForAutomation(
    int percentage)
{
    return ApplyApplicationSettingsIntent(
        ApplicationSettingsIntent::SetUiScale(
            percentage),
        {});
}

ApplicationSettingsResult
ShellUi::SetThemeSelectionForAutomation(
    ThemeSelection selection)
{
    return ApplyApplicationSettingsIntent(
        ApplicationSettingsIntent::SetThemeSelection(
            std::move(selection)),
        {});
}

ApplicationSettingsResult
ShellUi::SetPanelVisibilityForAutomation(
    ApplicationPanel panel,
    bool visible)
{
    return ApplyApplicationSettingsIntent(
        ApplicationSettingsIntent::SetPanelVisibility(
            panel,
            visible),
        {});
}

PanelVisibilityState
ShellUi::PanelVisibilityForAutomation() const
{
    return application_settings_.View().panel_visibility;
}

ShellAutomationView ShellUi::AutomationView()
{
    return AutomationViewForSnapshot(
        session_.CurrentSampleSnapshot());
}

ShellWindowTitleView ShellUi::WindowTitleView() const
{
    ShellWindowTitleView view;
    view.snapshot_owner =
        session_.CurrentSampleSnapshot();
    view.loading_source_path =
        source_activation_.VisibleLoadingSourcePath();
    if (view.snapshot_owner) {
        view.source_path =
            &view.snapshot_owner->source.path;
        view.sample_present = true;
        view.sample_name =
            view.snapshot_owner->current_spectrum.name;
        view.sample_index =
            view.snapshot_owner->collection.current_index;
        view.sample_count =
            view.snapshot_owner->collection.spectrum_count;
    }
    return view;
}

const ShellAutomationView&
ShellUi::PresentedAutomationView() const noexcept
{
    return presented_automation_view_;
}

const ShellAutomationPanelPresentation&
ShellUi::PresentedPanelVisibilityForAutomation() const noexcept
{
    return presented_panel_visibility_;
}

const ShellAutomationPanelPresentationStatus&
ShellUi::PanelPresentationStatusForAutomation() const noexcept
{
    return automation_panel_presentation_status_;
}

ShellAutomationView
ShellUi::AutomationViewForSnapshot(
    const SpectrumSnapshotHandle& snapshot)
{
    ShellAutomationView automation;
    const SourceCollectionSessionView& view =
        SessionView();
    const SourceCollectionActivationTransaction::Status
        activation_status = source_activation_.status();
    automation.loading_source_path =
        activation_status.loading_source_path;
    automation.loading =
        !automation.loading_source_path.empty();
    if (snapshot) {
        automation.source_id =
            snapshot->source.id;
        automation.source_path =
            snapshot->source.path;
        automation.spectrum = {
            .present = true,
            .index =
                snapshot->collection.current_index,
            .name =
                snapshot->current_spectrum.name,
            .count =
                snapshot->collection.spectrum_count,
        };
    } else {
        automation.spectrum.count =
            view.navigation.sample_count;
    }
    if (snapshot &&
        view.current_sample_snapshot == snapshot) {
        automation.labeling = {
            .has_active_task =
                view.labeling.has_active_task,
            .task_id = view.labeling.task_id,
            .task_name = view.labeling.task_name,
            .task_ids = view.labeling.task_ids,
            .current_spectrum_code =
                view.labeling.current_code,
        };
    }
    return automation;
}

void ShellUi::DrainSourceLoads(
    bool allow_snapshot_prefetch)
{
    HandleSessionAction(source_activation_.Drain(
        allow_snapshot_prefetch &&
            !latency_sensitive_plot_interaction_active()));
}

void ShellUi::BeginDeferredSourceRestore()
{
    source_activation_.BeginDeferredRestore();
}

void ShellUi::RestoreDeferredSpectrumViewport(
    std::optional<std::string>
        source_collection_identity)
{
    std::optional<SpectrumViewportState> restored =
        std::exchange(
            startup_spectrum_viewport_state_,
            std::nullopt);
    const std::optional<std::uint64_t>
        expected_mutation_revision =
            std::exchange(
                startup_spectrum_view_mutation_revision_,
                std::nullopt);
    const std::uint64_t current_mutation_revision =
        spectrum_view_session_.ViewportMutationRevision();
    if (!restored || !restored->locked ||
        !expected_mutation_revision ||
        current_mutation_revision !=
            *expected_mutation_revision ||
        !source_collection_identity ||
        *source_collection_identity !=
            restored->source_collection_identity) {
        return;
    }
    (void)spectrum_view_session_.RestoreLockedViewport(
        restored->limits);
}

SpectrumSnapshotHandle ShellUi::current_snapshot() const
{
    return session_.CurrentSampleSnapshot();
}

bool ShellUi::ArmRuntimeResourceCancellationCheckpoint()
{
    return source_activation_.load_queue_
        .ArmRuntimeResourceCancellationCheckpoint();
}

ShellRuntimeResourceObservation
ShellUi::runtime_resource_observation() const
{
    const SpectrumSnapshotHandle snapshot =
        session_.CurrentSourceSnapshot();
    return {
        .load_activity =
            source_activation_.load_queue_.ActivitySnapshot(),
        .presented_source_load =
            source_activation_.
                presented_source_load_observation(),
        .pending_load_count =
            source_activation_.PendingLoadCount(),
        .active_source_id =
            snapshot ? snapshot->source.id
                     : std::string{},
        .active_source_path =
            snapshot ? snapshot->source.path
                     : std::filesystem::path{},
        .load_error =
            std::string(source_activation_.ErrorMessage()),
    };
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
    if (snapshot) {
        automation_presentation_candidate_ = {
            .frame_index = frame_index,
            .view =
                AutomationViewForSnapshot(snapshot),
        };
    }
    source_activation_.RecordSpectrumDrawSubmission(
        frame_index,
        viewport_id,
        std::move(snapshot));
}

void ShellUi::RecordPanelVisibilityDrawSubmission(
    std::uint64_t frame_index,
    unsigned int viewport_id,
    PanelVisibilityState visibility)
{
    automation_panel_presentation_candidate_ = {
        .frame_index = frame_index,
        .main_viewport_id = viewport_id,
        .visibility = visibility,
    };
}

void ShellUi::RecordPanelDrawSubmission(
    ApplicationPanel panel,
    unsigned int viewport_id)
{
    if (!automation_panel_presentation_candidate_) {
        return;
    }
    automation_panel_presentation_candidate_
        ->draw_viewport_ids[
            static_cast<std::size_t>(panel)] =
        viewport_id;
}

void ShellUi::RecordPanelWindowDrawSubmission(
    ApplicationPanel panel)
{
    const std::size_t panel_index =
        static_cast<std::size_t>(panel);
    ImGuiWindow* window = ImGui::FindWindowByName(
        kApplicationPanelWindowIds[panel_index]);
    if (window == nullptr ||
        window->LastFrameActive !=
            ImGui::GetFrameCount() ||
        window->Viewport == nullptr) {
        return;
    }
    RecordPanelDrawSubmission(
        panel,
        window->Viewport->ID);
}

void ShellUi::PresentFrame(
    std::uint64_t frame_index,
    std::span<const NavigationLatencyPresentation> presentations,
    std::span<
        const ShellAutomationViewportPresentationState>
        viewport_states)
{
    const std::uint64_t presented_sequence_before =
        source_activation_.
            presented_spectrum_observation()
                .sequence;
    source_activation_.PresentFrame(
        frame_index,
        presentations);
    const auto& presented =
        source_activation_.
            presented_spectrum_observation();
    if (presented.sequence >
            presented_sequence_before &&
        automation_presentation_candidate_ &&
        automation_presentation_candidate_
                ->frame_index ==
            frame_index &&
        automation_presentation_candidate_
                ->view.source_id ==
            presented.source_id &&
        automation_presentation_candidate_
                ->view.spectrum.present &&
        automation_presentation_candidate_
                ->view.spectrum.index ==
            presented.spectrum_index) {
        presented_automation_view_ =
            automation_presentation_candidate_
                ->view;
    }
    if (!automation_panel_presentation_candidate_ ||
        automation_panel_presentation_candidate_
                ->frame_index !=
            frame_index) {
        return;
    }

    AutomationPanelPresentationCandidate& candidate =
        *automation_panel_presentation_candidate_;
    const auto viewport_presented =
        [&presentations](unsigned int viewport_id) {
            return std::ranges::any_of(
                presentations,
                [viewport_id](const auto& presentation) {
                    return presentation.viewport_id ==
                           viewport_id;
                });
        };
    const bool viewport_states_observed =
        !viewport_states.empty();
    const auto viewport_state =
        [&viewport_states](unsigned int viewport_id)
            -> const ShellAutomationViewportPresentationState* {
            const auto found =
                std::ranges::find_if(
                    viewport_states,
                    [viewport_id](const auto& state) {
                        return state.viewport_id ==
                               viewport_id;
                    });
            return found == viewport_states.end()
                       ? nullptr
                       : &*found;
        };
    candidate.main_viewport_presented =
        candidate.main_viewport_presented ||
        viewport_presented(candidate.main_viewport_id);

    for (std::size_t panel_index = 0;
         panel_index < kApplicationPanelCount;
         ++panel_index) {
        const ApplicationPanel panel =
            static_cast<ApplicationPanel>(
                panel_index);
        const bool visible =
            ApplicationPanelVisible(
                candidate.visibility,
                panel);
        const std::optional<unsigned int>
            draw_viewport_id =
                candidate.draw_viewport_ids[
                    panel_index];
        bool exposure_resolved = false;
        std::vector<unsigned int>& exposed_viewports =
            automation_panel_visible_viewports_[
                panel_index];
        bool presentation_blocked = false;
        if (visible && draw_viewport_id) {
            const auto* state =
                viewport_state(*draw_viewport_id);
            presentation_blocked =
                state != nullptr &&
                !state->renderable;
        } else if (!visible) {
            presentation_blocked =
                std::ranges::any_of(
                    exposed_viewports,
                    [&](unsigned int viewport_id) {
                        const auto* state =
                            viewport_state(viewport_id);
                        return state != nullptr &&
                               !state->renderable;
                    });
        }
        automation_panel_presentation_status_
            .frame_indices[panel_index] =
            frame_index;
        automation_panel_presentation_status_
            .blocked[panel_index] =
            presentation_blocked;

        std::erase_if(
            exposed_viewports,
            [&](unsigned int viewport_id) {
                if (visible &&
                    draw_viewport_id &&
                    *draw_viewport_id ==
                        viewport_id) {
                    return false;
                }
                const auto* state =
                    viewport_state(viewport_id);
                const bool resolved =
                    viewport_presented(viewport_id) ||
                    (viewport_states_observed &&
                     state == nullptr);
                exposure_resolved =
                    exposure_resolved || resolved;
                return resolved;
            });

        bool panel_presented = false;
        if (!presentation_blocked &&
            visible && draw_viewport_id &&
            viewport_presented(
                *draw_viewport_id)) {
            if (std::ranges::find(
                    exposed_viewports,
                    *draw_viewport_id) ==
                exposed_viewports.end()) {
                exposed_viewports.push_back(
                    *draw_viewport_id);
            }
            panel_presented = true;
        } else if (!presentation_blocked &&
                   !visible &&
                   exposed_viewports.empty() &&
                   (candidate.main_viewport_presented ||
                    exposure_resolved)) {
            panel_presented = true;
        }

        if (panel_presented) {
            SetApplicationPanelVisible(
                presented_panel_visibility_.visibility,
                panel,
                visible);
            presented_panel_visibility_
                .frame_indices[panel_index] =
                frame_index;
        }
    }
}

void ShellUi::OpenSourceFromFilePicker()
{
    const UiLanguage language =
        application_settings_.View().language;
    OpenSourceFromFilePicker(
        [language]() {
            return ShowSourceFilePicker(language);
        });
}

void ShellUi::OpenSourceFromFilePicker(
    const SourceCollectionPathPicker& choose_source_file)
{
    if (std::optional<std::filesystem::path> path =
            choose_source_file()) {
        OpenSource(*path);
    }
}

void ShellUi::OpenSourceFromFolderPicker()
{
    if (std::optional<std::filesystem::path> path =
            ShowSourceFolderPicker(
                application_settings_.View().language)) {
        OpenSource(*path);
    }
}

void ShellUi::OpenAnnotationFromFilePicker()
{
    if (std::optional<std::filesystem::path> path =
            ShowAnnotationFilePicker(
                application_settings_.View().language)) {
        source_collection_panel_ui_.
            PrepareAnnotationImportAttempt(
                SessionView(),
                *path);
        SourceCollectionSessionResult result =
            SubmitSessionCommand(SourceCollectionSessionIntent::EditSourceCollection(
                SourceCollectionIntent::AddReadOnlyAnnotationResult(*path)));
        source_collection_panel_ui_.
            CompleteAnnotationImportAttempt(
                result.loaded);
        SetPanelVisibility(
            ApplicationPanel::Annotations,
            true);
    }
}

const SourceCollectionSessionView& ShellUi::SessionView()
{
    return session_.View();
}

LocalUserStateHealthView ShellUi::PersistenceHealth()
{
    LocalUserStateHealthView health =
        SessionView().persistence;
    const auto append_setting = [&](
                                    LocalUserStateArea area,
                                    ApplicationSetting setting) {
        AppendLocalUserStateHealth(
            health,
            area,
            application_settings_.PersistenceStatus(setting));
    };
    append_setting(
        LocalUserStateArea::Language,
        ApplicationSetting::Language);
    append_setting(
        LocalUserStateArea::Appearance,
        ApplicationSetting::Appearance);
    append_setting(
        LocalUserStateArea::UiScale,
        ApplicationSetting::UiScale);
    append_setting(
        LocalUserStateArea::Input,
        ApplicationSetting::Input);
    append_setting(
        LocalUserStateArea::ExternalSource,
        ApplicationSetting::ExternalSource);
    append_setting(
        LocalUserStateArea::
            ProfileOutputDirectory,
        ApplicationSetting::ProfileOutputDirectory);
    append_setting(
        LocalUserStateArea::PanelVisibility,
        ApplicationSetting::PanelVisibility);
    AppendLocalUserStateHealth(
        health, LocalUserStateArea::SpectrumPlotPreferences,
        spectrum_plot_preferences_persistence_.PersistenceStatus());
    AppendLocalUserStateHealth(
        health,
        LocalUserStateArea::SpectrumViewportState,
        spectrum_viewport_state_persistence_.
            PersistenceStatus());
    AppendLocalUserStateHealth(
        health,
        LocalUserStateArea::SpectralLines,
        spectral_lines_panel_.PersistenceStatus());
    return health;
}

SourceCollectionSessionResult ShellUi::SubmitSessionCommand(
    SourceCollectionSessionIntent command,
    std::optional<
        SourceCollectionActivationTransaction::NavigationIntent>
        navigation)
{
    const bool labeling_command =
        command.intent_kind() ==
        SourceCollectionSessionIntentKind::ActiveSampleWorkflow;
    SourceCollectionSessionResult result =
        source_activation_.Submit(
            std::move(command),
            std::move(navigation));
    HandleSessionAction(result.action);
    if (labeling_command ||
        result.labeling_issue !=
            SampleLabelingOperationResult::Issue::None ||
        !result.message.empty()) {
        sample_workflow_panel_ui_.CaptureLabelingOperationResult(
            result,
            application_settings_.View().language);
    } else if (
        result.action.source_roster_changed ||
        result.action.snapshot_changed) {
        sample_workflow_panel_ui_.ClearLabelingOperationMessage();
    }
    return result;
}

void ShellUi::HandleSessionAction(const SourceCollectionSessionAction& action)
{
    if (action.snapshot_change_reason ==
            SourceCollectionSnapshotChangeReason::
                SourceCollectionChanged ||
        action.snapshot_change_reason ==
            SourceCollectionSnapshotChangeReason::
                SourceCollectionCleared) {
        source_collection_panel_ui_.
            SyncAnnotationDiagnosticSource(
                SessionView());
    }
    if (action.workflow_changed) {
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
    }
    if (action.navigation_inputs_changed) {
        source_collection_panel_ui_.SyncNavigationInputs(
            SessionView().navigation);
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

    const ImGuiID dockspace_id = ImGui::GetID("DockSpaceSampleNavigationV1");
    const ImVec2 dockspace_size = ImGui::GetContentRegionAvail();

    if (std::exchange(restore_default_layout_requested_, false)) {
        (void)application_settings_.Apply(
            ApplicationSettingsIntent::ShowAllPanels(), {});
        settings_panel_ui_.CloseForLayoutRecovery();
        SeedInitialDockLayout(dockspace_id, dockspace_size);
        layout_seeded_ = true;
        ImGui::GetIO().WantSaveIniSettings = true;
    }
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
    const UiLanguage language =
        application_settings_.View().language;
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
    const std::string immersive_plot_window =
        StableUiLabel(
            language,
            UiTextId::Spectrum,
            "SpectrumImmersiveV1");
    const bool plot_visible = ImGui::Begin(
        immersive_plot_window.c_str(),
        nullptr,
        flags);
    ImGui::PopStyleVar(3);
    if (!plot_visible) {
        ImGui::End();
        return;
    }
    const unsigned int viewport_id = ImGui::GetWindowViewport()->ID;

    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    const SpectrumViewRenderFeedback plot_feedback =
        spectrum_view_session_.Render(
            snapshot,
            language,
            SpectrumPlotProfileContext{status.profile, status.frame_index},
            SpectrumPlotOverlays{
                .spectral_lines = spectral_lines.visible_markers.data(),
                .spectral_line_count = spectral_lines.visible_markers.size(),
                .show_spectral_line_labels = spectral_lines.marker_labels_visible,
                .layout_scope_id = spectral_lines.layout_scope_id,
                .spectral_line_label_font = spectral_line_label_font_,
            },
            MakeImmersivePlotDisplayOptions(),
            touchpad_gestures_);
    if (plot_feedback.plot_submitted) {
        RecordSpectrumDrawSubmission(status.frame_index, viewport_id, snapshot);
    }
    if (const auto context_overlay =
            BuildImmersiveContextOverlayView(
                immersive_plot_mode_,
                SessionView(),
                language)) {
        (void)RenderImmersiveContextOverlay(
            *context_overlay);
    }

    if (status.profile_open) {
        const SemanticPalette& palette =
            ActiveSemanticPalette();
        const std::string_view recording_label = UiText(
            language,
            UiTextId::PerformanceRecordingBadge);
        const ImVec2 text_size = ImGui::CalcTextSize(
            recording_label.data(),
            recording_label.data() +
                recording_label.size());
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 label_min(
            window_pos.x + window_size.x - text_size.x - 30.0f,
            window_pos.y + 12.0f);
        const ImVec2 label_max(label_min.x + text_size.x + 18.0f, label_min.y + text_size.y + 10.0f);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(
            label_min,
            label_max,
            ImGui::GetColorU32(
                palette.overlay_background),
            4.0f);
        draw_list->AddCircleFilled(
            ImVec2(label_min.x + 9.0f, label_min.y + 5.0f + text_size.y * 0.5f),
            3.5f,
            ImGui::GetColorU32(palette.error));
        draw_list->AddText(
            ImVec2(
                label_min.x + 17.0f,
                label_min.y + 5.0f),
            ImGui::GetColorU32(palette.overlay_text),
            recording_label.data(),
            recording_label.data() +
                recording_label.size());
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
    RenderMainMenuBar(status, {});
}

void ShellUi::RenderMainMenuBar(
    const ShellStatus& status,
    const SourceCollectionPathPicker& choose_source_file)
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    const ApplicationSettingsView settings =
        application_settings_.View();
    const UiLanguage language = settings.language;
    const std::string file_menu = StableUiLabel(
        language,
        UiTextId::FileMenu,
        "FileMenu");
    if (ImGui::BeginMenu(file_menu.c_str())) {
        const std::string open_file = StableUiLabel(
            language,
            UiTextId::OpenFile,
            "OpenFile");
        if (ImGui::MenuItem(open_file.c_str())) {
            if (choose_source_file) {
                OpenSourceFromFilePicker(
                    choose_source_file);
            } else {
                OpenSourceFromFilePicker();
            }
        }
        const std::string open_folder = StableUiLabel(
            language,
            UiTextId::OpenFolder,
            "OpenFolder");
        if (ImGui::MenuItem(open_folder.c_str())) {
            OpenSourceFromFolderPicker();
        }
        const bool can_open_annotation = SessionView().can_add_read_only_annotation;
        if (!can_open_annotation) {
            ImGui::BeginDisabled();
        }
        const std::string open_annotation =
            StableUiLabel(
                language,
                UiTextId::OpenFileAsAnnotation,
                "OpenFileAsAnnotation");
        if (ImGui::MenuItem(
                open_annotation.c_str())) {
            OpenAnnotationFromFilePicker();
        }
        if (!can_open_annotation) {
            ImGui::EndDisabled();
        }
        ImGui::EndMenu();
    }

    const std::string view_menu = StableUiLabel(
        language,
        UiTextId::ViewMenu,
        "ViewMenu");
    if (ImGui::BeginMenu(view_menu.c_str())) {
        const PanelVisibilityState panel_visibility =
            settings.panel_visibility;
        const std::string immersive_plot_label =
            StableUiLabel(
                language,
                UiTextId::ImmersivePlotMode,
                "ImmersivePlotMode");
        if (ImGui::MenuItem(
                immersive_plot_label.c_str(),
                "F11",
                immersive_plot_mode_)) {
            immersive_plot_toggle_requested_ = true;
        }
        ImGui::Separator();
        const std::string restore_default_layout =
            StableUiLabel(
                language,
                UiTextId::RestoreDefaultLayout,
                "RestoreDefaultLayout");
        if (ImGui::MenuItem(
                restore_default_layout.c_str())) {
            restore_default_layout_requested_ = true;
        }
        ImGui::Separator();
        const auto render_panel_toggle =
            [this](
                UiLanguage current_language,
                UiTextId text_id,
                std::string_view stable_id,
                ApplicationPanel panel,
                bool visible) {
                const std::string label = StableUiLabel(
                    current_language,
                    text_id,
                    stable_id);
                if (ImGui::MenuItem(
                        label.c_str(),
                        nullptr,
                        visible)) {
                    (void)application_settings_.Apply(
                        ApplicationSettingsIntent::
                            TogglePanelVisibility(panel),
                        {});
                }
            };
        render_panel_toggle(
            language,
            UiTextId::Files,
            "ViewFiles",
            ApplicationPanel::Files,
            panel_visibility.files);
        render_panel_toggle(
            language,
            UiTextId::Navigation,
            "ViewNavigation",
            ApplicationPanel::Navigation,
            panel_visibility.navigation);
        render_panel_toggle(
            language,
            UiTextId::Annotations,
            "ViewAnnotations",
            ApplicationPanel::Annotations,
            panel_visibility.annotations);
        render_panel_toggle(
            language,
            UiTextId::Labeling,
            "ViewLabeling",
            ApplicationPanel::Labeling,
            panel_visibility.labeling);
        render_panel_toggle(
            language,
            UiTextId::SampleFilters,
            "ViewSampleFilters",
            ApplicationPanel::Filters,
            panel_visibility.filters);
        render_panel_toggle(
            language,
            UiTextId::SampleSorting,
            "ViewSampleSorting",
            ApplicationPanel::Sorting,
            panel_visibility.sorting);
        render_panel_toggle(
            language,
            UiTextId::CurveDisplay,
            "ViewSmoothing",
            ApplicationPanel::Smoothing,
            panel_visibility.smoothing);
        render_panel_toggle(
            language,
            UiTextId::Information,
            "ViewInformation",
            ApplicationPanel::Information,
            panel_visibility.information);
        render_panel_toggle(
            language,
            UiTextId::SpectralLines,
            "ViewSpectralLines",
            ApplicationPanel::SpectralLines,
            panel_visibility.spectral_lines);
        ImGui::EndMenu();
    }

    const std::string settings_label = StableUiLabel(
        language,
        UiTextId::Settings,
        "OpenSettings");
    if (ImGui::MenuItem(settings_label.c_str())) {
        settings_panel_ui_.Open();
    }

    const SourceCollectionActivationTransaction::Status
        activation_status = source_activation_.status();
    const LocalUserStateHealthView persistence =
        PersistenceHealth();
    if (RenderTopBarStatus(
            status,
            activation_status.loading,
            activation_status.failures,
            persistence,
            language)) {
        source_activation_.AcknowledgeLoadFailures();
    }

    ImGui::EndMenuBar();
}

void ShellUi::RenderFilesPanel(
    bool panel_open,
    UiLanguage language)
{
    source_collection_panel_ui_.RenderFiles(
        panel_session_interaction_,
        language,
        &panel_open,
        [language]() {
            return ShowSourceFilePicker(language);
        },
        [language]() {
            return ShowSourceFolderPicker(language);
        },
        [this](const std::filesystem::path& path) {
            OpenSource(path);
        },
        [this](const std::filesystem::path& path)
            -> std::optional<std::string> {
            const CurrentExecutableLaunchResult result =
                LaunchCurrentExecutableWithSource(path);
            if (result.succeeded()) {
                return std::nullopt;
            }
            return result.diagnostic;
        });
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(ApplicationPanel::Files, panel_open);
}

void ShellUi::RenderNavigationPanel(bool panel_open)
{
    SampleWorkflowShortcut shortcut;
    const ApplicationSettingsView settings =
        application_settings_.View();
    source_collection_panel_ui_.RenderNavigation(
        panel_session_interaction_,
        settings.language,
        settings.live_numeric_navigation,
        &panel_open,
        shortcut);
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(
        ApplicationPanel::Navigation,
        panel_open);
    QueueSampleWorkflowShortcut(shortcut);
}

void ShellUi::RenderAnnotationsPanel(bool panel_open)
{
    source_collection_panel_ui_.RenderAnnotations(
        panel_session_interaction_,
        application_settings_.View().language,
        &panel_open,
        [language = application_settings_
             .View()
             .language]() {
            return ShowAnnotationFilePicker(language);
        });
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(
        ApplicationPanel::Annotations,
        panel_open);
}

void ShellUi::RenderLabelingPanel(bool panel_open)
{
    SampleWorkflowShortcut shortcut;
    sample_workflow_panel_ui_.RenderLabeling(
        panel_session_interaction_,
        application_settings_.View().language,
        &panel_open,
        [language = application_settings_
             .View()
             .language](std::string_view suggested_filename) {
            return ShowLabelOutputFilePicker(
                language,
                suggested_filename);
        },
        [language = application_settings_
             .View()
             .language](
                 SampleLabelExportFormat format,
                 std::string_view suggested_filename) {
            return ShowLabelValuesExportFilePicker(
                language,
                format,
                suggested_filename);
        },
        shortcut);
    if (!panel_open) {
        sample_workflow_panel_ui_.FinalizeTaskNameEdit(
            panel_session_interaction_,
            application_settings_.View().language);
    }
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(
        ApplicationPanel::Labeling,
        panel_open);
    QueueSampleWorkflowShortcut(shortcut);
}

void ShellUi::RenderFiltersPanel(bool panel_open)
{
    sample_workflow_panel_ui_.RenderFilters(
        panel_session_interaction_,
        application_settings_.View().language,
        &panel_open);
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(
        ApplicationPanel::Filters,
        panel_open);
}

void ShellUi::RenderSortingPanel(bool panel_open)
{
    sample_workflow_panel_ui_.RenderSorting(
        panel_session_interaction_,
        application_settings_.View().language,
        &panel_open);
    HandleSessionAction(
        panel_session_interaction_.TakeAction());
    SetPanelVisibility(
        ApplicationPanel::Sorting,
        panel_open);
}

void ShellUi::RenderSmoothingPanel(bool panel_open)
{
    const UiLanguage language =
        application_settings_.View().language;
    const std::string smoothing_window = StableUiLabel(
        language,
        UiTextId::CurveDisplay,
        "SmoothingV1");
    if (!ImGui::Begin(
            smoothing_window.c_str(),
            &panel_open)) {
        ImGui::End();
        SetPanelVisibility(
            ApplicationPanel::Smoothing,
            panel_open);
        return;
    }

    RenderUiText(
        language,
        UiTextId::CurveDisplay);
    ImGui::Separator();

    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    if (!snapshot || !snapshot->capabilities.can_plot_current_spectrum) {
        RenderDisabledUiText(
            language,
            UiTextId::NoPlottableSpectrum);
        ImGui::End();
        SetPanelVisibility(
            ApplicationPanel::Smoothing,
            panel_open);
        return;
    }

    const SpectrumViewSessionView view = spectrum_view_session_.View();
    const float numeric_control_width =
        ImGui::CalcTextSize("000000").x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    const auto render_color_editor =
        [this, language, &view](
            SpectrumPlotSeries series,
            const char* color_control_id,
            const char* reset_control_id) {
            PlotSeriesColor selection =
                SpectrumSeriesColor(
                    view.plot_colors,
                    series);
            const ImVec4 resolved =
                spectrum_view_session_.ResolveSeriesColor(
                    series,
                    ActiveSemanticPalette());
            float rgba[4]{
                resolved.x,
                resolved.y,
                resolved.z,
                resolved.w,
            };

            ImGui::SameLine();
            ImGui::SetNextItemWidth(
                ImGui::GetFrameHeight());
            const std::string color_label =
                "###" + std::string(color_control_id);
            if (ImGui::ColorEdit4(
                    color_label.c_str(),
                    rgba,
                    ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_AlphaBar |
                        ImGuiColorEditFlags_AlphaPreviewHalf)) {
                selection =
                    PlotSeriesColor::ExplicitColor({
                        .red = rgba[0],
                        .green = rgba[1],
                        .blue = rgba[2],
                        .alpha = rgba[3],
                    });
                spectrum_view_session_.Submit(
                    SpectrumViewSessionCommand::
                        SetPlotSeriesColor(
                            series,
                            selection));
            }
            if (ImGui::IsItemHovered()) {
                const std::string_view color_tooltip =
                    UiText(language, UiTextId::CurveColor);
                const std::string_view options_tooltip =
                    UiText(language, UiTextId::ColorOptionsHint);
                ImGui::SetTooltip(
                    "%.*s\n%.*s",
                    static_cast<int>(color_tooltip.size()),
                    color_tooltip.data(),
                    static_cast<int>(options_tooltip.size()),
                    options_tooltip.data());
            }
            if (ImGui::BeginPopupContextItem(reset_control_id)) {
                const bool automatic =
                    selection.mode() ==
                    PlotSeriesColorMode::Auto;
                const std::string reset_label =
                    StableUiLabel(
                        language,
                        UiTextId::ResetColorToAuto,
                        reset_control_id);
                if (ImGui::MenuItem(
                        reset_label.c_str(),
                        nullptr,
                        false,
                        !automatic)) {
                    spectrum_view_session_.Submit(
                        SpectrumViewSessionCommand::
                            SetPlotSeriesColor(
                                series,
                                PlotSeriesColor::Auto()));
                }
                ImGui::EndPopup();
            }
        };
    const auto render_curve_leading_controls =
        [&render_color_editor, language](
            bool& visible,
            UiTextId name_text_id,
            const char* checkbox_control_id,
            SpectrumPlotSeries series,
            const char* color_control_id,
            const char* reset_control_id) {
            const std::string checkbox_label =
                "###" + std::string(checkbox_control_id);
            bool changed =
                ImGui::Checkbox(
                    checkbox_label.c_str(),
                    &visible);
            render_color_editor(
                series,
                color_control_id,
                reset_control_id);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const std::string_view name =
                UiText(language, name_text_id);
            ImGui::TextUnformatted(
                name.data(),
                name.data() + name.size());
            if (ImGui::IsItemClicked(
                    ImGuiMouseButton_Left)) {
                visible = !visible;
                changed = true;
            }
            return changed;
        };

    bool show_raw_curve = view.show_raw_curve;
    if (render_curve_leading_controls(
            show_raw_curve,
            UiTextId::RawSpectrum,
            "ShowRawCurve",
            SpectrumPlotSeries::RawSpectrum,
            "RawSpectrumColor",
            "RawSpectrumColorReset")) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetShowRawCurve(show_raw_curve));
    }
    ImGui::SameLine();
    bool show_points = view.show_points;
    const std::string show_points_label = StableUiLabel(
        language,
        UiTextId::DataPoints,
        "ShowSpectrumPoints");
    if (ImGui::Checkbox(show_points_label.c_str(), &show_points)) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetShowPoints(show_points));
    }

    bool show_gaussian_smoothed = view.show_gaussian_smoothed;
    if (render_curve_leading_controls(
            show_gaussian_smoothed,
            UiTextId::GaussianSmoothing,
            "ShowGaussianSmoothedCurve",
            SpectrumPlotSeries::GaussianSmoothing,
            "GaussianSmoothingColor",
            "GaussianSmoothingColorReset")) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetShowGaussianSmoothed(
                show_gaussian_smoothed));
    }
    ImGui::SameLine();
    float sigma = static_cast<float>(
        view.smoothing_parameters.gaussian_sigma);
    ImGui::SetNextItemWidth(numeric_control_width);
    const std::string sigma_label = StableUiLabel(
        language,
        UiTextId::GaussianSigma,
        "GaussianSigma");
    if (ImGui::DragFloat(
            sigma_label.c_str(),
            &sigma,
            0.01f,
            0.01f,
            100.0f,
            "%.2f",
            ImGuiSliderFlags_AlwaysClamp)) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetGaussianSigma(
                static_cast<double>(sigma)));
    }
    if (ImGui::IsItemHovered()) {
        const std::string_view hint = UiText(
            language,
            UiTextId::DragOrEnterValue);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(hint.size()),
            hint.data());
    }
    bool show_median_smoothed = view.show_median_smoothed;
    if (render_curve_leading_controls(
            show_median_smoothed,
            UiTextId::MedianSmoothing,
            "ShowMedianSmoothedCurve",
            SpectrumPlotSeries::MedianSmoothing,
            "MedianSmoothingColor",
            "MedianSmoothingColorReset")) {
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetShowMedianSmoothed(
                show_median_smoothed));
    }
    ImGui::SameLine();
    int kernel_size =
        view.smoothing_parameters.median_kernel_size;
    const int previous_kernel_size = kernel_size;
    ImGui::SetNextItemWidth(numeric_control_width);
    const std::string kernel_size_label = StableUiLabel(
        language,
        UiTextId::MedianKernelSize,
        "MedianKernelSize");
    const ImGuiID kernel_size_id =
        ImGui::GetCurrentWindow()->GetID(kernel_size_label.c_str());
    const bool kernel_text_input_active_before =
        ImGui::TempInputIsActive(kernel_size_id);
    if (ImGui::DragInt(
            kernel_size_label.c_str(),
            &kernel_size,
            0.1f,
            3,
            501,
            "%d",
            ImGuiSliderFlags_AlwaysClamp |
                ImGuiSliderFlags_NoSpeedTweaks)) {
        const bool kernel_text_input_active =
            kernel_text_input_active_before ||
            ImGui::TempInputIsActive(kernel_size_id);
        const int requested_kernel_size = kernel_text_input_active
            ? kernel_size
            : std::clamp(
                  previous_kernel_size +
                      (kernel_size - previous_kernel_size) * 2,
                  3,
                  501);
        spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::SetMedianKernelSize(
                requested_kernel_size));
    }
    const bool kernel_size_hovered = ImGui::IsItemHovered();
    const SpectrumViewSessionView current_view =
        spectrum_view_session_.View();
    const int effective_kernel_size =
        spectrum_view_session_.EffectiveMedianKernelSize(
            snapshot->current_spectrum.point_count);
    if (effective_kernel_size !=
        current_view.smoothing_parameters.median_kernel_size) {
        ImGui::SameLine();
        const std::string_view effective_label = UiText(
            language,
            UiTextId::EffectiveKernelCompact);
        ImGui::TextDisabled(
            "%.*s%d",
            static_cast<int>(effective_label.size()),
            effective_label.data(),
            effective_kernel_size);
    }
    if (kernel_size_hovered) {
        const std::string_view hint = UiText(
            language,
            UiTextId::OddKernelInputHint);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(hint.size()),
            hint.data());
    }
    ImGui::End();
    SetPanelVisibility(
        ApplicationPanel::Smoothing,
        panel_open);
}

void ShellUi::RenderInfoTagsPanel(bool panel_open)
{
    const UiLanguage language =
        application_settings_.View().language;
    const std::string information_window =
        StableUiLabel(
            language,
            UiTextId::Information,
            "InfoTagsV2");
    if (!ImGui::Begin(
            information_window.c_str(),
            &panel_open)) {
        ImGui::End();
        SetPanelVisibility(
            ApplicationPanel::Information,
            panel_open);
        return;
    }

    RenderUiText(
        language,
        UiTextId::Information);
    ImGui::Separator();
    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    if (snapshot) {
        const CurrentSpectrumSnapshot& current = snapshot->current_spectrum;
        const std::string_view separator =
            LabelValueSeparator(language);
        const std::string_view name_label = UiText(
            language,
            UiTextId::Name);
        const std::string_view none_value = UiText(
            language,
            UiTextId::NoneValue);
        ImGui::Text(
            "%.*s%.*s%.*s",
            static_cast<int>(name_label.size()),
            name_label.data(),
            static_cast<int>(separator.size()),
            separator.data(),
            static_cast<int>(
                current.name.empty()
                    ? none_value.size()
                    : current.name.size()),
            current.name.empty()
                ? none_value.data()
                : current.name.data());
        const std::string_view points_label = UiText(
            language,
            UiTextId::Points);
        ImGui::Text(
            "%.*s%.*s%zu",
            static_cast<int>(points_label.size()),
            points_label.data(),
            static_cast<int>(separator.size()),
            separator.data(),
            current.point_count);
        const std::string_view unknown_value = UiText(
            language,
            UiTextId::UnknownValue);
        ImGui::Text(
            "X%.*s%.*s",
            static_cast<int>(separator.size()),
            separator.data(),
            static_cast<int>(
                snapshot->axis.x_label.empty()
                    ? unknown_value.size()
                    : snapshot->axis.x_label.size()),
            snapshot->axis.x_label.empty()
                ? unknown_value.data()
                : snapshot->axis.x_label.data());
        ImGui::Text(
            "Y%.*s%.*s",
            static_cast<int>(separator.size()),
            separator.data(),
            static_cast<int>(
                snapshot->axis.y_label.empty()
                    ? unknown_value.size()
                    : snapshot->axis.y_label.size()),
            snapshot->axis.y_label.empty()
                ? unknown_value.data()
                : snapshot->axis.y_label.data());
        RenderMetadataLine(
            language,
            UiTextId::WavelengthMedium,
            MetadataValue(
                snapshot->source.metadata,
                "wavelength_medium"));
        RenderMetadataLine(
            language,
            UiTextId::ObserverCorrection,
            MetadataValue(snapshot->source.metadata, "observer_frame_correction"));
        RenderMetadataLine(
            language,
            UiTextId::RadialVelocity,
            MetadataValue(snapshot->source.metadata, "radial_velocity_km_s"),
            "km/s");
        RenderMetadataLine(
            language,
            UiTextId::RadialVelocitySource,
            MetadataValue(
                snapshot->source.metadata,
                "radial_velocity_source"));
        RenderMetadataLine(
            language,
            UiTextId::Redshift,
            MetadataValue(snapshot->source.metadata, "redshift"));
        RenderMetadataLine(
            language,
            UiTextId::RedshiftWarning,
            MetadataValue(
                snapshot->source.metadata,
                "redshift_warning"));
        RenderMetadataLine(
            language,
            UiTextId::TargetRedshift,
            MetadataValue(
                snapshot->source.metadata,
                "target_redshift"));
        RenderMetadataLine(
            language,
            UiTextId::TargetRedshiftSource,
            MetadataValue(
                snapshot->source.metadata,
                "target_redshift_source"));
        RenderMetadataLine(
            language,
            UiTextId::TargetRedshiftStatus,
            MetadataValue(
                snapshot->source.metadata,
                "target_redshift_status"));
        RenderMetadataLine(
            language,
            UiTextId::TargetRedshiftWarning,
            MetadataValue(
                snapshot->source.metadata,
                "target_redshift_warning"));
        RenderMetadataLine(
            language,
            UiTextId::HeliocentricCorrection,
            MetadataValue(snapshot->source.metadata, "heliocentric_correction_km_s"),
            "km/s");
        RenderMetadataLine(
            language,
            UiTextId::TargetRestFrame,
            MetadataValue(snapshot->source.metadata, "target_rest_frame_status"));
        RenderMetadataLine(
            language,
            UiTextId::RestFrameCorrection,
            MetadataValue(snapshot->source.metadata, "rest_frame_correction_status"));
    } else {
        RenderDisabledUiText(
            language,
            UiTextId::NoSnapshot);
    }

    ImGui::Spacing();
    RenderUiText(
        language,
        UiTextId::DiagnosticsHeading);
    ImGui::Separator();
    RenderDiagnosticRows(
        snapshot,
        language);

    ImGui::End();
    SetPanelVisibility(
        ApplicationPanel::Information,
        panel_open);
}

void ShellUi::RenderMainPlot(const ShellStatus& status)
{
    const UiLanguage language =
        application_settings_.View().language;
    const std::string spectrum_window = StableUiLabel(
        language,
        UiTextId::Spectrum,
        "SpectrumV2");
    if (!ImGui::Begin(spectrum_window.c_str())) {
        ImGui::End();
        return;
    }
    const unsigned int viewport_id = ImGui::GetWindowViewport()->ID;
    const SpectrumSnapshotHandle snapshot = session_.CurrentSampleSnapshot();
    const SpectralLinePlotView spectral_lines = spectral_lines_panel_.PlotView(snapshot);
    const SpectrumViewRenderFeedback plot_feedback =
        spectrum_view_session_.Render(
            snapshot,
            language,
            SpectrumPlotProfileContext{status.profile, status.frame_index},
            SpectrumPlotOverlays{
                .spectral_lines = spectral_lines.visible_markers.data(),
                .spectral_line_count = spectral_lines.visible_markers.size(),
                .show_spectral_line_labels = spectral_lines.marker_labels_visible,
                .layout_scope_id = spectral_lines.layout_scope_id,
                .spectral_line_label_font = spectral_line_label_font_,
            },
            {},
            touchpad_gestures_);
    if (plot_feedback.plot_submitted) {
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
    const ApplicationSettingsView settings =
        application_settings_.View();
    settings_panel_ui_.Render(
        settings,
        {
            .profile_open = status.profile_open,
            .profile_stopping = status.profile_stopping,
            .profile_path = status.profile_path,
            .profile_status_message = status.profile_status_message,
            .frame_capture_enabled =
                status.frame_capture_enabled,
            .frame_capture_pending =
                status.frame_capture_pending,
            .window_renderable =
                status.window_renderable,
            .frame_capture_output_directory =
                status.frame_capture_output_directory,
            .last_frame_capture_path =
                status.last_frame_capture_path,
            .frame_capture_status_message =
                status.frame_capture_status_message,
            .frame_capture_status =
                status.frame_capture_status,
            .frame_capture_status_operation =
                status.frame_capture_status_operation,
            .frame_capture_status_result =
                status.frame_capture_status_result,
        });
    if (settings_panel_ui_.TakeRestoreDefaultLayoutRequest()) {
        restore_default_layout_requested_ = true;
    }
    const ApplicationSettingsRuntimeState runtime{
        .profile_recording_in_progress =
            status.profile_open || status.profile_stopping,
    };
    if (std::optional<ApplicationSettingsIntent> intent =
            settings_panel_ui_.
                TakeApplicationSettingsIntent()) {
        (void)ApplyApplicationSettingsIntent(
            std::move(*intent),
            runtime);
    }
    if (settings_panel_ui_.TakeProfileOutputDirectorySelectionRequest()) {
        if (std::optional<std::filesystem::path> directory =
                ShowFolderPicker(
                    settings.language,
                    UiTextId::
                        ChooseProfileOutputFolderDialog)) {
            (void)ApplyApplicationSettingsIntent(
                ApplicationSettingsIntent::
                    SetProfileOutputDirectory(
                        std::move(*directory)),
                runtime);
        }
    }
}

ApplicationSettingsResult
ShellUi::ApplyApplicationSettingsIntent(
    ApplicationSettingsIntent intent,
    ApplicationSettingsRuntimeState runtime)
{
    ApplicationSettingsResult result =
        application_settings_.Apply(
            std::move(intent),
            runtime);
    if (result.applied() &&
        result.setting == ApplicationSetting::UiScale) {
        applied_ui_scale_percentage_ =
            application_settings_.View().
                ui_scale_percentage;
    } else if (
        result.applied() &&
        result.setting ==
            ApplicationSetting::Language) {
        applied_ui_language_ =
            application_settings_.View().language;
    } else if (
        result.applied() &&
        result.setting ==
            ApplicationSetting::Appearance) {
        applied_theme_selection_ =
            application_settings_.View().theme_selection;
    }
    return result;
}

void ShellUi::SetPanelVisibility(
    ApplicationPanel panel,
    bool visible)
{
    (void)application_settings_.Apply(
        ApplicationSettingsIntent::SetPanelVisibility(
            panel,
            visible),
        {});
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
            SourceCollectionActivationTransaction::
                NavigationIntent{
                NavigationLatencyInputKind::KeyboardPrevious,
                TakeNavigationKeyInput(NavigationLatencyInputKind::KeyboardPrevious)});
        return;
    case SampleWorkflowShortcutKind::NextSample:
        (void)SubmitSessionCommand(
            SourceCollectionSessionIntent::UpdateSampleNavigation(
                SampleNavigationIntent::Move(SampleNavigationRequest::Next())),
            SourceCollectionActivationTransaction::
                NavigationIntent{
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
            SourceCollectionActivationTransaction::
                NavigationIntent{
                    NavigationLatencyInputKind::AutoAdvance});
        return;
    }
}

void ShellUi::RenderSpectralLinesPanel(bool panel_open)
{
    spectral_lines_panel_ui_.Render(
        spectral_lines_panel_,
        session_.CurrentSampleSnapshot(),
        application_settings_.View().language,
        &panel_open);
    SetPanelVisibility(
        ApplicationPanel::SpectralLines,
        panel_open);
}

void ShellUi::SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size)
{
    // DockBuilder is an internal docking-branch API, so keep it limited to the
    // default layout seed and explicit recovery. Persistence remains standard ImGui.
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
    ImGui::DockBuilderDockWindow(kCurveDisplayWindow, smoothing_id);
    ImGui::DockBuilderDockWindow(kMainPlotWindow, center_id);
    ImGui::DockBuilderDockWindow(SpectralLinesPanelUi::WindowName(), spectral_lines_id);
    ImGui::DockBuilderFinish(dockspace_id);
    // Hidden windows may only exist in loaded settings; detached windows may
    // already be live. Recover both without clearing other settings handlers.
    for (const char* name : {
             SourceCollectionPanelUi::FilesWindowName(),
             SourceCollectionPanelUi::NavigationWindowName(),
             SourceCollectionPanelUi::AnnotationsWindowName(),
             SampleWorkflowPanelUi::LabelingWindowName(),
             SampleWorkflowPanelUi::FiltersWindowName(),
             SampleWorkflowPanelUi::SortingWindowName(),
             kInfoTagsWindow, kCurveDisplayWindow, kMainPlotWindow,
             SpectralLinesPanelUi::WindowName()}) {
        ImGui::SetWindowCollapsed(name, false);
        if (ImGuiWindowSettings* settings =
                ImGui::FindWindowSettingsByID(ImHashStr(name))) {
            settings->Collapsed = false;
        }
    }
}

}  // namespace spectiary
