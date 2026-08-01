#include "app/runtime_paths.h"

#include "app/local_user_state_paths.h"

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

void SetLocalUserStatePaths(
    RuntimePaths& paths,
    std::filesystem::path root)
{
    paths.local_user_state_root = std::move(root);
    paths.profile_log_directory =
        paths.local_user_state_root /
        local_user_state_paths::kProfileLogDirectory;
    paths.frame_capture_directory =
        paths.local_user_state_root /
        local_user_state_paths::kFrameCaptureDirectory;
    paths.imgui_ini_path =
        paths.local_user_state_root /
        local_user_state_paths::kImGuiIni;
    paths.ui_language_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kUiLanguageSettings;
    paths.ui_scale_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kUiScaleSettings;
    paths.input_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kInputSettings;
    paths.profile_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kProfileSettings;
    paths.panel_visibility_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kPanelVisibilityState;
    paths.source_session_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSourceSessionState;
    paths.sample_navigation_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSampleNavigationState;
    paths.sample_labeling_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSampleLabelingState;
    paths.sample_workflow_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSampleWorkflowState;
    paths.spectral_line_user_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSpectralLineUserState;
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
        SetLocalUserStatePaths(
            paths,
            paths.package_root / "Data");
        break;
    case StorageProfile::LocalAppData:
        SetLocalUserStatePaths(
            paths,
            std::move(local_app_data_root));
        break;
    }

    if (inputs.local_user_state_root_override) {
        SetLocalUserStatePaths(
            paths,
            std::move(
                *inputs.local_user_state_root_override));
    }
    return paths;
}

SpecForgeStartup::SpecForgeStartup(
    RuntimePaths runtime_paths,
    SpecForgeMetadataReadResult metadata)
    : runtime_paths_(std::move(runtime_paths)),
      metadata_(std::move(metadata))
{
}

const RuntimePaths& SpecForgeStartup::runtime_paths() const noexcept
{
    return runtime_paths_;
}

const SpecForgeMetadataReadResult& SpecForgeStartup::metadata()
    const noexcept
{
    return metadata_;
}

RuntimePathInputs CurrentProcessRuntimePathInputs(
    std::filesystem::path executable_path)
{
    return {
        .executable_path = std::move(executable_path),
        .local_app_data_user_state_root =
            DefaultLocalAppDataUserStateRoot(),
        .local_user_state_root_override =
            RuntimeResourceUserStateRootOverride(),
    };
}

SpecForgeStartup PrepareSpecForgeStartup(
    RuntimePathInputs inputs)
{
    if (inputs.executable_path.empty()) {
        throw std::invalid_argument(
            "The executable path is required for startup.");
    }

    SpecForgeMetadataReadResult metadata =
        ReadAdjacentSpecForgeMetadata(
            inputs.executable_path.parent_path(),
            CompiledBuildIdentity());
    if (metadata.startup_error) {
        throw std::runtime_error(*metadata.startup_error);
    }

    RuntimePaths paths = RuntimePathsForDeployment(
        metadata.deployment,
        std::move(inputs));
    return SpecForgeStartup(
        std::move(paths),
        std::move(metadata));
}

const SpecForgeStartup& DefaultSpecForgeStartup()
{
    static const SpecForgeStartup startup =
        PrepareSpecForgeStartup(
            CurrentProcessRuntimePathInputs(
                CurrentExecutablePath()));
    return startup;
}

RuntimePaths DefaultRuntimePaths()
{
    return DefaultSpecForgeStartup().runtime_paths();
}

}  // namespace specforge
