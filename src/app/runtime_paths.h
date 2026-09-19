#pragma once

#include "app/spectiary_metadata.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace spectiary {

struct RuntimePathInputs {
    // Absolute actual executable location; package_root is its parent only.
    std::filesystem::path executable_path;
    // Fully resolved physical root, independent of product display/artifact names.
    std::filesystem::path local_app_data_user_state_root;
    std::optional<std::filesystem::path>
        application_data_root_override;
    // Explicit isolated migration input (for automation seed staging only).
    std::filesystem::path legacy_application_data_root;
    // Platform seams. Invoked only when the selected profile needs them.
    std::function<std::filesystem::path()> local_app_data_directory;
    std::function<std::filesystem::path()> system_temp_directory;
};

struct RuntimePaths {
    Distribution distribution = Distribution::Standalone;
    StorageProfile storage_profile = StorageProfile::LocalAppData;
    std::filesystem::path executable_path;
    std::filesystem::path package_root;
    std::filesystem::path application_data_root;
    std::filesystem::path config_root;
    std::filesystem::path state_root;
    std::filesystem::path logs_root;
    std::filesystem::path unsaved_root;
    std::filesystem::path cache_root;
    std::filesystem::path temp_root;
    std::filesystem::path public_spectral_line_catalog_path;
    // Read-only migration input; never an active persistence owner.
    std::filesystem::path legacy_application_data_root;
    std::filesystem::path profile_log_directory;
    std::filesystem::path frame_capture_directory;
    std::filesystem::path imgui_ini_path;
    std::filesystem::path ui_language_settings_path;
    std::filesystem::path appearance_settings_path;
    std::filesystem::path ui_scale_settings_path;
    std::filesystem::path input_settings_path;
    std::filesystem::path external_source_settings_path;
    std::filesystem::path profile_settings_path;
    std::filesystem::path panel_visibility_state_path;
    std::filesystem::path legacy_spectrum_view_state_path; // Legacy migration input only.
    std::filesystem::path spectrum_plot_preferences_path;
    std::filesystem::path spectrum_viewport_state_path;
    std::filesystem::path source_session_state_path;
    std::filesystem::path sample_navigation_state_path;
    std::filesystem::path sample_labeling_state_path;
    std::filesystem::path sample_labeling_drafts_path;
    std::filesystem::path legacy_sample_labeling_state_path;
    std::filesystem::path sample_workflow_state_path;
    std::filesystem::path spectral_line_user_state_path;
};

class SpectiaryStartup {
public:
    SpectiaryStartup(const SpectiaryStartup&) = default;
    SpectiaryStartup& operator=(const SpectiaryStartup&) = default;
    SpectiaryStartup(SpectiaryStartup&&) noexcept = default;
    SpectiaryStartup& operator=(SpectiaryStartup&&) noexcept =
        default;

    [[nodiscard]] const RuntimePaths& runtime_paths() const noexcept;
    [[nodiscard]] const SpectiaryMetadataReadResult& metadata()
        const noexcept;

private:
    SpectiaryStartup(
        RuntimePaths runtime_paths,
        SpectiaryMetadataReadResult metadata);

    friend SpectiaryStartup PrepareSpectiaryStartup(
        RuntimePathInputs inputs);

    RuntimePaths runtime_paths_;
    SpectiaryMetadataReadResult metadata_;
};

[[nodiscard]] std::filesystem::path CurrentExecutablePath();
[[nodiscard]] std::filesystem::path DefaultLocalAppDataUserStateRoot();
[[nodiscard]] RuntimePathInputs CurrentProcessRuntimePathInputs(
    std::filesystem::path executable_path);
[[nodiscard]] RuntimePaths RuntimePathsForDeployment(
    DeploymentMetadata deployment,
    RuntimePathInputs inputs);
[[nodiscard]] SpectiaryStartup PrepareSpectiaryStartup(
    RuntimePathInputs inputs);
[[nodiscard]] const SpectiaryStartup& DefaultSpectiaryStartup();
[[nodiscard]] RuntimePaths DefaultRuntimePaths();

// Startup-only, bounded best-effort import. Existing destinations always win.
// Never scans, moves or deletes a directory or user-owned documents.
void MigrateLegacyApplicationStorage(const RuntimePaths& paths);

}  // namespace spectiary
