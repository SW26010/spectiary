#include "app/runtime_paths.h"

#include "app/local_user_state_paths.h"
#include "platform/win32_process_launcher.h"

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

std::filesystem::path PackageRootForExecutable(const std::filesystem::path& executable_path)
{
    if (executable_path.is_absolute() && executable_path.has_filename() &&
        executable_path.filename() != "." && executable_path.filename() != "..") {
        return executable_path.parent_path();
    }
    throw std::invalid_argument("An absolute executable path is required for the package root.");
}

std::filesystem::path CheckedRoot(const std::filesystem::path& root)
{
    if (root.empty() || !root.is_absolute() || root.native().find(L'\0') != std::wstring::npos) {
        throw std::invalid_argument("Storage roots must be resolved absolute paths.");
    }
    std::error_code error;
    // Also checks existing ancestors; no directories are created during resolution.
    const auto resolved = std::filesystem::weakly_canonical(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("Could not resolve storage root", root, error);
    }
    for (auto ancestor = resolved; !ancestor.empty();) {
        const auto status = std::filesystem::status(ancestor, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("Could not inspect storage root", ancestor, error);
        }
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                throw std::invalid_argument("Storage root has a non-directory ancestor.");
            }
            break;
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return root.lexically_normal();
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
    paths.appearance_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kAppearanceSettings;
    paths.ui_scale_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kUiScaleSettings;
    paths.input_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kInputSettings;
    paths.external_source_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kExternalSourceSettings;
    paths.profile_settings_path =
        paths.local_user_state_root /
        local_user_state_paths::kProfileSettings;
    paths.panel_visibility_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kPanelVisibilityState;
    paths.legacy_spectrum_view_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kLegacySpectrumViewState;
    paths.source_session_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSourceSessionState;
    paths.sample_navigation_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kSampleNavigationState;
    paths.legacy_sample_labeling_state_path =
        paths.local_user_state_root /
        local_user_state_paths::kLegacySampleLabelingState;
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
    const CurrentExecutablePathResult resolved =
        ResolveCurrentExecutablePath();
    if (resolved.resolved()) {
        return resolved.path;
    }
#endif

    throw std::runtime_error("Could not resolve the current executable path.");
}

namespace {
std::filesystem::path LocalAppDataDirectory()
{
#ifdef _WIN32
    PWSTR local_app_data_path = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data_path);
    if (SUCCEEDED(result) && local_app_data_path != nullptr) {
        std::filesystem::path root(local_app_data_path);
        CoTaskMemFree(local_app_data_path);
        return CheckedRoot(root);
    }
    if (local_app_data_path != nullptr) {
        CoTaskMemFree(local_app_data_path);
    }
#endif

    throw std::runtime_error("Could not resolve LocalAppData for persistent storage.");
}
}  // namespace

std::filesystem::path DefaultLocalAppDataUserStateRoot()
{
    return CheckedRoot(LocalAppDataDirectory() / project_identity::kLocalAppDataLeaf);
}

RuntimePaths RuntimePathsForDeployment(
    DeploymentMetadata deployment,
    RuntimePathInputs inputs)
{
    RuntimePaths paths;
    paths.distribution = deployment.distribution;
    paths.storage_profile = deployment.storage_profile;
    paths.executable_path = inputs.executable_path.empty() ? CurrentExecutablePath() : std::move(inputs.executable_path);
    paths.package_root = CheckedRoot(PackageRootForExecutable(paths.executable_path));
    paths.public_spectral_line_catalog_path =
        paths.package_root / "config" / "spectral_lines.public.tsv";

    switch (deployment.storage_profile) {
    case StorageProfile::Portable:
        paths.application_data_root = paths.package_root;
        SetLocalUserStatePaths(
            paths,
            paths.package_root / "Data");
        break;
    case StorageProfile::LocalAppData:
        if (inputs.local_user_state_root_override) {
            paths.application_data_root = CheckedRoot(*inputs.local_user_state_root_override);
            SetLocalUserStatePaths(paths, paths.application_data_root);
        } else if (!inputs.local_app_data_user_state_root.empty()) {
            paths.application_data_root = CheckedRoot(inputs.local_app_data_user_state_root);
            SetLocalUserStatePaths(paths, paths.application_data_root);
        } else {
            const auto local = CheckedRoot(inputs.local_app_data_directory
                ? inputs.local_app_data_directory() : LocalAppDataDirectory());
            paths.application_data_root = CheckedRoot(local / project_identity::kLocalAppDataLeaf);
            // #103-A deliberately preserves the existing business-file layout.
            SetLocalUserStatePaths(paths, CheckedRoot(local / "SpecForge"));
        }
        break;
    default:
        throw std::invalid_argument("Unknown storage profile.");
    }

    if (inputs.local_user_state_root_override) {
        // Isolation covers both transitional files and final role namespaces.
        // Portable locators and public resources still use package_root, which
        // remains tied to the executable rather than this injected data root.
        paths.application_data_root = CheckedRoot(*inputs.local_user_state_root_override);
        SetLocalUserStatePaths(paths, paths.application_data_root);
    }
    paths.config_root = paths.application_data_root / "config";
    paths.state_root = paths.application_data_root / "state";
    paths.spectrum_plot_preferences_path =
        paths.config_root / local_user_state_paths::kSpectrumPlotPreferences;
    paths.spectrum_viewport_state_path =
        paths.state_root / local_user_state_paths::kSpectrumViewportState;
    paths.logs_root = paths.application_data_root / "logs";
    paths.unsaved_root = paths.application_data_root / "unsaved";
    paths.sample_labeling_state_path =
        paths.state_root / local_user_state_paths::kSampleLabelingState;
    paths.sample_labeling_drafts_path =
        paths.unsaved_root / local_user_state_paths::kSampleLabelingDrafts;
    const auto disposable = CheckedRoot(inputs.system_temp_directory
        ? inputs.system_temp_directory() : std::filesystem::temp_directory_path());
    paths.temp_root = disposable / project_identity::kApplicationId / "temp";
    paths.cache_root = disposable / project_identity::kApplicationId / "cache";
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
