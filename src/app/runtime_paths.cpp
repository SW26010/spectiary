#include "app/runtime_paths.h"

#include "app/local_user_state_paths.h"
#include "platform/atomic_file.h"
#include "platform/win32_process_launcher.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void SetActiveStoragePaths(RuntimePaths& paths)
{
    paths.profile_log_directory = paths.logs_root;
    paths.frame_capture_directory = paths.logs_root / "captures";
    paths.imgui_ini_path = paths.state_root / local_user_state_paths::kImGuiIni;
    paths.ui_language_settings_path = paths.config_root / local_user_state_paths::kUiLanguageSettings;
    paths.appearance_settings_path = paths.config_root / local_user_state_paths::kAppearanceSettings;
    paths.ui_scale_settings_path = paths.config_root / local_user_state_paths::kUiScaleSettings;
    paths.input_settings_path = paths.config_root / local_user_state_paths::kInputSettings;
    paths.external_source_settings_path = paths.config_root / local_user_state_paths::kExternalSourceSettings;
    paths.profile_settings_path = paths.config_root / local_user_state_paths::kProfileSettings;
    paths.panel_visibility_state_path = paths.state_root / local_user_state_paths::kPanelVisibilityState;
    paths.source_session_state_path = paths.state_root / local_user_state_paths::kSourceSessionState;
    paths.sample_navigation_state_path = paths.state_root / local_user_state_paths::kSampleNavigationState;
    paths.sample_workflow_state_path = paths.state_root / local_user_state_paths::kSampleWorkflowState;
    paths.spectral_line_user_state_path = paths.state_root / local_user_state_paths::kSpectralLineUserState;
    if (!paths.legacy_application_data_root.empty()) {
        paths.legacy_spectrum_view_state_path = paths.legacy_application_data_root /
            local_user_state_paths::kLegacySpectrumViewState;
        paths.legacy_sample_labeling_state_path = paths.legacy_application_data_root /
            local_user_state_paths::kLegacySampleLabelingState;
    }
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
        paths.legacy_application_data_root = paths.package_root / "Data";
        break;
    case StorageProfile::LocalAppData:
        if (inputs.application_data_root_override) {
            paths.application_data_root = CheckedRoot(*inputs.application_data_root_override);
        } else if (!inputs.local_app_data_user_state_root.empty()) {
            paths.application_data_root = CheckedRoot(inputs.local_app_data_user_state_root);
        } else {
            const auto local = CheckedRoot(inputs.local_app_data_directory
                ? inputs.local_app_data_directory() : LocalAppDataDirectory());
            paths.application_data_root = CheckedRoot(local / project_identity::kLocalAppDataLeaf);
            paths.legacy_application_data_root = local / "SpecForge";
        }
        break;
    default:
        throw std::invalid_argument("Unknown storage profile.");
    }

    if (inputs.application_data_root_override) {
        // Overrides isolate all managed storage and disable production migration.
        // Portable locators and public resources still use package_root, which
        // remains tied to the executable rather than this injected data root.
        paths.application_data_root = CheckedRoot(*inputs.application_data_root_override);
        paths.legacy_application_data_root.clear();
    }
    if (!inputs.legacy_application_data_root.empty()) {
        paths.legacy_application_data_root = CheckedRoot(inputs.legacy_application_data_root);
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
    SetActiveStoragePaths(paths);
    const auto disposable = CheckedRoot(inputs.system_temp_directory
        ? inputs.system_temp_directory() : std::filesystem::temp_directory_path());
    paths.temp_root = disposable / project_identity::kApplicationId / "temp";
    paths.cache_root = disposable / project_identity::kApplicationId / "cache";
    return paths;
}

void MigrateLegacyApplicationStorage(const RuntimePaths& paths)
{
    if (paths.legacy_application_data_root.empty()) return;
    // An explicit allowlist of flat application-owned files. Old logs/captures
    // and unknown files remain inert; no directory traversal or cleanup.
    const auto import = [&](const std::filesystem::path& source,
                            const std::filesystem::path& target) {
        std::error_code error;
        if (std::filesystem::exists(target, error) || error) return;
        const auto status = std::filesystem::symlink_status(source, error);
        if (error || !std::filesystem::is_regular_file(status)) return;
        // Bound pre-release import cost and reset malformed JSON instead of
        // installing a broken legacy file as the new authoritative owner.
        const auto size = std::filesystem::file_size(source, error);
        if (error || size > 16U * 1024U * 1024U) return;
        nlohmann::json document;
        if (source.extension() == ".json") {
            std::ifstream stream(source, std::ios::binary);
            document = nlohmann::json::parse(stream, nullptr, false);
            if (!document.is_object()) return;
        }
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) return;
        const auto temporary = TemporarySiblingPath(target);
        // Publish a complete copy without replacing a destination created by
        // another startup/writer since the initial existence check.
        if (target == paths.profile_settings_path) {
            // The legacy profile setting only holds an output-directory
            // override. Reset it so no restored logger writes the retired root.
            document["output_directory"] = nullptr;
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << document.dump();
            stream.close();
            if (!stream) {
                std::filesystem::remove(temporary, error);
                return;
            }
        } else if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
            return;
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, error);
        }
    };
    if (paths.legacy_application_data_root == paths.application_data_root) {
        import(paths.legacy_application_data_root / local_user_state_paths::kImGuiIni, paths.imgui_ini_path);
    } else {
        import(paths.legacy_application_data_root / "specforge-imgui-v2.ini", paths.imgui_ini_path);
    }
    import(paths.legacy_application_data_root / paths.ui_language_settings_path.filename(), paths.ui_language_settings_path);
    import(paths.legacy_application_data_root / paths.appearance_settings_path.filename(), paths.appearance_settings_path);
    import(paths.legacy_application_data_root / paths.ui_scale_settings_path.filename(), paths.ui_scale_settings_path);
    import(paths.legacy_application_data_root / paths.input_settings_path.filename(), paths.input_settings_path);
    import(paths.legacy_application_data_root / paths.external_source_settings_path.filename(), paths.external_source_settings_path);
    import(paths.legacy_application_data_root / paths.profile_settings_path.filename(), paths.profile_settings_path);
    import(paths.legacy_application_data_root / paths.panel_visibility_state_path.filename(), paths.panel_visibility_state_path);
    import(paths.legacy_application_data_root / paths.source_session_state_path.filename(), paths.source_session_state_path);
    import(paths.legacy_application_data_root / paths.sample_navigation_state_path.filename(), paths.sample_navigation_state_path);
    import(paths.legacy_application_data_root / paths.sample_workflow_state_path.filename(), paths.sample_workflow_state_path);
    import(paths.legacy_application_data_root / paths.spectral_line_user_state_path.filename(), paths.spectral_line_user_state_path);
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
    const auto isolated_root = RuntimeResourceUserStateRootOverride();
    return {
        .executable_path = std::move(executable_path),
        .application_data_root_override = isolated_root,
        .legacy_application_data_root = isolated_root.value_or(std::filesystem::path{}),
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
