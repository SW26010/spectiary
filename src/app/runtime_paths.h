#pragma once

#include "app/specforge_metadata.h"

#include <filesystem>
#include <optional>

namespace specforge {

struct RuntimePathInputs {
    std::filesystem::path executable_path;
    std::filesystem::path local_app_data_user_state_root;
    std::optional<std::filesystem::path>
        local_user_state_root_override;
};

struct RuntimePaths {
    Distribution distribution = Distribution::Standalone;
    StorageProfile storage_profile = StorageProfile::LocalAppData;
    std::filesystem::path executable_path;
    std::filesystem::path package_root;
    std::filesystem::path public_spectral_line_catalog_path;
    std::filesystem::path local_user_state_root;
    std::filesystem::path profile_log_directory;
    std::filesystem::path imgui_ini_path;
    std::filesystem::path ui_language_settings_path;
    std::filesystem::path ui_scale_settings_path;
    std::filesystem::path profile_settings_path;
    std::filesystem::path panel_visibility_state_path;
    std::filesystem::path source_session_state_path;
    std::filesystem::path sample_navigation_state_path;
    std::filesystem::path sample_labeling_state_path;
    std::filesystem::path sample_workflow_state_path;
    std::filesystem::path spectral_line_user_state_path;
};

class SpecForgeStartup {
public:
    SpecForgeStartup(const SpecForgeStartup&) = default;
    SpecForgeStartup& operator=(const SpecForgeStartup&) = default;
    SpecForgeStartup(SpecForgeStartup&&) noexcept = default;
    SpecForgeStartup& operator=(SpecForgeStartup&&) noexcept =
        default;

    [[nodiscard]] const RuntimePaths& runtime_paths() const noexcept;
    [[nodiscard]] const SpecForgeMetadataReadResult& metadata()
        const noexcept;

private:
    SpecForgeStartup(
        RuntimePaths runtime_paths,
        SpecForgeMetadataReadResult metadata);

    friend SpecForgeStartup PrepareSpecForgeStartup(
        RuntimePathInputs inputs);

    RuntimePaths runtime_paths_;
    SpecForgeMetadataReadResult metadata_;
};

[[nodiscard]] std::filesystem::path CurrentExecutablePath();
[[nodiscard]] std::filesystem::path DefaultLocalAppDataUserStateRoot();
[[nodiscard]] RuntimePathInputs CurrentProcessRuntimePathInputs(
    std::filesystem::path executable_path);
[[nodiscard]] RuntimePaths RuntimePathsForDeployment(
    DeploymentMetadata deployment,
    RuntimePathInputs inputs);
[[nodiscard]] SpecForgeStartup PrepareSpecForgeStartup(
    RuntimePathInputs inputs);
[[nodiscard]] const SpecForgeStartup& DefaultSpecForgeStartup();
[[nodiscard]] RuntimePaths DefaultRuntimePaths();

}  // namespace specforge
