#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"
#include "ui/source_collection_session_state_cache_io.h"
#include "ui/source_collection_session.h"
#include "ui/sample_workflow_preparation.h"

#include <nlohmann/json.hpp>
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace specforge;
namespace fs = std::filesystem;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class F> void RequireFailure(F operation)
{
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    Require(failed, "invalid persistent root must fail explicitly");
}

void TestStorageContext(const fs::path& test_root)
{
    const auto package = test_root / "package";
    const auto local = test_root / "local";
    const auto disposable = test_root / "system-temp";
    int local_calls = 0;
    RuntimePathInputs inputs{
        .executable_path = package / "renamed.exe",
        .local_app_data_directory = [&] { ++local_calls; return local; },
        .system_temp_directory = [&] { return disposable; },
    };
    const auto installed = RuntimePathsForDeployment({}, inputs);
    Require(local_calls == 1, "resolve LocalAppData once");
    Require(installed.application_data_root == local / project_identity::kLocalAppDataLeaf,
            "use the explicit product storage leaf");
    Require(installed.application_data_root.filename() == "Spectiary", "final leaf is Spectiary");
    Require(installed.legacy_application_data_root == local / "SpecForge", "old root is migration-only");
    Require(!fs::exists(local), "path resolution must not create or migrate data");
    Require(installed.package_root == package, "package root is independent of executable name");
    inputs.local_app_data_directory = [&]() -> fs::path {
        ++local_calls;
        throw std::runtime_error("LocalAppData unavailable");
    };
    const DeploymentMetadata portable{.distribution = Distribution::Portable,
                                      .storage_profile = StorageProfile::Portable};
    const auto paths = RuntimePathsForDeployment(portable, inputs);
    Require(local_calls == 1, "Portable must not query LocalAppData");
    Require(paths.application_data_root == package, "Portable application root equals package root");
    Require(paths.legacy_application_data_root == package / "Data", "Portable legacy root is migration-only");
    for (const auto& p : {installed, paths}) {
        Require(p.config_root == p.application_data_root / "config", "config namespace");
        Require(p.state_root == p.application_data_root / "state", "state namespace");
        Require(p.logs_root == p.application_data_root / "logs", "logs namespace");
        Require(p.unsaved_root == p.application_data_root / "unsaved", "unsaved namespace");
        Require(p.temp_root == disposable / project_identity::kApplicationId / "temp", "system temp");
        Require(p.cache_root == disposable / project_identity::kApplicationId / "cache", "disposable cache");
    }
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    for (const auto& invalid : {fs::path{}, fs::path("relative")}) {
        inputs.local_app_data_directory = [=] { return invalid; };
        RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    }
    inputs.application_data_root_override = test_root / "injected";
    const auto overridden = RuntimePathsForDeployment({}, inputs);
    Require(overridden.application_data_root == *inputs.application_data_root_override,
            "override injects an already resolved root without platform lookup");
    Require(overridden.legacy_application_data_root.empty(), "override disables production legacy discovery");
    const auto portable_override = RuntimePathsForDeployment(portable, inputs);
    Require(portable_override.package_root == package &&
            portable_override.application_data_root == *inputs.application_data_root_override,
            "Portable override isolates managed storage without changing package identity");
    Require(portable_override.application_data_root == *inputs.application_data_root_override,
            "Portable runtime override isolates existing persistence");
    Require(portable_override.spectrum_plot_preferences_path ==
                *inputs.application_data_root_override / "config" / "spectrum-plot-preferences.json" &&
            portable_override.spectrum_viewport_state_path ==
                *inputs.application_data_root_override / "state" / "spectrum-viewport-state.json",
            "Portable override must isolate both spectrum owners");
    Require(portable_override.config_root == overridden.config_root &&
            portable_override.state_root == overridden.state_root &&
            portable_override.logs_root == overridden.logs_root &&
            portable_override.unsaved_root == overridden.unsaved_root,
            "both profiles must use the injected root for managed role namespaces");
    const auto source = package / "work" / "source.npy";
    const auto locator = PersistedPathReferenceJson(source, portable_override);
    Require(locator.at("path_kind") == "package_relative" &&
            ReadPersistedPathReference(locator, portable_override) == source &&
            ReadPersistedPathReference(locator, paths) == source &&
            portable_override.public_spectral_line_catalog_path == paths.public_spectral_line_catalog_path,
            "storage isolation must not rebase Portable source locators or public resources");
    inputs.application_data_root_override = fs::path{};
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    inputs.application_data_root_override = "relative";
    RequireFailure([&] { (void)RuntimePathsForDeployment(portable, inputs); });
    const auto file = test_root / "not-directory";
    std::ofstream(file) << "sentinel";
    inputs.application_data_root_override = file / "nested";
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
}

