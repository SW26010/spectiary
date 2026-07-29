#include "ui/settings_panel.h"

#include "app/runtime_paths.h"
#include "ui/profile_recording_ui_state.h"
#include "specforge/specforge_build_identity.h"
#include "ui/ui_scale_settings.h"

#include <Windows.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

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

std::string StableUiLabel(
    UiLanguage language,
    UiTextId text_id,
    std::string_view stable_id)
{
    std::string label(UiText(language, text_id));
    label += "###";
    label += stable_id;
    return label;
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

void RenderUnavailableNote(const char* explanation)
{
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Not available yet. %s", explanation);
    ImGui::PopTextWrapPos();
}

void RenderReadOnlyValue(const char* label, const char* value)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
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
    const BuildSourceIdentity& build_source)
{
    if (build_source.mode == "working_tree") {
        return "Source: Working tree";
    }
    if (build_source.mode == "head") {
        constexpr std::size_t kDisplayedRevisionLength = 12;
        return "Source: HEAD " +
            build_source.revision.substr(
                0,
                kDisplayedRevisionLength);
    }
    return "Source: " + build_source.mode;
}

std::string_view FormatBuildMetadataStatusForAbout(
    BuildMetadataStatus status)
{
    switch (status) {
    case BuildMetadataStatus::Available:
        return {};
    case BuildMetadataStatus::Unavailable:
        return "Build metadata unavailable";
    case BuildMetadataStatus::Mismatch:
        return "Build metadata mismatch";
    }
    return "Build metadata unavailable";
}

std::string_view FormatProfileOutputDirectoryStatus(
    ApplicationSettingsStatusKind kind)
{
    switch (kind) {
    case ApplicationSettingsStatusKind::Ready:
        return {};
    case ApplicationSettingsStatusKind::LoadWarning:
        return "The saved profile output directory could not "
               "be loaded; using the current fallback directory.";
    case ApplicationSettingsStatusKind::PersistenceError:
        return "The profile output directory could not be saved.";
    case ApplicationSettingsStatusKind::Rejected:
        return "The profile output directory could not be changed.";
    }
    return {};
}

std::string FormatDiagnosticInformation(
    const SettingsPanelEnvironment& environment,
    const std::filesystem::path& profile_output_directory)
{
    std::string diagnostics;
    diagnostics.reserve(320);
    diagnostics += "SpecForge ";
    diagnostics += environment.version;
    diagnostics += "\nDistribution: ";
    diagnostics += environment.distribution;
    diagnostics += "\nSource mode: ";
    diagnostics += environment.build_source.mode;
    if (environment.build_source.mode == "head") {
        diagnostics += "\nSource revision: ";
        diagnostics += environment.build_source.revision;
    }
    diagnostics += "\nGraphics: Direct3D 11 / SDR";
    diagnostics += "\nData directory: ";
    diagnostics += PathToUtf8(environment.data_directory);
    diagnostics += "\nLog directory: ";
    diagnostics += PathToUtf8(profile_output_directory);
    return diagnostics;
}

std::string SettingsPanelUi::SectionLabel(
    SettingsSection section,
    UiLanguage language)
{
    switch (section) {
    case SettingsSection::General:
        return "General";
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
        return "Input";
    case SettingsSection::DataAndRecovery:
        return "Data & Recovery";
    case SettingsSection::Diagnostics:
        return "Diagnostics";
    case SettingsSection::About:
        return "About";
    }
    return "Settings";
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

SettingsPanelUi::SettingsPanelUi(SettingsPanelEnvironment environment)
    : environment_(std::move(environment))
{
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
        ImGui::CalcTextSize(widest_navigation_label.c_str()).x +
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
        RenderGeneral();
        return;
    case SettingsSection::Appearance:
        RenderAppearance(settings);
        return;
    case SettingsSection::Language:
        RenderLanguage(settings);
        return;
    case SettingsSection::Input:
        RenderInput();
        return;
    case SettingsSection::DataAndRecovery:
        RenderDataAndRecovery();
        return;
    case SettingsSection::Diagnostics:
        RenderDiagnostics(settings, status);
        return;
    case SettingsSection::About:
        RenderAbout(settings);
        return;
    }
}

void SettingsPanelUi::RenderGeneral()
{
    RenderSectionHeading("General", "Choose how SpecForge starts and restores your local workspace.");

    bool restore_previous_session = true;
    ImGui::BeginDisabled();
    ImGui::Checkbox("Restore the previous session at startup", &restore_previous_session);
    ImGui::EndDisabled();
    RenderUnavailableNote("Session restoration is currently managed automatically.");
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
    if (!setting_status.detail.empty()) {
        ImGui::TextDisabled(
            "%s",
            setting_status.detail.c_str());
    }
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
        UiText(language, UiTextId::LocalizationInProgress);
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
    if (!setting_status.detail.empty()) {
        ImGui::TextDisabled(
            "%s",
            setting_status.detail.c_str());
    }
    ImGui::PopTextWrapPos();
}

