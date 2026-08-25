#include "ui/settings_panel.h"

#include "app/embedded_legal_documents.h"
#include "app/runtime_paths.h"
#include "app/specforge_metadata_validation.h"
#include "platform/file_sha256.h"
#include "ui/profile_recording_ui_state.h"
#include "specforge/specforge_build_identity.h"
#include "ui/theme.h"
#include "ui/ui_scale_settings.h"

#include <Windows.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace specforge {

std::span<const AppearanceThemeOption>
AppearanceThemeOptions() noexcept
{
    static const std::array options = {
        AppearanceThemeOption{
            .text_id = UiTextId::FollowSystemTheme,
            .selection = ThemeSelection::FollowSystem(),
        },
        AppearanceThemeOption{
            .text_id = UiTextId::DarkTheme,
            .selection = ThemeSelection::Explicit(
                BuiltInDarkThemeId()),
        },
        AppearanceThemeOption{
            .text_id = UiTextId::LightTheme,
            .selection = ThemeSelection::Explicit(
                BuiltInLightThemeId()),
        },
    };
    return options;
}

std::optional<int> AppearanceThemeOptionIndex(
    const ThemeSelection& selection)
{
    const std::span<const AppearanceThemeOption> options =
        AppearanceThemeOptions();
    for (std::size_t index = 0;
         index < options.size();
         ++index) {
        if (options[index].selection == selection) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

std::optional<ThemeSelection>
AppearanceThemeSelectionAt(int index)
{
    const std::span<const AppearanceThemeOption> options =
        AppearanceThemeOptions();
    if (index < 0 ||
        static_cast<std::size_t>(index) >= options.size()) {
        return std::nullopt;
    }
    return options[static_cast<std::size_t>(index)].selection;
}

PlatformWorkArea ResolvePlatformWorkArea(
    const ImGuiViewport& viewport,
    std::span<const ImGuiPlatformMonitor> monitors)
{
    const ImVec2 center = viewport.GetCenter();
    const ImGuiPlatformMonitor* closest = nullptr;
    float closest_distance_squared = FLT_MAX;
    for (const ImGuiPlatformMonitor& monitor : monitors) {
        const float monitor_max_x =
            monitor.MainPos.x + monitor.MainSize.x;
        const float monitor_max_y =
            monitor.MainPos.y + monitor.MainSize.y;
        const float distance_x = center.x < monitor.MainPos.x
            ? monitor.MainPos.x - center.x
            : (center.x > monitor_max_x
                ? center.x - monitor_max_x
                : 0.0f);
        const float distance_y = center.y < monitor.MainPos.y
            ? monitor.MainPos.y - center.y
            : (center.y > monitor_max_y
                ? center.y - monitor_max_y
                : 0.0f);
        const float distance_squared =
            distance_x * distance_x +
            distance_y * distance_y;
        if (distance_squared < closest_distance_squared) {
            closest = &monitor;
            closest_distance_squared = distance_squared;
        }
    }

    return closest != nullptr
        ? PlatformWorkArea{
            .position = closest->WorkPos,
            .size = closest->WorkSize,
        }
        : PlatformWorkArea{
            .position = viewport.WorkPos,
            .size = viewport.WorkSize,
        };
}

namespace {

constexpr float kMinimumNavigationWidth = 190.0f;
constexpr float kInitialSettingsWidth = 860.0f;
constexpr float kInitialSettingsHeight = 560.0f;
constexpr auto kArtifactIdentityRetryDelay =
    std::chrono::milliseconds{250};

ImVec4 SettingsFeedbackColor(bool warning)
{
    const SemanticPalette& palette =
        ActiveSemanticPalette();
    return warning ? palette.warning : palette.error;
}

constexpr std::array<SettingsSection, 7> kSettingsSections = {
    SettingsSection::General,
    SettingsSection::Appearance,
    SettingsSection::Language,
    SettingsSection::Input,
    SettingsSection::DataAndRecovery,
    SettingsSection::Diagnostics,
    SettingsSection::About,
};

struct LegalDisclosureRenderResult {
    ImRect bounds;
};

LegalDisclosureRenderResult RenderEmbeddedLegalDisclosure(
    LegalDocument document,
    UiTextId title_text_id,
    const char* disclosure_id,
    const char* content_child_id,
    UiLanguage language,
    std::optional<LegalDocument>& expanded_document)
{
    const std::string disclosure_label = StableUiLabel(
        language,
        title_text_id,
        disclosure_id);
    const bool was_expanded =
        expanded_document == document;
    ImGui::SetNextItemOpen(was_expanded, ImGuiCond_Always);
    const bool disclosure_open =
        ImGui::CollapsingHeader(disclosure_label.c_str());
    LegalDisclosureRenderResult result{
        .bounds = ImRect(
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax()),
    };
    if (disclosure_open != was_expanded) {
        if (disclosure_open) {
            expanded_document = document;
        } else {
            expanded_document.reset();
        }
    }
    if (expanded_document != document) {
        return result;
    }

    const std::string_view content =
        EmbeddedLegalDocumentContent(document);
    const float content_height =
        ImGui::GetTextLineHeightWithSpacing() * 16.0f;
    if (ImGui::BeginChild(
            content_child_id,
            ImVec2(0.0f, content_height),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_None)) {
        ImGui::PushTextWrapPos(0.0f);
        if (content.empty()) {
            const std::string_view unavailable = UiText(
                language,
                UiTextId::LegalDocumentUnavailable);
            ImGui::TextDisabled(
                "%.*s",
                static_cast<int>(unavailable.size()),
                unavailable.data());
        } else {
            ImGui::TextUnformatted(
                content.data(),
                content.data() + content.size());
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    result.bounds.Add(ImGui::GetItemRectMin());
    result.bounds.Add(ImGui::GetItemRectMax());

    const std::string copy_label = StableUiLabel(
        language,
        UiTextId::CopyDocument,
        "SpecForgeCopyLegalDocument");
    ImGui::BeginDisabled(content.empty());
    if (ImGui::Button(copy_label.c_str())) {
        ImGui::SetClipboardText(std::string(content).c_str());
    }
    ImGui::EndDisabled();
    result.bounds.Add(ImGui::GetItemRectMin());
    result.bounds.Add(ImGui::GetItemRectMax());
    return result;
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
    std::string items;
    for (const AppearanceThemeOption& option :
         AppearanceThemeOptions()) {
        items += UiText(language, option.text_id);
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

}  // namespace

struct SettingsPanelUi::ArtifactIdentityComputation {
    std::mutex mutex;
    bool running = false;
    std::optional<ArtifactIdentityResult> completed;
    std::jthread worker;
};

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
    std::string_view source = build_source.mode;
    if (build_source.mode == "working_tree") {
        source = UiText(language, UiTextId::WorkingTree);
    } else if (build_source.mode == "head") {
        source = build_source.revision;
    }
    AppendLabeledValue(
        formatted,
        language,
        UiTextId::Source,
        source);
    return formatted;
}

namespace {

ArtifactIdentityResult VerifyArtifactMetadataAgainstExecutableSha256(
    std::string_view executable_sha256,
    const BuildMetadataReadResult& build_metadata)
{
    ArtifactIdentityResult result;
    result.executable_sha256 = std::string(executable_sha256);

    if (build_metadata.status != BuildMetadataStatus::Available ||
        !build_metadata.metadata) {
        return result;
    }

    const BuildMetadata& metadata = *build_metadata.metadata;
    if (!metadata.finalized_artifact ||
        !metadata_validation::IsValidUtcTimestamp(
            metadata.finalized_artifact->completed_at_utc) ||
        metadata.finalized_artifact->artifact.file !=
            metadata_contract::kCanonicalExecutableFileName ||
        !metadata_validation::IsValidSha256(
            metadata.finalized_artifact->artifact.sha256)) {
        return result;
    }

    if (metadata.finalized_artifact->artifact.sha256 !=
        executable_sha256) {
        result.status = ArtifactIdentityStatus::Mismatch;
        return result;
    }

    result.status = ArtifactIdentityStatus::Available;
    result.completed_at_utc =
        metadata.finalized_artifact->completed_at_utc;
    return result;
}

}  // namespace

ArtifactIdentityResult VerifyExecutableArtifactIdentity(
    const std::filesystem::path& executable_path,
    const BuildMetadataReadResult& build_metadata)
{
    const std::optional<std::string> executable_sha256 =
        ComputeFileSha256(executable_path);
    if (!executable_sha256) {
        return {};
    }
    return VerifyArtifactMetadataAgainstExecutableSha256(
        *executable_sha256,
        build_metadata);
}

AboutArtifactPresentation AboutArtifactPresentationFor(
    const ArtifactIdentityResult& artifact_identity,
    const BuildMetadataReadResult& build_metadata)
{
    const bool show_metadata_derived_fields =
        !artifact_identity.executable_sha256.empty() &&
        artifact_identity.status == ArtifactIdentityStatus::Available &&
        !artifact_identity.completed_at_utc.empty() &&
        build_metadata.status == BuildMetadataStatus::Available &&
        build_metadata.metadata.has_value();
    return {
        .show_executable_sha256 =
            !artifact_identity.executable_sha256.empty(),
        .show_metadata_derived_fields = show_metadata_derived_fields,
        .show_completed_at_utc = show_metadata_derived_fields,
        .show_third_party_versions = show_metadata_derived_fields,
        .show_third_party_fallback = !show_metadata_derived_fields,
    };
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
    case ApplicationSettingsStatusReason::UnsupportedTheme:
        return UiText(
            language,
            UiTextId::UnsupportedApplicationTheme);
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

void SettingsPanelUi::RequestPlatformWindowFocus(
    ImGuiViewport& viewport)
{
    ImGuiPlatformIO& platform_io =
        ImGui::GetPlatformIO();
    if (&viewport != ImGui::GetMainViewport() &&
        viewport.PlatformWindowCreated &&
        platform_io.Platform_SetWindowFocus != nullptr) {
        platform_io.Platform_SetWindowFocus(&viewport);
    }
}

SettingsPanelUi::SettingsPanelUi(SettingsPanelEnvironment environment)
    : environment_(std::move(environment))
{
}

SettingsPanelUi::~SettingsPanelUi() = default;

void SettingsPanelUi::StartArtifactIdentityComputation(
    std::optional<std::string> executable_sha256)
{
    if (!artifact_identity_computation_) {
        artifact_identity_computation_ =
            std::make_unique<ArtifactIdentityComputation>();
    }

    ArtifactIdentityComputation* computation =
        artifact_identity_computation_.get();
    {
        std::lock_guard lock(computation->mutex);
        if (computation->running) {
            return;
        }
        computation->running = true;
    }

    const std::filesystem::path executable_path =
        environment_.executable_path;
    const BuildMetadataReadResult build_metadata =
        environment_.build_metadata;
    const std::string cached_executable_sha256 =
        executable_sha256.value_or(std::string{});
    try {
        computation->worker = std::jthread(
            [computation,
             executable_path = std::move(executable_path),
             build_metadata = std::move(build_metadata),
             executable_sha256 = std::move(executable_sha256),
             cached_executable_sha256](
                std::stop_token) {
                ArtifactIdentityResult result;
                result.executable_sha256 = cached_executable_sha256;
                try {
                    result = executable_sha256
                        ? VerifyArtifactMetadataAgainstExecutableSha256(
                              *executable_sha256,
                              build_metadata)
                        : VerifyExecutableArtifactIdentity(
                              executable_path,
                              build_metadata);
                } catch (...) {
                    result = ArtifactIdentityResult{};
                    result.executable_sha256 =
                        cached_executable_sha256;
                }

                std::lock_guard lock(computation->mutex);
                computation->completed = std::move(result);
                computation->running = false;
            });
    } catch (...) {
        std::lock_guard lock(computation->mutex);
        ArtifactIdentityResult result;
        result.executable_sha256 = cached_executable_sha256;
        computation->completed = std::move(result);
        computation->running = false;
    }
}

const ArtifactIdentityResult&
SettingsPanelUi::ArtifactIdentityForAbout()
{
    if (!artifact_identity_computation_) {
        artifact_identity_computation_ =
            std::make_unique<ArtifactIdentityComputation>();
    }

    std::optional<ArtifactIdentityResult> completed;
    {
        std::lock_guard lock(artifact_identity_computation_->mutex);
        completed = std::exchange(
            artifact_identity_computation_->completed,
            std::nullopt);
    }
    if (completed) {
        artifact_identity_ = std::move(completed);
        artifact_identity_retry_at_ =
            artifact_identity_->executable_sha256.empty()
                ? std::chrono::steady_clock::now() +
                    kArtifactIdentityRetryDelay
                : (std::chrono::steady_clock::time_point::max)();
    }

    if (!artifact_identity_) {
        artifact_identity_ = ArtifactIdentityResult{
            .status = ArtifactIdentityStatus::Pending,
        };
        std::optional<std::string> executable_sha256;
        try {
            executable_sha256 = ComputeFileSha256(
                environment_.executable_path);
        } catch (...) {
            executable_sha256 = std::nullopt;
        }
        if (executable_sha256) {
            artifact_identity_->executable_sha256 =
                *executable_sha256;
        }
        StartArtifactIdentityComputation(
            std::move(executable_sha256));
    }
    else if (
        artifact_identity_->executable_sha256.empty() &&
        (artifact_identity_retry_requested_ ||
         std::chrono::steady_clock::now() >= artifact_identity_retry_at_)) {
        artifact_identity_ = ArtifactIdentityResult{
            .status = ArtifactIdentityStatus::Pending,
        };
        artifact_identity_retry_requested_ = false;
        StartArtifactIdentityComputation();
    }
    return *artifact_identity_;
}

void SettingsPanelUi::Open()
{
    if (!open_) {
        action_failed_ = false;
        action_status_.clear();
        ui_scale_draft_percentage_.reset();
        expanded_legal_document_.reset();
        if (artifact_identity_ &&
            artifact_identity_->executable_sha256.empty()) {
            artifact_identity_retry_requested_ = true;
            artifact_identity_retry_at_ =
                std::chrono::steady_clock::now();
        }
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
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    const std::span<const ImGuiPlatformMonitor> monitors =
        platform_io.Monitors.Size > 0
        ? std::span<const ImGuiPlatformMonitor>(
            platform_io.Monitors.Data,
            static_cast<std::size_t>(platform_io.Monitors.Size))
        : std::span<const ImGuiPlatformMonitor>{};
    const PlatformWorkArea work_area = ResolvePlatformWorkArea(
        *viewport,
        monitors);
    // A secondary viewport's work area is the Settings platform window
    // itself. Use its monitor for constraints so shrinking the window does
    // not also lower the maximum size on the next frame.
    const ImVec2 work_position = work_area.position;
    const ImVec2 work_size = work_area.size;
    const float user_scale =
        static_cast<float>(settings.ui_scale_percentage) /
        static_cast<float>(kDefaultUiScalePercentage);
    const ImVec2 preferred_size(
        kInitialSettingsWidth * user_scale,
        kInitialSettingsHeight * user_scale);
    const ImVec2 maximum_size(
        std::max(1.0f, work_size.x),
        std::max(1.0f, work_size.y));
    const ImVec2 initial_size(
        std::min(preferred_size.x, maximum_size.x),
        std::min(preferred_size.y, maximum_size.y));
    const ImVec2 initial_position(
        work_position.x +
            std::max(0.0f, work_size.x - initial_size.x) * 0.5f,
        work_position.y +
            std::max(0.0f, work_size.y - initial_size.y) * 0.5f);
    ImGui::SetNextWindowSize(initial_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        initial_size,
        maximum_size);
    ImGui::SetNextWindowPos(
        initial_position,
        ImGuiCond_FirstUseEver);
    const bool focus_requested =
        std::exchange(focus_requested_, false);
    if (focus_requested) {
        ImGui::SetNextWindowFocus();
    }

    const std::string settings_window =
        SettingsWindowLabel(settings.language);
    const bool contents_visible = ImGui::Begin(
            settings_window.c_str(),
            &open_,
            ImGuiWindowFlags_NoCollapse);
    ImGuiViewport* settings_viewport =
        ImGui::GetWindowViewport();
    settings_viewport_id_ = settings_viewport->ID;
    if (focus_requested) {
        RequestPlatformWindowFocus(
            *settings_viewport);
    }
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
    if (selected_section_ != SettingsSection::About) {
        expanded_legal_document_.reset();
    }
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
        const ImVec4 feedback_color =
            SettingsFeedbackColor(warning);
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

    int theme = AppearanceThemeOptionIndex(
        settings.theme_selection).value_or(0);
    const ImVec4 semantic_accent =
        ActiveSemanticPalette().accent;
    float accent_color[3] = {
        semantic_accent.x,
        semantic_accent.y,
        semantic_accent.z,
    };
    const std::string theme_label =
        AppearanceThemeLabel(language);
    const std::string theme_items =
        AppearanceThemeItems(language);
    const std::string accent_color_label =
        AppearanceAccentColorLabel(language);
    if (ImGui::Combo(
        theme_label.c_str(),
        &theme,
        theme_items.c_str())) {
        if (std::optional<ThemeSelection> selection =
                AppearanceThemeSelectionAt(theme)) {
            SetThemeSelection(std::move(*selection));
        }
    }
    ImGui::BeginDisabled();
    ImGui::ColorEdit3(
        accent_color_label.c_str(),
        accent_color,
        ImGuiColorEditFlags_NoInputs);
    ImGui::EndDisabled();
    const std::string_view accent_color_unavailable = UiText(
        language,
        UiTextId::AppearanceAccentColorUnavailable);
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled(
        "%.*s",
        static_cast<int>(accent_color_unavailable.size()),
        accent_color_unavailable.data());
    ImGui::PopTextWrapPos();

    const ApplicationSettingsStatus& appearance_status =
        settings.StatusFor(ApplicationSetting::Appearance);
    if (appearance_status.kind !=
        ApplicationSettingsStatusKind::Ready) {
        ImGui::Spacing();
        const bool warning =
            appearance_status.kind ==
            ApplicationSettingsStatusKind::LoadWarning;
        const ImVec4 feedback_color =
            SettingsFeedbackColor(warning);
        const UiTextId feedback_text_id = warning
            ? UiTextId::ThemeLoadWarning
            : (appearance_status.kind ==
                    ApplicationSettingsStatusKind::Rejected
                ? UiTextId::ThemeRejected
                : UiTextId::ThemeSaveError);
        const std::string_view feedback = UiText(
            language,
            feedback_text_id);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(
            feedback_color,
            "%.*s",
            static_cast<int>(feedback.size()),
            feedback.data());
        RenderApplicationSettingsStatusReason(
            appearance_status,
            language);
        ImGui::PopTextWrapPos();
    }

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
    const ImVec4 feedback_color =
        SettingsFeedbackColor(warning);
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

void SettingsPanelUi::SetThemeSelection(
    ThemeSelection selection)
{
    application_settings_intent_ =
        ApplicationSettingsIntent::SetThemeSelection(
            std::move(selection));
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
    const ImVec4 feedback_color =
        SettingsFeedbackColor(!failed);
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
        const ImVec4 feedback_color =
            SettingsFeedbackColor(warning);
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
            ImGui::TextColored(
                ActiveSemanticPalette().error,
                "%s",
                action_status_.c_str());
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
            SettingsFeedbackColor(warning),
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
                ActiveSemanticPalette().error,
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

    const ArtifactIdentityResult& artifact_identity =
        ArtifactIdentityForAbout();
    const AboutArtifactPresentation presentation =
        AboutArtifactPresentationFor(
            artifact_identity,
            environment_.build_metadata);

    if (presentation.show_metadata_derived_fields) {
        ImGui::Spacing();
        const std::string build_details = StableUiLabel(
            language,
            UiTextId::BuildDetails,
            "SpecForgeBuildDetails");
        ImGui::SeparatorText(build_details.c_str());
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
    if (presentation.show_completed_at_utc &&
        !artifact_identity.completed_at_utc.empty()) {
        RenderReadOnlyValue(
            UiText(language, UiTextId::MetadataCompletedAt).data(),
            artifact_identity.completed_at_utc.c_str());
    }
    if (presentation.show_executable_sha256) {
        RenderReadOnlyValue(
            UiText(language, UiTextId::ExecutableSha256).data(),
            artifact_identity.executable_sha256.c_str());
    }

    ImGui::Spacing();
    const std::string third_party_heading =
        StableUiLabel(
            language,
            UiTextId::ThirdPartyComponents,
            "SpecForgeThirdPartyComponents");
    ImGui::SeparatorText(
        third_party_heading.c_str());
    if (presentation.show_third_party_versions) {
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
    } else if (presentation.show_third_party_fallback) {
        ImGui::BulletText(
            "%.*s",
            static_cast<int>(
                UiText(
                    language,
                    UiTextId::DearImGuiComponentFallback)
                    .size()),
            UiText(
                language,
                UiTextId::DearImGuiComponentFallback)
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

    const LegalDisclosureRenderResult third_party_notices =
        RenderEmbeddedLegalDisclosure(
            LegalDocument::ThirdPartyNotices,
            UiTextId::ThirdPartyNotices,
            "SpecForgeOpenThirdPartyNotices",
            "##SpecForgeThirdPartyNoticesContent",
            language,
            expanded_legal_document_);
    const LegalDisclosureRenderResult data_sources =
        RenderEmbeddedLegalDisclosure(
            LegalDocument::DataSources,
            UiTextId::DataSources,
            "SpecForgeOpenDataSources",
            "##SpecForgeDataSourcesContent",
            language,
            expanded_legal_document_);

    if (expanded_legal_document_ &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse_position = ImGui::GetIO().MousePos;
        const ImGuiWindow* current_window = ImGui::GetCurrentWindow();
        const ImGuiWindow* hovered_window = GImGui->HoveredWindow;
        const bool settings_window_hovered =
            hovered_window != nullptr &&
            (hovered_window == current_window ||
             hovered_window->RootWindow == current_window->RootWindow);
        // Include both headers so clicking the other disclosure switches it
        // directly; the expanded disclosure bounds also include its child
        // and Copy Document control.
        const bool disclosure_target =
            settings_window_hovered &&
            (third_party_notices.bounds.Contains(mouse_position) ||
             data_sources.bounds.Contains(mouse_position));
        if (!disclosure_target) {
            expanded_legal_document_.reset();
        }
    }

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
            ImGui::TextColored(
                ActiveSemanticPalette().error,
                "%s",
                action_status_.c_str());
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
