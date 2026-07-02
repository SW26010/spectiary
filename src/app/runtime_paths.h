#pragma once

#include <filesystem>

namespace specforge {

enum class ReleaseProfile {
    Portable,
    Installed,
};

struct RuntimePathInputs {
    std::filesystem::path executable_path;
    std::filesystem::path installed_local_user_state_root;
};

struct RuntimePaths {
    ReleaseProfile release_profile = ReleaseProfile::Portable;
    std::filesystem::path executable_path;
    std::filesystem::path package_root;
    std::filesystem::path local_user_state_root;
    std::filesystem::path profile_log_directory;
    std::filesystem::path imgui_ini_path;
};

[[nodiscard]] const char* ReleaseProfileName(ReleaseProfile profile);
[[nodiscard]] ReleaseProfile BuildReleaseProfile();
[[nodiscard]] std::filesystem::path CurrentExecutablePath();
[[nodiscard]] std::filesystem::path DefaultInstalledLocalUserStateRoot();
[[nodiscard]] RuntimePaths RuntimePathsForProfile(ReleaseProfile profile, RuntimePathInputs inputs);
[[nodiscard]] RuntimePaths DefaultRuntimePaths();

}  // namespace specforge
