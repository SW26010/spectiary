#include "ui/settings_panel.h"

#include "app/embedded_legal_documents.h"
#include "app/runtime_paths.h"
#include "app/specforge_metadata_validation.h"
#include "domain/stable_sha256.h"
#include "ui/profile_recording_ui_state.h"
#include "specforge/specforge_build_identity.h"
#include "ui/ui_scale_settings.h"

#include <Windows.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr float kMinimumNavigationWidth = 190.0f;
constexpr float kInitialSettingsWidth = 860.0f;
constexpr float kInitialSettingsHeight = 560.0f;

constexpr std::array<SettingsSection, 7> kSettingsSections = {
    SettingsSection::General,
    SettingsSection::Appearance,
    SettingsSection::Language,
    SettingsSection::Input,
    SettingsSection::DataAndRecovery,
    SettingsSection::Diagnostics,
    SettingsSection::About,
};

void RenderEmbeddedLegalDocument(
    LegalDocument document,
    UiTextId title_text_id,
    const char* button_id,
    const char* popup_id,
    UiLanguage language)
{
    const std::string button_label = StableUiLabel(
        language,
        title_text_id,
        button_id);
    const std::string popup_label = StableUiLabel(
        language,
        title_text_id,
        popup_id);
    if (ImGui::Button(button_label.c_str())) {
        ImGui::OpenPopup(popup_label.c_str());
    }

    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 viewport_margin(
        std::max(font_size, style.WindowPadding.x),
        std::max(font_size, style.WindowPadding.y));
    const ImVec2 maximum_size(
        std::max(
            1.0f,
            viewport->WorkSize.x - viewport_margin.x * 2.0f),
        std::max(
            1.0f,
            viewport->WorkSize.y - viewport_margin.y * 2.0f));
    const ImVec2 minimum_size(
        std::min(font_size * 24.0f, maximum_size.x),
        std::min(font_size * 16.0f, maximum_size.y));
    const ImVec2 popup_size(
        std::min(font_size * 52.0f, maximum_size.x),
        std::min(font_size * 34.0f, maximum_size.y));
    const ImVec2 popup_position(
        viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowSize(
        popup_size,
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        minimum_size,
        maximum_size);
    ImGui::SetNextWindowPos(
        popup_position,
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(viewport->ID);
    bool popup_open = true;
    if (!ImGui::BeginPopupModal(
            popup_label.c_str(),
            &popup_open,
            ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const std::string_view content =
        EmbeddedLegalDocumentContent(document);
    const float footer_height =
        ImGui::GetFrameHeightWithSpacing();
    if (content.empty()) {
        if (ImGui::BeginChild(
                "##EmbeddedLegalDocumentContent",
                ImVec2(0.0f, -footer_height),
                true)) {
            const std::string_view unavailable = UiText(
                language,
                UiTextId::LegalDocumentUnavailable);
            ImGui::TextDisabled(
                "%.*s",
                static_cast<int>(unavailable.size()),
                unavailable.data());
        }
        ImGui::EndChild();
    } else {
        std::string selectable_content(content);
        (void)ImGui::InputTextMultiline(
            "##EmbeddedLegalDocumentContent",
            selectable_content.data(),
            selectable_content.size() + 1,
            ImVec2(0.0f, -footer_height),
            ImGuiInputTextFlags_ReadOnly |
                ImGuiInputTextFlags_WordWrap);
    }

    const std::string copy_label = StableUiLabel(
        language,
        UiTextId::CopyDocument,
        "SpecForgeCopyLegalDocument");
    ImGui::BeginDisabled(content.empty());
    if (ImGui::Button(copy_label.c_str())) {
        ImGui::SetClipboardText(std::string(content).c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string close_label = StableUiLabel(
        language,
        UiTextId::Close,
        "SpecForgeCloseLegalDocument");
    if (ImGui::Button(close_label.c_str())) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

std::string SettingsWindowLabel(UiLanguage language)
{
    return StableUiLabel(
        language,
        UiTextId::Settings,
        "SpecForgeSettingsV1");
}

std::string AppearanceThemeItems(UiLanguage language)
{
    constexpr std::array kThemeTextIds = {
        UiTextId::FollowSystemTheme,
        UiTextId::LightTheme,
        UiTextId::DarkTheme,
    };

    std::string items;
    for (const UiTextId text_id : kThemeTextIds) {
        items += UiText(language, text_id);
        items.push_back('\0');
    }
    items.push_back('\0');
    return items;
}

void RenderApplicationSettingsStatusReason(
    const ApplicationSettingsStatus& status,
    UiLanguage language)
{
    const std::string_view reason =
        FormatApplicationSettingsStatusReason(
            status.reason,
            language);
    if (reason.empty()) {
        return;
    }

    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(reason.size()),
        reason.data());
    if (!status.detail.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        const std::string_view diagnostic_label =
            UiText(language, UiTextId::DiagnosticDetails);
        ImGui::TextUnformatted(
            diagnostic_label.data(),
            diagnostic_label.data() +
                diagnostic_label.size());
        ImGui::Separator();
        ImGui::PushTextWrapPos(
            ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(status.detail.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

UiTextId LanguageNameTextId(UiLanguage language)
{
    switch (language) {
    case UiLanguage::English:
        return UiTextId::EnglishLanguageName;
    case UiLanguage::SimplifiedChinese:
        return UiTextId::SimplifiedChineseLanguageName;
    case UiLanguage::Count:
        return UiTextId::EnglishLanguageName;
    }
    return UiTextId::EnglishLanguageName;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string_view LabelValueSeparator(UiLanguage language)
{
    return language == UiLanguage::SimplifiedChinese
        ? std::string_view{"："}
        : std::string_view{": "};
}

void AppendLabeledValue(
    std::string& output,
    UiLanguage language,
    UiTextId label,
    std::string_view value)
{
    output += UiText(language, label);
    output += LabelValueSeparator(language);
    output += value;
}

std::string FormatFrameCaptureStatus(
    UiLanguage language,
    const SettingsPanelStatus& status)
{
    UiTextId text_id = UiTextId::FrameCaptureFailed;
    switch (status.frame_capture_status) {
    case OnDemandFrameCaptureStatus::None:
        return std::string(
            status.frame_capture_status_message);
    case OnDemandFrameCaptureStatus::Ready:
        text_id = UiTextId::FrameCaptureReady;
        break;
    case OnDemandFrameCaptureStatus::Disabled:
        text_id = UiTextId::FrameCaptureDisabled;
        break;
    case OnDemandFrameCaptureStatus::WindowUnavailable:
        text_id =
            UiTextId::FrameCaptureWindowUnavailable;
        break;
    case OnDemandFrameCaptureStatus::Pending:
        text_id = UiTextId::FrameCaptureRequested;
        break;
    case OnDemandFrameCaptureStatus::Captured:
        text_id = UiTextId::FrameCaptureCompleted;
        break;
    case OnDemandFrameCaptureStatus::
            FailedPreparingOutputDirectory:
        text_id =
            UiTextId::FrameCaptureDirectoryFailed;
        break;
    case OnDemandFrameCaptureStatus::Failed:
        text_id = UiTextId::FrameCaptureFailed;
        break;
    case OnDemandFrameCaptureStatus::FailedCapture: {
        const std::string format(UiText(
            language,
            UiTextId::FrameCaptureBackendFailed));
        const std::string operation(
            status.frame_capture_status_operation);
        const std::string result(
            status.frame_capture_status_result);
        const int required = std::snprintf(
            nullptr,
            0,
            format.c_str(),
            operation.c_str(),
            result.c_str());
        if (required <= 0) {
            return std::string(UiText(
                language,
                UiTextId::FrameCaptureFailed));
        }
        std::vector<char> buffer(
            static_cast<std::size_t>(required) + 1);
        (void)std::snprintf(
            buffer.data(),
            buffer.size(),
            format.c_str(),
            operation.c_str(),
            result.c_str());
        return std::string(
            buffer.data(),
            static_cast<std::size_t>(required));
    }
    }
    return std::string(UiText(language, text_id));
}

void RenderSectionHeading(
    std::string_view title,
    std::string_view description)
{
    ImGui::TextUnformatted(
        title.data(),
        title.data() + title.size());
    ImGui::Separator();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(description.size()),
        description.data());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

void RenderReadOnlyValue(const char* label, const char* value)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
}

std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path)
{
    constexpr std::size_t kHashBufferSize = 64U * 1024U;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.good()) {
            return std::nullopt;
        }

        StableSha256 sha256;
        std::array<char, kHashBufferSize> buffer = {};
        for (;;) {
            stream.read(
                buffer.data(),
                static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_count = stream.gcount();
            if (read_count > 0) {
                sha256.Append(std::string_view(
                    buffer.data(),
                    static_cast<std::size_t>(read_count)));
            }
            if (stream.eof()) {
                break;
            }
            if (stream.fail()) {
                return std::nullopt;
            }
        }
        return sha256.FinishHex();
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

SettingsPanelEnvironment SettingsPanelEnvironmentForStartup(
    const SpecForgeStartup& startup)
{
    const RuntimePaths& paths = startup.runtime_paths();
    return {
        .version = build_info::kSpecForgeVersion,
        .distribution =
            DistributionName(paths.distribution),
        .configuration = build_info::kBuildConfiguration,
        .target_architecture = build_info::kTargetArchitecture,
        .executable_path = paths.executable_path,
        .build_source = {
            .mode = build_info::kBuildSourceMode,
            .revision = build_info::kBuildSourceRevision,
        },
        .build_metadata =
            startup.metadata().build_metadata,
        .data_directory = paths.local_user_state_root,
    };
}

std::string FormatBuildSourceForAbout(
    const BuildSourceIdentity& build_source,
    UiLanguage language)
{
    std::string formatted;
    AppendLabeledValue(
        formatted,
        language,
        UiTextId::Source,
        build_source.mode == "working_tree"
            ? UiText(language, UiTextId::WorkingTree)
        : build_source.mode == "head"
            ? UiText(language, UiTextId::Head)
            : std::string_view{build_source.mode});
    if (build_source.mode == "working_tree") {
        return formatted;
    }
    if (build_source.mode == "head") {
        constexpr std::size_t kDisplayedRevisionLength = 12;
        formatted += " ";
        formatted += build_source.revision.substr(
            0,
            kDisplayedRevisionLength);
    }
    return formatted;
}

std::string_view FormatBuildMetadataStatusForAbout(
    BuildMetadataStatus status,
    UiLanguage language)
{
    switch (status) {
    case BuildMetadataStatus::Available:
        return {};
    case BuildMetadataStatus::Unavailable:
        return UiText(
            language,
            UiTextId::BuildMetadataUnavailable);
    case BuildMetadataStatus::Mismatch:
        return UiText(
            language,
            UiTextId::BuildMetadataMismatch);
    }
    return UiText(
        language,
        UiTextId::BuildMetadataUnavailable);
}

ArtifactIdentityResult VerifyExecutableArtifactIdentity(
    const std::filesystem::path& executable_path,
    const BuildMetadataReadResult& build_metadata)
{
    ArtifactIdentityResult result;
    if (build_metadata.status != BuildMetadataStatus::Available ||
        !build_metadata.metadata) {
        return result;
    }

    const BuildMetadata& metadata = *build_metadata.metadata;
    if (!metadata.completed_at_utc || !metadata.artifact ||
        !metadata_validation::IsValidUtcTimestamp(
            *metadata.completed_at_utc) ||
        metadata.artifact->file != "SpecForge.exe" ||
        !metadata_validation::IsValidSha256(
            metadata.artifact->sha256)) {
        return result;
    }

    result.completed_at_utc = *metadata.completed_at_utc;
    result.metadata_sha256 = metadata.artifact->sha256;
    const std::optional<std::string> executable_sha256 =
        ComputeFileSha256(executable_path);
    if (!executable_sha256) {
        return result;
    }

    result.executable_sha256 = *executable_sha256;
    result.status = result.metadata_sha256 == result.executable_sha256
        ? ArtifactIdentityStatus::Available
        : ArtifactIdentityStatus::Mismatch;
    return result;
}

std::string_view FormatArtifactIdentityStatusForAbout(
    ArtifactIdentityStatus status,
    UiLanguage language)
{
    switch (status) {
    case ArtifactIdentityStatus::Available:
        return UiText(
            language,
            UiTextId::ArtifactIdentityVerified);
    case ArtifactIdentityStatus::Unavailable:
        return UiText(
            language,
            UiTextId::ArtifactIdentityUnavailable);
    case ArtifactIdentityStatus::Mismatch:
        return UiText(
            language,
            UiTextId::ArtifactIdentityMismatch);
    }
    return UiText(
        language,
        UiTextId::ArtifactIdentityUnavailable);
}

std::string_view FormatProfileOutputDirectoryStatus(
    ApplicationSettingsStatusKind kind,
    UiLanguage language)
{
    switch (kind) {
    case ApplicationSettingsStatusKind::Ready:
        return {};
    case ApplicationSettingsStatusKind::LoadWarning:
        return UiText(
            language,
            UiTextId::ProfileOutputLoadWarning);
    case ApplicationSettingsStatusKind::PersistenceError:
        return UiText(
            language,
            UiTextId::ProfileOutputSaveError);
    case ApplicationSettingsStatusKind::Rejected:
        return UiText(
            language,
            UiTextId::ProfileOutputRejected);
    }
    return {};
}

std::string_view FormatApplicationSettingsStatusReason(
    ApplicationSettingsStatusReason reason,
    UiLanguage language)
{
    switch (reason) {
    case ApplicationSettingsStatusReason::None:
        return {};
    case ApplicationSettingsStatusReason::SavedValueUnreadable:
        return UiText(
            language,
            UiTextId::SavedSettingsValueUnreadable);
    case ApplicationSettingsStatusReason::SettingsWriteFailed:
        return UiText(
            language,
            UiTextId::SettingsFileWriteFailed);
    case ApplicationSettingsStatusReason::UnsupportedLanguage:
        return UiText(
            language,
            UiTextId::UnsupportedApplicationLanguage);
    case ApplicationSettingsStatusReason::UiScaleOutOfRange:
        return UiText(
            language,
            UiTextId::UiScaleOutsideSupportedRange);
    case ApplicationSettingsStatusReason::
        EnvironmentOverrideActive:
        return UiText(
            language,
            UiTextId::ProfileEnvironmentOverrideActive);
    case ApplicationSettingsStatusReason::RecordingInProgress:
        return UiText(
            language,
            UiTextId::ProfileRecordingInProgress);
    case ApplicationSettingsStatusReason::
        EmptyProfileOutputDirectory:
        return UiText(
            language,
            UiTextId::ProfileOutputDirectoryEmpty);
    }
    return {};
}

std::string FormatDiagnosticInformation(
    const SettingsPanelEnvironment& environment,
    const std::filesystem::path& profile_output_directory,
    UiLanguage language)
{
    std::string diagnostics;
    diagnostics.reserve(320);
    diagnostics += "SpecForge ";
    diagnostics += environment.version;
    diagnostics += "\n";
    AppendLabeledValue(
        diagnostics,
        language,
        UiTextId::Distribution,
        environment.distribution);
    diagnostics += "\n";
    AppendLabeledValue(
        diagnostics,
        language,
        UiTextId::SourceMode,
        environment.build_source.mode);
    if (environment.build_source.mode == "head") {
        diagnostics += "\n";
        AppendLabeledValue(
            diagnostics,
            language,
            UiTextId::SourceRevision,
            environment.build_source.revision);
    }
    diagnostics += "\n";
    AppendLabeledValue(
        diagnostics,
        language,
        UiTextId::Graphics,
        "Direct3D 11 / SDR");
    diagnostics += "\n";
    AppendLabeledValue(
        diagnostics,
        language,
        UiTextId::DataDirectory,
        PathToUtf8(environment.data_directory));
    diagnostics += "\n";
    AppendLabeledValue(
        diagnostics,
        language,
        UiTextId::LogDirectory,
        PathToUtf8(profile_output_directory));
    return diagnostics;
}

std::string SettingsPanelUi::SectionLabel(
    SettingsSection section,
    UiLanguage language)
{
    switch (section) {
    case SettingsSection::General:
        return StableUiLabel(
            language,
            UiTextId::General,
            "SpecForgeSettingsGeneral");
    case SettingsSection::Appearance:
        return StableUiLabel(
            language,
            UiTextId::Appearance,
            "SpecForgeSettingsAppearance");
    case SettingsSection::Language:
        return StableUiLabel(
            language,
            UiTextId::Language,
            "SpecForgeSettingsLanguage");
    case SettingsSection::Input:
        return StableUiLabel(
            language,
            UiTextId::Input,
            "SpecForgeSettingsInput");
    case SettingsSection::DataAndRecovery:
        return StableUiLabel(
            language,
            UiTextId::DataAndRecovery,
            "SpecForgeSettingsDataAndRecovery");
    case SettingsSection::Diagnostics:
        return StableUiLabel(
            language,
            UiTextId::Diagnostics,
            "SpecForgeSettingsDiagnostics");
    case SettingsSection::About:
        return StableUiLabel(
            language,
            UiTextId::About,
            "SpecForgeSettingsAbout");
    }
    return StableUiLabel(
        language,
        UiTextId::Settings,
        "SpecForgeSettingsFallback");
}

std::string SettingsPanelUi::AppearanceThemeLabel(
    UiLanguage language)
{
    return StableUiLabel(
        language,
        UiTextId::Theme,
        "SpecForgeAppearanceTheme");
}

std::string SettingsPanelUi::AppearanceAccentColorLabel(
    UiLanguage language)
{
    return StableUiLabel(
        language,
        UiTextId::AccentColor,
        "SpecForgeAppearanceAccentColor");
}

float SettingsPanelUi::VisibleLabelWidth(std::string_view label)
{
    return ImGui::CalcTextSize(
        label.data(),
        label.data() + label.size(),
        true).x;
}

SettingsPanelUi::SettingsPanelUi(SettingsPanelEnvironment environment)
    : environment_(std::move(environment))
{
}

const ArtifactIdentityResult&
SettingsPanelUi::ArtifactIdentityForAbout()
{
    if (!artifact_identity_) {
        artifact_identity_ = VerifyExecutableArtifactIdentity(
            environment_.executable_path,
            environment_.build_metadata);
    }
    return *artifact_identity_;
}

void SettingsPanelUi::Open()
{
    if (!open_) {
        action_failed_ = false;
        action_status_.clear();
        ui_scale_draft_percentage_.reset();
    }
    open_ = true;
    focus_requested_ = true;
}

void SettingsPanelUi::Render(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    if (!open_) {
        return;
    }

    const ImGuiViewport* viewport =
        settings_viewport_id_ != 0
        ? ImGui::FindViewportByID(settings_viewport_id_)
        : nullptr;
    if (viewport == nullptr) {
        viewport = ImGui::GetMainViewport();
    }
    const float user_scale =
        static_cast<float>(settings.ui_scale_percentage) /
        static_cast<float>(kDefaultUiScalePercentage);
    const ImVec2 preferred_size(
        kInitialSettingsWidth * user_scale,
        kInitialSettingsHeight * user_scale);
    const ImVec2 maximum_size(
        std::max(1.0f, viewport->WorkSize.x),
        std::max(1.0f, viewport->WorkSize.y));
    const ImVec2 initial_size(
        std::min(preferred_size.x, maximum_size.x),
        std::min(preferred_size.y, maximum_size.y));
    const ImVec2 initial_position(
        viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - initial_size.x) * 0.5f,
        viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - initial_size.y) * 0.5f);
    ImGui::SetNextWindowSize(initial_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        initial_size,
        maximum_size);
    ImGui::SetNextWindowPos(
        initial_position,
        ImGuiCond_FirstUseEver);
    if (focus_requested_) {
        ImGui::SetNextWindowFocus();
        focus_requested_ = false;
    }

    const std::string settings_window =
        SettingsWindowLabel(settings.language);
    const bool contents_visible = ImGui::Begin(
            settings_window.c_str(),
            &open_,
            ImGuiWindowFlags_NoCollapse);
    settings_viewport_id_ =
        ImGui::GetWindowViewport()->ID;
    if (!contents_visible) {
        ImGui::End();
        return;
    }

    const std::string widest_navigation_label =
        SectionLabel(
            SettingsSection::DataAndRecovery,
            settings.language);
    const float navigation_width = std::max(
        kMinimumNavigationWidth,
        VisibleLabelWidth(widest_navigation_label) +
            ImGui::GetStyle().WindowPadding.x * 2.0f);
    if (ImGui::BeginChild("##SettingsNavigation", ImVec2(navigation_width, 0.0f), true)) {
        RenderNavigation(settings.language);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##SettingsContent", ImVec2(0.0f, 0.0f), false)) {
        if (content_scroll_reset_requested_) {
            ImGui::SetScrollY(0.0f);
            content_scroll_reset_requested_ = false;
        }
        RenderSelectedSection(settings, status);
    }
    ImGui::EndChild();
    ImGui::End();
}

bool SettingsPanelUi::open() const
{
    return open_;
}

bool SettingsPanelUi::TakeProfileRecordingToggleRequest()
{
    const bool requested = profile_recording_toggle_requested_;
    profile_recording_toggle_requested_ = false;
    return requested;
}

bool SettingsPanelUi::TakeFrameCaptureRequest()
{
    const bool requested = frame_capture_requested_;
    frame_capture_requested_ = false;
    return requested;
}

bool SettingsPanelUi::TakeProfileOutputDirectorySelectionRequest()
{
    const bool requested = profile_output_directory_selection_requested_;
    profile_output_directory_selection_requested_ = false;
    return requested;
}

std::optional<ApplicationSettingsIntent>
SettingsPanelUi::TakeApplicationSettingsIntent()
{
    std::optional<ApplicationSettingsIntent> requested =
        std::exchange(application_settings_intent_, std::nullopt);
    return requested;
}

void SettingsPanelUi::RenderNavigation(UiLanguage language)
{
    ImGui::TextDisabled("SPECFORGE");
    ImGui::Spacing();
    for (const SettingsSection section : kSettingsSections) {
        const bool selected = selected_section_ == section;
        const std::string label = SectionLabel(section, language);
        if (ImGui::Selectable(label.c_str(), selected)) {
            if (selected_section_ == SettingsSection::Appearance &&
                section != SettingsSection::Appearance) {
                ui_scale_draft_percentage_.reset();
            }
            selected_section_ = section;
            content_scroll_reset_requested_ = true;
            action_status_.clear();
        }
    }
}

void SettingsPanelUi::RenderSelectedSection(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    switch (selected_section_) {
    case SettingsSection::General:
        RenderGeneral(settings);
        return;
    case SettingsSection::Appearance:
        RenderAppearance(settings);
        return;
    case SettingsSection::Language:
        RenderLanguage(settings);
        return;
    case SettingsSection::Input:
        RenderInput(settings);
        return;
    case SettingsSection::DataAndRecovery:
        RenderDataAndRecovery(settings.language);
        return;
    case SettingsSection::Diagnostics:
        RenderDiagnostics(settings, status);
        return;
    case SettingsSection::About:
        RenderAbout(settings);
        return;
    }
}

void SettingsPanelUi::RenderGeneral(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::General),
        UiText(language, UiTextId::GeneralPageDescription));

    ImGui::Spacing();
    bool restore_previous_session = true;
    const std::string restore_label = StableUiLabel(
        language,
        UiTextId::RestorePreviousSession,
        "SpecForgeRestorePreviousSession");
    ImGui::BeginDisabled();
    ImGui::Checkbox(
        restore_label.c_str(),
        &restore_previous_session);
    ImGui::EndDisabled();
    const std::string_view unavailable = UiText(
        language,
        UiTextId::SessionRestorationUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(unavailable.size()),
        unavailable.data());
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SeparatorText(
        UiText(language, UiTextId::FileOpening).data());

    bool open_external_source_as_folder =
        settings.open_external_source_as_folder;
    const std::string open_external_source_label =
        StableUiLabel(
            language,
            UiTextId::OpenExternalSourceAsFolder,
            "SpecForgeOpenExternalSourceAsFolder");
    if (ImGui::Checkbox(
            open_external_source_label.c_str(),
            &open_external_source_as_folder)) {
        SetOpenExternalSourceAsFolder(
            open_external_source_as_folder);
    }
    ImGui::PushTextWrapPos();
    const std::string_view open_external_source_description =
        UiText(
            language,
            UiTextId::OpenExternalSourceAsFolderDescription);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(open_external_source_description.size()),
        open_external_source_description.data());
    ImGui::PopTextWrapPos();

    bool include_external_subfolders = false;
    const std::string include_external_subfolders_label =
        StableUiLabel(
            language,
            UiTextId::IncludeExternalSubfolders,
            "SpecForgeIncludeExternalSubfolders");
    ImGui::BeginDisabled();
    ImGui::Checkbox(
        include_external_subfolders_label.c_str(),
        &include_external_subfolders);
    ImGui::EndDisabled();

    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(ApplicationSetting::ExternalSource);
    if (setting_status.kind !=
        ApplicationSettingsStatusKind::Ready) {
        ImGui::Spacing();
        const bool warning =
            setting_status.kind ==
            ApplicationSettingsStatusKind::LoadWarning;
        const ImVec4 feedback_color = warning
            ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
            : ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
        const std::string_view feedback = UiText(
            language,
            warning
                ? UiTextId::ExternalSourceSettingsLoadWarning
                : UiTextId::ExternalSourceSettingsSaveError);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(
            feedback_color,
            "%.*s",
            static_cast<int>(feedback.size()),
            feedback.data());
        RenderApplicationSettingsStatusReason(
            setting_status,
            language);
        ImGui::PopTextWrapPos();
    }
}

void SettingsPanelUi::RenderAppearance(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::Appearance),
        UiText(language, UiTextId::AppearancePageDescription));

    int theme = 2;
    float accent_color[3] = {0.24f, 0.55f, 0.86f};
    const std::string theme_label =
        AppearanceThemeLabel(language);
    const std::string theme_items =
        AppearanceThemeItems(language);
    const std::string accent_color_label =
        AppearanceAccentColorLabel(language);
    ImGui::BeginDisabled();
    ImGui::Combo(
        theme_label.c_str(),
        &theme,
        theme_items.c_str());
    ImGui::ColorEdit3(
        accent_color_label.c_str(),
        accent_color,
        ImGuiColorEditFlags_NoInputs);
    ImGui::EndDisabled();
    const std::string_view theme_unavailable = UiText(
        language,
        UiTextId::AppearanceThemeUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(theme_unavailable.size()),
        theme_unavailable.data());
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    int ui_scale = ui_scale_draft_percentage_.value_or(
        settings.ui_scale_percentage);
    const std::string ui_scale_label = StableUiLabel(
        language,
        UiTextId::UiScale,
        "SpecForgeUiScale");
    const bool ui_scale_changed = ImGui::SliderInt(
        ui_scale_label.c_str(),
        &ui_scale,
        kMinimumUiScalePercentage,
        kMaximumUiScalePercentage,
        "%d%%");
    const bool ui_scale_active = ImGui::IsItemActive();
    const bool ui_scale_edit_finished =
        ImGui::IsItemDeactivatedAfterEdit();
    if (ui_scale_changed || ui_scale_active) {
        ui_scale_draft_percentage_ = ui_scale;
    }
    if (ui_scale_edit_finished) {
        ui_scale_draft_percentage_.reset();
        if (ui_scale != settings.ui_scale_percentage) {
            SetUiScalePercentage(ui_scale);
        }
    } else if (!ui_scale_active) {
        ui_scale_draft_percentage_.reset();
    }
    ImGui::SameLine();
    const std::string reset_label = StableUiLabel(
        language,
        UiTextId::Reset,
        "SpecForgeUiScaleReset");
    if (ImGui::Button(reset_label.c_str())) {
        ui_scale_draft_percentage_.reset();
        SetUiScalePercentage(kDefaultUiScalePercentage);
    }
    const std::string_view ui_scale_description = UiText(
        language,
        UiTextId::UiScaleDescription);
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(ui_scale_description.size()),
        ui_scale_description.data());
    ImGui::PopTextWrapPos();

    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(ApplicationSetting::UiScale);
    if (setting_status.kind ==
        ApplicationSettingsStatusKind::Ready) {
        return;
    }

    ImGui::Spacing();
    const bool warning =
        setting_status.kind ==
        ApplicationSettingsStatusKind::LoadWarning;
    const ImVec4 feedback_color = warning
        ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
        : ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
    const UiTextId feedback_text_id = warning
        ? UiTextId::UiScaleLoadWarning
        : (setting_status.kind ==
                ApplicationSettingsStatusKind::Rejected
            ? UiTextId::UiScaleRejected
            : UiTextId::UiScaleSaveError);
    const std::string_view feedback =
        UiText(language, feedback_text_id);
    ImGui::PushTextWrapPos();
    ImGui::TextColored(
        feedback_color,
        "%.*s",
        static_cast<int>(feedback.size()),
        feedback.data());
    RenderApplicationSettingsStatusReason(
        setting_status,
        language);
    ImGui::PopTextWrapPos();
}

void SettingsPanelUi::SetUiScalePercentage(int percentage)
{
    application_settings_intent_ =
        ApplicationSettingsIntent::SetUiScale(percentage);
}

void SettingsPanelUi::RenderLanguage(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(ApplicationSetting::Language);
    RenderSectionHeading(
        UiText(language, UiTextId::Language),
        UiText(language, UiTextId::LanguagePageDescription));

    const std::string selector_label = StableUiLabel(
        language,
        UiTextId::ApplicationLanguage,
        "SpecForgeApplicationLanguage");
    const char* preview =
        UiText(language, LanguageNameTextId(language)).data();
    if (ImGui::BeginCombo(selector_label.c_str(), preview)) {
        constexpr std::array kLanguages = {
            UiLanguage::English,
            UiLanguage::SimplifiedChinese,
        };
        for (const UiLanguage candidate : kLanguages) {
            const bool selected = language == candidate;
            const std::string option_label = StableUiLabel(
                language,
                LanguageNameTextId(candidate),
                candidate == UiLanguage::English
                    ? "SpecForgeUiLanguageEnglish"
                    : "SpecForgeUiLanguageSimplifiedChinese");
            if (ImGui::Selectable(
                    option_label.c_str(),
                    selected) &&
                ShouldSubmitLanguageSelection(
                    settings,
                    candidate)) {
                application_settings_intent_ =
                    ApplicationSettingsIntent::SetLanguage(
                        candidate);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    const std::string_view coverage =
        UiText(language, UiTextId::ApplicationLanguageScope);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(coverage.size()),
        coverage.data());
    ImGui::PopTextWrapPos();

    if (setting_status.kind ==
        ApplicationSettingsStatusKind::Ready) {
        return;
    }

    ImGui::Spacing();
    const bool failed =
        setting_status.kind !=
        ApplicationSettingsStatusKind::LoadWarning;
    const UiTextId feedback_text_id = failed
        ? UiTextId::LanguageSaveError
        : UiTextId::LanguageLoadWarning;
    const std::string_view feedback =
        UiText(language, feedback_text_id);
    const ImVec4 feedback_color = failed
        ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
        : ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
    ImGui::PushTextWrapPos();
    ImGui::TextColored(
        feedback_color,
        "%.*s",
        static_cast<int>(feedback.size()),
        feedback.data());
    RenderApplicationSettingsStatusReason(
        setting_status,
        language);
    ImGui::PopTextWrapPos();
}

void SettingsPanelUi::RenderInput(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::Input),
        UiText(language, UiTextId::InputPageDescription));

    bool live_numeric_navigation =
        settings.live_numeric_navigation;
    const std::string live_numeric_navigation_label =
        StableUiLabel(
            language,
            UiTextId::LiveNumericNavigation,
            "SpecForgeLiveNumericNavigation");
    if (ImGui::Checkbox(
            live_numeric_navigation_label.c_str(),
            &live_numeric_navigation)) {
        SetLiveNumericNavigation(
            live_numeric_navigation);
    }
    ImGui::PushTextWrapPos();
    const std::string_view live_description = UiText(
        language,
        UiTextId::LiveNumericNavigationDescription);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(live_description.size()),
        live_description.data());
    ImGui::PopTextWrapPos();

    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(ApplicationSetting::Input);
    if (setting_status.kind !=
        ApplicationSettingsStatusKind::Ready) {
        ImGui::Spacing();
        const bool warning =
            setting_status.kind ==
            ApplicationSettingsStatusKind::LoadWarning;
        const ImVec4 feedback_color = warning
            ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
            : ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
        const std::string_view feedback = UiText(
            language,
            warning
                ? UiTextId::InputSettingsLoadWarning
                : UiTextId::InputSettingsSaveError);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(
            feedback_color,
            "%.*s",
            static_cast<int>(feedback.size()),
            feedback.data());
        RenderApplicationSettingsStatusReason(
            setting_status,
            language);
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float mouse_zoom_sensitivity = 1.0f;
    float touchpad_zoom_sensitivity = 1.0f;
    bool reverse_zoom_direction = false;
    const std::string mouse_zoom_label = StableUiLabel(
        language,
        UiTextId::MouseZoomSensitivity,
        "SpecForgeMouseZoomSensitivity");
    const std::string touchpad_zoom_label = StableUiLabel(
        language,
        UiTextId::TouchpadZoomSensitivity,
        "SpecForgeTouchpadZoomSensitivity");
    const std::string reverse_zoom_label = StableUiLabel(
        language,
        UiTextId::ReverseZoomDirection,
        "SpecForgeReverseZoomDirection");
    const std::string shortcuts_label = StableUiLabel(
        language,
        UiTextId::ViewKeyboardShortcuts,
        "SpecForgeViewKeyboardShortcuts");
    ImGui::BeginDisabled();
    ImGui::SliderFloat(
        mouse_zoom_label.c_str(),
        &mouse_zoom_sensitivity,
        0.5f,
        2.0f,
        "%.1fx");
    ImGui::SliderFloat(
        touchpad_zoom_label.c_str(),
        &touchpad_zoom_sensitivity,
        0.5f,
        2.0f,
        "%.1fx");
    ImGui::Checkbox(
        reverse_zoom_label.c_str(),
        &reverse_zoom_direction);
    ImGui::Button(shortcuts_label.c_str());
    ImGui::EndDisabled();
    const std::string_view unavailable = UiText(
        language,
        UiTextId::InputBehaviorUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(unavailable.size()),
        unavailable.data());
    ImGui::PopTextWrapPos();
}

void SettingsPanelUi::SetLiveNumericNavigation(bool enabled)
{
    application_settings_intent_ =
        ApplicationSettingsIntent::
            SetLiveNumericNavigation(enabled);
}

void SettingsPanelUi::SetOpenExternalSourceAsFolder(bool enabled)
{
    application_settings_intent_ =
        ApplicationSettingsIntent::
            SetOpenExternalSourceAsFolder(enabled);
}

void SettingsPanelUi::RenderDataAndRecovery(
    UiLanguage language)
{
    RenderSectionHeading(
        UiText(language, UiTextId::DataAndRecovery),
        UiText(
            language,
            UiTextId::DataAndRecoveryPageDescription));

    const std::string data_path = PathToUtf8(environment_.data_directory);
    const std::string_view application_data = UiText(
        language,
        UiTextId::ApplicationData);
    ImGui::TextUnformatted(
        application_data.data(),
        application_data.data() +
            application_data.size());
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", data_path.c_str());
    ImGui::PopTextWrapPos();
    const std::string open_data_label = StableUiLabel(
        language,
        UiTextId::OpenDataFolder,
        "SpecForgeOpenDataFolder");
    if (ImGui::Button(open_data_label.c_str())) {
        OpenDirectory(
            environment_.data_directory,
            language,
            UiTextId::DataFolderPrepareError,
            UiTextId::DataFolderOpenError,
            UiTextId::DataFolderOpened);
    }
    ImGui::SameLine();
    const std::string copy_data_path_label = StableUiLabel(
        language,
        UiTextId::CopyPath,
        "SpecForgeCopyDataPath");
    if (ImGui::Button(copy_data_path_label.c_str())) {
        CopyPath(
            environment_.data_directory,
            language,
            UiTextId::DataPathCopied);
    }

    ImGui::Spacing();
    const std::string configuration_portability =
        StableUiLabel(
            language,
            UiTextId::ConfigurationPortability,
            "SpecForgeConfigurationPortability");
    ImGui::SeparatorText(
        configuration_portability.c_str());
    const std::string import_label = StableUiLabel(
        language,
        UiTextId::ImportSettings,
        "SpecForgeImportSettings");
    const std::string export_label = StableUiLabel(
        language,
        UiTextId::ExportSettings,
        "SpecForgeExportSettings");
    ImGui::BeginDisabled();
    ImGui::Button(import_label.c_str());
    ImGui::SameLine();
    ImGui::Button(export_label.c_str());
    ImGui::EndDisabled();
    const std::string_view portability_unavailable = UiText(
        language,
        UiTextId::ConfigurationFormatUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(
            portability_unavailable.size()),
        portability_unavailable.data());
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    const std::string recovery_heading = StableUiLabel(
        language,
        UiTextId::RecoveryAndReset,
        "SpecForgeRecoveryAndReset");
    ImGui::SeparatorText(recovery_heading.c_str());
    const std::string reset_layout_label =
        StableUiLabel(
            language,
            UiTextId::ResetWindowLayout,
            "SpecForgeResetWindowLayout");
    const std::string reset_settings_label =
        StableUiLabel(
            language,
            UiTextId::ResetApplicationSettings,
            "SpecForgeResetApplicationSettings");
    const std::string erase_state_label =
        StableUiLabel(
            language,
            UiTextId::EraseAllApplicationState,
            "SpecForgeEraseAllApplicationState");
    ImGui::BeginDisabled();
    ImGui::Button(reset_layout_label.c_str());
    ImGui::Button(reset_settings_label.c_str());
    ImGui::Button(erase_state_label.c_str());
    ImGui::EndDisabled();
    const std::string_view reset_unavailable = UiText(
        language,
        UiTextId::ResetOperationsUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(reset_unavailable.size()),
        reset_unavailable.data());
    ImGui::PopTextWrapPos();

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::RenderDiagnostics(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::Diagnostics),
        UiText(
            language,
            UiTextId::DiagnosticsPageDescription));

    const ProfileRecordingUiPresentation presentation =
        ResolveProfileRecordingUiPresentation(
            status.profile_open,
            status.profile_stopping);
    const std::string_view performance_profile = UiText(
        language,
        UiTextId::PerformanceProfile);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(performance_profile.size()),
        performance_profile.data());
    ImGui::SameLine();
    const std::string_view recording_state = UiText(
        language,
        status.profile_open
            ? UiTextId::Recording
            : (status.profile_stopping
                ? UiTextId::Finishing
                : UiTextId::NotRecording));
    ImGui::TextUnformatted(
        recording_state.data(),
        recording_state.data() +
            recording_state.size());

    if (!presentation.menu_action_enabled) {
        ImGui::BeginDisabled();
    }
    const UiTextId recording_action_id =
        status.profile_stopping
        ? UiTextId::FinishingRecordingAction
        : (status.profile_open
            ? UiTextId::StopRecording
            : UiTextId::StartRecording);
    const std::string recording_action_label =
        StableUiLabel(
            language,
            recording_action_id,
            "SpecForgeProfileRecordingToggle");
    if (ImGui::Button(
            recording_action_label.c_str())) {
        profile_recording_toggle_requested_ = true;
    }
    if (!presentation.menu_action_enabled) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const std::string_view auto_stop = UiText(
        language,
        UiTextId::ProfileAutoStopNote);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(auto_stop.size()),
        auto_stop.data());

    if (status.profile_path != nullptr) {
        ImGui::Spacing();
        const std::string_view current_profile =
            UiText(
                language,
                UiTextId::CurrentProfile);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(current_profile.size()),
            current_profile.data());
        ImGui::PushTextWrapPos();
        const std::string active_path = PathToUtf8(*status.profile_path);
        ImGui::TextUnformatted(active_path.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!status.profile_status_message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(status.profile_status_message.size()),
            status.profile_status_message.data());
        ImGui::PopTextWrapPos();
    }

    if (status.frame_capture_enabled) {
        ImGui::Spacing();
        const std::string capture_heading =
            StableUiLabel(
                language,
                UiTextId::ExperimentalFrameCapture,
                "SpecForgeExperimentalFrameCapture");
        ImGui::SeparatorText(capture_heading.c_str());
        ImGui::PushTextWrapPos();
        const std::string_view capture_description =
            UiText(
                language,
                UiTextId::
                    ExperimentalFrameCaptureDescription);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(
                capture_description.size()),
            capture_description.data());
        ImGui::PopTextWrapPos();

        const bool capture_disabled =
            status.frame_capture_pending ||
            !status.window_renderable;
        if (capture_disabled) {
            ImGui::BeginDisabled();
        }
        const std::string capture_button =
            StableUiLabel(
                language,
                UiTextId::CaptureNextMainFrame,
                "SpecForgeCaptureNextMainFrame");
        if (ImGui::Button(capture_button.c_str())) {
            frame_capture_requested_ = true;
        }
        if (capture_disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const std::string_view capture_note = UiText(
            language,
            status.frame_capture_pending
                ? UiTextId::FrameCaptureWaiting
                : UiTextId::FrameCaptureReadbackNote);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(capture_note.size()),
            capture_note.data());

        const std::string capture_status =
            FormatFrameCaptureStatus(
                language,
                status);
        if (!capture_status.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextDisabled(
                "%s",
                capture_status.c_str());
            ImGui::PopTextWrapPos();
        }
        if (status.last_frame_capture_path != nullptr) {
            ImGui::PushTextWrapPos();
            const std::string path = PathToUtf8(
                *status.last_frame_capture_path);
            ImGui::TextUnformatted(path.c_str());
            ImGui::PopTextWrapPos();
        } else if (
            status.frame_capture_output_directory != nullptr) {
            ImGui::PushTextWrapPos();
            const std::string directory = PathToUtf8(
                *status.frame_capture_output_directory);
            const std::string_view output_directory_label =
                UiText(
                    language,
                    UiTextId::
                        FrameCaptureOutputDirectory);
            ImGui::TextDisabled(
                "%.*s: %s",
                static_cast<int>(
                    output_directory_label.size()),
                output_directory_label.data(),
                directory.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::Spacing();
    const std::string profile_output_heading =
        StableUiLabel(
            language,
            UiTextId::ProfileOutputDirectory,
            "SpecForgeProfileOutputDirectory");
    ImGui::SeparatorText(
        profile_output_heading.c_str());
    const std::string output_path =
        PathToUtf8(settings.profile_output_directory);
    const ApplicationSettingsStatus& setting_status =
        settings.StatusFor(
            ApplicationSetting::ProfileOutputDirectory);
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(output_path.c_str());
    ImGui::PopTextWrapPos();

    switch (settings.profile_output_directory_source) {
    case ProfileOutputDirectorySource::Default:
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::
                        ProfileDirectorySourceDefault)
                    .size()),
            UiText(
                language,
                UiTextId::
                    ProfileDirectorySourceDefault)
                .data());
        break;
    case ProfileOutputDirectorySource::UserSetting:
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::
                        ProfileDirectorySourceSaved)
                    .size()),
            UiText(
                language,
                UiTextId::
                    ProfileDirectorySourceSaved)
                .data());
        break;
    case ProfileOutputDirectorySource::Environment:
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::
                        ProfileDirectorySourceEnvironment)
                    .size()),
            UiText(
                language,
                UiTextId::
                    ProfileDirectorySourceEnvironment)
                .data());
        break;
    }

    const bool directory_editing_disabled =
        status.profile_open ||
        status.profile_stopping ||
        settings.profile_output_directory_source ==
            ProfileOutputDirectorySource::Environment;
    if (directory_editing_disabled) {
        ImGui::BeginDisabled();
    }
    const std::string choose_folder_label =
        StableUiLabel(
            language,
            UiTextId::ChooseFolder,
            "SpecForgeChooseProfileOutputFolder");
    if (ImGui::Button(choose_folder_label.c_str())) {
        profile_output_directory_selection_requested_ = true;
    }
    if (directory_editing_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const bool reset_disabled =
        !CanRestoreProfileOutputDirectory(
            settings,
            status);
    if (reset_disabled) {
        ImGui::BeginDisabled();
    }
    const std::string restore_default_label =
        StableUiLabel(
            language,
            UiTextId::RestoreDefault,
            "SpecForgeRestoreProfileOutputDefault");
    if (ImGui::Button(
            restore_default_label.c_str())) {
        ResetProfileOutputDirectory();
    }
    if (reset_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const std::string open_output_label =
        StableUiLabel(
            language,
            UiTextId::OpenOutputFolder,
            "SpecForgeOpenProfileOutputFolder");
    if (ImGui::Button(open_output_label.c_str())) {
        OpenDirectory(
            settings.profile_output_directory,
            language,
            UiTextId::ProfileFolderPrepareError,
            UiTextId::ProfileFolderOpenError,
            UiTextId::ProfileFolderOpened);
    }
    ImGui::SameLine();
    const std::string copy_profile_path_label =
        StableUiLabel(
            language,
            UiTextId::CopyPath,
            "SpecForgeCopyProfileOutputPath");
    if (ImGui::Button(
            copy_profile_path_label.c_str())) {
        CopyPath(
            settings.profile_output_directory,
            language,
            UiTextId::ProfilePathCopied);
    }

    if (settings.profile_output_directory_source ==
        ProfileOutputDirectorySource::Environment) {
        ImGui::PushTextWrapPos();
        const std::string_view notice = UiText(
            language,
            UiTextId::ProfileEnvironmentOverrideNotice);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(notice.size()),
            notice.data());
        ImGui::PopTextWrapPos();
    } else if (status.profile_open || status.profile_stopping) {
        ImGui::PushTextWrapPos();
        const std::string_view notice = UiText(
            language,
            UiTextId::
                StopRecordingBeforeChangingOutput);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(notice.size()),
            notice.data());
        ImGui::PopTextWrapPos();
    }

    if (setting_status.kind !=
        ApplicationSettingsStatusKind::Ready) {
        ImGui::Spacing();
        const bool warning =
            setting_status.kind ==
            ApplicationSettingsStatusKind::LoadWarning;
        const std::string_view feedback =
            FormatProfileOutputDirectoryStatus(
                setting_status.kind,
                language);
        ImGui::TextColored(
            warning
                ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
                : ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
            "%.*s",
            static_cast<int>(feedback.size()),
            feedback.data());
        RenderApplicationSettingsStatusReason(
            setting_status,
            language);
    }

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(
                ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                "%s",
                action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::RenderAbout(
    const ApplicationSettingsView& settings)
{
    const UiLanguage language = settings.language;
    RenderSectionHeading(
        UiText(language, UiTextId::About),
        UiText(language, UiTextId::AboutPageDescription));

    ImGui::TextUnformatted("SpecForge");
    const std::string_view tagline = UiText(
        language,
        UiTextId::ProductTagline);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(tagline.size()),
        tagline.data());
    ImGui::PushTextWrapPos();
    const std::string_view copyright_notice = UiText(
        language,
        UiTextId::CopyrightNotice);
    ImGui::TextUnformatted(
        copyright_notice.data(),
        copyright_notice.data() +
            copyright_notice.size());
    const std::string_view proprietary_notice = UiText(
        language,
        UiTextId::ProprietarySoftwareNotice);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(proprietary_notice.size()),
        proprietary_notice.data());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    RenderReadOnlyValue(
        UiText(language, UiTextId::Version).data(),
        environment_.version.c_str());
    RenderReadOnlyValue(
        UiText(language, UiTextId::Distribution).data(),
        environment_.distribution.c_str());
    RenderReadOnlyValue(
        UiText(language, UiTextId::Configuration).data(),
        environment_.configuration.c_str());
    RenderReadOnlyValue(
        UiText(language, UiTextId::Architecture).data(),
        environment_.target_architecture.c_str());
    const std::string source_text =
        FormatBuildSourceForAbout(
            environment_.build_source,
            language);
    ImGui::TextUnformatted(source_text.c_str());
    RenderReadOnlyValue(
        UiText(language, UiTextId::Graphics).data(),
        "Direct3D 11 / SDR");

    ImGui::Spacing();
    const std::string build_details = StableUiLabel(
        language,
        UiTextId::BuildDetails,
        "SpecForgeBuildDetails");
    ImGui::SeparatorText(build_details.c_str());
    const std::string_view metadata_status_text =
        FormatBuildMetadataStatusForAbout(
            environment_.build_metadata.status,
            language);
    if (!metadata_status_text.empty()) {
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(metadata_status_text.size()),
            metadata_status_text.data());
    } else if (
        environment_.build_metadata.status ==
            BuildMetadataStatus::Available &&
        environment_.build_metadata.metadata) {
        const BuildMetadata& metadata =
            *environment_.build_metadata.metadata;
        const std::string compiler =
            metadata.compiler_id + " " + metadata.compiler_version;
        RenderReadOnlyValue(
            UiText(language, UiTextId::Compiler).data(),
            compiler.c_str());
        RenderReadOnlyValue(
            UiText(language, UiTextId::CMake).data(),
            metadata.cmake_version.c_str());
        RenderReadOnlyValue(
            UiText(language, UiTextId::Generator).data(),
            metadata.generator.c_str());
        RenderReadOnlyValue(
            UiText(language, UiTextId::WindowsSdk).data(),
            metadata.windows_sdk_version
                ? metadata.windows_sdk_version->c_str()
                : UiText(
                      language,
                      UiTextId::NotReported)
                      .data());
    }

    ImGui::Spacing();
    const std::string artifact_identity_heading = StableUiLabel(
        language,
        UiTextId::ArtifactIdentity,
        "SpecForgeArtifactIdentity");
    ImGui::SeparatorText(artifact_identity_heading.c_str());
    const ArtifactIdentityResult& artifact_identity =
        ArtifactIdentityForAbout();
    if (!artifact_identity.completed_at_utc.empty()) {
        RenderReadOnlyValue(
            UiText(language, UiTextId::MetadataCompletedAt).data(),
            artifact_identity.completed_at_utc.c_str());
    }
    if (!artifact_identity.metadata_sha256.empty()) {
        RenderReadOnlyValue(
            UiText(language, UiTextId::MetadataSha256).data(),
            artifact_identity.metadata_sha256.c_str());
    }
    if (!artifact_identity.executable_sha256.empty()) {
        RenderReadOnlyValue(
            UiText(language, UiTextId::ExecutableSha256).data(),
            artifact_identity.executable_sha256.c_str());
    }
    const std::string_view artifact_identity_status =
        FormatArtifactIdentityStatusForAbout(
            artifact_identity.status,
            language);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(artifact_identity_status.size()),
        artifact_identity_status.data());

    ImGui::Spacing();
    const std::string third_party_heading =
        StableUiLabel(
            language,
            UiTextId::ThirdPartyComponents,
            "SpecForgeThirdPartyComponents");
    ImGui::SeparatorText(
        third_party_heading.c_str());
    if (environment_.build_metadata.status ==
            BuildMetadataStatus::Available &&
        environment_.build_metadata.metadata) {
        const BuildMetadata& metadata =
            *environment_.build_metadata.metadata;
        ImGui::BulletText(
            UiText(
                language,
                UiTextId::DearImGuiComponent)
                .data(),
            metadata.dear_imgui_version.c_str());
        ImGui::BulletText(
            UiText(
                language,
                UiTextId::ImPlotComponent)
                .data(),
            metadata.implot_version.c_str());
        ImGui::BulletText(
            UiText(
                language,
                UiTextId::ZlibComponent)
                .data(),
            metadata.zlib_version.c_str());
    } else {
        ImGui::BulletText(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::
                        DearImGuiComponentFallback)
                    .size()),
            UiText(
                language,
                UiTextId::
                    DearImGuiComponentFallback)
                .data());
        ImGui::BulletText(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::ImPlotComponentFallback)
                    .size()),
            UiText(
                language,
                UiTextId::ImPlotComponentFallback)
                .data());
        ImGui::BulletText(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::ZlibComponentFallback)
                    .size()),
            UiText(
                language,
                UiTextId::ZlibComponentFallback)
                .data());
    }
    ImGui::BulletText(
        "%.*s",
        static_cast<int>(
            UiText(
                language,
                UiTextId::ModifiedStbNotice)
                .size()),
        UiText(
            language,
            UiTextId::ModifiedStbNotice)
            .data());

    ImGui::Spacing();
    const std::string legal_documents_heading =
        StableUiLabel(
            language,
            UiTextId::LegalDocuments,
            "SpecForgeLegalDocuments");
    ImGui::SeparatorText(
        legal_documents_heading.c_str());
    ImGui::PushTextWrapPos();
    const std::string_view legal_documents_description = UiText(
        language,
        UiTextId::LegalDocumentsDescription);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(legal_documents_description.size()),
        legal_documents_description.data());
    ImGui::PopTextWrapPos();

    const std::array<std::string, 3> legal_document_buttons = {
        StableUiLabel(
            language,
            UiTextId::EndUserLicenseAgreement,
            "SpecForgeOpenEula"),
        StableUiLabel(
            language,
            UiTextId::ThirdPartyNotices,
            "SpecForgeOpenThirdPartyNotices"),
        StableUiLabel(
            language,
            UiTextId::DataSources,
            "SpecForgeOpenDataSources"),
    };
    float legal_document_row_width =
        ImGui::GetStyle().ItemSpacing.x *
        static_cast<float>(legal_document_buttons.size() - 1);
    for (const std::string& label : legal_document_buttons) {
        legal_document_row_width +=
            ImGui::CalcTextSize(
                label.c_str(),
                nullptr,
                true).x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
    }
    const bool render_legal_documents_inline =
        legal_document_row_width <=
        ImGui::GetContentRegionAvail().x;

    RenderEmbeddedLegalDocument(
        LegalDocument::Eula,
        UiTextId::EndUserLicenseAgreement,
        "SpecForgeOpenEula",
        "SpecForgeEulaDocument",
        language);
    if (render_legal_documents_inline) {
        ImGui::SameLine();
    }
    RenderEmbeddedLegalDocument(
        LegalDocument::ThirdPartyNotices,
        UiTextId::ThirdPartyNotices,
        "SpecForgeOpenThirdPartyNotices",
        "SpecForgeThirdPartyNoticesDocument",
        language);
    if (render_legal_documents_inline) {
        ImGui::SameLine();
    }
    RenderEmbeddedLegalDocument(
        LegalDocument::DataSources,
        UiTextId::DataSources,
        "SpecForgeOpenDataSources",
        "SpecForgeDataSourcesDocument",
        language);

    ImGui::Spacing();
    const std::string diagnostics_heading =
        StableUiLabel(
            language,
            UiTextId::Diagnostics,
            "SpecForgeAboutDiagnostics");
    ImGui::SeparatorText(
        diagnostics_heading.c_str());
    const std::string log_path =
        PathToUtf8(settings.profile_output_directory);
    const std::string_view performance_logs = UiText(
        language,
        UiTextId::PerformanceLogs);
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(performance_logs.size()),
        performance_logs.data());
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(log_path.c_str());
    ImGui::PopTextWrapPos();
    const std::string open_log_label =
        StableUiLabel(
            language,
            UiTextId::OpenLogFolder,
            "SpecForgeOpenLogFolder");
    if (ImGui::Button(open_log_label.c_str())) {
        OpenDirectory(
            settings.profile_output_directory,
            language,
            UiTextId::LogFolderPrepareError,
            UiTextId::LogFolderOpenError,
            UiTextId::LogFolderOpened);
    }
    ImGui::SameLine();
    const std::string copy_diagnostics_label =
        StableUiLabel(
            language,
            UiTextId::CopyDiagnosticInformation,
            "SpecForgeCopyDiagnosticInformation");
    if (ImGui::Button(
            copy_diagnostics_label.c_str())) {
        CopyDiagnosticInformation(
            settings.profile_output_directory,
            language);
    }

    if (!action_status_.empty()) {
        ImGui::Spacing();
        if (action_failed_) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", action_status_.c_str());
        } else {
            ImGui::TextDisabled("%s", action_status_.c_str());
        }
    }
}

