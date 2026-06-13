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

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kDockHostWindow = "SpecForge Dock Host###SpecForgeDockHostV2";
constexpr const char* kMainPlotWindow = "Spectrum###SpecForgeSpectrumV2";
constexpr const char* kFilesWindow = "Files###SpecForgeFilesV2";
constexpr const char* kInfoTagsWindow = "Info & Tags###SpecForgeInfoTagsV2";
constexpr const char* kSpectralLinesWindow = "Spectral Lines###SpecForgeSpectralLinesV2";
constexpr float kStatusBarHeight = 28.0f;
const ImVec4 kFallbackSpectrumLineColor = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);
constexpr const char* kMarkerReferenceDragPayload = "SpecForgeMarkerReference";
constexpr const char* kUserGroupDragPayload = "SpecForgeUserGroup";
constexpr const char* kRenameGroupingViewPopup = "Rename grouping view###SpecForgeRenameGroupingViewPopup";
constexpr const char* kRenameUserGroupPopup = "Rename group###SpecForgeRenameUserGroupPopup";
constexpr const char* kDeleteGroupingViewPopup = "Delete grouping view###SpecForgeDeleteGroupingViewPopup";

struct MarkerReferenceDragPayload {
    std::string view_id;
    std::string source_group_id;
    std::string marker_id;
};

struct UserGroupDragPayload {
    std::string view_id;
    std::string group_id;
};

struct UserGroupReorderLine {
    float y = 0.0f;
    float x_min = 0.0f;
    float x_max = 0.0f;
};

struct UserGroupReorderGapResult {
    UserGroupReorderLine line;
    float min_y = 0.0f;
};

enum class ActionIcon {
    Minus,
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

bool ContainsCaseInsensitive(std::string_view text, std::string_view pattern)
{
    if (pattern.empty()) {
        return true;
    }
    return LowerAscii(std::string(text)).find(LowerAscii(std::string(pattern))) != std::string::npos;
}

bool HasNonWhitespace(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
}

std::string EncodeMarkerReferenceDragPayload(
    std::string_view view_id,
    std::string_view source_group_id,
    std::string_view marker_id)
{
    std::string payload;
    payload.reserve(view_id.size() + source_group_id.size() + marker_id.size() + 2);
    payload.append(view_id);
    payload.push_back('\0');
    payload.append(source_group_id);
    payload.push_back('\0');
    payload.append(marker_id);
    return payload;
}

std::optional<MarkerReferenceDragPayload> DecodeMarkerReferenceDragPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0) {
        return std::nullopt;
    }

    const auto* bytes = static_cast<const char*>(payload.Data);
    const std::string_view data(bytes, static_cast<std::size_t>(payload.DataSize));
    const std::size_t first_separator = data.find('\0');
    if (first_separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second_separator = data.find('\0', first_separator + 1);
    if (second_separator == std::string_view::npos) {
        return std::nullopt;
    }

    MarkerReferenceDragPayload decoded;
    decoded.view_id = std::string(data.substr(0, first_separator));
    decoded.source_group_id =
        std::string(data.substr(first_separator + 1, second_separator - first_separator - 1));
    decoded.marker_id = std::string(data.substr(second_separator + 1));
    if (decoded.view_id.empty() || decoded.source_group_id.empty() || decoded.marker_id.empty()) {
        return std::nullopt;
    }
    return decoded;
}

std::string EncodeUserGroupDragPayload(std::string_view view_id, std::string_view group_id)
{
    std::string payload;
    payload.reserve(view_id.size() + group_id.size() + 1);
    payload.append(view_id);
    payload.push_back('\0');
    payload.append(group_id);
    return payload;
}

std::optional<UserGroupDragPayload> DecodeUserGroupDragPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0) {
        return std::nullopt;
    }

    const auto* bytes = static_cast<const char*>(payload.Data);
    const std::string_view data(bytes, static_cast<std::size_t>(payload.DataSize));
    const std::size_t separator = data.find('\0');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    UserGroupDragPayload decoded;
    decoded.view_id = std::string(data.substr(0, separator));
    decoded.group_id = std::string(data.substr(separator + 1));
    if (decoded.view_id.empty() || decoded.group_id.empty()) {
        return std::nullopt;
    }
    return decoded;
}