void TestLegacyBrandState(const fs::path& test_root)
{
    const auto file = test_root / "legacy-brand.json";
    const auto write = [&](const nlohmann::json& value) {
        std::ofstream out(file); out << value.dump();
    };
    // Read-only migration keeps user data; next write has only current identity.
    write({{"format_kind", "specforge.sample_labeling.drafts"},
           {"schema_version", 1}, {"drafts", nlohmann::json::array({{{"name", "SpecForge"}, {"values", {1, 2, 3}}}})}});
    auto read = LoadVersionedJsonCacheFile(file, "spectiary.sample_labeling.drafts", {1}, "draft");
    Require(read.document.has_value(), "historical checkpoint remains readable");
    Require(read.document->root["drafts"][0]["name"] == "SpecForge" &&
            read.document->root["drafts"][0]["values"][2] == 3,
            "migration must preserve user text and draft values");
    Require(read.document->root["format_kind"] == "spectiary.sample_labeling.drafts",
            "loaded checkpoint adopts current format identity");
    write({{"format_kind", "specforge.appearance.settings"}, {"schema_version", 1},
           {"theme_id", "specforge.theme.dark"}});
    read = LoadVersionedJsonCacheFile(file, "spectiary.appearance.settings", {1}, "appearance");
    Require(read.document && read.document->root["theme_id"] == "builtin.theme.dark",
            "historical theme selection survives identity cutover");
    write({{"format_kind", "specforge.catalog_user_state.cache"}, {"schema_version", 6},
           {"catalogs", {{"specforge.public", {{"name", "specforge.public"},
              {"references", {{{"catalog_identity", "specforge.public"}}}}}}}}});
    read = LoadVersionedJsonCacheFile(file, "spectiary.catalog_user_state.cache", {6}, "catalog");
    Require(read.document && read.document->root["catalogs"].contains("public-spectral-lines.v1"),
            "historical public catalog key migrates");
    const auto& catalog = read.document->root["catalogs"]["public-spectral-lines.v1"];
    Require(catalog["name"] == "specforge.public" &&
            catalog["references"][0]["catalog_identity"] == "public-spectral-lines.v1",
            "catalog references migrate without rewriting user names");
    write({{"format_kind", "specforge.unknown"}, {"schema_version", 1}});
    Require(!LoadVersionedJsonCacheFile(file, "spectiary.unknown", {1}, "unknown").document,
            "compatibility is an explicit allowlist, never arbitrary prefix replacement");
}

void TestBoundedMigration(const fs::path& root)
{
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto base = root / (profile == StorageProfile::Portable ? "portable-migration" : "local-migration");
        const auto paths = RuntimePathsForDeployment({.storage_profile = profile}, {
            .executable_path = base / "package/app.exe",
            .local_app_data_directory = [=] { return base / "local"; },
            .system_temp_directory = [=] { return base / "temp"; },
        });
        const auto write = [](const fs::path& path, const char* text) {
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        };
        const auto read = [](const fs::path& path) {
            std::ifstream stream(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), {});
        };
        const auto legacy = paths.legacy_application_data_root;
        write(legacy / "ui-language.json", R"({"legacy":true})");
        write(legacy / "appearance-settings.json", R"({"legacy":true})");
        write(paths.appearance_settings_path, "new target must win even if corrupt");
        write(legacy / "input-settings.json", "broken json");
        write(legacy / "profile-settings.json", R"({"output_directory":"retired logs"})");
        write(legacy / "spectral-line-grouping-views.json", R"({"groups":[1,2]})");
        write(legacy / "specforge-imgui-v2.ini", "layout fixture");
        write(legacy / "unknown/nested/user.asdf", "canonical legacy sentinel");
        write(paths.application_data_root / "user.asdf", "canonical root sentinel");
        write(paths.application_data_root / "my-work/source.fits", "user source sentinel");
        write(legacy / "logs/old.log", "old log");
        const auto first = std::async(std::launch::async, [&] { MigrateLegacyApplicationStorage(paths); });
        MigrateLegacyApplicationStorage(paths);
        first.wait();
        Require(read(paths.ui_language_settings_path) == R"({"legacy":true})", "known setting imports");
        Require(read(paths.appearance_settings_path) == "new target must win even if corrupt", "never replace existing target");
        Require(!fs::exists(paths.input_settings_path), "malformed legacy settings reset");
        Require(nlohmann::json::parse(read(paths.profile_settings_path))["output_directory"].is_null(),
            "legacy profile destination resets to final logs root");
        Require(read(paths.spectral_line_user_state_path) == R"({"groups":[1,2]})", "grouping views import whole");
        Require(read(paths.imgui_ini_path) == "layout fixture", "layout imports to neutral filename");
        Require(read(legacy / "unknown/nested/user.asdf") == "canonical legacy sentinel", "unknown legacy work untouched");
        Require(read(paths.application_data_root / "user.asdf") == "canonical root sentinel", "canonical root work untouched");
        Require(read(paths.application_data_root / "my-work/source.fits") == "user source sentinel", "user subdirectory untouched");
        Require(read(legacy / "logs/old.log") == "old log" && !fs::exists(paths.logs_root / "old.log"), "no recursive log migration");
        write(paths.ui_language_settings_path, R"({"active":true})");
        MigrateLegacyApplicationStorage(paths);
        Require(read(paths.ui_language_settings_path) == R"({"active":true})", "new startup cannot resurrect stale data");
        Require(read(legacy / "ui-language.json") == R"({"legacy":true})", "active updates never write legacy input");
    }
}

