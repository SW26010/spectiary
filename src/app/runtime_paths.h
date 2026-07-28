#pragma once

#include "app/specforge_metadata.h"

#include <filesystem>

namespace specforge {

struct RuntimePathInputs {
    std::filesystem::path executable_path;
    std::filesystem::path local_app_data_user_state_root;
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
};

[[nodiscard]] std::filesystem::path CurrentExecutablePath();
[[nodiscard]] std::filesystem::path DefaultLocalAppDataUserStateRoot();
[[nodiscard]] RuntimePaths RuntimePathsForDeployment(
    DeploymentMetadata deployment,
    RuntimePathInputs inputs);
[[nodiscard]] RuntimePaths DefaultRuntimePaths();

}  // namespace specforge