std::optional<UserGroupDragPayload> CurrentUserGroupDragPayload()
{
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(kUserGroupDragPayload)) {
        return std::nullopt;
    }
    return DecodeUserGroupDragPayload(*payload);
}

void DrawUserGroupReorderLine(float y, float x_min, float x_max)
{
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(x_min, y), ImVec2(x_max, y), color, 2.0f);
    draw_list->AddCircleFilled(ImVec2(x_min, y), 3.0f, color);
}

std::optional<UserGroupDragPayload> AcceptUserGroupReorderPayload(std::string_view view_id, bool& accepted)
{
    accepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUserGroupDragPayload, flags)) {
        if (std::optional<UserGroupDragPayload> drag = DecodeUserGroupDragPayload(*payload)) {
            if (drag->view_id == view_id) {
                accepted = true;
                if (payload->IsDelivery()) {
                    return drag;
                }
            }
        }
    }
    return std::nullopt;
}

UserGroupReorderGapResult RenderUserGroupReorderGap(bool highlight)
{
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const float height = std::max(ImGui::GetStyle().ItemSpacing.y * 2.0f, 6.0f);
    ImGui::InvisibleButton("##user_group_reorder_gap", ImVec2(width, height));
    const ImVec2 gap_min = ImGui::GetItemRectMin();
    const ImVec2 gap_max = ImGui::GetItemRectMax();

    UserGroupReorderGapResult result;
    result.line = UserGroupReorderLine{
        (gap_min.y + gap_max.y) * 0.5f,
        gap_min.x,
        gap_max.x,
    };
    result.min_y = gap_min.y;
    if (highlight) {
        DrawUserGroupReorderLine(result.line.y, result.line.x_min, result.line.x_max);
    }
    return result;
}

std::optional<UserGroupDragPayload> RenderUserGroupReorderTarget(
    std::string_view view_id,
    const ImRect& hit_rect,
    const UserGroupReorderLine& line,
    ImGuiID target_id)
{
    std::optional<UserGroupDragPayload> delivered;
    bool highlight = false;
    if (ImGui::BeginDragDropTargetCustom(hit_rect, target_id)) {
        delivered = AcceptUserGroupReorderPayload(view_id, highlight);
        ImGui::EndDragDropTarget();
    }
    if (highlight) {
        DrawUserGroupReorderLine(line.y, line.x_min, line.x_max);
    }
    return delivered;
}

std::string MarkerWavelengthText(const SpectralLineMarker& marker)
{
    std::array<char, 64> buffer = {};
    if (marker.kind == SpectralLineMarkerKind::Line && marker.vacuum_angstrom) {
        std::snprintf(buffer.data(), buffer.size(), "%.3f", *marker.vacuum_angstrom);
        return buffer.data();
    }
    if (marker.kind == SpectralLineMarkerKind::Band && marker.start_vacuum_angstrom && marker.end_vacuum_angstrom) {
        std::snprintf(
            buffer.data(),
            buffer.size(),
            "%.3f-%.3f",
            *marker.start_vacuum_angstrom,
            *marker.end_vacuum_angstrom);
        return buffer.data();
    }
    return {};
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

void RenderWrappedStatusText(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool RenderGroupVisibilityControl(GroupVisibilityState state, bool& next_visible)
{
    next_visible = true;
    if (state == GroupVisibilityState::SearchFiltered || state == GroupVisibilityState::Empty) {
        bool value = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##group_visibility", &value);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                state == GroupVisibilityState::SearchFiltered
                    ? "Search is filtering this group; bulk visibility is disabled."
                    : "No resolved markers in this group.");
        }
        return false;
    }

    bool value = state == GroupVisibilityState::AllVisible;
    if (state == GroupVisibilityState::Mixed) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    const bool changed = ImGui::Checkbox("##group_visibility", &value);
    if (state == GroupVisibilityState::Mixed) {
        ImGui::PopItemFlag();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show or hide all resolved markers in this group.");
    }
    if (changed) {
        next_visible = state == GroupVisibilityState::Mixed ? true : value;
    }
    return changed;
}