void SettingsPanelUi::RenderInput()
{
    RenderSectionHeading("Input", "Tune mouse, touchpad, and keyboard behavior for spectrum inspection.");

    float mouse_zoom_sensitivity = 1.0f;
    float touchpad_zoom_sensitivity = 1.0f;
    bool reverse_zoom_direction = false;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("Mouse zoom sensitivity", &mouse_zoom_sensitivity, 0.5f, 2.0f, "%.1fx");
    ImGui::SliderFloat("Touchpad zoom sensitivity", &touchpad_zoom_sensitivity, 0.5f, 2.0f, "%.1fx");
    ImGui::Checkbox("Reverse zoom direction", &reverse_zoom_direction);
    ImGui::Button("View keyboard shortcuts");
    ImGui::EndDisabled();
    RenderUnavailableNote("Input behavior currently follows the built-in interaction model.");
}

void SettingsPanelUi::RenderDataAndRecovery()
{
    RenderSectionHeading(
        "Data & Recovery",
        "Inspect local application storage. Scientific source files and label result files remain user-owned.");

    const std::string data_path = PathToUtf8(environment_.data_directory);
    ImGui::TextUnformatted("Application data");
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", data_path.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Open Data Folder")) {
        OpenDirectory(environment_.data_directory, "data folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Path##Data")) {
        CopyPath(environment_.data_directory, "Data path copied.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Configuration portability");
    ImGui::BeginDisabled();
    ImGui::Button("Import Settings...");
    ImGui::SameLine();
    ImGui::Button("Export Settings...");
    ImGui::EndDisabled();
    RenderUnavailableNote("A public, versioned settings-file format has not been defined.");

    ImGui::Spacing();
    ImGui::SeparatorText("Recovery and reset");
    ImGui::BeginDisabled();
    ImGui::Button("Reset Window Layout");
    ImGui::Button("Reset Application Settings");
    ImGui::Button("Erase All Application State...");
    ImGui::EndDisabled();
    RenderUnavailableNote("Reset operations need explicit data boundaries and confirmation behavior.");

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
    RenderSectionHeading(
        "Diagnostics",
        "Record bounded performance profiles for investigating interaction and loading latency.");

    const ProfileRecordingUiPresentation presentation =
        ResolveProfileRecordingUiPresentation(
            status.profile_open,
            status.profile_stopping);
    ImGui::TextDisabled("Performance profile");
    ImGui::SameLine();
    ImGui::TextUnformatted(
        status.profile_open
            ? "Recording"
            : (status.profile_stopping ? "Finishing..." : "Not recording"));

    if (!presentation.menu_action_enabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(presentation.menu_action.data())) {
        profile_recording_toggle_requested_ = true;
    }
    if (!presentation.menu_action_enabled) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Automatically stops after 5 minutes or 100 MiB.");

    if (status.profile_path != nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled("Current profile");
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
        const UiLanguage language = settings.language;
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

        if (!status.frame_capture_status_message.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextDisabled(
                "%.*s",
                static_cast<int>(
                    status.frame_capture_status_message
                        .size()),
                status.frame_capture_status_message
                    .data());
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
    ImGui::SeparatorText("Profile output directory");
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
        ImGui::TextDisabled("Source: storage-profile default");
        break;
    case ProfileOutputDirectorySource::UserSetting:
        ImGui::TextDisabled("Source: saved setting");
        break;
    case ProfileOutputDirectorySource::Environment:
        ImGui::TextDisabled("Source: SPECFORGE_PROFILE_DIR environment override");
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
    if (ImGui::Button("Choose Folder...")) {
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
    if (ImGui::Button("Restore Default")) {
        ResetProfileOutputDirectory();
    }
    if (reset_disabled) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Open Output Folder")) {
        OpenDirectory(
            settings.profile_output_directory,
            "profile output folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Path##ProfileOutput")) {
        CopyPath(
            settings.profile_output_directory,
            "Profile output path copied.");
    }

    if (settings.profile_output_directory_source ==
        ProfileOutputDirectorySource::Environment) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "Remove SPECFORGE_PROFILE_DIR before changing this path in Settings.");
        ImGui::PopTextWrapPos();
    } else if (status.profile_open || status.profile_stopping) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled(
            "Stop the current recording before changing its output directory.");
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
                setting_status.kind);
        ImGui::TextColored(
            warning
                ? ImVec4(0.95f, 0.75f, 0.30f, 1.0f)
                : ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
            "%.*s",
            static_cast<int>(feedback.size()),
            feedback.data());
        if (!setting_status.detail.empty()) {
            ImGui::TextDisabled(
                "%s",
                setting_status.detail.c_str());
        }
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
    RenderSectionHeading("About", "Version, licensing, and diagnostic information for this build.");

    ImGui::TextUnformatted("SpecForge");
    ImGui::TextDisabled("Local astronomical spectrum inspection and labeling.");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted("Copyright (c) 2026 SpecForge.");
    ImGui::TextDisabled("Proprietary software. Use is subject to the SpecForge EULA.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    RenderReadOnlyValue("Version", environment_.version.c_str());
    RenderReadOnlyValue(
        "Distribution",
        environment_.distribution.c_str());
    RenderReadOnlyValue("Configuration", environment_.configuration.c_str());
    RenderReadOnlyValue(
        "Architecture",
        environment_.target_architecture.c_str());
    const std::string source_text =
        FormatBuildSourceForAbout(environment_.build_source);
    ImGui::TextUnformatted(source_text.c_str());
    RenderReadOnlyValue("Graphics", "Direct3D 11 / SDR");

    ImGui::Spacing();
    ImGui::SeparatorText("Build details");
    const std::string_view metadata_status_text =
        FormatBuildMetadataStatusForAbout(
            environment_.build_metadata.status);
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
        RenderReadOnlyValue("Compiler", compiler.c_str());
        RenderReadOnlyValue("CMake", metadata.cmake_version.c_str());
        RenderReadOnlyValue("Generator", metadata.generator.c_str());
        RenderReadOnlyValue(
            "Windows SDK",
            metadata.windows_sdk_version
                ? metadata.windows_sdk_version->c_str()
                : "Not reported");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Third-party components");
    if (environment_.build_metadata.status ==
            BuildMetadataStatus::Available &&
        environment_.build_metadata.metadata) {
        const BuildMetadata& metadata =
            *environment_.build_metadata.metadata;
        ImGui::BulletText(
            "Dear ImGui %s (docking / Win32 / DirectX 11) - MIT License",
            metadata.dear_imgui_version.c_str());
        ImGui::BulletText(
            "ImPlot %s - MIT License",
            metadata.implot_version.c_str());
        ImGui::BulletText(
            "zlib %s - zlib License",
            metadata.zlib_version.c_str());
    } else {
        ImGui::BulletText(
            "Dear ImGui (docking / Win32 / DirectX 11) - MIT License");
        ImGui::BulletText("ImPlot - MIT License");
        ImGui::BulletText("zlib - zlib License");
    }
    ImGui::BulletText("Modified stb headers bundled with Dear ImGui - MIT License");
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Full terms: Legal/EULA.txt and Legal/THIRD_PARTY_NOTICES.txt.");
    ImGui::TextDisabled("Scientific data attribution: Legal/DATA_SOURCES.txt.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::SeparatorText("Diagnostics");
    const std::string log_path =
        PathToUtf8(settings.profile_output_directory);
    ImGui::TextDisabled("Performance logs");
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(log_path.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Open Log Folder")) {
        OpenDirectory(
            settings.profile_output_directory,
            "log folder");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Diagnostic Information")) {
        CopyDiagnosticInformation(
            settings.profile_output_directory);
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

void SettingsPanelUi::OpenDirectory(const std::filesystem::path& path, const char* label)
{
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        action_failed_ = true;
        action_status_ = std::string("Could not prepare the ") + label + ".";
        return;
    }

    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32) {
        action_failed_ = true;
        action_status_ = std::string("Could not open the ") + label + ".";
        return;
    }

    action_failed_ = false;
    action_status_ = std::string("Opened the ") + label + ".";
}

void SettingsPanelUi::CopyPath(const std::filesystem::path& path, const char* label)
{
    const std::string path_text = PathToUtf8(path);
    ImGui::SetClipboardText(path_text.c_str());
    action_failed_ = false;
    action_status_ = label;
}

void SettingsPanelUi::CopyDiagnosticInformation(
    const std::filesystem::path& profile_output_directory)
{
    const std::string diagnostics =
        FormatDiagnosticInformation(
            environment_,
            profile_output_directory);
    ImGui::SetClipboardText(diagnostics.c_str());
    action_failed_ = false;
    action_status_ = "Diagnostic information copied.";
}

}  // namespace specforge