RuntimePaths Context(const fs::path& package, StorageProfile profile)
{
    return RuntimePathsForDeployment({.storage_profile = profile}, {
        .executable_path = package / "any.exe",
        .local_app_data_user_state_root = package / "local" / "Spectiary",
    });
}

void TestLocators(const fs::path& root)
{
    const auto portable = Context(root / "package-a", StorageProfile::Portable);
    const auto moved = Context(root / "package-b", StorageProfile::Portable);
    const auto local = Context(root / "package-a", StorageProfile::LocalAppData);
    Require(UserPathDisplayText({}, portable).empty(), "empty optional UI paths remain displayable");
    const auto file = portable.package_root / "work" / "source.npy";
    const auto encoded = PersistedPathReferenceJson(file, portable);
    Require(encoded.at("path_kind") == "package_relative", "Portable locator is relative");
    Require(ReadPersistedPathReference(encoded, moved) == moved.package_root / "work" / "source.npy",
            "explicit package-relative relocation uses supplied context");
    Require(!ReadPersistedPathReference(encoded, local), "contradictory local profile relative locator fails closed");
    for (const auto& p : {file, local.application_data_root / "work.npy"}) {
        const auto absolute = PersistedPathReferenceJson(p, local);
        Require(absolute.at("path_kind") == "absolute", "LocalAppData locators stay absolute");
        Require(ReadPersistedPathReference(absolute, moved) == p, "absolute paths never rebase");
    }
    for (const char* invalid : {"", "relative.npy", "C:relative.npy", "\\rooted.npy"}) {
        Require(!ReadPersistedPathReference({{"path_kind", "absolute"}, {"path", invalid}}, portable),
                "absolute locator must be genuinely absolute");
    }
    for (const char* invalid : {"", "..", "../file", "a/../../file", "a/../file", "C:relative.npy",
                               "C:/absolute.npy", "/rooted", "\\rooted", "\\\\server\\share\\file"}) {
        Require(!ReadPersistedPathReference({{"path_kind", "package_relative"}, {"path", invalid}}, portable),
                "package-relative locator must not contain root or traversal semantics");
    }
    for (const nlohmann::json malformed : {nlohmann::json{}, nlohmann::json(3),
            nlohmann::json{{"path_kind", "unknown"}, {"path", "x"}},
            nlohmann::json{{"path_kind", "absolute"}, {"path", 4}},
            nlohmann::json{{"path_kind", "package_relative"}, {"path", std::string("a\0b", 3)}}}) {
        Require(!ReadPersistedPathReference(malformed, portable), "malformed locator fails closed");
    }
    Require(ReadPersistedPathReference({{"path_kind", "package_relative"}, {"path", "."}}, portable)
            == portable.package_root, "package root locator");
    const auto old_path = root / "old" / portable.package_root.filename() / "source.npy";
    fs::create_directories(portable.package_root);
    std::ofstream(portable.package_root / "source.npy") << "different file";
    Require(ReadPersistedPathReference(LocalUserStatePathToUtf8(old_path), portable) == old_path,
            "matching basenames must never guess legacy relocation");
    Require(!ReadPersistedPathReference("relative.npy", portable), "legacy relative strings are not cwd locators");
    RequireFailure([&] { (void)PersistedPathReferenceJson({}, portable); });
    auto invalid_context = portable;
    invalid_context.package_root = "relative-package";
    RequireFailure([&] { (void)PersistedPathReferenceJson(file, invalid_context); });
    Require(!ReadPersistedPathReference(encoded, invalid_context), "unresolved context must not use cwd");

    // Independent contexts can be used concurrently; neither queries process metadata.
    auto worker = std::async(std::launch::async, [=] { return ReadPersistedPathReference(encoded, moved); });
    Require(ReadPersistedPathReference(encoded, portable) == file, "original context remains unchanged");
    Require(worker.get() == moved.package_root / "work" / "source.npy", "worker retains its injected context");
}

