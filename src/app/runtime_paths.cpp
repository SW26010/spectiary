#include "app/runtime_paths.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
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

std::optional<std::filesystem::path>
RuntimeResourceUserStateRootOverride()
{
#ifdef _WIN32
    wchar_t* workload_path = nullptr;
    std::size_t workload_path_size = 0;
    if (_wdupenv_s(
            &workload_path,
            &workload_path_size,
            L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD") != 0 ||
        workload_path == nullptr ||
        workload_path[0] == L'\0') {
        std::free(workload_path);
        return std::nullopt;
    }
    std::free(workload_path);

    wchar_t* state_root = nullptr;
    std::size_t state_root_size = 0;
    if (_wdupenv_s(
            &state_root,
            &state_root_size,
            L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR") != 0 ||
        state_root == nullptr ||
        state_root[0] == L'\0') {
        std::free(state_root);
        return std::nullopt;
    }
    std::filesystem::path result(state_root);
    std::free(state_root);
    return result;
#else
    const char* workload_path =
        std::getenv("SPECFORGE_RUNTIME_RESOURCE_WORKLOAD");
    const char* state_root =
        std::getenv("SPECFORGE_RUNTIME_RESOURCE_STATE_DIR");
    if (workload_path == nullptr || *workload_path == '\0' ||
        state_root == nullptr || *state_root == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(state_root);
#endif
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
    RuntimePaths paths = RuntimePathsForDeployment(
        metadata.deployment,
        std::move(inputs));
    if (std::optional<std::filesystem::path> state_root =
            RuntimeResourceUserStateRootOverride()) {
        paths.local_user_state_root = std::move(*state_root);
        paths.profile_log_directory =
            paths.local_user_state_root / "logs";
        paths.imgui_ini_path =
            paths.local_user_state_root /
            "specforge-imgui-v2.ini";
    }
    return paths;
}

}  // namespace specforge
