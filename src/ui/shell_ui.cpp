#include "ui/shell_ui.h"

#include "domain/spectrum_fixture.h"
#include "domain/spectrum_loader.h"
#include "plot/spectrum_plot.h"
#include "ui/sample_name_autocomplete.h"

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kDockHostWindow = "SpecForge Dock Host###SpecForgeDockHostV2";
constexpr const char* kMainPlotWindow = "Spectrum###SpecForgeSpectrumV2";
constexpr const char* kFilesWindow = "Files###SpecForgeFilesV2";
constexpr const char* kInfoTagsWindow = "Info###SpecForgeInfoTagsV2";
constexpr const char* kSmoothingWindow = "Smoothing###SpecForgeSmoothingV1";
constexpr const char* kNavigationWindow = "Navigation###SpecForgeNavigationV1";
constexpr const char* kSampleNavigationNameMatchesWindow = "Sample name matches###SpecForgeSampleNameMatchesV1";
constexpr const char* kAnnotationsWindow = "Annotations###SpecForgeAnnotationsV1";
constexpr float kStatusBarSeparatorThickness = 1.0f;
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);

enum class ActionIcon {
    Trash,
};

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

std::string TrimAscii(std::string_view value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::optional<std::size_t> ParseRowIndex(std::string_view text)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::size_t value = 0;
    for (const char character : trimmed) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

std::optional<std::size_t> ParseSampleNumber(std::string_view text)
{
    const std::optional<std::size_t> value = ParseRowIndex(text);
    if (!value || *value == 0) {
        return std::nullopt;
    }
    return value;
}

template <std::size_t Size>
void CopyToBuffer(std::array<char, Size>& buffer, std::string_view text)
{
    static_assert(Size > 0);
    std::fill(buffer.begin(), buffer.end(), '\0');
    const std::size_t copy_size = std::min(text.size(), Size - 1);
    std::copy_n(text.begin(), copy_size, buffer.begin());
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

void RenderDisabledText(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
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

void ClearSmoothingCache(SpectrumPlotState& state)
{
    state.smoothing_cache_source.reset();
    state.smoothed_y_values.reset();
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

bool ActionIconButton(
    const char* id,
    const ImRect& hit_rect,
    ActionIcon icon,
    const char* tooltip,
    bool reveal_on_hover)
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

    const bool reveal_icon = !reveal_on_hover || hovered || active;
    const ImU32 icon_color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float icon_width = std::min(TrashIconButtonWidth(), width);
    const float icon_left = min.x + std::max(0.0f, (width - icon_width) * 0.5f);
    const float icon_top = min.y + std::max(0.0f, (max.y - min.y - height) * 0.5f);
    const float stroke = 1.35f;

    if (reveal_icon && icon == ActionIcon::Trash) {
        const float left = icon_left + icon_width * 0.14f;
        const float right = icon_left + icon_width * 0.86f;
        const float handle_left = icon_left + icon_width * 0.38f;
        const float handle_right = icon_left + icon_width * 0.62f;
        const float top = icon_top + height * 0.25f;
        const float lid_y = icon_top + height * 0.34f;
        const float body_top = icon_top + height * 0.43f;
        const float body_bottom = icon_top + height * 0.73f;

        draw_list->AddLine(ImVec2(handle_left, top), ImVec2(handle_right, top), icon_color, stroke);
        draw_list->AddLine(ImVec2(left, lid_y), ImVec2(right, lid_y), icon_color, stroke);
        draw_list->AddRect(
            ImVec2(left + icon_width * 0.05f, body_top),
            ImVec2(right - icon_width * 0.05f, body_bottom),
            icon_color,
            2.0f,
            0,
            stroke);
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
    }

    if (hovered && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool TrashIconButton(const char* id, const ImRect& hit_rect)
{
    return ActionIconButton(id, hit_rect, ActionIcon::Trash, "Remove from list", false);
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
    : snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
    RefreshSystemColors();
}

ShellUi::~ShellUi()
{
    (void)sample_labeling_.FlushStateCache();
    spectral_lines_panel_.FlushCache();
}

void ShellUi::Render(const ShellStatus& status)
{
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
    sample_labeling_.MaybeSaveStateCache(status.frame_index);
    spectral_lines_panel_.MaybeSaveCache(status.frame_index);
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
    EnsureSnapshotMatchesNavigation();
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
    EnsureSnapshotMatchesNavigation();
}

void ShellUi::RemoveSource(std::size_t source_index)
{
    if (source_index >= sources_.size()) {
        return;
    }

    const bool removed_current = current_source_index_ && *current_source_index_ == source_index;
    sample_navigation_.RemoveSource(sources_[source_index].key);
    std::optional<std::size_t> next_current_index;
    if (removed_current && sources_.size() > 1) {
        next_current_index = source_index + 1 < sources_.size() ? source_index : source_index - 1;
    }

    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(source_index));

    if (removed_current) {
        current_source_index_.reset();
        sample_labeling_.ClearActiveSource();
        sample_filters_.Clear();
        active_sample_workflow_identity_.reset();
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
        if (next_current_index) {
            ActivateSource(*next_current_index);
        } else {
            SetSnapshot(MakeSmallSyntheticSpectrumSnapshot());
            sample_navigation_.ClearActiveSource();
            SyncNavigationInputs();
        }
        return;
    }

    if (current_source_index_ && *current_source_index_ > source_index) {
        current_source_index_ = *current_source_index_ - 1;
    }
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
        std::string message;
        (void)sample_navigation_.AddReadOnlyAnnotationToActiveSource(*path, &message);
        panel_visibility_.annotations = true;
        ApplySampleFiltersToNavigation();
    }
}

void ShellUi::SetSnapshot(SpectrumSnapshotHandle snapshot)
{
    const bool show_smoothed = plot_state_.show_smoothed;
    const bool show_raw_when_smoothed = plot_state_.show_raw_when_smoothed;
    const SpectrumSmoothingSettings smoothing = plot_state_.smoothing;
    snapshot_ = std::move(snapshot);
    plot_state_ = SpectrumPlotState{};
    plot_state_.show_smoothed = show_smoothed;
    plot_state_.show_raw_when_smoothed = show_raw_when_smoothed;
    plot_state_.smoothing = smoothing;
}

void ShellUi::EnsureSnapshotMatchesNavigation()
{
    SyncSampleNavigationSession();
    const std::optional<std::size_t> navigation_index = sample_navigation_.current_index();
    if (navigation_index && snapshot_ && snapshot_->collection.spectrum_count > 0 &&
        snapshot_->collection.current_index != *navigation_index) {
        LoadActiveSourceAt(*navigation_index);
        return;
    }
    SyncNavigationInputs();
}

void ShellUi::LoadActiveSourceAt(std::size_t spectrum_index)
{
    const bool has_active_source =
        current_source_index_ && *current_source_index_ < sources_.size() && !sources_[*current_source_index_].path.empty();
    if (!has_active_source) {
        return;
    }

    const std::filesystem::path path = sources_[*current_source_index_].path;
    SpectrumSnapshotHandle loaded_snapshot = LoadSpectrumSnapshotFromPath(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot));
    SyncSampleNavigationSession();
    SyncNavigationInputs();
}

SampleNavigationResult ShellUi::RequestSampleNavigation(const SampleNavigationRequest& request)
{
    const SampleNavigationResult result = sample_navigation_.Navigate(request);
    if (result.has_active_source && result.target_found &&
        (request.kind == SampleNavigationRequestKind::Previous || request.kind == SampleNavigationRequestKind::Next)) {
        (void)sample_labeling_.RememberActivePosition(result.current_index);
    }
    if (result.has_active_source && result.target_found) {
        if (!snapshot_ || snapshot_->collection.current_index != result.current_index) {
            LoadActiveSourceAt(result.current_index);
        } else {
            SyncNavigationInputs();
        }
    }
    return result;
}

void ShellUi::SyncSampleNavigationSession()
{
    const bool has_active_source =
        current_source_index_ && *current_source_index_ < sources_.size() && snapshot_ && !snapshot_->source.path.empty();
    if (!has_active_source) {
        sample_navigation_.ClearActiveSource();
        sample_labeling_.ClearActiveSource();
        sample_filters_.Clear();
        active_sample_workflow_identity_.reset();
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
        return;
    }

    sample_navigation_.ActivateSource(sources_[*current_source_index_].key, snapshot_);
    SyncSampleWorkflowSession();
    ApplySampleFiltersToNavigation();
}

void ShellUi::SyncNavigationInputs()
{
    const std::optional<std::size_t> navigation_index = sample_navigation_.current_index();
    if (navigation_index) {
        std::snprintf(
            row_index_buffer_.data(),
            row_index_buffer_.size(),
            "%zu",
            *navigation_index + 1);
    } else {
        row_index_buffer_.fill('\0');
    }
    if (snapshot_) {
        CopyToBuffer(sample_name_query_buffer_, snapshot_->current_spectrum.name);
        sample_navigation_.SetSampleNameQuery(snapshot_->current_spectrum.name);
    } else {
        sample_name_query_buffer_.fill('\0');
        sample_navigation_.SetSampleNameQuery({});
    }
    ClearSampleNameSearch();
}

void ShellUi::BeginSampleNameSearch()
{
    sample_name_search_active_ = true;
    sample_name_search_restore_name_ = snapshot_ ? snapshot_->current_spectrum.name : std::string{};
}

void ShellUi::ClearSampleNameSearch()
{
    sample_name_matches_open_ = false;
    sample_name_search_active_ = false;
    sample_name_search_restore_name_.clear();
}

void ShellUi::RestoreFailedSampleNameSearch()
{
    CopyToBuffer(sample_name_query_buffer_, sample_name_search_restore_name_);
    sample_navigation_.SetSampleNameQuery(sample_name_search_restore_name_);
    ClearSampleNameSearch();
}

void ShellUi::CommitSampleNameSearch(std::size_t target_row, const std::string& matched_name)
{
    CopyToBuffer(sample_name_query_buffer_, matched_name);
    sample_navigation_.SetSampleNameQuery(matched_name);
    (void)RequestSampleNavigation(SampleNavigationRequest::LocateRow(target_row));
    ClearSampleNameSearch();
}

void ShellUi::SyncSampleWorkflowSession()
{
    if (!snapshot_ || snapshot_->source.path.empty() || snapshot_->collection.spectrum_count == 0) {
        sample_labeling_.ClearActiveSource();
        sample_filters_.Clear();
        active_sample_workflow_identity_.reset();
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
        return;
    }

    const SampleCollectionIdentity identity = BuildSampleCollectionIdentity(*snapshot_);
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        sample_filters_.Clear();
        sample_workflow_panel_ui_.ResetForSampleWorkflow();
        active_sample_workflow_identity_ = identity.id;
    }
    sample_labeling_.ActivateSource(identity.id, identity.spectrum_count);
}

std::vector<SampleFilterSource> ShellUi::BuildSampleFilterSources() const
{
    return sample_workflow_panel_ui_.BuildFilterSources(sample_navigation_, sample_labeling_);
}

void ShellUi::ApplySampleFiltersToNavigation()
{
    const std::size_t sample_count =
        sample_navigation_.spectrum_count().value_or(snapshot_ ? snapshot_->collection.spectrum_count : 0);
    if (sample_count == 0) {
        sample_navigation_.ClearSampleFilter();
        return;
    }

    const SampleFilterEvaluation evaluation = sample_filters_.Evaluate(BuildSampleFilterSources(), sample_count);
    if (evaluation.active) {
        sample_navigation_.SetSampleFilter(evaluation.included_samples);
    } else {
        sample_navigation_.ClearSampleFilter();
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
        const bool can_open_annotation =
            sample_navigation_.active_context() != nullptr && sample_navigation_.spectrum_count().value_or(0) > 0;
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
    if (!ImGui::Begin(kFilesWindow, &panel_visibility_.files)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Files");
    ImGui::Separator();

    if (ImGui::Button("Add file...")) {
        OpenSourceFromFilePicker();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add folder...")) {
        OpenSourceFromFolderPicker();
    }
    ImGui::SameLine();
    const std::string source_count =
        std::to_string(sources_.size()) + (sources_.size() == 1 ? " source" : " sources");
    RenderDisabledText(source_count);

    ImGui::Spacing();
    const bool has_active_source =
        !sources_.empty() && current_source_index_ && *current_source_index_ < sources_.size() && snapshot_ &&
        !snapshot_->source.path.empty();
    if (!has_active_source) {
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

void ShellUi::RenderNavigationPanel()
{
    if (!ImGui::Begin(kNavigationWindow, &panel_visibility_.navigation)) {
        ImGui::End();
        return;
    }

    if (!snapshot_ || snapshot_->source.path.empty() || snapshot_->collection.spectrum_count == 0) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return;
    }

    const std::size_t navigation_index =
        sample_navigation_.current_index().value_or(snapshot_->collection.current_index);
    const std::size_t navigation_count =
        sample_navigation_.spectrum_count().value_or(snapshot_->collection.spectrum_count);

    ImGui::TextUnformatted("sample:");
    ImGui::SameLine();
    const float sample_input_width =
        std::max(72.0f, ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::SetNextItemWidth(sample_input_width);
    const bool index_changed = ImGui::InputText(
        "##SampleNavigationSample",
        row_index_buffer_.data(),
        row_index_buffer_.size(),
        ImGuiInputTextFlags_CharsDecimal);
    const bool index_deactivated_after_edit = ImGui::IsItemDeactivatedAfterEdit();
    if (index_changed) {
        const std::optional<std::size_t> target_sample = ParseSampleNumber(row_index_buffer_.data());
        if (target_sample && *target_sample <= navigation_count) {
            const std::size_t target_row = *target_sample - 1;
            if (target_row != navigation_index) {
                (void)RequestSampleNavigation(SampleNavigationRequest::LocateRow(target_row));
            }
        }
    }
    if (index_deactivated_after_edit) {
        SyncNavigationInputs();
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Text("/%llu", static_cast<unsigned long long>(navigation_count));
    ImGui::SameLine();
    const bool can_previous = sample_navigation_.can_move_previous();
    const bool can_next = sample_navigation_.can_move_next();
    const ImVec2 sample_step_button_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    if (!can_previous) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("-##PreviousSample", sample_step_button_size)) {
        (void)RequestSampleNavigation(SampleNavigationRequest::Previous());
    }
    if (!can_previous) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!can_next) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("+##NextSample", sample_step_button_size)) {
        (void)RequestSampleNavigation(SampleNavigationRequest::Next());
    }
    if (!can_next) {
        ImGui::EndDisabled();
    }

    RenderSampleNameSearch(navigation_index);

    ImGui::End();
}

void ShellUi::RenderSampleNameSearch(std::size_t navigation_index)
{
    ImGui::TextUnformatted("name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    const bool sample_name_changed = ImGui::InputText(
        "##SampleNavigationName",
        sample_name_query_buffer_.data(),
        sample_name_query_buffer_.size());
    const float sample_name_input_width = ImGui::GetItemRectSize().x;
    const float sample_name_input_left = ImGui::GetItemRectMin().x;
    const float sample_name_input_bottom = ImGui::GetItemRectMax().y;
    const bool sample_name_input_activated = ImGui::IsItemActivated();
    const bool sample_name_input_active = ImGui::IsItemActive();

    if (sample_name_input_activated || (sample_name_changed && !sample_name_search_active_)) {
        BeginSampleNameSearch();
    }

    const SampleCollectionContext* context = sample_navigation_.active_context();
    if (sample_name_changed) {
        sample_navigation_.SetSampleNameQuery(sample_name_query_buffer_.data());
        const std::vector<std::size_t>& matches = sample_navigation_.sample_name_matches();
        const std::string typed_name = sample_name_query_buffer_.data();
        if (context != nullptr && !context->sample_names.empty() && !typed_name.empty() && !matches.empty()) {
            const SampleNameAutocompleteEvaluation evaluation =
                EvaluateSampleNameAutocomplete(context->sample_names, matches, typed_name);
            if (evaluation.exact_match) {
                CommitSampleNameSearch(*evaluation.exact_match, context->sample_names[*evaluation.exact_match]);
                return;
            }
            sample_name_matches_open_ = true;
        } else {
            sample_name_matches_open_ = false;
        }
    }

    const std::vector<std::size_t>& matches = sample_navigation_.sample_name_matches();
    const std::string typed_name = sample_name_query_buffer_.data();
    const SampleNameAutocompleteEvaluation evaluation =
        context == nullptr ? SampleNameAutocompleteEvaluation{}
                           : EvaluateSampleNameAutocomplete(context->sample_names, matches, typed_name);
    const bool has_partial_matches = evaluation.has_partial_matches;
    if (has_partial_matches && sample_name_input_active) {
        sample_name_matches_open_ = true;
    } else if (!has_partial_matches) {
        sample_name_matches_open_ = false;
    }

    bool sample_name_dropdown_interacting = false;
    std::optional<std::size_t> selected_row;
    std::string selected_name;
    if (sample_name_matches_open_ && has_partial_matches) {
        const float row_height = ImGui::GetTextLineHeightWithSpacing();
        const std::size_t visible_rows = std::min<std::size_t>(matches.size(), 12);
        const float dropdown_height =
            row_height * static_cast<float>(visible_rows) + ImGui::GetStyle().WindowPadding.y * 2.0f;
        const ImVec2 dropdown_min(sample_name_input_left, sample_name_input_bottom);
        const ImVec2 dropdown_max(
            sample_name_input_left + sample_name_input_width,
            sample_name_input_bottom + dropdown_height);
        sample_name_dropdown_interacting = ImGui::IsMouseHoveringRect(dropdown_min, dropdown_max, false);
        constexpr ImGuiWindowFlags dropdown_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
        ImGui::SetNextWindowPos(dropdown_min, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(sample_name_input_width, dropdown_height), ImGuiCond_Always);
        if (ImGui::Begin(kSampleNavigationNameMatchesWindow, nullptr, dropdown_flags)) {
            sample_name_dropdown_interacting =
                sample_name_dropdown_interacting ||
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            for (const std::size_t row : matches) {
                if (row >= context->sample_names.size()) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(row));
                if (ImGui::Selectable(context->sample_names[row].c_str(), row == navigation_index)) {
                    selected_row = row;
                    selected_name = context->sample_names[row];
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
    }

    if (selected_row) {
        CommitSampleNameSearch(*selected_row, selected_name);
        return;
    }

    if (ShouldRestoreSampleNameSearch(
            sample_name_search_active_,
            selected_row.has_value(),
            sample_name_input_active,
            sample_name_dropdown_interacting)) {
        RestoreFailedSampleNameSearch();
    }
}

void ShellUi::RenderAnnotationsPanel()
{
    if (!ImGui::Begin(kAnnotationsWindow, &panel_visibility_.annotations)) {
        ImGui::End();
        return;
    }

    const SampleCollectionContext* context = sample_navigation_.active_context();
    if (!snapshot_ || context == nullptr || snapshot_->source.path.empty()) {
        ImGui::TextDisabled("No active source");
        ImGui::End();
        return;
    }

    if (!context->messages.empty()) {
        for (const std::string& message : context->messages) {
            ImGui::TextDisabled("%s", message.c_str());
        }
        ImGui::Separator();
    }

    if (context->annotations.empty()) {
        ImGui::TextDisabled("No read-only annotations");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "sample_annotations",
            2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Result");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();

        const std::size_t current_index =
            sample_navigation_.current_index().value_or(snapshot_->collection.current_index);
        for (const SampleAnnotationResult& annotation : context->annotations) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(annotation.name.c_str());
            if (ImGui::IsItemHovered()) {
                const std::string path = NarrowPath(annotation.path);
                ImGui::SetTooltip("%s", path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (current_index < annotation.values.size()) {
                ImGui::TextUnformatted(annotation.values[current_index].display_text.c_str());
            } else {
                ImGui::TextDisabled("(missing)");
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void ShellUi::RenderLabelingPanel()
{
    sample_workflow_panel_ui_.RenderLabeling(
        snapshot_,
        sample_navigation_,
        sample_labeling_,
        label_shortcut_context_active_,
        &panel_visibility_.labeling,
        [this](const SampleNavigationRequest& request) {
            return RequestSampleNavigation(request);
        },
        [this]() {
            ApplySampleFiltersToNavigation();
        },
        []() {
            return ShowLabelOutputFilePicker();
        });
}

void ShellUi::RenderFiltersPanel()
{
    sample_workflow_panel_ui_.RenderFilters(
        snapshot_,
        sample_navigation_,
        sample_labeling_,
        sample_filters_,
        &panel_visibility_.filters,
        [this]() {
            ApplySampleFiltersToNavigation();
        });
}

void ShellUi::RenderSmoothingPanel()
{
    if (!ImGui::Begin(kSmoothingWindow, &panel_visibility_.smoothing)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Smoothing");
    ImGui::Separator();

    if (!snapshot_ || !snapshot_->capabilities.can_plot_current_spectrum) {
        ImGui::TextDisabled("No plottable spectrum");
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Show smoothed curve", &plot_state_.show_smoothed);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        plot_state_.show_smoothed = false;
        plot_state_.show_raw_when_smoothed = true;
        plot_state_.smoothing = SpectrumSmoothingSettings{};
        ClearSmoothingCache(plot_state_);
    }

    static constexpr const char* kMethodLabels[] = {"None", "Gaussian", "Median"};
    int method_index = SmoothingMethodIndex(plot_state_.smoothing.method);
    if (ImGui::Combo("Method", &method_index, kMethodLabels, IM_ARRAYSIZE(kMethodLabels))) {
        plot_state_.smoothing.method = SmoothingMethodFromIndex(method_index);
        ClearSmoothingCache(plot_state_);
    }

    if (plot_state_.smoothing.method == SpectrumSmoothingMethod::Gaussian) {
        float sigma = static_cast<float>(plot_state_.smoothing.gaussian_sigma);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::DragFloat("Sigma", &sigma, 0.05f, 0.01f, 100.0f, "%.2f")) {
            plot_state_.smoothing.gaussian_sigma = std::max(0.01, static_cast<double>(sigma));
            ClearSmoothingCache(plot_state_);
        }
    } else if (plot_state_.smoothing.method == SpectrumSmoothingMethod::Median) {
        const int normalized_kernel_size = NormalizeMedianKernelSize(plot_state_.smoothing.median_kernel_size);
        if (normalized_kernel_size != plot_state_.smoothing.median_kernel_size) {
            plot_state_.smoothing.median_kernel_size = normalized_kernel_size;
            ClearSmoothingCache(plot_state_);
        }
        int kernel_size = plot_state_.smoothing.median_kernel_size;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Kernel size", &kernel_size, 2, 10)) {
            plot_state_.smoothing.median_kernel_size = NormalizeMedianKernelSize(kernel_size);
            ClearSmoothingCache(plot_state_);
        }
        const int effective_kernel_size =
            EffectiveMedianKernelSize(plot_state_.smoothing.median_kernel_size, snapshot_->current_spectrum.point_count);
        if (effective_kernel_size != plot_state_.smoothing.median_kernel_size) {
            ImGui::TextDisabled("Effective kernel: %d", effective_kernel_size);
        }
    }

    if (plot_state_.smoothing.method == SpectrumSmoothingMethod::None && plot_state_.show_smoothed) {
        ImGui::TextDisabled("No smoothing method selected");
    }

    if (!plot_state_.show_smoothed || plot_state_.smoothing.method == SpectrumSmoothingMethod::None) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Show raw overlay", &plot_state_.show_raw_when_smoothed);
    if (!plot_state_.show_smoothed || plot_state_.smoothing.method == SpectrumSmoothingMethod::None) {
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
    } else {
        ImGui::TextDisabled("No snapshot");
    }

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
    label_shortcut_context_active_ =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const std::vector<const SpectralLineMarker*> spectral_lines =
        spectral_lines_panel_.FilteredMarkers(snapshot_, false);
    RenderSpectrumPlot(
        snapshot_,
        plot_state_,
        SpectrumPlotProfileContext{status.profile, status.frame_index},
        plot_style_,
        SpectrumPlotOverlays{spectral_lines.data(), spectral_lines.size(), spectral_lines_panel_.show_labels()});
    ImGui::End();
}

void ShellUi::RenderSpectralLinesPanel()
{
    spectral_lines_panel_ui_.Render(spectral_lines_panel_, snapshot_, &panel_visibility_.spectral_lines);
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

    ImGui::DockBuilderDockWindow(kFilesWindow, files_id);
    ImGui::DockBuilderDockWindow(kNavigationWindow, navigation_id);
    ImGui::DockBuilderDockWindow(kAnnotationsWindow, annotations_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::LabelingWindowName(), labeling_id);
    ImGui::DockBuilderDockWindow(SampleWorkflowPanelUi::FiltersWindowName(), filters_id);
    ImGui::DockBuilderDockWindow(kInfoTagsWindow, info_tags_id);
    ImGui::DockBuilderDockWindow(kSmoothingWindow, smoothing_id);
    ImGui::DockBuilderDockWindow(kMainPlotWindow, center_id);
    ImGui::DockBuilderDockWindow(SpectralLinesPanelUi::WindowName(), spectral_lines_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

}  // namespace specforge