void SettingsPanelUi::ResetProfileOutputDirectory()
{
    application_settings_intent_ =
        ApplicationSettingsIntent::
            RestoreDefaultProfileOutputDirectory();
}

bool SettingsPanelUi::ShouldSubmitLanguageSelection(
    const ApplicationSettingsView& settings,
    UiLanguage candidate)
{
    return candidate != settings.language ||
        settings
                .StatusFor(ApplicationSetting::Language)
                .kind !=
            ApplicationSettingsStatusKind::Ready;
}

bool SettingsPanelUi::CanRestoreProfileOutputDirectory(
    const ApplicationSettingsView& settings,
    const SettingsPanelStatus& status)
{
    if (status.profile_open ||
        status.profile_stopping ||
        settings.profile_output_directory_source ==
            ProfileOutputDirectorySource::Environment) {
        return false;
    }
    return settings.profile_output_directory_source ==
               ProfileOutputDirectorySource::UserSetting ||
        settings
                .StatusFor(
                    ApplicationSetting::
                        ProfileOutputDirectory)
                .kind !=
            ApplicationSettingsStatusKind::Ready;
}

void SettingsPanelUi::OpenDirectory(
    const std::filesystem::path& path,
    UiLanguage language,
    UiTextId prepare_error,
    UiTextId open_error,
    UiTextId opened)
{
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        action_failed_ = true;
        action_status_ = UiText(
            language,
            prepare_error);
        return;
    }

    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32) {
        action_failed_ = true;
        action_status_ = UiText(
            language,
            open_error);
        return;
    }

    action_failed_ = false;
    action_status_ = UiText(language, opened);
}

void SettingsPanelUi::CopyPath(
    const std::filesystem::path& path,
    UiLanguage language,
    UiTextId copied)
{
    const std::string path_text = PathToUtf8(path);
    ImGui::SetClipboardText(path_text.c_str());
    action_failed_ = false;
    action_status_ = UiText(language, copied);
}

void SettingsPanelUi::CopyDiagnosticInformation(
    const std::filesystem::path& profile_output_directory,
    UiLanguage language)
{
    const std::string diagnostics =
        FormatDiagnosticInformation(
            environment_,
            profile_output_directory,
            language);
    ImGui::SetClipboardText(diagnostics.c_str());
    action_failed_ = false;
    action_status_ = UiText(
        language,
        UiTextId::DiagnosticInformationCopied);
}

}  // namespace specforge