void TestReservedNamespaces(const fs::path& root)
{
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto paths = Context(root / "reserved", profile);
        for (const auto* name : {"config", "state", "logs", "unsaved", "CONFIG", "StAtE"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) == UserFilePathStatus::ReservedNamespace,
                    "reserved directory itself is rejected");
            Require(CheckUserFilePath(paths.application_data_root / name / "nested" / "file.npy", paths)
                    == UserFilePathStatus::ReservedNamespace, "reserved descendants are rejected");
        }
        for (const auto* name : {"user.npy", "workspace/file.npy", "config-other/file.npy", "workspace/state/file.npy",
                                "state/../user.npy"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) == UserFilePathStatus::Allowed,
                    "user files outside reserved namespaces are legal");
        }
        Require(CheckUserFilePath(paths.application_data_root / "workspace/../state/file.npy", paths)
                == UserFilePathStatus::ReservedNamespace, "normalized traversal cannot bypass reserved namespace");
        for (const auto* name : {"state./file.npy", "state /file.npy", "state:stream"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) != UserFilePathStatus::Allowed,
                    "Win32 path aliases fail closed");
        }
        Require(CheckUserFilePath("relative.npy", paths) == UserFilePathStatus::Invalid, "no cwd-dependent checks");
        Require(CheckUserFilePath(root / "outside.npy", paths) == UserFilePathStatus::Allowed, "outside user file");
    }
}

void TestConsumersKeepInjectedContext(const fs::path& root)
{
    const auto original = Context(root / "original", StorageProfile::Portable);
    const auto relocated = Context(root / "relocated", StorageProfile::Portable);
    const auto source_path = original.package_root / "workspace" / "source.npy";
    const auto session_cache = root / "session.json";
    SourceCollectionSessionStateCache cache;
    cache.sources.push_back({.path = source_path});
    cache.active_source_index = 0;
    Require(SaveSourceCollectionSessionStateCache(original, session_cache, cache), "write source locator");
    SourceCollectionSession session(session_cache, {}, {}, {},
        SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs, relocated);
    const auto restore = session.TakeDeferredRestorePlan();
    Require(restore && restore->sources.size() == 1 &&
            restore->sources.front().path == relocated.package_root / "workspace/source.npy",
            "session restoration must use injected package rather than current executable package");

    const auto workflow_cache = root / "workflow.json";
    SampleWorkflowStateCache workflow;
    workflow.sources_by_identity["source"].selected_sample_sort_source_id =
        "annotation:" + LocalUserStatePathToUtf8(source_path);
    Require(SaveSampleWorkflowStateCache(original, workflow_cache, workflow), "write annotation locator");
    SampleWorkflowPreparationPaths paths{
        .labeling_state_cache_path = root / "absent-labeling.json",
        .workflow_state_cache_path = workflow_cache,
        .navigation_state_cache_path = root / "absent-navigation.json",
        .runtime_paths = relocated,
    };
    auto worker = std::async(std::launch::async, [paths] {
        return LoadSampleWorkflowPreparationCacheBundle(paths);
    });
    const auto bundle = worker.get();
    Require(bundle.workflow.sources_by_identity.at("source").selected_sample_sort_source_id ==
            "annotation:" + LocalUserStatePathToUtf8(relocated.package_root / "workspace" / "source.npy"),
            "background preparation must carry startup locator context by value");
}
} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / ("specforge-storage-context-" + std::to_string(GetCurrentProcessId()));
    try {
        fs::create_directories(root);
        TestStorageContext(root);
        TestLegacyBrandState(root);
        TestBoundedMigration(root);
        TestLocators(root);
        TestReservedNamespaces(root);
        TestConsumersKeepInjectedContext(root);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
