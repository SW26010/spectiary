#include "app/runtime_paths.h"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shlobj_core.h>
#endif

namespace specforge {
namespace {

std::filesystem::path FallbackExecutablePath()
{
    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    if (error) {
        current = std::filesystem::temp_directory_path();
    }
    return current / "SpecForge.exe";
}

std::filesystem::path PackageRootForExecutable(const std::filesystem::path& executable_path)
{
    if (executable_path.has_parent_path()) {
        return executable_path.parent_path();
    }

    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    return error ? std::filesystem::temp_directory_path() : current;
}

}  // namespace

std::filesystem::path CurrentExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            break;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#endif

    return FallbackExecutablePath();
}

std::filesystem::path DefaultLocalAppDataUserStateRoot()
{
#ifdef _WIN32
    PWSTR local_app_data_path = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data_path);
    if (SUCCEEDED(result) && local_app_data_path != nullptr) {
        std::filesystem::path root(local_app_data_path);
        CoTaskMemFree(local_app_data_path);
        return root / L"SpecForge";
    }
    if (local_app_data_path != nullptr) {
        CoTaskMemFree(local_app_data_path);
    }
#endif

    return std::filesystem::temp_directory_path() / "SpecForge";
}

RuntimePaths RuntimePathsForDeployment(
    DeploymentMetadata deployment,
    RuntimePathInputs inputs)
{
    RuntimePaths paths;
    paths.distribution = deployment.distribution;
    paths.storage_profile = deployment.storage_profile;
    paths.executable_path = inputs.executable_path.empty() ? CurrentExecutablePath() : std::move(inputs.executable_path);
    paths.package_root = PackageRootForExecutable(paths.executable_path);
    paths.public_spectral_line_catalog_path =
        paths.package_root / "config" / "spectral_lines.public.tsv";

    std::filesystem::path local_app_data_root =
        inputs.local_app_data_user_state_root.empty()
        ? DefaultLocalAppDataUserStateRoot()
        : std::move(inputs.local_app_data_user_state_root);

    switch (deployment.storage_profile) {
    case StorageProfile::Portable:
        paths.local_user_state_root = paths.package_root / "Data";
        break;
    case StorageProfile::LocalAppData:
        paths.local_user_state_root = std::move(local_app_data_root);
        break;
    }

    paths.profile_log_directory = paths.local_user_state_root / "logs";
    paths.imgui_ini_path = paths.local_user_state_root / "specforge-imgui-v2.ini";
    return paths;
}

RuntimePaths DefaultRuntimePaths()
{
    const SpecForgeMetadataReadResult& metadata =
        DefaultSpecForgeMetadata();
    if (metadata.startup_error) {
        throw std::runtime_error(*metadata.startup_error);
    }

    RuntimePathInputs inputs;
    inputs.executable_path = CurrentExecutablePath();
    inputs.local_app_data_user_state_root =
        DefaultLocalAppDataUserStateRoot();
    return RuntimePathsForDeployment(
        metadata.deployment,
        std::move(inputs));
}

}  // namespace specforge
