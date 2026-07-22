#pragma once

#include <filesystem>
#include <string>

namespace specforge {

enum class SettingsSection {
    General,
    Appearance,
    Language,
    Input,
    DataAndRecovery,
    About,
};

struct SettingsPanelEnvironment {
    std::string version;
    std::string release_profile;
    std::filesystem::path data_directory;
    std::filesystem::path log_directory;
};

[[nodiscard]] SettingsPanelEnvironment DefaultSettingsPanelEnvironment();

class SettingsPanelUi {
public:
    SettingsPanelUi();
    explicit SettingsPanelUi(SettingsPanelEnvironment environment);

    void Open();
    void Render();

    [[nodiscard]] bool open() const;

private:
    void RenderNavigation();
    void RenderSelectedSection();
    void RenderGeneral();
    void RenderAppearance();
    void RenderLanguage();
    void RenderInput();
    void RenderDataAndRecovery();
    void RenderAbout();

    void OpenDirectory(const std::filesystem::path& path, const char* label);
    void CopyPath(const std::filesystem::path& path, const char* label);
    void CopyDiagnosticInformation();

    SettingsPanelEnvironment environment_;
    SettingsSection selected_section_ = SettingsSection::General;
    bool open_ = false;
    bool focus_requested_ = false;
    bool content_scroll_reset_requested_ = false;
    bool action_failed_ = false;
    std::string action_status_;

    friend struct SettingsPanelUiTestAccess;
};

}  // namespace specforge
