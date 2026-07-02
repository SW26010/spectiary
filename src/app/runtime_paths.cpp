#include "app/runtime_paths.h"

#include <filesystem>
#include <system_error>
#include <utility>

#if defined(SPECFORGE_RELEASE_PROFILE_PORTABLE) && defined(SPECFORGE_RELEASE_PROFILE_INSTALLED)
#error "SpecForge must be compiled with exactly one release profile definition."
#endif

#if !defined(SPECFORGE_RELEASE_PROFILE_PORTABLE) && !defined(SPECFORGE_RELEASE_PROFILE_INSTALLED)
#error "SpecForge must be compiled with a release profile definition."
#endif

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

const char* ReleaseProfileName(ReleaseProfile profile)
{
    switch (profile) {
    case ReleaseProfile::Portable:
        return "Portable";
    case ReleaseProfile::Installed:
        return "Installed";
    }

    return "Unknown";
}

ReleaseProfile BuildReleaseProfile()
{
#if defined(SPECFORGE_RELEASE_PROFILE_PORTABLE)
    return ReleaseProfile::Portable;
#elif defined(SPECFORGE_RELEASE_PROFILE_INSTALLED)
    return ReleaseProfile::Installed;
#endif
}

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

std::filesystem::path DefaultInstalledLocalUserStateRoot()
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

RuntimePaths RuntimePathsForProfile(ReleaseProfile profile, RuntimePathInputs inputs)
{
    RuntimePaths paths;
    paths.release_profile = profile;
    paths.executable_path = inputs.executable_path.empty() ? CurrentExecutablePath() : std::move(inputs.executable_path);
    paths.package_root = PackageRootForExecutable(paths.executable_path);

    std::filesystem::path installed_root = inputs.installed_local_user_state_root.empty()
                                               ? DefaultInstalledLocalUserStateRoot()
                                               : std::move(inputs.installed_local_user_state_root);

    switch (profile) {
    case ReleaseProfile::Portable:
        paths.local_user_state_root = paths.package_root / "Data";
        break;
    case ReleaseProfile::Installed:
        paths.local_user_state_root = std::move(installed_root);
        break;
    }

    paths.profile_log_directory = paths.local_user_state_root / "logs";
    paths.imgui_ini_path = paths.local_user_state_root / "specforge-imgui-v2.ini";
    return paths;
}

RuntimePaths DefaultRuntimePaths()
{
    RuntimePathInputs inputs;
    inputs.executable_path = CurrentExecutablePath();
    inputs.installed_local_user_state_root = DefaultInstalledLocalUserStateRoot();
    return RuntimePathsForProfile(BuildReleaseProfile(), std::move(inputs));
}

}  // namespace specforge