void RenderSharedReferenceMarker()
{
    ImGui::TextDisabled("*");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Shared marker reference: this marker also appears in another group in this view.");
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

    if (reveal_icon && icon == ActionIcon::Minus) {
        const float y = icon_top + height * 0.5f;
        draw_list->AddLine(
            ImVec2(icon_left + icon_width * 0.18f, y),
            ImVec2(icon_left + icon_width * 0.82f, y),
            icon_color,
            stroke);
    } else if (reveal_icon && icon == ActionIcon::Trash) {
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

bool HiddenActionIconButton(const char* id, const ImRect& hit_rect, ActionIcon icon, const char* tooltip)
{
    return ActionIconButton(id, hit_rect, icon, tooltip, true);
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
    spectral_lines_panel_.FlushCache();
}

void ShellUi::Render(const ShellStatus& status)
{
    spectral_lines_panel_.SetFrameIndex(status.frame_index);
    RenderDockHost(status);
    RenderFilesPanel();
    RenderInfoTagsPanel();
    RenderMainPlot(status);
    RenderSpectralLinesPanel();
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

    const ImGuiID dockspace_id = ImGui::GetID("SpecForgeDockSpaceFourPaneV2");
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
                "Spectrum: %llu / %llu",
                static_cast<unsigned long long>(snapshot_->collection.current_index + 1),
                static_cast<unsigned long long>(snapshot_->collection.spectrum_count));
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

void ShellUi::RenderSpectralLineGroupingView(const GroupingView& view, GroupingView* editable_view)
{
    const bool editable = editable_view != nullptr && !editable_view->read_only;
    const SpectralLineCatalog& catalog = spectral_lines_panel_.catalog();
    const CatalogIdentity& identity = spectral_lines_panel_.catalog_identity();
    CatalogUserState& user_state = spectral_lines_panel_.user_state();
    CatalogPanelState& panel_state = spectral_lines_panel_.panel_state();
    const std::string search(spectral_lines_panel_.filter_buffer().data());
    const bool search_active = !search.empty();

    if (editable) {
        if (ImGui::Button("+ Group")) {
            spectral_lines_panel_.AddUserGroupToView(*editable_view);
        }
        ImGui::SameLine();
    }

    const std::vector<const SpectralLineMarker*> visible_markers =
        spectral_lines_panel_.FilteredMarkers(snapshot_, false);
    const std::string marker_count = std::to_string(visible_markers.size()) + " plot-visible / " +
                                     std::to_string(catalog.markers.size()) + " catalog markers";
    RenderDisabledText(marker_count);

    const std::unordered_map<std::string, int> shared_counts = MarkerReferenceCounts(view, identity);
    const std::optional<UserGroupDragPayload> active_user_group_drag =
        editable ? CurrentUserGroupDragPayload() : std::nullopt;
    const bool group_reorder_drag_active =
        editable && active_user_group_drag && active_user_group_drag->view_id == editable_view->id;

    if (view.groups.empty()) {
        ImGui::TextDisabled("No groups in this view.");
        return;
    }

    std::optional<UserGroupReorderGapResult> current_reorder_gap;
    std::optional<float> previous_group_midpoint_y;
    bool group_context_popup_open = false;
    const auto render_reorder_gap = [&](const UserGroup& target_group) {
        if (!group_reorder_drag_active) {
            current_reorder_gap = std::nullopt;
            return;
        }
        ImGui::PushID("group_reorder_gap");
        ImGui::PushID(target_group.id.c_str());
        current_reorder_gap = RenderUserGroupReorderGap(false);
        ImGui::PopID();
        ImGui::PopID();
    };

    for (std::size_t group_index = 0; group_index < view.groups.size(); ++group_index) {
        const UserGroup& group = view.groups[group_index];

        render_reorder_gap(group);

        std::vector<const MarkerReference*> matching_references;
        matching_references.reserve(group.marker_references.size());
        for (const MarkerReference& reference : group.marker_references) {
            if (MarkerMatchesSearch(catalog, identity, reference, search)) {
                matching_references.push_back(&reference);
            }
        }
        const bool group_has_search_matches = !matching_references.empty();
        const bool group_dimmed_by_search = search_active && !group_has_search_matches;

        ImGui::PushID(group.id.c_str());
        ImGui::AlignTextToFramePadding();
        bool next_group_visible = true;
        if (RenderGroupVisibilityControl(
                VisibilityStateForGroup(user_state, group, catalog, identity, search_active),
                next_group_visible)) {
            spectral_lines_panel_.SetGroupVisibility(group, next_group_visible, search_active);
        }

        ImGui::SameLine();
        const bool ordinary_group = !group.is_unassigned && group.id != UnassignedUserGroupId();
        const bool group_context_active = group_context_view_id_ && group_context_group_id_ &&
                                          *group_context_view_id_ == view.id &&
                                          *group_context_group_id_ == group.id;
        ImGuiTreeNodeFlags group_flags = ImGuiTreeNodeFlags_SpanFullWidth;
        if (group_reorder_drag_active) {
            group_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        } else if (editable && ordinary_group) {
            group_flags |= ImGuiTreeNodeFlags_AllowOverlap;
        }
        if (group_context_active) {
            group_flags |= ImGuiTreeNodeFlags_Selected;
        }
        const std::string expansion_key = GroupExpansionKey(view.id, group.id);
        const bool group_was_expanded = panel_state.expanded_group_ids.find(expansion_key) !=
                                        panel_state.expanded_group_ids.end();
        if (group_reorder_drag_active) {
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        } else if (search_active && group_has_search_matches) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else {
            ImGui::SetNextItemOpen(group_was_expanded, ImGuiCond_Always);
        }
        if (group_dimmed_by_search) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (group_reorder_drag_active) {
            const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        }
        const bool group_open = ImGui::TreeNodeEx(
            "group",
            group_flags,
            "%s (%zu)",
            group.name.c_str(),
            matching_references.size());
        const ImVec2 group_item_min = ImGui::GetItemRectMin();
        const ImVec2 group_item_max = ImGui::GetItemRectMax();
        if (group_reorder_drag_active) {
            ImGui::PopStyleColor(2);
        }
        if (group_dimmed_by_search) {
            ImGui::PopStyleColor();
        }
        if (!group_reorder_drag_active && ImGui::IsItemToggledOpen()) {
            spectral_lines_panel_.SetGroupExpanded(view.id, group.id, group_open);
        }
        const bool group_contents_open = !group_reorder_drag_active && group_open;
        const float group_midpoint_y = (group_item_min.y + group_item_max.y) * 0.5f;

        if (group_reorder_drag_active && current_reorder_gap) {
            const float target_min_y = previous_group_midpoint_y.value_or(current_reorder_gap->min_y);
            const ImRect target_rect(
                ImVec2(current_reorder_gap->line.x_min, target_min_y),
                ImVec2(current_reorder_gap->line.x_max, group_midpoint_y));
            if (std::optional<UserGroupDragPayload> drop = RenderUserGroupReorderTarget(
                    editable_view->id,
                    target_rect,
                    current_reorder_gap->line,
                    ImGui::GetID("user_group_reorder_target"))) {
                spectral_lines_panel_.ReorderUserGroupBeforeInView(
                    *editable_view,
                    drop->group_id,
                    group.id);
            }
        }

        bool group_deleted = false;
        if (editable && ordinary_group && !group_reorder_drag_active && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            group_context_view_id_ = editable_view->id;
            group_context_group_id_ = group.id;
        }
        if (editable && ordinary_group && !group_reorder_drag_active &&
            ImGui::BeginPopupContextItem("user_group_context")) {
            group_context_popup_open = true;
            group_context_view_id_ = editable_view->id;
            group_context_group_id_ = group.id;
            if (ImGui::Selectable("Rename")) {
                renaming_group_view_id_ = editable_view->id;
                renaming_group_id_ = group.id;
                std::snprintf(
                    renaming_group_name_.data(),
                    renaming_group_name_.size(),
                    "%s",
                    group.name.c_str());
                renaming_group_popup_requested_ = true;
            }
            if (ImGui::Selectable("Delete")) {
                group_deleted = spectral_lines_panel_.DeleteUserGroupFromView(*editable_view, group.id);
                group_context_view_id_.reset();
                group_context_group_id_.reset();
            }
            ImGui::EndPopup();
        }
        if (group_deleted) {
            if (group_contents_open) {
                ImGui::TreePop();
            }
            ImGui::PopID();
            continue;
        }

        if (editable && ordinary_group && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string drag_payload = EncodeUserGroupDragPayload(editable_view->id, group.id);
            ImGui::SetDragDropPayload(
                kUserGroupDragPayload,
                drag_payload.data(),
                static_cast<int>(drag_payload.size()));
            ImGui::TextUnformatted(group.name.c_str());
            ImGui::TextDisabled("Drop between groups to reorder");
            ImGui::EndDragDropSource();
        }

        if (editable && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMarkerReferenceDragPayload)) {
                if (std::optional<MarkerReferenceDragPayload> drag = DecodeMarkerReferenceDragPayload(*payload)) {
                    if (drag->view_id == editable_view->id) {
                        const bool copy = ImGui::GetIO().KeyCtrl;
                        spectral_lines_panel_.MoveOrCopyMarkerReferenceToGroup(
                            *editable_view,
                            drag->marker_id,
                            drag->source_group_id,
                            group.id,
                            copy);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (editable && ordinary_group && !group_reorder_drag_active) {
            const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
            const float action_width = ImGui::GetFrameHeight();
            const ImRect delete_rect(
                ImVec2(std::max(group_item_min.x, group_item_max.x - action_width), group_item_min.y),
                group_item_max);
            group_deleted = HiddenActionIconButton(
                "delete_group",
                delete_rect,
                ActionIcon::Trash,
                "Disband group") &&
                            spectral_lines_panel_.DeleteUserGroupFromView(*editable_view, group.id);
            ImGui::SetCursorScreenPos(saved_cursor);
        }
        if (group_deleted) {
            if (group_contents_open) {
                ImGui::TreePop();
            }
            ImGui::PopID();
            continue;
        }

        previous_group_midpoint_y = group_midpoint_y;

        if (group_contents_open) {
            for (const MarkerReference* reference : matching_references) {
                if (reference == nullptr) {
                    continue;
                }

                const SpectralLineMarker* marker = FindCatalogMarker(catalog, identity, *reference);
                const bool resolved = marker != nullptr;
                const bool marker_visible = resolved && IsMarkerVisible(user_state, reference->marker_id);
                const std::string label = resolved ? marker->label : reference->marker_id;

                ImGui::PushID(reference->marker_id.c_str());

                ImGui::AlignTextToFramePadding();
                bool checkbox_value = marker_visible;
                if (!resolved) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox("##marker_visibility", &checkbox_value) && resolved) {
                    spectral_lines_panel_.SetMarkerEnabled(*marker, checkbox_value);
                }
                if (!resolved) {
                    ImGui::EndDisabled();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(resolved ? "Show on plot" : "Unresolved marker references are not plotted.");
                }

                ImGui::SameLine();
                ImGuiTreeNodeFlags marker_flags =
                    ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet |
                    ImGuiTreeNodeFlags_SpanFullWidth;
                if (editable && ordinary_group) {
                    marker_flags |= ImGuiTreeNodeFlags_AllowOverlap;
                }
                const std::string marker_suffix = resolved ? MarkerWavelengthText(*marker) : "unresolved";
                if (!resolved || !marker_visible) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                ImGui::TreeNodeEx("marker", marker_flags, "%s  %s", label.c_str(), marker_suffix.c_str());
                const ImVec2 marker_item_min = ImGui::GetItemRectMin();
                const ImVec2 marker_item_max = ImGui::GetItemRectMax();
                if (!resolved || !marker_visible) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    if (resolved && !marker->notes.empty()) {
                        ImGui::SetTooltip("%s\n%s", marker_suffix.c_str(), marker->notes.c_str());
                    } else {
                        ImGui::SetTooltip("%s", marker_suffix.c_str());
                    }
                }
                if (editable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    const std::string drag_payload =
                        EncodeMarkerReferenceDragPayload(editable_view->id, group.id, reference->marker_id);
                    ImGui::SetDragDropPayload(
                        kMarkerReferenceDragPayload,
                        drag_payload.data(),
                        static_cast<int>(drag_payload.size()));
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::TextDisabled("Drop: move, Ctrl+drop: copy");
                    ImGui::EndDragDropSource();
                }
                if (editable && ImGui::BeginPopupContextItem("marker_context")) {
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Copy to group")) {
                        bool has_target = false;
                        for (const UserGroup& target_group : editable_view->groups) {
                            if (target_group.id == group.id) {
                                continue;
                            }
                            has_target = true;
                            if (ImGui::Selectable(target_group.name.c_str())) {
                                spectral_lines_panel_.CopyMarkerReferenceToGroup(
                                    *editable_view,
                                    reference->marker_id,
                                    target_group.id);
                            }
                        }
                        if (!has_target) {
                            ImGui::TextDisabled("No other groups");
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }

                if (IsSharedMarkerReference(shared_counts, *reference)) {
                    ImGui::SameLine();
                    RenderSharedReferenceMarker();
                }

                bool reference_removed = false;
                if (editable && ordinary_group) {
                    const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
                    const float action_width = ImGui::GetFrameHeight();
                    const ImRect remove_rect(
                        ImVec2(std::max(marker_item_min.x, marker_item_max.x - action_width), marker_item_min.y),
                        marker_item_max);
                    reference_removed = HiddenActionIconButton(
                        "remove_reference",
                        remove_rect,
                        ActionIcon::Minus,
                        "Remove from this group") &&
                                        spectral_lines_panel_.RemoveMarkerReferenceFromGroup(
                                            *editable_view,
                                            reference->marker_id,
                                            group.id);
                    ImGui::SetCursorScreenPos(saved_cursor);
                }

                ImGui::PopID();
                if (reference_removed) {
                    break;
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (!group_context_popup_open && !renaming_group_popup_requested_) {
        group_context_view_id_.reset();
        group_context_group_id_.reset();
    }
}

void ShellUi::RenderSpectralLinesPanel()
{
    const SpectralLineCatalog& catalog = spectral_lines_panel_.catalog();
    const CatalogIdentity& identity = spectral_lines_panel_.catalog_identity();
    const std::optional<GroupingView>& catalog_grouping_view = spectral_lines_panel_.catalog_grouping_view();
    CatalogUserState& user_state = spectral_lines_panel_.user_state();
    std::array<char, 96>& filter = spectral_lines_panel_.filter_buffer();

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

    const char* selected_catalog = identity.display_name.c_str();
    if (ImGui::BeginCombo("Catalog", selected_catalog)) {
        ImGui::Selectable(selected_catalog, true);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", identity.id.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &spectral_lines_panel_.show_labels());

    ImGui::Spacing();
    if (!catalog.load_error.empty()) {
        const std::string error = "Catalog load failed: " + catalog.load_error;
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), error);
    } else if (catalog.markers.empty()) {
        ImGui::TextDisabled("No public catalog markers loaded.");
    }
    if (!spectral_lines_panel_.warning().empty()) {
        RenderWrappedStatusText(SeverityColor(SpectrumDiagnosticSeverity::Warning), spectral_lines_panel_.warning());
    }

    ImGui::InputTextWithHint(
        "Search",
        "id, label, catalog group, or plot label",
        filter.data(),
        filter.size());

    ImGui::Spacing();
    spectral_lines_panel_.NormalizeViewSelection();

    std::optional<GroupingView> pending_duplicate;
    std::optional<GroupingView> pending_rename;
    std::optional<GroupingView> pending_delete;
    const auto create_new_view = [this]() {
        spectral_lines_panel_.CreateUserGroupingView();
    };

    if (ImGui::BeginTabBar("spectral_line_grouping_views", ImGuiTabBarFlags_Reorderable)) {
        if (catalog_grouping_view) {
            const bool selected = user_state.active_view_id == catalog_grouping_view->id;
            const ImGuiTabItemFlags flags = spectral_lines_panel_.ShouldSelectTab(catalog_grouping_view->id)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(catalog_grouping_view->name.c_str(), nullptr, flags)) {
                if (!selected) {
                    spectral_lines_panel_.SetActiveView(catalog_grouping_view->id);
                }
                spectral_lines_panel_.AcknowledgeTabSelection(catalog_grouping_view->id);
                if (ImGui::BeginPopupContextItem("catalog_grouping_view_context")) {
                    if (ImGui::Selectable("Duplicate as user view")) {
                        pending_duplicate = *catalog_grouping_view;
                    }
                    ImGui::EndPopup();
                }
                RenderSpectralLineGroupingView(*catalog_grouping_view, nullptr);
                ImGui::EndTabItem();
            }
        }

        for (std::size_t index = 0; index < user_state.grouping_views.size(); ++index) {
            GroupingView& user_view = user_state.grouping_views[index];
            const bool selected = user_state.active_view_id == user_view.id;
            const ImGuiTabItemFlags flags = spectral_lines_panel_.ShouldSelectTab(user_view.id)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(user_view.name.c_str(), nullptr, flags)) {
                if (!selected) {
                    spectral_lines_panel_.SetActiveView(user_view.id);
                }
                spectral_lines_panel_.AcknowledgeTabSelection(user_view.id);
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
                RenderSpectralLineGroupingView(effective_view, &user_view);
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
        spectral_lines_panel_.DuplicateUserGroupingView(*pending_duplicate);
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
    if (renaming_group_popup_requested_) {
        ImGui::OpenPopup(kRenameUserGroupPopup);
        renaming_group_popup_requested_ = false;
    }

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
        const auto finish_rename = [this]() {
            if (renaming_grouping_view_id_) {
                spectral_lines_panel_.RenameUserGroupingView(
                    *renaming_grouping_view_id_,
                    renaming_grouping_view_name_.data());
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

    if (ImGui::BeginPopupModal(kRenameUserGroupPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool submitted = ImGui::InputText(
            "Name",
            renaming_group_name_.data(),
            renaming_group_name_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid_name = HasNonWhitespace(renaming_group_name_.data());
        const auto finish_rename = [this, &user_state]() {
            if (renaming_group_view_id_ && renaming_group_id_) {
                for (GroupingView& view : user_state.grouping_views) {
                    if (view.id == *renaming_group_view_id_) {
                        spectral_lines_panel_.RenameUserGroupInView(
                            view,
                            *renaming_group_id_,
                            renaming_group_name_.data());
                        break;
                    }
                }
            }
            renaming_group_view_id_.reset();
            renaming_group_id_.reset();
            renaming_group_name_.fill('\0');
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
            renaming_group_view_id_.reset();
            renaming_group_id_.reset();
            renaming_group_name_.fill('\0');
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal(kDeleteGroupingViewPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete grouping view \"%s\"?", deleting_grouping_view_name_.c_str());
        ImGui::TextDisabled("Catalog markers and marker visibility are not deleted.");
        if (ImGui::Button("Delete")) {
            if (deleting_grouping_view_id_) {
                spectral_lines_panel_.DeleteUserGroupingView(*deleting_grouping_view_id_);
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
